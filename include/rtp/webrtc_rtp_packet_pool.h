#ifndef __WEBRTC_RTP_PACKET_POOL_H__
#define __WEBRTC_RTP_PACKET_POOL_H__

#include "rtp_packet.h"

/**
 * PIPELINE STAGE: RTP Packetizer -> [THIS pool] -> WebRTC RTP Packet Queue -> WebRTC Sender Thread
 *
 * A separate pool from rtp_packet_pool.h, holding plaintext copies of
 * the same packets rtp_packetizer_thread produces for the RTSP path.
 *
 * A copy is used instead of sharing rtp_packet_t instances across both
 * paths because rtp_packet_t (see rtp_packet.h) deliberately has no
 * reference counting - it assumes exactly one consumer at a time.
 * Reusing that assumption for a second consumer would be a
 * double-free/use-after-release hazard. A copy is also necessary once
 * SRTP is involved: webrtc_sender_thread.cpp must encrypt a different
 * copy of each packet per viewer (different SRTP keys per session), so
 * one buffer could never be shared across destinations on this path.
 *
 * Reuses rtp_packet_t as the element type purely for convenience (same
 * shape, same buffer size) - these slots are otherwise independent
 * from rtp_packet_pool's.
 */

int webrtc_rtp_packet_pool_init(void);
void webrtc_rtp_packet_pool_cleanup(void);

rtp_packet_t *webrtc_rtp_packet_pool_acquire(void);
void webrtc_rtp_packet_pool_release(rtp_packet_t *packet);

#endif // __WEBRTC_RTP_PACKET_POOL_H__
