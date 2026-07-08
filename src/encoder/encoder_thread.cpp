#include <pthread.h>
#include <cstdio>
#include <cstring>

#include "raw_frame.h"
#include "raw_frame_pool.h"
#include "raw_frame_queue.h"
#include "encoded_frame.h"
#include "encoded_frame_pool.h"
#include "encoded_frame_queue.h"
#include "encoder_thread.h"
#include "bcm2835_encoder.h"
#include "app_state.h"
#include "log.h"

static const char *TAG = "ENCODER";

static pthread_t g_encoder_thread;

static void *encoder_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

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
            LOG_WARN(TAG, "encoded pool empty");
            raw_frame_pool_release(raw);
            continue;
        }

        encoded->pts_us = raw->pts_us;
        encoded->sequence = raw->sequence;

        if (bcm2835_encoder_encode_frame(raw, encoded) == 0)
        {
            LOG_INFO(TAG, "frame encoded size = %zu", encoded->size);

            if (encoded_frame_queue_push(encoded) < 0)
            {
                LOG_WARN(TAG, "encoded queue full");
                encoded_frame_pool_release(encoded);
            }
        }
        else
        {
            LOG_WARN(TAG, "encode failed seq = %u", encoded->sequence);
            encoded_frame_pool_release(encoded);
        }

        raw_frame_pool_release(raw);
    }

    LOG_INFO(TAG, "thread exit");

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

    // Wake the thread up if it's blocked waiting for a raw frame,
    // otherwise pthread_join below would hang forever once the
    // camera has stopped producing frames.
    raw_frame_queue_shutdown();

    pthread_join(g_encoder_thread, nullptr);
}
