#ifndef __RTP_PACKET_QUEUE_H__
#define __RTP_PACKET_QUEUE_H__

#include "rtp_packet.h"

/**
 * PIPELINE STAGE: RTP Packetizer Thread -> [THIS] -> UDP Sender Thread
 *
 * Same bounded ring-buffer pattern as raw_frame_queue/encoded_frame_queue
 * (see raw_frame_queue.cpp for the detailed shutdown-mechanism rationale).
 * Producer is rtp_packetizer_thread, consumer is udp_sender_thread.
 */

int rtp_packet_queue_init(void);
void rtp_packet_queue_cleanup(void);
int rtp_packet_queue_push(rtp_packet_t *packet);

// Blocks until rtp_packetizer_thread pushes a packet, or returns nullptr
// once rtp_packet_queue_shutdown() has been called and the queue has drained.
rtp_packet_t *rtp_packet_queue_pop(void);

/**
 * Wake up any thread blocked in rtp_packet_queue_pop() so it can observe
 * shutdown and exit instead of waiting forever. Called by
 * udp_sender_thread_stop() before pthread_join() - see udp_sender_thread.cpp.
 */
void rtp_packet_queue_shutdown(void);

#endif // __RTP_PACKET_QUEUE_H__
