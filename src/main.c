#include "v4l2_capture.h"
#include "frame_queue.h"
#include "capture_thread.h"
#include "stream_thread.h"
#include "signal_handler.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int main()
{
    struct frame_queue *queue = malloc(sizeof(struct frame_queue));
    if (!queue)
    {
        printf("Queue Mallocation failed\n");
        return -1;
    }
    queue_init(queue);
    
    setup_signals();

    // ------------------------
    // Camera init
    // ------------------------
    if (capture_init("/dev/video0") < 0)
    {
        printf("capture_init failed\n");
        return -1;
    }
    
    printf("capture_init OK\n");

    if (capture_start() < 0)
    {
        printf("capture_init failed\n");
        return -1;
    }
    printf("capture_start OK\n");

    // ------------------------
    // Threads 
    // ------------------------
    if (capture_thread_start(queue) < 0)
    {
        return -1;
    }

    if (stream_thread_start(queue) < 0)
    {
        return -1;
    }

    // ------------------------
    // Main loop 
    // ------------------------
    while (1)
    {
        sleep(1);

        printf("\n");
        printf("============== STATS ===============\n");
        printf("Queue count     : %d\n", queue->count);
        printf("Frames pushed   : %lu\n", queue->pushed_frames);
        printf("Frames popped   : %lu\n", queue->popped_frames);
        printf("Frames dropped  : %lu\n", queue->dropped_frames);
        printf("====================================\n");
    }

    capture_thread_stop();
    stream_thread_stop();
    capture_stop();
    capture_close();

    return 0;
}
