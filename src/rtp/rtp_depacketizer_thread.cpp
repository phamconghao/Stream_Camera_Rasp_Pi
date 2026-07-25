#include <pthread.h>
#include <cstdio>
#include <atomic>

#include "rtp_depacketizer_thread.h"
#include "rtp_jitter_buffer.h"
#include "rtp_packet_pool.h"
#include "rtp_depacketizer.h"
#include "encoded_frame_pool.h"
#include "encoded_frame_queue.h"
#include "keyframe_requester.h"
#include "log.h"

/**
 * ============================================================================
 * PIPELINE STAGE: RTP Jitter Buffer -> [THIS] -> Encoded Frame Pool/Queue
 * ============================================================================
 *
 * Loop, once per RTP packet popped (in sequence order) from the jitter
 * buffer:
 *   1. If the jitter buffer reports packets were lost right before this
 *      one, discard whatever access unit is mid-reassembly - a gap
 *      means the bitstream from here to the next access unit boundary
 *      is corrupt (see rtp_depacketizer_reset()) - and ask the sender
 *      for a fresh keyframe (Phase 18 recovery, see keyframe_requester.h)
 *      so the decoder gets back a clean picture quickly instead of
 *      waiting for the next regularly-scheduled IDR.
 *   2. Lazily acquire an encoded_frame_t on the first packet of a new
 *      access unit.
 *   3. Feed the packet into rtp_depacketizer_process_packet(), which
 *      appends reconstructed NAL bytes into the access unit buffer.
 *   4. On marker bit (access unit complete): push it to
 *      encoded_frame_queue for decoder_thread, optionally mirror the
 *      raw Annex-B bytes to disk for offline inspection, and reset
 *      state for the next access unit.
 *   5. Release the rtp_packet_t back to its pool either way.
 */

static const char *TAG = "DEPKT_THREAD";

// Elementary-stream dump alongside the decoded YUV output (see
// yuv_writer.cpp for the decoded side) - lets you verify the
// depacketizer's reassembly independently of whether the decoder
// itself is working, e.g. by feeding this file to `ffplay received.h264`.
// Relative path: written into the current working directory the
// receiver was launched from (same convention as yuv_writer's
// output_path argument), so this works on any machine/user account
// rather than a path specific to one development environment.
#define H264_DUMP_PATH "received.h264"

static pthread_t g_depacketizer_thread;
static std::atomic<bool> g_depacketizer_running(false);
static FILE *g_h264_dump = nullptr;

static void *rtp_depacketizer_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    rtp_depacketizer_state_t state;
    rtp_depacketizer_state_init(&state);

    encoded_frame_t *current_au = nullptr;

    while (g_depacketizer_running)
    {
        uint32_t lost_count = 0;
        rtp_packet_t *packet = rtp_jitter_buffer_pop(&lost_count);
        if (!packet)
        {
            continue;
        }

        if (lost_count > 0)
        {
            LOG_WARN(TAG, "%u packet(s) lost - discarding in-progress access unit", lost_count);
            rtp_depacketizer_reset(&state, current_au);
            if (current_au)
            {
                encoded_frame_pool_release(current_au);
                current_au = nullptr;
            }

            // Phase 18 recovery: rather than silently waiting for
            // whatever the next regularly-scheduled IDR happens to be
            // (which could be several seconds away), ask the sender to
            // force one now. keyframe_requester_request() is
            // internally rate-limited, so it's safe to call this on
            // every single loss event without flooding the control
            // channel.
            keyframe_requester_request();
        }

        if (!current_au)
        {
            current_au = encoded_frame_pool_acquire();
            if (!current_au)
            {
                // Pool exhausted (decoder too slow) - drop this packet,
                // its access unit is lost either way once we resume.
                LOG_WARN(TAG, "encoded frame pool empty, dropping packet seq=%u", packet->sequence_number);
                rtp_packet_pool_release(packet);
                continue;
            }
        }

        bool access_unit_complete = rtp_depacketizer_process_packet(&state, packet, current_au);

        if (access_unit_complete)
        {
            LOG_INFO(TAG, "access unit complete, size=%zu", current_au->size);

            if (g_h264_dump && current_au->size > 0)
            {
                fwrite(current_au->data, 1, current_au->size, g_h264_dump);
                fflush(g_h264_dump);
            }

            if (current_au->size > 0)
            {
                if (encoded_frame_queue_push(current_au) < 0)
                {
                    LOG_WARN(TAG, "encoded frame queue full, dropping access unit");
                    encoded_frame_pool_release(current_au);
                }
            }
            else
            {
                // Nothing usable was reassembled (e.g. every NAL in
                // this access unit got dropped along the way).
                encoded_frame_pool_release(current_au);
            }

            current_au = nullptr;
        }

        rtp_packet_pool_release(packet);
    }

    if (current_au)
    {
        encoded_frame_pool_release(current_au);
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

int rtp_depacketizer_thread_start(void)
{
    g_h264_dump = fopen(H264_DUMP_PATH, "wb");
    if (!g_h264_dump)
    {
        LOG_WARN(TAG, "could not open %s for h264 dump - continuing without it", H264_DUMP_PATH);
    }

    g_depacketizer_running = true;

    if (pthread_create(&g_depacketizer_thread, nullptr, rtp_depacketizer_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        g_depacketizer_running = false;
        return -1;
    }

    return 0;
}

void rtp_depacketizer_thread_stop(void)
{
    g_depacketizer_running = false;

    // Wake the thread up if it's blocked in rtp_jitter_buffer_pop(),
    // otherwise pthread_join below would hang forever once
    // udp_receiver_thread has stopped feeding the jitter buffer.
    rtp_jitter_buffer_shutdown();

    pthread_join(g_depacketizer_thread, nullptr);

    if (g_h264_dump)
    {
        fclose(g_h264_dump);
        g_h264_dump = nullptr;
    }
}
