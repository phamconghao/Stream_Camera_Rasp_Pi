#ifndef __RTP_PACKET_QUEUE_H__
#define __RTP_PACKET_QUEUE_H__

#include "rtp_packet.h"

int rtp_packet_queue_init(void);
void rtp_packet_queue_cleanup(void);
int rtp_packet_queue_push(rtp_packet_t *packet);
rtp_packet_t *rtp_packet_queue_pop(void);

/**
 * Wake up any thread blocked in rtp_packet_queue_pop() so it can observe
 * shutdown and exit instead of waiting forever. After this call,
 * rtp_packet_queue_pop() returns nullptr immediately once the queue is
 * empty. Not used yet (no consumer thread exists), but call this from
 * whatever thread_stop() eventually reads this queue (e.g. a future UDP
 * sender thread) before pthread_join, the same way raw_frame_queue and
 * encoded_frame_queue already do — otherwise that stop() will hang
 * forever once its producer stops.
 */
void rtp_packet_queue_shutdown(void);

#endif // __RTP_PACKET_QUEUE_H__