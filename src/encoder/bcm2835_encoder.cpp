#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "bcm2835_encoder.h"

/**
 * ============================================================================
 * PIPELINE STAGE 2: Raw Frame Queue -> [encoder_thread -> THIS FILE] -> Encoded Frame Queue
 * ============================================================================
 *
 * Drives the Raspberry Pi's hardware H.264 encoder (the BCM2835 video
 * codec, exposed by the kernel as a V4L2 "Memory-to-Memory" (M2M)
 * device at /dev/video11) directly via ioctl(), with no external
 * library (no GStreamer/libav) - just raw V4L2 calls.
 *
 * V4L2 M2M concept for anyone unfamiliar: a normal V4L2 device has one
 * queue (e.g. a webcam's CAPTURE queue). An M2M *codec* device instead
 * exposes TWO queues on the same file descriptor:
 *   - OUTPUT queue:  where WE feed data IN (here: raw YUV420 frames).
 *     ("output" is named from the *driver's* point of view - output
 *     from userspace into the driver - not the final result.)
 *   - CAPTURE queue: where the driver puts the RESULT for us to read
 *     (here: encoded H.264 access units).
 * Both queues use the same QUERYBUF/QBUF/DQBUF/STREAMON ioctl dance as
 * a regular V4L2 capture device, just applied to two independent queues.
 *
 * Buffer flow per frame (see bcm2835_encoder_encode_frame at the bottom):
 *   1. Copy raw YUV420 bytes into an mmap'd OUTPUT buffer, QBUF it
 *      (hand it to the driver to encode).
 *   2. DQBUF the CAPTURE queue (blocks until the driver has produced
 *      one encoded access unit) - copy its bytes into `encoded->data`.
 *   3. DQBUF the OUTPUT queue (reclaim the buffer we queued in step 1).
 *   4. Re-QBUF the CAPTURE buffer so the driver can reuse that slot.
 *
 * Known limitation (not yet fixed): this always uses OUTPUT buffer
 * index 0 for every frame instead of rotating through
 * OUTPUT_BUFFER_COUNT buffers. This works because the encode call is
 * fully synchronous (QBUF then immediately DQBUF before returning), so
 * there's never more than one OUTPUT buffer in flight at a time - but
 * it means this encoder cannot be pipelined/parallelized across frames
 * without also fixing this to rotate buffer indices.
 */

#define OUTPUT_BUFFER_COUNT 4  // YUV420 input buffers (see limitation above: only index 0 is currently used)
#define CAPTURE_BUFFER_COUNT 4 // H.264 output buffers, rotated normally via capture_index

struct encoder_buffer_t
{
    void *start;   // mmap'd userspace pointer for this V4L2 buffer
    size_t length; // mmap'd region size, as reported by QUERYBUF
};

static int g_fd = -1; // /dev/video11 file descriptor
static encoder_buffer_t g_output_buffers[OUTPUT_BUFFER_COUNT];
static encoder_buffer_t g_capture_buffers[CAPTURE_BUFFER_COUNT];

// QUERYBUF asks the driver where each buffer lives in the device's
// memory (as an mmap offset); we then mmap() each one into our address
// space so we can memcpy into/out of them directly.
// QUERYBUF for OUTPUT queue helper
static int query_output_buffers()
{
    for (uint32_t i = 0; i < OUTPUT_BUFFER_COUNT; i++)
    {
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;

        if (ioctl(g_fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            perror("QUERYBUF OUTPUT");

            return -1;
        }

        g_output_buffers[i].length = planes[0].length;
        g_output_buffers[i].start = mmap(nullptr,
                                        planes[0].length,
                                        PROT_READ | PROT_WRITE,
                                        MAP_SHARED,
                                        g_fd,
                                        planes[0].m.mem_offset);
        
        if (g_output_buffers[i].start == MAP_FAILED)
        {
            perror("mmap output");

            return -1;
        }
        
        std::cout << "[ENC] OUTPUT buf " << i << " size = " << planes[0].length << std::endl;
    }

    return 0;
}

// QUERYBUF for CAPTURE queue helper
static int query_capture_buffers()
{
    for (uint32_t i = 0; i < CAPTURE_BUFFER_COUNT; i++)
    {
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;

        if (ioctl(g_fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            perror("QUERYBUF CAPTURE");

            return -1;
        }

        g_capture_buffers[i].length = planes[0].length;
        g_capture_buffers[i].start = mmap(nullptr,
                                        planes[0].length,
                                        PROT_READ | PROT_WRITE,
                                        MAP_SHARED,
                                        g_fd,
                                        planes[0].m.mem_offset);

        if (g_capture_buffers[i].start == MAP_FAILED)
        {
            perror("mmap capture");

            return -1;
        }
        
        std::cout << "[ENC] CAPTURE buf " << i << " size = " << planes[0].length << std::endl;
    }

    return 0;
}

// QUEUE CAPTURE buffers helper
static int queue_capture_buffers()
{
    for (uint32_t i = 0; i < CAPTURE_BUFFER_COUNT; i++)
    {
        struct v4l2_plane planes[1];
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.m.planes = planes;
        buf.length = 1;

        if (ioctl(g_fd, VIDIOC_QBUF, &buf) < 0)
        {
            perror("QBUF capture");
            return -1;
        }

        std::cout << "[ENC] queue capture " << i << std::endl;
    }

    return 0;
}

// Start streaming on capture and output queues helper
static int start_capture_stream()
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (ioctl(g_fd, VIDIOC_STREAMON, &type) < 0)
    {
        perror("STREAMON capture");
        return -1;
    }

    std::cout << "[ENC] capture stream on" << std::endl;

    return 0;
}

static int start_output_stream()
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

    if (ioctl(g_fd, VIDIOC_STREAMON, &type) < 0)
    {
        perror("STREAMON output");
        return -1;
    }

    std::cout << "[ENC] output stream on" << std::endl;

    return 0;
}

// DEQUEUE CAPTURE buffer helper
static int dequeue_capture_buffer(uint32_t *buffer_index, uint32_t *bytes_used)
{
    struct v4l2_plane planes[1];
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = 1;

    if (ioctl(g_fd, VIDIOC_DQBUF, &buf) < 0)
    {
        perror("VIDIOC_DQBUF CAPTURE");

        return -1;
    }

    *buffer_index = buf.index;
    *bytes_used = planes[0].bytesused;

    std::cout << "[ENC] capture dequeued index = " << buf.index << " bytesused = " << planes[0].bytesused << std::endl;

    return 0;
}

// DEQUEUE Output Buffer
static int dequeue_output_buffer()
{
    struct v4l2_plane planes[1];
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = 1;

    if (ioctl(g_fd, VIDIOC_DQBUF, &buf) < 0)
    {
        perror("DQBUF OUTPUT");

        return -1;
    }

    std::cout << "[ENC] output dequeued index = " << buf.index << " bytesused = " << planes[0].bytesused << std::endl;
    return 0;
}

// QUEUE CAPTURE buffer helper
static int queue_capture_buffer(uint32_t index)
{
    struct v4l2_plane planes[1];
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    buf.memory = V4L2_MEMORY_MMAP;

    buf.index = index;

    buf.m.planes = planes;

    buf.length = 1;

    if (ioctl(g_fd, VIDIOC_QBUF, &buf) < 0)
    {
        perror("QBUF CAPTURE");

        return -1;
    }

    return 0;
}

// One-time setup: opens /dev/video11, sets the pixel format on both
// queues (YUV420 in on OUTPUT, H.264 out on CAPTURE), requests buffers,
// mmaps them, and starts streaming on both queues so the encoder is
// ready to accept frames via bcm2835_encoder_encode_frame().
int bcm2835_encoder_init(int width, int height)
{
    g_fd = open("/dev/video11", O_RDWR);
    if (g_fd < 0)
    {
        perror("open encoder");
        return -1;
    }

    std::cout << "[ENC] open ok fd = " << g_fd << std::endl;

    // QUERY CAP
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));

    if (ioctl(g_fd, VIDIOC_QUERYCAP, &cap) < 0)
    {
        perror("VIDIOC_QUERYCAP");
        return -1;
    }

    std::cout << "[ENC] driver = " << cap.driver << std::endl;
    std::cout << "[ENC] card = " << cap.card << std::endl;

    // OUTPUT Queue Format
    // Queue gets YUV format from camera
    struct v4l2_format fmt_out;
    memset(&fmt_out, 0, sizeof(fmt_out));

    fmt_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt_out.fmt.pix_mp.width = width;
    fmt_out.fmt.pix_mp.height = height;
    fmt_out.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt_out.fmt.pix_mp.num_planes = 1;

    if (ioctl(g_fd, VIDIOC_S_FMT, &fmt_out) < 0)
    {
        perror("VIDIOC_S_FMT OUTPUT");
        return -1;
    }

    std::cout << "[ENC] output format set" << std::endl;

    // CAPTURE Queue Format
    // Queue stores H264 output
    struct v4l2_format fmt_cap;
    memset(&fmt_cap, 0, sizeof(fmt_cap));

    fmt_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt_cap.fmt.pix_mp.width = width;
    fmt_cap.fmt.pix_mp.height = height;
    fmt_cap.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    fmt_cap.fmt.pix_mp.num_planes = 1;

    if (ioctl(g_fd, VIDIOC_S_FMT, &fmt_cap) < 0)
    {
        perror("VIDIOC_S_FMT CAPTURE");
        return -1;
    }

    std::cout << "[ENC] capture format set" << std::endl;

    // Force Baseline profile - bcm2835-codec's own default is High
    // profile, which the WebRTC path cannot use: browsers only ever
    // offer a small, fixed set of (payload type, profile-level-id)
    // pairs for H.264 in their SDP offer, and this project's SDP
    // answer builder (webrtc_sdp.cpp) just reuses whichever payload
    // type the offer listed FIRST rather than picking one whose
    // declared profile actually matches what this encoder produces.
    // Left at the driver default (High), that PT's decoder gets
    // initialized for Baseline (per the offer) while actually
    // receiving High-profile bitstream (CABAC, 8x8 transform, etc. -
    // syntax Baseline can't parse) - packets arrive and decrypt fine,
    // but nothing ever decodes, silently. Baseline is what every
    // browser's first/default H.264 entry expects, and is also what
    // this project's RTSP path (VLC/ffplay) already tolerates fine, so
    // this doesn't cost either path anything.
    struct v4l2_control profile_ctrl;
    memset(&profile_ctrl, 0, sizeof(profile_ctrl));
    profile_ctrl.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
    profile_ctrl.value = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;

    if (ioctl(g_fd, VIDIOC_S_CTRL, &profile_ctrl) < 0)
    {
        perror("S_CTRL H264_PROFILE");
        return -1;
    }

    std::cout << "[ENC] profile set to baseline" << std::endl;

    // Level must be high enough for the actual frame size or the
    // bitstream is non-conformant: H.264 Annex A caps macroblocks/frame
    // (MaxFS) and macroblocks/sec (MaxMBPS) per level. At 1920x1080,
    // coded height rounds up to 1088 (68 macroblock rows) per H.264's
    // 16x16 macroblock grid, i.e. 120x68 = 8160 MBs/frame - Level 3.1
    // (previously used, sized for 640x480's 1200 MBs/frame) only allows
    // MaxFS=3600, nowhere near enough. Level 4.2 comfortably covers
    // 8160 MBs/frame @ 30fps (244800 MB/s, matching this project's
    // fixed 30fps target - see rtp_packetizer_thread.h) with headroom
    // (MaxMBPS=522240), vs. level 4.0's 245760 MaxMBPS being an
    // uncomfortably tight ~0.4% margin for the same math.
    //
    // Because this raises the level above what browsers' own H.264
    // offer entries declare for the Baseline PT (profile-level-id=
    // 42001f = level 3.1), build_webrtc_sdp_answer() must also set
    // level-asymmetry-allowed=1 in the answer's fmtp - RFC 6184 8.2.2:
    // without it, a receiver is entitled to assume it will never see a
    // level higher than what it offered, and Chrome/Edge's decoder
    // factory can and does hold to that when deciding whether to even
    // open a decoder for the SPS it receives.
    struct v4l2_control level_ctrl;
    memset(&level_ctrl, 0, sizeof(level_ctrl));
    level_ctrl.id = V4L2_CID_MPEG_VIDEO_H264_LEVEL;
    level_ctrl.value = V4L2_MPEG_VIDEO_H264_LEVEL_4_2;

    if (ioctl(g_fd, VIDIOC_S_CTRL, &level_ctrl) < 0)
    {
        perror("S_CTRL H264_LEVEL");
        return -1;
    }

    std::cout << "[ENC] level set to 4.2" << std::endl;

    // Without this, bcm2835-codec only ever emits SPS/PPS ONCE, right
    // after this init - every later IDR (including ones force-requested
    // via bcm2835_encoder_force_keyframe(), e.g. for a newly-joined
    // WebRTC viewer) goes out as a bare IDR slice with no parameter
    // sets alongside it. RTSP viewers don't notice: VLC/ffmpeg bootstrap
    // their decoder from the SDP answer's sprop-parameter-sets and never
    // need an in-band SPS/PPS at all. WebRTC's decoder does NOT do that
    // - aiortc and every browser tested only ever initialize their H.264
    // decoder from an in-band SPS/PPS actually present in the RTP
    // stream. A viewer that joins after the one-time initial emission
    // therefore never receives usable parameter sets and can never
    // decode a single frame, no matter how many (parameter-set-less)
    // IDRs it's sent - exactly the "connects fine, zero frames decoded"
    // bug this fixes. Setting this control makes the encoder re-emit
    // SPS/PPS before every IDR, matching what RTSP's own SDP path
    // already assumed was happening.
    struct v4l2_control repeat_seq_ctrl;
    memset(&repeat_seq_ctrl, 0, sizeof(repeat_seq_ctrl));
    repeat_seq_ctrl.id = V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER;
    repeat_seq_ctrl.value = 1;

    if (ioctl(g_fd, VIDIOC_S_CTRL, &repeat_seq_ctrl) < 0)
    {
        perror("S_CTRL REPEAT_SEQ_HEADER");
        return -1;
    }

    std::cout << "[ENC] repeat-sequence-header (SPS/PPS before every IDR) enabled" << std::endl;

    // REQBUFS for OUTPUT queue
    struct v4l2_requestbuffers req_out;
    memset(&req_out, 0, sizeof(req_out));
    req_out.count = OUTPUT_BUFFER_COUNT;
    req_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req_out.memory = V4L2_MEMORY_MMAP;

    if (ioctl(g_fd, VIDIOC_REQBUFS, &req_out) < 0)
    {
        perror("VIDIOC_REQBUFS OUTPUT");
        return -1;
    }

    std::cout << "[ENC] output buffers = " << req_out.count << std::endl;
    
    // REQBUFS for CAPTURE queue
    struct v4l2_requestbuffers req_cap;
    memset(&req_cap, 0, sizeof(req_cap));
    req_cap.count = CAPTURE_BUFFER_COUNT;
    req_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req_cap.memory = V4L2_MEMORY_MMAP;

    if (ioctl(g_fd, VIDIOC_REQBUFS, &req_cap) < 0)
    {
        perror("VIDIOC_REQBUFS CAPTURE");
        return -1;
    }

    std::cout << "[ENC] capture buffers = " << req_cap.count << std::endl;
    
    query_output_buffers();
    query_capture_buffers();
    
    if (queue_capture_buffers() < 0)
    {
        return -1;
    }

    if (start_capture_stream() < 0)
    {
        return -1;
    }

    if (start_output_stream() < 0)
    {
        return -1;
    }

    std::cout << "[ENC] capture sizeimage = " << fmt_cap.fmt.pix_mp.plane_fmt[0].sizeimage << std::endl;

    return 0;
}

void bcm2835_encoder_cleanup(void)
{
    if (g_fd >= 0)
    {
        close(g_fd);
        g_fd = -1;
    }
}

// Debug/offline utility: encodes a single raw YUV420 file straight from
// disk to an H.264 file on disk, bypassing the whole raw_frame/queue
// pipeline. Useful for testing the encoder in isolation (e.g. with a
// dumped frame_000.yuv from camera_capture.cpp) without needing the
// camera or the rest of the threads running. Not used by the live
// pipeline (main.cpp calls bcm2835_encoder_encode_frame() instead).
int bcm2835_encoder_encode_file(const char *input_file, const char *output_file)
{
    FILE *fp = fopen(input_file, "rb");
    if (!fp)
    {
        perror("fopen input");

        return -1;
    }

    const size_t frame_size = 1920 * 1080 * 3 / 2; // YUV420 frame size for 1920x1080
    std::vector<uint8_t> yuv_data(frame_size);
    size_t n = fread(yuv_data.data(), 1, frame_size, fp);

    if (n != frame_size)
    {
        std::cout << "short read" << std::endl;
        fclose(fp);

        return -1;
    }

    memcpy(g_output_buffers[0].start, yuv_data.data(), frame_size);

    struct v4l2_plane planes[1];
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.m.planes = planes;
    buf.length = 1;
    buf.field = V4L2_FIELD_NONE;
    planes[0].bytesused = frame_size;

    if (ioctl(g_fd, VIDIOC_QBUF, &buf) < 0)
    {
        perror("QBUF encode file");
        fclose(fp);
        return -1;
    }

    std::cout << "[ENC] output queued" << std::endl;

    uint32_t capture_index;
    uint32_t bytes_used;

    if (dequeue_capture_buffer(&capture_index, &bytes_used) < 0)
    {
        fclose(fp);
        return -1;
    }

    // Write output
    FILE *out = fopen(output_file, "wb");
    if (!out)
    {
        perror("fopen output");
        fclose(fp);

        return -1;
    }
    fwrite(g_capture_buffers[capture_index].start, 1, bytes_used, out);
    
    fclose(out);
    fclose(fp);

    return 0;
}

// Called by encoder_thread once per raw frame. Synchronous/blocking:
// feeds `raw` into the OUTPUT queue and blocks (via DQBUF) until the
// hardware has produced one encoded H.264 access unit into `encoded`.
// See the file-level comment above for the full QBUF/DQBUF buffer dance.
// Returns 0 on success, -1 on any V4L2 ioctl failure.
int bcm2835_encoder_encode_frame(raw_frame_t *raw, encoded_frame_t *encoded)
{
    uint32_t capture_index;
    uint32_t bytes_used;

    // Always uses OUTPUT buffer index 0 (see the "known limitation"
    // note at the top of this file) - safe today only because this
    // whole function is synchronous (one QBUF+DQBUF pair per call).
    memcpy(g_output_buffers[0].start, raw->data, raw->size);

    struct v4l2_plane planes[1];
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.m.planes = planes;
    buf.length = 1;
    buf.field = V4L2_FIELD_NONE;
    planes[0].bytesused = raw->size;
    planes[0].length = raw->size;

    // Hand the raw YUV420 frame to the driver to encode.
    if (ioctl(g_fd, VIDIOC_QBUF, &buf) < 0)
    {
        perror("QBUF OUTPUT");
        return -1;
    }

    // Blocks until the driver has finished encoding this frame and has
    // an H.264 access unit ready in one of the CAPTURE buffers.
    if (dequeue_capture_buffer(&capture_index, &bytes_used) < 0)
    {
        return -1;
    }

    // Reclaim the OUTPUT buffer we queued above (the driver is done
    // reading from it now that the CAPTURE side produced output).
    if (dequeue_output_buffer() < 0)
    {
        return -1;
    }

    // Copy the encoded bytes out of the driver's mmap'd CAPTURE buffer
    // into our own encoded_frame_t (pool-owned memory), then give that
    // CAPTURE buffer slot back to the driver so it can be reused for
    // the next frame.
    memcpy(encoded->data, g_capture_buffers[capture_index].start, bytes_used);
    queue_capture_buffer(capture_index);
    encoded->size = bytes_used;
    encoded->pts_us = raw->pts_us;
    encoded->sequence = raw->sequence;

    return 0;
}

// V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME is a "button" control: writing
// any value to it (the value itself is ignored) tells the encoder
// "make the next frame you encode an IDR", independent of whatever
// GOP/I-frame-period it's otherwise following. VIDIOC_S_CTRL is safe
// to call concurrently with the QBUF/DQBUF traffic in
// bcm2835_encoder_encode_frame() from another thread - V4L2 drivers
// serialize ioctl() calls on the same fd internally, and this control
// only affects which buffer's output gets marked as a keyframe, not
// the OUTPUT/CAPTURE queue state itself.
int bcm2835_encoder_force_keyframe(void)
{
    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME;
    ctrl.value = 1;

    if (ioctl(g_fd, VIDIOC_S_CTRL, &ctrl) < 0)
    {
        perror("S_CTRL FORCE_KEY_FRAME");
        return -1;
    }

    return 0;
}

// V4L2_CID_MPEG_VIDEO_BITRATE takes effect on subsequently encoded
// frames without needing to reinitialize the device or drop any
// buffers already in flight - safe to call live, same as
// FORCE_KEY_FRAME above.
int bcm2835_encoder_set_bitrate(uint32_t bitrate_bps)
{
    struct v4l2_control ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.id = V4L2_CID_MPEG_VIDEO_BITRATE;
    ctrl.value = static_cast<int32_t>(bitrate_bps);

    if (ioctl(g_fd, VIDIOC_S_CTRL, &ctrl) < 0)
    {
        perror("S_CTRL BITRATE");
        return -1;
    }

    return 0;
}