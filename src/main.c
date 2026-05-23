#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include "frame_queue.h"
#include "capture_thread.h"
#include "stream_thread.h"
#include "v4l2_capture.h"
#include "shared_frame.h"

volatile int running = 1;

struct frame_queue frame_q;

static void signal_handler(int sig)
{
    (void)sig;

    running = 0;
}

int main(void)
{
    signal(SIGINT,
           signal_handler);

    shared_frame_init();

    if (capture_init("/dev/video0") < 0)
    {
        printf("capture_init failed\n");

        return -1;
    }

    if (capture_start() < 0)
    {
        printf("capture_start failed\n");

        return -1;
    }

    pthread_t capture_thread;

    pthread_t stream_thread;

    pthread_create(&capture_thread,
                   NULL,
                   capture_thread_func,
                   NULL);

    pthread_create(&stream_thread,
                   NULL,
                   stream_thread_func,
                   NULL);

    pthread_join(capture_thread,
                 NULL);

    pthread_join(stream_thread,
                 NULL);

    capture_close();

    return 0;
}
