#ifndef __RTP_PACKET_POOL_H__
#define __RTP_PACKET_POOL_H__

#include "rtp_packet.h"

int rtp_packet_pool_init();
void rtp_packet_pool_cleanup();
rtp_packet_t *rtp_packet_pool_acquire();
void rtp_packet_pool_release(rtp_packet_t *packet);

#endif // __RTP_PACKET_POOL_H__