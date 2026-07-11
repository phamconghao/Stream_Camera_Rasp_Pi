#ifndef __RTP_PACKETIZER_H__
#define __RTP_PACKETIZER_H__

#include "rtp_packet.h"
#include "h264_nal_parser.h"

int rtp_packetize_single_nal(const h264_nal_t *nal, rtp_packet_t *packet);

#endif // __RTP_PACKETIZER_H__