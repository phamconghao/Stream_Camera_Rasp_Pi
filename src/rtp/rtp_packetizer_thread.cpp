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
 *   3. For each NAL, pick one of two packing modes:
 *        - fits in one RTP payload -> rtp_packetize_single_nal(), one packet.
 *        - too big (typically an IDR/keyframe) -> FU-A fragmentation:
 *          rtp_fu_a_fragment_count() + a loop of rtp_packetize_fu_a_fragment(),
 *          several packets, one sequence number each, same timestamp.
 *      Every produced packet is pushed into rtp_packet_queue.
 *   4. Advance the RTP timestamp by one frame duration (only once per
 *      access unit - all NALs/fragments belonging to the same frame
 *      share the same RTP timestamp per RFC 3550/6184, only the
 *      sequence number increments per packet).
 *   5. Release the encoded_frame_t back to its pool.
 *
 * The RTP marker bit is set on the packet that completes the access
 * unit: for a single-NAL packet that's nal.is_last_nal; for FU-A it's
 * the LAST fragment of the LAST NAL (nal.is_last_nal && this is the
 * final fragment) - this is how an RTP receiver knows "the frame is
 * complete, start decoding", per RFC 6184 section 5.1.
 */

static const char *TAG = "RTP";

static pthread_t g_rtp_thread;
static uint16_t g_rtp_sequence = 0;   // increments by 1 per RTP packet (wraps naturally at 65535 -> 0, which is valid RTP behavior)
static uint32_t g_rtp_timestamp = 0;  // increments by RTP_TIMESTAMP_STEP once per access unit (frame), shared by every NAL/fragment in that frame

static constexpr uint32_t RTP_CLOCK = 90000;   // RFC 6184 mandates a 90kHz clock rate for H.264 RTP timestamps
static constexpr uint32_t FRAME_RATE = 30;     // assumed capture frame rate; not yet derived from the actual camera config
static constexpr uint32_t RTP_TIMESTAMP_STEP = RTP_CLOCK / FRAME_RATE; // = 3000 ticks per frame at 30fps/90kHz
static constexpr uint32_t RTP_SSRC = 0x12345678; // arbitrary fixed stream identifier; fine for one sender, one receiver
static constexpr uint8_t RTP_PAYLOAD_TYPE_H264 = 96; // dynamic payload type (96-127 range per RFC 3551), negotiated via SDP in a real RTSP session

// Owned exclusively by this module — independent from app_state::g_running
// and from encoder_thread's flag, so the RTP sender can later be
// started/stopped per RTSP session without touching the encoder.
static std::atomic<bool> g_rtp_running(false);

// Fills in the 5 fields every rtp_packet_t needs before either
// rtp_packetize_single_nal() or rtp_packetize_fu_a_fragment() is called
// (both functions expect these to already be set - see rtp_packetizer.h).
static void stamp_packet(rtp_packet_t *packet, bool marker)
{
    packet->sequence_number = g_rtp_sequence++;
    packet->timestamp = g_rtp_timestamp;
    packet->ssrc = RTP_SSRC;
    packet->payload_type = RTP_PAYLOAD_TYPE_H264;
    packet->marker = marker;
}

// Acquires one packet, packs+stamps it, and pushes it into
// rtp_packet_queue, releasing it back to the pool on any failure along
// the way. Shared by both the single-NAL and FU-A paths below.
static void emit_single_nal_packet(const h264_nal_t *nal)
{
    rtp_packet_t *packet = rtp_packet_pool_acquire();
    if (!packet)
    {
        // Pool exhausted (UDP sender too slow to drain it) -
        // drop this one NAL's packet rather than block.
        LOG_WARN(TAG, "RTP packet pool empty");
        return;
    }

    stamp_packet(packet, nal->is_last_nal);

    if (rtp_packetize_single_nal(nal, packet) < 0)
    {
        // Shouldn't happen (caller already checked via
        // rtp_nal_needs_fragmentation before choosing this path), but
        // kept as a safety net.
        LOG_WARN(TAG, "single-NAL packetize failed type=%s size=%zu",
                 h264_nal_type_string(nal->nal_type), nal->size);
        rtp_packet_pool_release(packet);
        return;
    }

    LOG_INFO(TAG, "[RTP] seq=%u ts=%u marker=%d payload=%zu",
             packet->sequence_number,
             packet->timestamp,
             packet->marker ? 1 : 0,
             packet->size - RTP_HEADER_SIZE);

    if (rtp_packet_queue_push(packet) < 0)
    {
        // Downstream (UDP sender) too slow / queue full - drop this
        // packet rather than block the packetizer.
        LOG_WARN(TAG, "RTP packet queue full");
        rtp_packet_pool_release(packet);
    }
}

// Splits `nal` into FU-A fragments (RFC 6184 5.8) and pushes one RTP
// packet per fragment. Used for NALs too large for a single RTP
// payload - typically IDR/keyframes, which run much larger than P-slices.
static void emit_fu_a_packets(const h264_nal_t *nal)
{
    size_t fragment_count = rtp_fu_a_fragment_count(nal);

    LOG_INFO(TAG, "[FU-A] NAL type=%s size=%zu -> %zu fragments",
             h264_nal_type_string(nal->nal_type), nal->size, fragment_count);

    for (size_t frag = 0; frag < fragment_count; frag++)
    {
        rtp_packet_t *packet = rtp_packet_pool_acquire();
        if (!packet)
        {
            // Pool exhausted mid-fragmentation: stop here rather than
            // send a partial/corrupt access unit with gaps - the
            // receiver would fail to reassemble this NAL either way
            // once a fragment is missing, so there's no point
            // continuing to burn packets on the remaining fragments.
            LOG_WARN(TAG, "RTP packet pool empty during FU-A (frag %zu/%zu)", frag, fragment_count);
            break;
        }

        bool is_last_fragment = (frag == fragment_count - 1);
        stamp_packet(packet, is_last_fragment && nal->is_last_nal);

        if (rtp_packetize_fu_a_fragment(nal, frag, packet) < 0)
        {
            LOG_WARN(TAG, "FU-A fragment build failed frag=%zu/%zu", frag, fragment_count);
            rtp_packet_pool_release(packet);
            continue;
        }

        LOG_INFO(TAG, "[RTP-FUA] seq=%u ts=%u marker=%d frag=%zu/%zu payload=%zu",
                 packet->sequence_number,
                 packet->timestamp,
                 packet->marker ? 1 : 0,
                 frag + 1, fragment_count,
                 packet->size - RTP_HEADER_SIZE);

        if (rtp_packet_queue_push(packet) < 0)
        {
            LOG_WARN(TAG, "RTP packet queue full (FU-A frag %zu/%zu)", frag, fragment_count);
            rtp_packet_pool_release(packet);
        }
    }
}

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

            if (rtp_nal_needs_fragmentation(&nal))
            {
                emit_fu_a_packets(&nal);
            }
            else
            {
                emit_single_nal_packet(&nal);
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
