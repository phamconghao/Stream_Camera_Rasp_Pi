#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <errno.h>

#include "v4l2_capture.h"

#define WIDTH 640
#define HEIGHT 480
#define BUFFER_COUNT 4

struct buffer
{
    void *start;
    size_t length;
};

static int fd = -1;
static struct buffer buffers[BUFFER_COUNT];
static struct v4l2_buffer current_buf;

// ==========================================
//                  INIT
// ==========================================

int capture_init(const char *device)
{
    fd = open(device, O_RDWR);

    printf("fd = %d\n", fd);
    if (fd < 0)
    {
        perror("open");
        return 1;
    }

    // Set format
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = WIDTH;
    fmt.fmt.pix.height = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        perror("VIDIOC_S_FMT");
        return -1;
    }

    // Set FPS
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 30;

    if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0)
    {
        perror("VIDIOC_S_PARM");
    }
    else
    {
        printf("[FPS SET] %d FPS\n", parm.parm.capture.timeperframe.denominator / parm.parm.capture.timeperframe.numerator);
    }

    // Request buffers
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0)
    {
        perror("VIDIOC_REQBFS");
        return -1;
    }

    // mmap buffers
    for (int i = 0; i < BUFFER_COUNT; i++)
    {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            perror("VIDIOC_QUERYBUF");
            return -1;
        }

        buffers[i].length = buf.length;
        buffers[i].start = mmap(
            NULL,
            buf.length,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd,
            buf.m.offset
        );

        if (buffers[i].start == MAP_FAILED)
        {
            perror("mmap");
            return -1;
        }

        // Queue buffer
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
        {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }

    printf("Camera init OK\n");
    return 0;
}

// ================================================
//                  START STREAM
// ================================================
int capture_start()
{
    // Start stream
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0)
    {
        perror("VIDIOC_STREAMON");
        return -1;
    }

    printf("Camera stream started\n");
    return 0;
}

// =================================================
//                  GET FRAME
// =================================================
int capture_get_frame(void **data, size_t *size, int *index)
{
    memset(&current_buf, 0, sizeof(current_buf));
    current_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    current_buf.memory = V4L2_MEMORY_MMAP;

    // Dequeue filled buffer
    if (ioctl(fd, VIDIOC_DQBUF, &current_buf) < 0)
    {
        printf("DQBUG fd = %d\n", fd);
        perror("VIDIOC_DQBUF");
        return -1;
    }

    *index = current_buf.index;
    *data = buffers[current_buf.index].start;
    *size = current_buf.bytesused;

    return 0;
}

// ==================================================
//                  RELEASE FRAME
// ==================================================
void capture_release_frame(int index)
{
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;

    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
    {
        perror("VIDIOC_QBUF");
    }
}

// ===================================================
//                  STOP STREAM
// ===================================================
void capture_stop()
{    
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE; 
    ioctl(fd, VIDIOC_STREAMOFF, &type); 
}

// ====================================================
//                  CLOSE DEVICE
// ====================================================
void capture_close()
{
    for (int i = 0; i < BUFFER_COUNT; i++)
    {
        munmap(buffers[i].start, buffers[i].length);
    }

    if (fd >= 0)
    {
        close(fd);
    }

    fd = -1;
}

