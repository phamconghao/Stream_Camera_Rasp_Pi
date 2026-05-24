#include <stdio.h>
#include <time.h>

#include "capture_thread.h"
#include "shared_frame.h"
#include "v4l2_capture.h"
#include "frame_pool.h"

extern volatile int running;

void *capture_thread_func(void *arg)
{
    (void)arg;

    while (running)
    {
        void *frame;

        size_t size;

        int index;

        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC, &ts);

        if (capture_get_frame(&frame,
                              &size,
                              &index) < 0)
        {
            continue;
        }
        unsigned char *p = frame;
        printf("JPEG header: %02x %02x %02x %02x\n",
               p[0], p[1], p[2], p[3]);

        printf("[CAPTURE] ptr=%p size=%zu index=%d\n", frame, size, index);

        frame_acquire(index);
        shared_frame_update(frame, size, index, &ts);
    }

    return NULL;
}
