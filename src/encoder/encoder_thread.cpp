#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <atomic>

#include "raw_frame.h"
#include "raw_frame_pool.h"
#include "raw_frame_queue.h"
#include "encoded_frame.h"
#include "encoded_frame_pool.h"
#include "encoded_frame_queue.h"
#include "encoder_thread.h"
#include "bcm2835_encoder.h"
#include "log.h"

/**
 * ============================================================================
 * PIPELINE STAGE 2 (thread wrapper): Raw Frame Queue -> [THIS] -> Encoded Frame Queue
 * ============================================================================
 *
 * The one dedicated thread of the pipeline's encoding stage. Its job is
 * purely orchestration/pool management - the actual H.264 encoding work
 * happens inside bcm2835_encoder_encode_frame() (hardware-accelerated,
 * blocking call). Loop, once per raw frame:
 *   1. Pop a raw_frame_t from raw_frame_queue (blocks if empty).
 *   2. Acquire a free encoded_frame_t from encoded_frame_pool.
 *   3. Call the hardware encoder to fill it in.
 *   4. Push the encoded_frame_t into encoded_frame_queue for
 *      rtp_packetizer_thread to consume.
 *   5. Release the raw_frame_t back to raw_frame_pool either way.
 */

static const char *TAG = "ENCODER";

static pthread_t g_encoder_thread;

// Owned exclusively by this module. Independent from app_state::g_running
// and from every other thread's flag, so this thread can be started and
// stopped on its own (e.g. later, per-session logic won't need to touch
// any other thread's state).
static std::atomic<bool> g_encoder_running(false);

static void *encoder_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    while (g_encoder_running)
    {
        // Blocks until camera_capture pushes a frame, or until
        // raw_frame_queue_shutdown() is called (returns nullptr then,
        // so the while-loop re-checks g_encoder_running and exits).
        raw_frame_t *raw = raw_frame_queue_pop();
        if (!raw)
        {
            continue;
        }

        encoded_frame_t *encoded = encoded_frame_pool_acquire();
        if (!encoded)
        {
            // Downstream (RTP packetizer) too slow / pool exhausted -
            // drop this frame's encode rather than block.
            LOG_WARN(TAG, "encoded pool empty");
            raw_frame_pool_release(raw);
            continue;
        }

        // Carry PTS/sequence across from raw -> encoded so downstream
        // RTP timestamps stay tied to the original capture time.
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

// Spawns the encoder thread. Safe to call independently of any other
// thread's start/stop (see the file-level comment on g_encoder_running).
int encoder_thread_start(void)
{
    g_encoder_running = true;

    if (pthread_create(&g_encoder_thread, nullptr, encoder_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        return -1;
    }

    return 0;
}

void encoder_thread_stop(void)
{
    g_encoder_running = false;

    // Wake the thread up if it's blocked waiting for a raw frame,
    // otherwise pthread_join below would hang forever once the
    // camera has stopped producing frames.
    raw_frame_queue_shutdown();

    pthread_join(g_encoder_thread, nullptr);
}
