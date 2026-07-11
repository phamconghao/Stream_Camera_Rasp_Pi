#ifndef __RTP_PACKET_QUEUE_H__
#define __RTP_PACKET_QUEUE_H__

#include "rtp_packet.h"

int rtp_packet_queue_init(void);
void rtp_packet_queue_cleanup(void);
int rtp_packet_queue_push(rtp_packet_t *packet);
rtp_packet_t *rtp_packet_queue_pop(void);

#endif // __RTP_PACKET_QUEUE_H__