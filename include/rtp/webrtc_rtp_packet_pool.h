#ifndef __WEBRTC_RTP_PACKET_POOL_H__
#define __WEBRTC_RTP_PACKET_POOL_H__

#include "rtp_packet.h"

/**
 * PIPELINE STAGE: RTP Packetizer -> [THIS pool] -> WebRTC RTP Packet Queue -> WebRTC Sender Thread
 *
 * PHASE 22.6.1: a SEPARATE pool from rtp_packet_pool.h, holding
 * plaintext COPIES of the same packets rtp_packetizer_thread already
 * produces for the RTSP path (rtp_packet_pool.h/rtp_packet_queue.h).
 *
 * Why a copy instead of sharing rtp_packet_t instances across both
 * paths: rtp_packet_t (see rtp_packet.h) deliberately has NO
 * reference counting - "each rtp_packet_t only ever has exactly one
 * consumer (udp_sender_thread) at a time" per its own header comment.
 * Reusing that assumption for a second consumer would be a real
 * double-free/use-after-release hazard. A copy sidesteps that
 * entirely, AND is necessary regardless once SRTP enters the picture:
 * webrtc_sender_thread.cpp (Phase 22.6.4) must encrypt a DIFFERENT
 * copy of each packet per WebRTC viewer (different SRTP keys per
 * session), so there was never a way to share one buffer across every
 * destination on this path even in principle.
 *
 * Reuses rtp_packet_t as the element type purely for convenience
 * (same shape, same RTP_MAX_PACKET_SIZE-sized buffer) - these slots
 * are otherwise completely independent from rtp_packet_pool's.
 */

int webrtc_rtp_packet_pool_init(void);
void webrtc_rtp_packet_pool_cleanup(void);

rtp_packet_t *webrtc_rtp_packet_pool_acquire(void);
void webrtc_rtp_packet_pool_release(rtp_packet_t *packet);

#endif // __WEBRTC_RTP_PACKET_POOL_H__
