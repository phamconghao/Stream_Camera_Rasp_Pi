#include <iostream>
#include <pthread.h>
#include <atomic>

#include "encoded_frame.h"
#include "encoded_frame_pool.h"
#include "encoded_frame_queue.h"
#include "rtp_packetizer_thread.h"
#include "h264_nal_parser.h"
#include "app_state.h"

static pthread_t g_rtp_thread;

static void *rtp_packetizer_thread_func(void *arg)
{
    (void)arg;

    std::cout << "[RTP] thread started" << std::endl;

    while (g_running)
    {
        encoded_frame_t *encoded = encoded_frame_queue_pop();
        if (!encoded)
        {
            continue;
        }

        std::cout << "[RTP] frame seq = " << encoded->sequence << " size = " << encoded->size << std::endl;

        /**
         * TODO:
         * 
         * Parser init
         * 
         * Parse each NAL
         * 
         * In:
         *      type
         *      size
         */

        encoded_frame_pool_release(encoded);
    }

    std::cout << "[RTP] thread exit" << std::endl;

    return nullptr;
}

int rtp_packetizer_thread_start(void)
{
    g_running = true;

    if (pthread_create(&g_rtp_thread, nullptr, rtp_packetizer_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        return -1;
    }

    return 0;
}

void rtp_packetizer_thread_stop(void)
{
    g_running = false;

    // Wake the thread up if it's blocked waiting for an encoded frame,
    // otherwise pthread_join below would hang forever.
    encoded_frame_queue_shutdown();

    pthread_join(g_rtp_thread, nullptr);
}