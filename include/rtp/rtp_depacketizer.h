#ifndef __RTP_DEPACKETIZER_H__
#define __RTP_DEPACKETIZER_H__

#include <cstdint>
#include <cstddef>

#include "rtp_packet.h"
#include "encoded_frame.h"

/**
 * ============================================================================
 * PIPELINE STAGE: RTP Jitter Buffer -> [THIS, pure logic] -> Encoded Frame Pool/Queue
 * ============================================================================
 *
 * Mirror image of rtp_packetizer.h on the transmit side. Pure functions,
 * no threading, no I/O - used by rtp_depacketizer_thread.
 *
 * Two jobs, per RFC 6184:
 *   1. Parse the 12-byte RTP header out of a raw received datagram
 *      (rtp_depacketize_header) so the jitter buffer/depacketizer
 *      thread can read sequence_number/marker/etc. without hand-rolling
 *      the byte offsets everywhere.
 *   2. Reassemble the H.264 bitstream from RTP payloads
 *      (rtp_depacketizer_process_packet): each packet's payload is
 *      either a Single-NAL (RFC 6184 5.6, one whole NAL per packet) or
 *      one fragment of an FU-A (5.8, one NAL split across several
 *      packets, used for the IDR frames rtp_packetizer.cpp fragments on
 *      the sender side). Either way, the reconstructed bytes are
 *      appended to `out_frame` with an Annex-B start code prepended to
 *      each NAL, so the result is byte-for-byte the same
 *      encoded_frame_t format bcm2835_encoder produced on the sender -
 *      bcm2835_decoder can consume it directly.
 */

// H.264/RFC 6184 NAL header type field values relevant to depacketizing
// (the rest, 1-23, are all "single NAL unit packet" per 5.6).
#define RTP_NAL_TYPE_FU_A 28

// Holds in-progress FU-A reassembly state across multiple packets. One
// instance lives for the whole depacketizer thread lifetime (see
// rtp_depacketizer_thread.cpp) since FU-A fragments of one NAL can
// arrive spread across several rtp_depacketizer_process_packet() calls.
typedef struct
{
    bool in_progress;        // true between an FU-A start (S=1) fragment and its end (E=1) fragment
    uint8_t reconstructed_nal_header; // (FU indicator & 0xE0) | (FU header type), written once at S=1
} rtp_depacketizer_state_t;

void rtp_depacketizer_state_init(rtp_depacketizer_state_t *state);

/**
 * Parses the fixed 12-byte RTP header (RFC 3550) out of `data`/`size`
 * into `out_packet`'s header fields (sequence_number, timestamp, ssrc,
 * payload_type, marker) and copies the raw bytes into out_packet->data
 * for rtp_depacketizer_process_packet() to read the payload back out
 * of. Returns 0 on success, -1 if `size` is smaller than RTP_HEADER_SIZE
 * (malformed/truncated datagram).
 */
int rtp_depacketize_header(const uint8_t *data, size_t size, rtp_packet_t *out_packet);

/**
 * Feeds one already-header-parsed packet (in sequence order - callers
 * must only pass packets popped from rtp_jitter_buffer_pop(), never
 * out of order) into the reassembly state machine. Appends
 * reconstructed NAL bytes (Annex-B start code + payload) to
 * `out_frame->data`, advancing `out_frame->size`. `out_frame` must have
 * been freshly acquired (size == 0) at the start of a new access unit.
 *
 * Returns true if this packet completed an access unit (its marker bit
 * was set and reassembly was in a valid state) - the caller should push
 * `out_frame` downstream and acquire a new one for the next access
 * unit. Returns false otherwise (more packets needed).
 */
bool rtp_depacketizer_process_packet(rtp_depacketizer_state_t *state,
                                      const rtp_packet_t *packet,
                                      encoded_frame_t *out_frame);

/**
 * Discards any FU-A reassembly in progress and resets `out_frame` back
 * to empty (size = 0). Called by rtp_depacketizer_thread whenever
 * rtp_jitter_buffer_pop() reports lost packets, since a gap in the
 * sequence means whatever NAL/access-unit was mid-reassembly is now
 * corrupt and must not be handed to the decoder.
 */
void rtp_depacketizer_reset(rtp_depacketizer_state_t *state, encoded_frame_t *out_frame);

#endif // __RTP_DEPACKETIZER_H__
