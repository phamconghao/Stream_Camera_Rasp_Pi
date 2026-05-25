#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>

#include "capture_thread.h"
#include "shared_frame.h"
#include "v4l2_capture.h"
#include "frame_pool.h"
#include "fps_counter.h"



extern volatile int running;

void *capture_thread_func(void *arg)
{
    (void)arg;

    // RT priority
    struct sched_param param;
    param.sched_priority = 80;

    // CPU affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);

    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0)
    {
        perror("pthread_setschedparam");
    }
    else
    {
        printf("[RT PRIORITY ENABLED]\n");
    }

    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) != 0)
    {
        perror("pthread_setaffinity_np");
    }
    else
    {
        printf("[CAPTURE THREAD CPU0]\n");
    }

    while (running)
    {
        void *frame;

        size_t size;

        int index;

        struct timespec ts;

        
        if (capture_get_frame(&frame, &size, &index) < 0)
        {
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &ts);

        unsigned char *p = frame;
        printf("JPEG header: %02x %02x %02x %02x\n",
               p[0], p[1], p[2], p[3]);

        // printf("[CAPTURE] ptr=%p size=%zu index=%d\n", frame, size, index);

        frame_acquire(index);
        shared_frame_update(frame, size, index, &ts);
        fps_counter_tick();
    }

    return NULL;
}
