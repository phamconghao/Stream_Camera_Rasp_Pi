#include "capture_thread.h"
#include "v4l2_capture.h"
#include "frame_queue.h"

#include <stdio.h>
#include <pthread.h>
#include <time.h>

extern volatile int running;
extern struct frame_queue frame_q;

static pthread_t capture_tid;
/*
static int running = 0;
static struct frame_queue *frame_q;
*/

// ===============================================
//                  THREAD FUNCTION
// ===============================================
static void *capture_thread_func(void *arg)
{ 
    (void)arg;
    printf("Capture thread started\n");
    /*
    int fps_counter = 0;
    time_t last_time = time(NULL);
    */

    while (running)
    {
        void *frame;
        size_t size;
        struct timespec ts;
        int index;

        // ------------------------
        // Get frame from V4L2
        // ------------------------
        if (capture_get_frame(&frame, &size, &index) < 0)
        {
            continue;
        }

        printf("\n[CAPTURE]\n");
        printf("ptr     = %p\n", frame);
        printf("size    = %zu\n", size);
        printf("index   = %d\n", index);

        // ------------------------
        // Timestamp 
        // ------------------------
        clock_gettime(CLOCK_MONOTONIC, &ts);

        // ------------------------
        // Push to queue
        // ------------------------
        queue_push(&frame_q, frame, size, index, &ts);
        
        printf("[QUEUE PUSHED]\n");
        
        // --------------------------------
        // Release buffer back to driver
        // --------------------------------

        // --------------------------------
        // FPS 
        // --------------------------------
        /*
        fps_counter++;
        time_t now = time(NULL);
        if (now != last_time)
        {
            printf("Capture FPS: %d\n", fps_counter);

            fps_counter = 0;
            last_time = now;
        }
        */
    }

    printf("Capture thread stopped\n");

    return NULL;
}

// ===============================================
//                  START THREAD
// ===============================================
int capture_thread_start(struct frame_queue *queue)
{
    frame_q = *queue;
    running = 1;
    if (pthread_create(&capture_tid, NULL, capture_thread_func, NULL) != 0)
    {
        perror("pthread_create");

        return -1;
    }

    printf("Capture thread running\n");
    return 0;
}

// ===============================================
//                  STOP THREAD
// ===============================================
void capture_thread_stop()
{
    running = 0;
    pthread_join(capture_tid, NULL);
}
