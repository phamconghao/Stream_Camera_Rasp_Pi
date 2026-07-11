#include <arpa/inet.h>
#include <cstring>

#include "rtp_packetizer.h"

/* Build RTP header helper */
static void build_rtp_header(rtp_packet_t *packet)
{
    uint8_t *buffer = packet->data;

    memset(buffer, 0, RTP_HEADER_SIZE);

    buffer[0] = 0x80;

    buffer[1] = (packet->marker ? 0x80 : 0x00) | (packet->payload_type & 0x7F);

    uint16_t seq = htons(packet->sequence_number);

    memcpy(buffer + 2, &seq, sizeof(seq));

    uint32_t ts = htonl(packet->timestamp);

    memcpy(buffer + 4, &ts, sizeof(ts));

    uint32_t ssrc = htonl(packet->ssrc);

    memcpy(buffer + 8, &ssrc, sizeof(ssrc));
}

/* Annex B start code helper */
static size_t annexb_start_code_size(const h264_nal_t *nal)
{
    if (nal->size >= 4 &&
        nal->data[0] == 0 &&
        nal->data[1] == 0 &&
        nal->data[2] == 0 &&
        nal->data[3] == 1)
    {
        return 4;
    }

    if (nal->size >= 3 &&
        nal->data[0] == 0 &&
        nal->data[1] == 0 &&
        nal->data[2] == 1)
    {
        return 3;
    }

    return 0;
}

/* Copy single NAL payload helper */
static void copy_single_nal_payload(rtp_packet_t *packet, const h264_nal_t *nal)
{
    size_t start_code = annexb_start_code_size(nal);
    size_t payload_size = nal->size - start_code;
    memcpy(packet->data + RTP_HEADER_SIZE, nal->data + start_code, payload_size);
    packet->size = RTP_HEADER_SIZE + payload_size;
}

int rtp_packetize_single_nal(const h264_nal_t *nal, rtp_packet_t *packet)
{
    if (!nal || !packet)
        return -1;

    size_t payload_size = nal->size - annexb_start_code_size(nal);

    if (payload_size > RTP_MAX_PAYLOAD_SIZE)
        return -1;

    build_rtp_header(packet);

    copy_single_nal_payload(packet, nal);

    return 0;
}