#include <pthread.h>
#include <cstdio>
#include <atomic>

#include "decoder_thread.h"
#include "bcm2835_decoder.h"
#include "encoded_frame_pool.h"
#include "encoded_frame_queue.h"
#include "raw_frame_pool.h"
#include "raw_frame_queue.h"
#include "log.h"

/**
 * ============================================================================
 * PIPELINE STAGE: Encoded Frame Queue -> [THIS] -> Raw Frame Queue
 * ============================================================================
 *
 * Mirror image of encoder_thread.cpp. Loop, once per access unit:
 *   1. Pop an encoded_frame_t from encoded_frame_queue (blocks if empty).
 *   2. Acquire a free raw_frame_t from raw_frame_pool.
 *   3. Call the hardware decoder to fill it in.
 *   4. Push the raw_frame_t into raw_frame_queue for yuv_writer_thread.
 *   5. Release the encoded_frame_t back to encoded_frame_pool either way.
 */

static const char *TAG = "DECODER";

static pthread_t g_decoder_thread;
static std::atomic<bool> g_decoder_running(false);

static void *decoder_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    while (g_decoder_running)
    {
        encoded_frame_t *encoded = encoded_frame_queue_pop();
        if (!encoded)
        {
            continue;
        }

        raw_frame_t *raw = raw_frame_pool_acquire();
        if (!raw)
        {
            // Downstream (YUV writer) too slow / pool exhausted -
            // drop this access unit's decode rather than block.
            LOG_WARN(TAG, "raw frame pool empty");
            encoded_frame_pool_release(encoded);
            continue;
        }

        if (bcm2835_decoder_decode_frame(encoded, raw) == 0)
        {
            LOG_INFO(TAG, "frame decoded size = %zu", raw->size);

            if (raw_frame_queue_push(raw) < 0)
            {
                LOG_WARN(TAG, "raw frame queue full");
                raw_frame_pool_release(raw);
            }
        }
        else
        {
            LOG_WARN(TAG, "decode failed seq = %u", encoded->sequence);
            raw_frame_pool_release(raw);
        }

        encoded_frame_pool_release(encoded);
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

int decoder_thread_start(int width, int height)
{
    if (bcm2835_decoder_init(width, height) < 0)
    {
        return -1;
    }

    g_decoder_running = true;

    if (pthread_create(&g_decoder_thread, nullptr, decoder_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        bcm2835_decoder_cleanup();
        g_decoder_running = false;
        return -1;
    }

    return 0;
}

void decoder_thread_stop(void)
{
    g_decoder_running = false;

    // Wake the thread up if it's blocked waiting for an access unit,
    // otherwise pthread_join below would hang forever once
    // rtp_depacketizer_thread has stopped producing them.
    encoded_frame_queue_shutdown();

    pthread_join(g_decoder_thread, nullptr);

    bcm2835_decoder_cleanup();
}
