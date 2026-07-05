#ifndef __RTP_PACKET_H__
#define __RTP_PACKET_H__

#include <stdint.h>
#include <stddef.h>

#define RTP_HEADER_SIZE 12
#define RTP_MAX_PAYLOAD_SIZE 1400
#define RTP_MAX_PACKET_SIZE (RTP_HEADER_SIZE + RTP_MAX_PAYLOAD_SIZE)

typedef struct
{
    bool marker;
    uint8_t data[RTP_MAX_PACKET_SIZE];
    uint8_t payload_type;
    uint16_t sequence_number;
    uint32_t timestamp;
    uint32_t ssrc;
    size_t size;
} rtp_packet_t;

#endif // __RTP_PACKET_H__