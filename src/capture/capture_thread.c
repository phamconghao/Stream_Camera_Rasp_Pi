#include <stdio.h>
#include <time.h>

#include "capture_thread.h"
#include "frame_queue.h"
#include "v4l2_capture.h"

extern volatile int running;

extern struct frame_queue frame_q;

void *capture_thread_func(void *arg)
{
    (void)arg;

    while (running)
    {
        void *frame;

        size_t size;

        int index;

        struct timespec ts;

        clock_gettime(CLOCK_MONOTONIC,
                      &ts);

        if (capture_get_frame(&frame,
                              &size,
                              &index) < 0)
        {
            continue;
        }

        printf("[CAPTURE] ptr=%p size=%zu index=%d\n",
               frame,
               size,
               index);

        queue_push(&frame_q,
                   frame,
                   size,
                   index,
                   &ts);
    }

    return NULL;
}
