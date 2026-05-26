#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>

#include "capture_thread.h"
#include "stream_thread.h"
#include "shared_frame.h"
#include "frame_pool.h"

static volatile int running = 1;

/*
 * ============================================================
 *                  SIGNAL HANDLER
 * ============================================================
 */

static void signal_handler(int sig)
{
    (void)sig;

    printf("\n[CTRL+C]\n");

    running = 0;

    stream_thread_stop();

    capture_thread_stop();
}

/*
 * ============================================================
 *                          MAIN
 * ============================================================
 */

int main(void)
{
    pthread_t capture_tid;

    pthread_t stream_tid;

    /*
     * Ignore SIGPIPE
     */

    signal(SIGPIPE, SIG_IGN);

    /*
     * Ctrl+C handler
     */

    signal(SIGINT, signal_handler);

    /*
     * Init shared frame
     */

    shared_frame_init();

    /*
     * Init frame ownership pool
     */

    frame_pool_init();

    /*
     * Create capture thread
     */

    if (pthread_create(&capture_tid, NULL, capture_thread_func, NULL) != 0)
    {
        perror("pthread_create capture");

        return -1;
    }

    /*
     * Create stream thread
     */

    if (pthread_create(&stream_tid, NULL, stream_thread_func, NULL) != 0)
    {
        perror("pthread_create stream");

        return -1;
    }

    /*
     * Main loop
     */

    while (running)
    {
        sleep(1);
    }

    /*
     * Wait threads
     */

    pthread_join(capture_tid, NULL);

    pthread_join(stream_tid, NULL);

    /*
     * Cleanup
     */

    shared_frame_cleanup();

    printf("[APP EXIT]\n");

    return 0;
}