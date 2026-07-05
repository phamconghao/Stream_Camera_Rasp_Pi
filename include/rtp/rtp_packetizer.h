#ifndef __RTP_PACKETIZER_H__
#define __RTP_PACKETIZER_H__

#include "rtp_packet.h"
#include "h264_nal_parser.h"

int rtp_build_header(uint8_t *buffer, uint16_t sequence, uint32_t timestamp, uint32_t ssrc, bool marker, uint8_t payload_type);
int rtp_packetize_single_nal(const h264_nal_t *nal, uint16_t sequence, uint32_t timestamp, uint32_t ssrc, rtp_packet_t *packet);

#endif // __RTP_PACKETIZER_H__