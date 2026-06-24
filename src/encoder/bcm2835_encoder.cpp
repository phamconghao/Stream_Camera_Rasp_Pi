#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "bcm2835_encoder.h"

#define OUTPUT_BUFFER_COUNT 4
#define CAPTURE_BUFFER_COUNT 4

struct encoder_buffer_t
{
    void *start;
    size_t length;
};

static int g_fd = -1;
static encoder_buffer_t g_output_buffers[OUTPUT_BUFFER_COUNT];
static encoder_buffer_t g_capture_buffers[CAPTURE_BUFFER_COUNT];

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

int bcm2835_encoder_encode_file(const char *input_file, const char *output_file)
{
    FILE *fp = fopen(input_file, "rb");
    if (!fp)
    {
        perror("fopen input");

        return -1;
    }

    const size_t frame_size = 640 * 480 * 3 / 2; // YUV420 frame size for 640x480
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

int bcm2835_encoder_encode_frame(raw_frame_t *raw, encoded_frame_t *encoded)
{
    uint32_t capture_index;
    uint32_t bytes_used;

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

    if (ioctl(g_fd, VIDIOC_QBUF, &buf) < 0)
    {
        perror("QBUF OUTPUT");
        return -1;
    }

    if (dequeue_capture_buffer(&capture_index, &bytes_used) < 0)
    {
        return -1;
    }

    if (dequeue_output_buffer() < 0)
    {
        return -1;
    }

    memcpy(encoded->data, g_capture_buffers[capture_index].start, bytes_used);
    queue_capture_buffer(capture_index);
    encoded->size = bytes_used;
    encoded->pts_us = raw->pts_us;

    return 0;
}