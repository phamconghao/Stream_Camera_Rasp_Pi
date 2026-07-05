#include <iostream>

#include "encoded_frame.h"
#include "encoded_frame_pool.h"
#include "encoded_frame_queue.h"
#include "rtp_packetizer_thread.h"
#include "h264_nal_parser.h"

extern bool g_running;

void *rtp_packetizer_thread_func(void *arg)
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