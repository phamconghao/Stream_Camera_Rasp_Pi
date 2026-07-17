#include <arpa/inet.h>
#include <cstring>

#include "rtp_packetizer.h"

/**
 * Writes a 12-byte RTP header (RFC 3550 section 5.1) directly into the
 * first RTP_HEADER_SIZE bytes of packet->data. Multi-byte fields are
 * converted to network byte order (big-endian) via htons/htonl, since
 * that's what RTP requires on the wire.
 *
 * Byte layout:
 *   byte 0:     0x80 = version 2, no padding, no extension, 0 CSRC count
 *   byte 1:     marker bit (bit 7) | payload type (bits 0-6, 96 = H.264 dynamic PT)
 *   bytes 2-3:  sequence number
 *   bytes 4-7:  RTP timestamp (90kHz clock for video)
 *   bytes 8-11: SSRC (stream identifier, constant per session)
 */
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

// Defensive check for a leading Annex-B start code (00 00 01 / 00 00 00 01).
// In today's pipeline this is effectively always 0: h264_nal_parser_next()
// already strips the start code before handing out h264_nal_t, so
// nal->data never actually starts with one here. Kept as a safety net
// in case rtp_packetize_single_nal() is ever called with a NAL that
// still has its start code attached (e.g. from a different/future NAL
// source), so payload_size below stays correct either way.
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

// Copies the NAL bytes (after any start code) right after the RTP
// header we just built, and records the total packet size.
/* Copy single NAL payload helper */
static void copy_single_nal_payload(rtp_packet_t *packet, const h264_nal_t *nal)
{
    size_t start_code = annexb_start_code_size(nal);
    size_t payload_size = nal->size - start_code;
    memcpy(packet->data + RTP_HEADER_SIZE, nal->data + start_code, payload_size);
    packet->size = RTP_HEADER_SIZE + payload_size;
}

// See rtp_packetizer.h for the full contract. Note: packet->marker,
// packet->payload_type, packet->sequence_number, packet->timestamp,
// and packet->ssrc must already be set by the caller (rtp_packetizer_thread)
// before calling this - this function only builds the header bytes from
// those fields and appends the payload; it does not choose sequence
// numbers or timestamps itself.
int rtp_packetize_single_nal(const h264_nal_t *nal, rtp_packet_t *packet)
{
    if (!nal || !packet)
        return -1;

    size_t payload_size = nal->size - annexb_start_code_size(nal);

    // FU-A fragmentation (RFC 6184 5.8) would be needed here for NALs
    // that don't fit in one RTP payload - not implemented yet, so such
    // NALs are simply rejected (caller logs and drops them).
    if (payload_size > RTP_MAX_PAYLOAD_SIZE)
        return -1;

    build_rtp_header(packet);

    copy_single_nal_payload(packet, nal);
    packet->nal_type = nal->nal_type;

    return 0;
}
