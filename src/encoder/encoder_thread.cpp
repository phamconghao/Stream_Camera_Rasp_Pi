#include <iostream>
#include <pthread.h>
#include <atomic>
#include <cstring>

#include "raw_frame.h"
#include "raw_frame_pool.h"
#include "raw_frame_queue.h"
#include "encoded_frame.h"
#include "encoded_frame_pool.h"
#include "encoded_frame_queue.h"
#include "encoder_thread.h"
#include "bcm2835_encoder.h"

static pthread_t g_encoder_thread;
static std::atomic<bool> g_running(false);

static void *encoder_thread_func(void *arg)
{
    (void)arg;

    std::cout << "[ENCODER] thread started" << std::endl;

    while (g_running)
    {
        raw_frame_t *raw = raw_frame_queue_pop();
        if (!raw)
        {
            continue;
        }

        encoded_frame_t *encoded = encoded_frame_pool_acquire();
        if (!encoded)
        {
            std::cout << "[ENCODER] encoded pool empty" << std::endl;
            raw_frame_pool_release(raw);
            continue;
        }

        // memcpy(encoded->data, raw->data, raw->size);
        // encoded->size = raw->size;
        // encoded->pts_us = raw->pts_us;
        // encoded->sequence = raw->sequence;

        // std::cout << "[ENCODER] seq = " << encoded->sequence << " size = " << encoded->size << std::endl;

        if (bcm2835_encoder_encode_frame(raw, encoded) == 0)
        {
            if (encoded_frame_queue_push(encoded) < 0)
            {
                std::cout << "[ENC] queue full" << std::endl;
                encoded_frame_pool_release(encoded);
            }
        }
        else
        {
            encoded_frame_pool_release(encoded);
        }

        std::cout << "[ENC] frame encoded size = " << encoded->size << std::endl;
        
        raw_frame_pool_release(raw);
    }

    std::cout << "[ENCODER] thread exit" << std::endl;

    return nullptr;
}

int encoder_thread_start(void)
{
    g_running = true;

    if (pthread_create(&g_encoder_thread, nullptr, encoder_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        return -1;
    }

    return 0;
}

void encoder_thread_stop(void)
{
    g_running = false;
    pthread_join(g_encoder_thread, nullptr);
}