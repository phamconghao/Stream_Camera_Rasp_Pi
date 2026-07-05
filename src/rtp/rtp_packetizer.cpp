#include <arpa/inet.h>
#include <cstring>

#include "rtp_packetizer.h"

int rtp_build_header(uint8_t *buffer, uint16_t sequence, uint32_t timestamp, uint32_t ssrc, bool marker, uint8_t payload_type)
{
    if (buffer == nullptr)
    {
        return -1;
    }

    memset(buffer, 0, RTP_HEADER_SIZE);

    /**
     * Byte 0
     * 
     * Version = 2
     * Padding = 0
     * Extension = 0
     * CSRC Count = 0
     */
    buffer[0] = 0x80;

    /**
     * Byte 1
     * 
     * Marker
     * Payload Type
     */
    buffer[1] = (marker ? 0x80: 0x00) | (payload_type & 0x7F);

    uint16_t seq = htons(sequence);
    memcpy(buffer + 2, &seq, sizeof(seq));

    uint32_t ts = htonl(timestamp);
    memcpy(buffer + 4, &ts, sizeof(ts));

    uint32_t s = htonl(ssrc);
    memcpy(buffer + 8, &s, sizeof(s));

    return RTP_HEADER_SIZE;
}

int rtp_packetize_single_nal(const h264_nal_t *nal, uint16_t sequence, uint32_t timestamp, uint32_t ssrc, rtp_packet_t *packet)
{
    if (!nal || !packet)
    {
        return -1;
    }

    /**
     * Single NAL only use when payload less than MTU
     */
    if (nal->size > RTP_MAX_PAYLOAD_SIZE)
    {
        return -1;
    }

    int ret = rtp_build_header(packet->data, sequence, timestamp, ssrc, true, 96);

    if (ret < 0)
    {
        return -1;
    }

    memcpy(packet->data + RTP_HEADER_SIZE, nal->data, nal->size);
    packet->size = RTP_HEADER_SIZE + nal->size;
    packet->sequence_number = sequence;
    packet->timestamp = timestamp;
    packet->ssrc = ssrc;
    packet->payload_type = 96;
    packet->marker = true;

    return 0;
}