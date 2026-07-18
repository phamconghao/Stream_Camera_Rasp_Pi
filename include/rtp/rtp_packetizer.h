#ifndef __RTP_PACKETIZER_H__
#define __RTP_PACKETIZER_H__

#include "rtp_packet.h"
#include "h264_nal_parser.h"

/**
 * PIPELINE STAGE: H264 Parser -> [THIS] -> RTP Packet Pool/Queue
 *
 * Pure packing logic (no threading, no queues) - turns h264_nal_t into
 * one or more rtp_packet_t by prepending an RTP header (RFC 3550) and
 * following the RTP-over-H.264 payload rules (RFC 6184). Called by
 * rtp_packetizer_thread once per NAL.
 *
 * Two packing modes:
 *   - Single NAL Unit (RFC 6184 5.6): the whole NAL fits in one RTP
 *     packet. Use rtp_packetize_single_nal().
 *   - FU-A (Fragmentation Unit, RFC 6184 5.8): the NAL is bigger than
 *     one RTP payload (this happens most often for IDR/keyframes, which
 *     tend to be much larger than P-frames) and must be split across
 *     several RTP packets. Use rtp_nal_needs_fragmentation() to decide,
 *     then rtp_fu_a_fragment_count() + rtp_packetize_fu_a_fragment() in
 *     a loop - see rtp_packetizer_thread.cpp for the calling pattern.
 *
 * Contract shared by all the packetize_* functions below: the caller
 * must set packet->sequence_number / timestamp / ssrc / payload_type /
 * marker BEFORE calling - these functions only build the header bytes
 * from those fields plus the NAL payload; they never choose sequence
 * numbers or timestamps themselves.
 */

/**
 * Builds a "Single NAL Unit" RTP packet: the entire NAL is copied as-is
 * into one RTP payload. Only valid when nal fits within
 * RTP_MAX_PAYLOAD_SIZE - check with rtp_nal_needs_fragmentation() first.
 * Returns 0 on success, -1 if the NAL is too large (use FU-A instead).
 */
int rtp_packetize_single_nal(const h264_nal_t *nal, rtp_packet_t *packet);

/**
 * True if `nal` does not fit in a single RTP payload and must be sent
 * as a series of FU-A fragments instead.
 */
bool rtp_nal_needs_fragmentation(const h264_nal_t *nal);

/**
 * Number of FU-A packets `nal` will be split into. Only meaningful when
 * rtp_nal_needs_fragmentation(nal) is true. Returns 0 for invalid input.
 */
size_t rtp_fu_a_fragment_count(const h264_nal_t *nal);

/**
 * Builds the RTP packet for FU-A fragment `fragment_index` (0-based, in
 * [0, rtp_fu_a_fragment_count(nal))) of `nal`. Every fragment shares the
 * same sequence-number/timestamp contract as rtp_packetize_single_nal -
 * the caller is expected to increment packet->sequence_number for each
 * fragment while keeping packet->timestamp constant across all
 * fragments of the same NAL (they belong to the same access unit).
 * Returns 0 on success, -1 on invalid nal/index.
 */
int rtp_packetize_fu_a_fragment(const h264_nal_t *nal, size_t fragment_index, rtp_packet_t *packet);

#endif // __RTP_PACKETIZER_H__
