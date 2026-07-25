#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "bcm2835_decoder.h"

/**
 * ============================================================================
 * PIPELINE STAGE: Encoded Frame Queue -> [decoder_thread -> THIS FILE] -> Raw Frame Queue
 * ============================================================================
 *
 * Drives the Raspberry Pi's hardware H.264 decoder - the same BCM2835
 * video codec block as the encoder, exposed by the kernel as a
 * separate V4L2 M2M device at /dev/video10 - directly via ioctl(), no
 * external library. This is the mirror image of bcm2835_encoder.cpp:
 * same QUERYBUF/QBUF/DQBUF/STREAMON dance, just with the OUTPUT and
 * CAPTURE queues carrying the opposite pixel formats:
 *   - OUTPUT queue:  WE feed H.264 access units IN here.
 *   - CAPTURE queue: the driver puts decoded YUV420 frames here for us
 *     to read.
 *
 * NOTE ON DEVICE NODE: /dev/video10 is the conventional bcm2835-codec
 * decode node on Raspberry Pi OS (as /dev/video11 is for encode), but
 * V4L2 device numbering isn't guaranteed - if this fails to open on a
 * given image, run `v4l2-ctl --list-devices` and check which node
 * reports "bcm2835-codec-decode" as its card name, then adjust below.
 *
 * NOTE ON RESOLUTION: unlike a "real" streaming decoder, this skips
 * V4L2's dynamic resolution negotiation (VIDIOC_SUBSCRIBE_EVENT for
 * V4L2_EVENT_SOURCE_CHANGE, which normally fires after the driver has
 * parsed the stream's SPS/PPS). Instead both queues' formats are set
 * up-front from the width/height the caller already knows (this
 * pipeline is 640x480 end-to-end, fixed at compile time on the sender
 * side too - see bcm2835_encoder_encode_file()). Handling a stream
 * whose resolution isn't known ahead of time is a known limitation,
 * left for when RTSP/SDP-negotiated resolution is added.
 *
 * Same known limitation as the encoder: always uses OUTPUT buffer index
 * 0 (safe only because bcm2835_decoder_decode_frame() is fully
 * synchronous - one QBUF+DQBUF pair per call, so there's never more
 * than one OUTPUT buffer in flight).
 */

#define OUTPUT_BUFFER_COUNT 4  // H.264 access-unit input buffers (only index 0 currently used, see limitation above)
#define CAPTURE_BUFFER_COUNT 4 // decoded YUV420 output buffers, rotated normally via capture_index

struct decoder_buffer_t
{
    void *start;   // mmap'd userspace pointer for this V4L2 buffer
    size_t length; // mmap'd region size, as reported by QUERYBUF
};

static int g_fd = -1; // /dev/video10 file descriptor
static decoder_buffer_t g_output_buffers[OUTPUT_BUFFER_COUNT];
static decoder_buffer_t g_capture_buffers[CAPTURE_BUFFER_COUNT];

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

        std::cout << "[DEC] OUTPUT buf " << i << " size = " << planes[0].length << std::endl;
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

        std::cout << "[DEC] CAPTURE buf " << i << " size = " << planes[0].length << std::endl;
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

        std::cout << "[DEC] queue capture " << i << std::endl;
    }

    return 0;
}

static int start_capture_stream()
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (ioctl(g_fd, VIDIOC_STREAMON, &type) < 0)
    {
        perror("STREAMON capture");
        return -1;
    }

    std::cout << "[DEC] capture stream on" << std::endl;

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

    std::cout << "[DEC] output stream on" << std::endl;

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

    std::cout << "[DEC] capture dequeued index = " << buf.index << " bytesused = " << planes[0].bytesused << std::endl;

    return 0;
}

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

    std::cout << "[DEC] output dequeued index = " << buf.index << " bytesused = " << planes[0].bytesused << std::endl;
    return 0;
}

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

// One-time setup: opens /dev/video10, sets the pixel format on both
// queues (H.264 in on OUTPUT, YUV420 out on CAPTURE), requests
// buffers, mmaps them, and starts streaming on both queues so the
// decoder is ready to accept access units via
// bcm2835_decoder_decode_frame().
int bcm2835_decoder_init(int width, int height)
{
    g_fd = open("/dev/video10", O_RDWR);
    if (g_fd < 0)
    {
        perror("open decoder");
        return -1;
    }

    std::cout << "[DEC] open ok fd = " << g_fd << std::endl;

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));

    if (ioctl(g_fd, VIDIOC_QUERYCAP, &cap) < 0)
    {
        perror("VIDIOC_QUERYCAP");
        return -1;
    }

    std::cout << "[DEC] driver = " << cap.driver << std::endl;
    std::cout << "[DEC] card = " << cap.card << std::endl;

    // OUTPUT Queue Format - the compressed H.264 access units we feed in.
    struct v4l2_format fmt_out;
    memset(&fmt_out, 0, sizeof(fmt_out));

    fmt_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt_out.fmt.pix_mp.width = width;
    fmt_out.fmt.pix_mp.height = height;
    fmt_out.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    fmt_out.fmt.pix_mp.num_planes = 1;
    // Generous per-buffer size for one access unit - mirrors
    // MAX_ENCODED_FRAME_SIZE (encoded_frame.h) so a full-size IDR
    // always fits.
    fmt_out.fmt.pix_mp.plane_fmt[0].sizeimage = 1024 * 1024;

    if (ioctl(g_fd, VIDIOC_S_FMT, &fmt_out) < 0)
    {
        perror("VIDIOC_S_FMT OUTPUT");
        return -1;
    }

    std::cout << "[DEC] output format set" << std::endl;

    // CAPTURE Queue Format - the decoded YUV420 frames the driver
    // produces. See the file-level comment: resolution is set
    // up-front rather than negotiated dynamically via SOURCE_CHANGE.
    struct v4l2_format fmt_cap;
    memset(&fmt_cap, 0, sizeof(fmt_cap));

    fmt_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt_cap.fmt.pix_mp.width = width;
    fmt_cap.fmt.pix_mp.height = height;
    fmt_cap.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt_cap.fmt.pix_mp.num_planes = 1;

    if (ioctl(g_fd, VIDIOC_S_FMT, &fmt_cap) < 0)
    {
        perror("VIDIOC_S_FMT CAPTURE");
        return -1;
    }

    std::cout << "[DEC] capture format set" << std::endl;

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

    std::cout << "[DEC] output buffers = " << req_out.count << std::endl;

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

    std::cout << "[DEC] capture buffers = " << req_cap.count << std::endl;

    if (query_output_buffers() < 0)
    {
        return -1;
    }

    if (query_capture_buffers() < 0)
    {
        return -1;
    }

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

    return 0;
}

void bcm2835_decoder_cleanup(void)
{
    if (g_fd >= 0)
    {
        close(g_fd);
        g_fd = -1;
    }
}

// Called by decoder_thread once per access unit. Synchronous/blocking:
// feeds `encoded` into the OUTPUT queue and blocks (via DQBUF) until
// the hardware has produced one decoded YUV420 frame into `raw`.
// Returns 0 on success, -1 on any V4L2 ioctl failure.
int bcm2835_decoder_decode_frame(encoded_frame_t *encoded, raw_frame_t *raw)
{
    uint32_t capture_index;
    uint32_t bytes_used;

    // Always uses OUTPUT buffer index 0 (see the "known limitation"
    // note at the top of this file) - safe today only because this
    // whole function is synchronous.
    memcpy(g_output_buffers[0].start, encoded->data, encoded->size);

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
    planes[0].bytesused = encoded->size;
    planes[0].length = encoded->size;

    // Hand the H.264 access unit to the driver to decode.
    if (ioctl(g_fd, VIDIOC_QBUF, &buf) < 0)
    {
        perror("QBUF OUTPUT");
        return -1;
    }

    // Blocks until the driver has finished decoding this access unit
    // and has a YUV420 frame ready in one of the CAPTURE buffers.
    if (dequeue_capture_buffer(&capture_index, &bytes_used) < 0)
    {
        return -1;
    }

    // Reclaim the OUTPUT buffer we queued above.
    if (dequeue_output_buffer() < 0)
    {
        return -1;
    }

    // Copy the decoded bytes out of the driver's mmap'd CAPTURE buffer
    // into our own raw_frame_t (pool-owned memory), then give that
    // CAPTURE buffer slot back to the driver so it can be reused.
    memcpy(raw->data, g_capture_buffers[capture_index].start, bytes_used);
    queue_capture_buffer(capture_index);
    raw->size = bytes_used;
    raw->pts_us = encoded->pts_us;
    raw->sequence = encoded->sequence;

    return 0;
}
