#ifndef __RTP_PACKET_POOL_H__
#define __RTP_PACKET_POOL_H__

#include "rtp_packet.h"

/**
 * PIPELINE STAGE: RTP Packetizer -> [THIS pool] -> RTP Packet Queue -> UDP Sender
 *
 * Same fixed-size pool pattern as raw_frame_pool/encoded_frame_pool -
 * pre-allocated slots (RTP_PACKET_POOL_SIZE, see rtp_packet_pool.cpp),
 * no per-packet malloc/free, thread-safe acquire/release. No ref-
 * counting here (unlike raw_frame_t) since each rtp_packet_t only ever
 * has exactly one consumer (udp_sender_thread) at a time.
 */

int rtp_packet_pool_init();
void rtp_packet_pool_cleanup();

// Check out a free rtp_packet_t. Returns nullptr if the pool is
// exhausted (udp_sender_thread too slow to drain it) - caller
// (rtp_packetizer_thread) must drop that packet rather than block.
rtp_packet_t *rtp_packet_pool_acquire();

// Return a packet to the pool once udp_sender_thread has sent it (or
// once rtp_packetizer_thread decides to drop it before sending).
void rtp_packet_pool_release(rtp_packet_t *packet);

#endif // __RTP_PACKET_POOL_H__
