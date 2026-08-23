#ifndef __WEBRTC_RTP_PACKET_QUEUE_H__
#define __WEBRTC_RTP_PACKET_QUEUE_H__

#include "rtp_packet.h"

/**
 * PIPELINE STAGE: RTP Packetizer Thread -> [THIS] -> WebRTC Sender Thread
 *
 * Same bounded ring-buffer pattern as rtp_packet_queue.h, a separate
 * queue instance feeding webrtc_sender_thread.cpp instead of
 * udp_sender_thread. See webrtc_rtp_packet_pool.h for why this path
 * needs its own pool/queue rather than sharing rtp_packet_queue's.
 */

int webrtc_rtp_packet_queue_init(void);
void webrtc_rtp_packet_queue_cleanup(void);
int webrtc_rtp_packet_queue_push(rtp_packet_t *packet);

// Blocks until rtp_packetizer_thread pushes a packet, or returns
// nullptr once webrtc_rtp_packet_queue_shutdown() has been called and
// the queue has drained.
rtp_packet_t *webrtc_rtp_packet_queue_pop(void);

void webrtc_rtp_packet_queue_shutdown(void);

#endif // __WEBRTC_RTP_PACKET_QUEUE_H__
