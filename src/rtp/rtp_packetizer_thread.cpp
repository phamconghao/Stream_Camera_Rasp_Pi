#include <pthread.h>
#include <cstdio>
#include <atomic>

#include "encoded_frame_pool.h"
#include "encoded_frame_queue.h"
#include "rtp_packet_pool.h"
#include "rtp_packet_queue.h"
#include "rtp_packetizer.h"
#include "rtp_packetizer_thread.h"
#include "h264_nal_parser.h"
#include "log.h"

/**
 * ============================================================================
 * PIPELINE STAGE: Encoded Frame Queue -> [THIS] -> RTP Packet Pool/Queue
 * ============================================================================
 *
 * Consumes whole H.264 access units and turns them into a stream of RTP
 * packets ready for udp_sender_thread. Loop, once per access unit:
 *   1. Pop an encoded_frame_t from encoded_frame_queue.
 *   2. Walk every NAL inside it via h264_nal_parser (SPS/PPS/slice...).
 *   3. For each NAL: acquire an rtp_packet_t, stamp it with the current
 *      sequence/timestamp/SSRC/payload-type, pack the NAL into it via
 *      rtp_packetize_single_nal(), and push it into rtp_packet_queue.
 *   4. Advance the RTP timestamp by one frame duration (only once per
 *      access unit - all NALs belonging to the same frame share the
 *      same RTP timestamp per RFC 3550/6184, only the sequence number
 *      increments per packet).
 *   5. Release the encoded_frame_t back to its pool.
 *
 * The RTP marker bit (packet->marker) is set on the LAST NAL of the
 * access unit (nal.is_last_nal) - this is how an RTP receiver knows
 * "this packet completes one video frame", per RFC 6184 section 5.1.
 */

static const char *TAG = "RTP";

static pthread_t g_rtp_thread;
static uint16_t g_rtp_sequence = 0;   // increments by 1 per RTP packet (wraps naturally at 65535 -> 0, which is valid RTP behavior)
static uint32_t g_rtp_timestamp = 0;  // increments by RTP_TIMESTAMP_STEP once per access unit (frame), shared by every NAL in that frame

static constexpr uint32_t RTP_CLOCK = 90000;   // RFC 6184 mandates a 90kHz clock rate for H.264 RTP timestamps
static constexpr uint32_t FRAME_RATE = 30;     // assumed capture frame rate; not yet derived from the actual camera config
static constexpr uint32_t RTP_TIMESTAMP_STEP = RTP_CLOCK / FRAME_RATE; // = 3000 ticks per frame at 30fps/90kHz
static constexpr uint32_t RTP_SSRC = 0x12345678; // arbitrary fixed stream identifier; fine for one sender, one receiver
static constexpr uint8_t RTP_PAYLOAD_TYPE_H264 = 96; // dynamic payload type (96-127 range per RFC 3551), negotiated via SDP in a real RTSP session

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
        // Blocks until encoder_thread pushes an access unit, or returns
        // nullptr once encoded_frame_queue_shutdown() has been called.
        encoded_frame_t *encoded = encoded_frame_queue_pop();
        if (!encoded)
        {
            continue;
        }

        LOG_INFO(TAG, "[ENCODED] frame seq=%u size=%zu", encoded->sequence, encoded->size);

        h264_nal_parser_t parser;
        h264_nal_parser_init(&parser, encoded->data, encoded->size);

        h264_nal_t nal;
        while (h264_nal_parser_next(&parser, &nal))
        {
            LOG_INFO(TAG, "[NAL] %s size=%zu last=%d",
                     h264_nal_type_string(nal.nal_type),
                     nal.size,
                     nal.is_last_nal ? 1 : 0);

            rtp_packet_t *packet = rtp_packet_pool_acquire();
            if (!packet)
            {
                // Pool exhausted (UDP sender too slow to drain it) -
                // drop this one NAL's packet rather than block.
                LOG_WARN(TAG, "RTP packet pool empty");
                continue;
            }

            packet->sequence_number = g_rtp_sequence++;
            packet->timestamp = g_rtp_timestamp;
            packet->ssrc = RTP_SSRC;
            packet->payload_type = RTP_PAYLOAD_TYPE_H264;
            packet->marker = nal.is_last_nal;

            if (rtp_packetize_single_nal(&nal, packet) < 0)
            {
                // NAL too big for one RTP payload - FU-A fragmentation
                // would handle this but isn't implemented yet, so it's
                // dropped (this would show up as a corrupted/incomplete
                // frame on the receiver until FU-A is added).
                LOG_WARN(TAG, "drop NAL type=%s size=%zu: FU-A not implemented yet",
                         h264_nal_type_string(nal.nal_type),
                         nal.size);
                rtp_packet_pool_release(packet);
                continue;
            }

            LOG_INFO(TAG, "[RTP] seq=%u ts=%u marker=%d payload=%zu",
                     packet->sequence_number,
                     packet->timestamp,
                     packet->marker ? 1 : 0,
                     packet->size - RTP_HEADER_SIZE);

            if (rtp_packet_queue_push(packet) < 0)
            {
                // Downstream (UDP sender) too slow / queue full - drop
                // this packet rather than block the packetizer.
                LOG_WARN(TAG, "RTP packet queue full");
                rtp_packet_pool_release(packet);
            }
        }

        // One timestamp increment per access unit, not per NAL/packet -
        // every packet from this frame already got the timestamp value
        // set above, before this line executes.
        g_rtp_timestamp += RTP_TIMESTAMP_STEP;

        encoded_frame_pool_release(encoded);
        LOG_INFO(TAG, "[ENCODED] frame released");
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

// Spawns the RTP packetizer thread. Independent of every other thread's
// start/stop (see g_rtp_running above).
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
