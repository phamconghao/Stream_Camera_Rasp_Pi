#include <pthread.h>
#include <cstdio>
#include <atomic>

#include "encoded_frame.h"
#include "encoded_frame_pool.h"
#include "encoded_frame_queue.h"
#include "rtp_packetizer_thread.h"
#include "h264_nal_parser.h"
#include "log.h"

static const char *TAG = "RTP";

static pthread_t g_rtp_thread;

// Owned exclusively by this module — independent from app_state::g_running
// and from encoder_thread's flag, so the RTP sender can later be
// started/stopped per RTSP session without touching the encoder.
static std::atomic<bool> g_rtp_running(false);

static void *rtp_packetizer_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    while (g_rtp_running)
    {
        encoded_frame_t *encoded = encoded_frame_queue_pop();
        if (!encoded)
        {
            continue;
        }

        LOG_INFO(TAG, "frame seq = %u size = %zu", encoded->sequence, encoded->size);

        h264_nal_parser_t parser;
        h264_nal_parser_init(&parser, encoded->data, encoded->size);

        h264_nal_t nal;
        while (h264_nal_parser_next(&parser, &nal))
        {
            LOG_INFO(TAG, "  NAL type = %s size = %zu",
                     h264_nal_type_string(nal.nal_type), nal.size);

            /**
             * TODO (Phase 8):
             *   - if nal.size fits in one RTP payload (< MTU), send as
             *     a Single NAL Unit packet
             *   - otherwise, split into FU-A fragments
             *   - SPS/PPS pairs can be combined into a STAP-A packet
             */
        }

        encoded_frame_pool_release(encoded);
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

int rtp_packetizer_thread_start(void)
{
    g_rtp_running = true;

    if (pthread_create(&g_rtp_thread, nullptr, rtp_packetizer_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        return -1;
    }

    return 0;
}

void rtp_packetizer_thread_stop(void)
{
    g_rtp_running = false;

    // Wake the thread up if it's blocked waiting for an encoded frame,
    // otherwise pthread_join below would hang forever.
    encoded_frame_queue_shutdown();

    pthread_join(g_rtp_thread, nullptr);
}
