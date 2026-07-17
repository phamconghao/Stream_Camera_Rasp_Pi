#ifndef __RTP_PACKETIZER_H__
#define __RTP_PACKETIZER_H__

#include "rtp_packet.h"
#include "h264_nal_parser.h"

/**
 * PIPELINE STAGE: H264 Parser -> [THIS] -> RTP Packet Pool/Queue
 *
 * Pure packing logic (no threading, no queues) - turns one h264_nal_t
 * into one rtp_packet_t by prepending an RTP header (RFC 3550) and
 * setting the RTP-over-H.264 payload type (RFC 6184). Called by
 * rtp_packetizer_thread once per NAL.
 */

/**
 * Builds a "Single NAL Unit" RTP packet (RFC 6184 section 5.6): the
 * entire NAL is copied as-is into one RTP payload. Only valid when
 * nal->size <= RTP_MAX_PAYLOAD_SIZE; NALs larger than that are not yet
 * supported (would need FU-A fragmentation, RFC 6184 section 5.8 -
 * not implemented). Returns 0 on success, -1 if the NAL is too large.
 */
int rtp_packetize_single_nal(const h264_nal_t *nal, rtp_packet_t *packet);

#endif // __RTP_PACKETIZER_H__
