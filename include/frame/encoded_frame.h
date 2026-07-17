#ifndef __ENCODED_FRAME_H__
#define __ENCODED_FRAME_H__

#include <stdint.h>
#include <stddef.h>
#include <atomic>

/**
 * ============================================================================
 * PIPELINE STAGE: Encoder -> Encoded Frame Pool/Queue -> RTP Packetizer Thread
 * ============================================================================
 *
 * encoded_frame_t holds ONE H.264 access unit in Annex-B format (i.e.
 * `data` contains one or more complete NAL units back-to-back, each
 * still prefixed with its 00 00 01 / 00 00 00 01 start code, exactly
 * as produced by the BCM2835 hardware encoder).
 *
 * Lifecycle: encoder_thread fills one of these via bcm2835_encoder_
 * encode_frame(), pushes the pointer into encoded_frame_queue, and
 * rtp_packetizer_thread pops it, walks every NAL inside via
 * h264_nal_parser, and releases it back to the pool once every NAL has
 * been turned into RTP packet(s).
 */

// 1MB is generous headroom for a single 640x480 H.264 access unit
// (a keyframe/IDR is the largest case; this project targets far less
// in practice at that resolution).
#define MAX_ENCODED_FRAME_SIZE (1024 * 1024)

typedef struct
{
    uint8_t *data;          // Annex-B H.264 bytes (may contain multiple NAL units), owned by encoded_frame_pool
    size_t size;            // actual bytes filled in `data` for this access unit
    uint64_t pts_us;        // carried over from the source raw_frame_t's pts_us
    uint32_t sequence;      // carried over from the source raw_frame_t's sequence
    std::atomic<int> ref_count; // pool bookkeeping, same convention as raw_frame_t
} encoded_frame_t;

#endif  // __ENCODED_FRAME_H__
