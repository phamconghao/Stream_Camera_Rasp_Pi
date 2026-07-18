#include <arpa/inet.h>
#include <cstring>

#include "rtp_packetizer.h"

// RFC 6184 section 5.2: NAL unit type 28 is reserved (within the RTP
// payload's own type field, not the H.264 bitstream's NAL type space)
// to mean "this is a Fragmentation Unit type A (FU-A) packet".
static constexpr uint8_t H264_NAL_TYPE_FU_A = 28;

// FU-A adds 2 bytes of overhead per packet (FU indicator + FU header)
// on top of the normal RTP_HEADER_SIZE, eating into the usual
// RTP_MAX_PAYLOAD_SIZE budget for the actual NAL bytes carried.
static constexpr size_t FU_A_HEADER_SIZE = 2;

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

// Shared by every packetize_* function below: the actual NAL bytes
// (header + RBSP) after stripping any leftover Annex-B start code.
static size_t nal_payload_size(const h264_nal_t *nal)
{
    return nal->size - annexb_start_code_size(nal);
}

static const uint8_t *nal_payload_data(const h264_nal_t *nal)
{
    return nal->data + annexb_start_code_size(nal);
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

    size_t payload_size = nal_payload_size(nal);

    // Use FU-A instead for NALs that don't fit in one RTP payload.
    if (payload_size > RTP_MAX_PAYLOAD_SIZE)
        return -1;

    build_rtp_header(packet);

    copy_single_nal_payload(packet, nal);
    packet->nal_type = nal->nal_type;

    return 0;
}

bool rtp_nal_needs_fragmentation(const h264_nal_t *nal)
{
    if (!nal)
        return false;

    return nal_payload_size(nal) > RTP_MAX_PAYLOAD_SIZE;
}

// Every fragment after the first carries FU_A_HEADER_SIZE bytes of
// overhead (FU indicator + FU header) instead of the original 1-byte
// NAL header, so the usable payload per fragment is
// RTP_MAX_PAYLOAD_SIZE - FU_A_HEADER_SIZE, applied to the NAL's payload
// *excluding* its own 1-byte header (that header's bits get folded into
// the FU indicator/FU header instead - see rtp_packetize_fu_a_fragment).
size_t rtp_fu_a_fragment_count(const h264_nal_t *nal)
{
    if (!nal)
        return 0;

    size_t total = nal_payload_size(nal);
    if (total < 1)
        return 0;

    size_t nal_body_size = total - 1; // exclude the 1-byte NAL header
    size_t chunk_size = RTP_MAX_PAYLOAD_SIZE - FU_A_HEADER_SIZE;

    return (nal_body_size + chunk_size - 1) / chunk_size; // ceil division
}

// RFC 6184 section 5.8: an FU-A payload is
//   [FU indicator][FU header][fragment of the NAL body]
// FU indicator (1 byte): reuses the original NAL header's F and NRI
//   bits, but replaces its Type field with 28 (FU-A) so the receiver
//   knows to look at the FU header for the real type.
// FU header (1 byte): S (start, bit 7) | E (end, bit 6) | R (reserved,
//   bit 5, must be 0) | Type (bits 0-4, the ORIGINAL NAL type) - this
//   is how the receiver reconstructs the original NAL header once all
//   fragments are reassembled.
int rtp_packetize_fu_a_fragment(const h264_nal_t *nal, size_t fragment_index, rtp_packet_t *packet)
{
    if (!nal || !packet)
        return -1;

    size_t total = nal_payload_size(nal);
    if (total < 1)
        return -1;

    size_t nal_body_size = total - 1;
    size_t chunk_size = RTP_MAX_PAYLOAD_SIZE - FU_A_HEADER_SIZE;
    size_t fragment_count = rtp_fu_a_fragment_count(nal);

    if (fragment_count == 0 || fragment_index >= fragment_count)
        return -1;

    const uint8_t *payload = nal_payload_data(nal);
    uint8_t nal_header = payload[0];
    uint8_t f = (nal_header >> 7) & 0x01;
    uint8_t nri = (nal_header >> 5) & 0x03;
    uint8_t original_type = nal_header & 0x1F;

    size_t offset = fragment_index * chunk_size;
    size_t remaining = nal_body_size - offset;
    size_t this_chunk = (remaining < chunk_size) ? remaining : chunk_size;

    bool is_first = (fragment_index == 0);
    bool is_last = (fragment_index == fragment_count - 1);

    build_rtp_header(packet);

    uint8_t *out = packet->data + RTP_HEADER_SIZE;

    // FU indicator: original F/NRI bits, type forced to FU-A (28).
    out[0] = (f << 7) | (nri << 5) | (H264_NAL_TYPE_FU_A & 0x1F);

    // FU header: S/E bits mark the first/last fragment, type carries
    // the ORIGINAL NAL type so the receiver can rebuild the real header.
    out[1] = (is_first ? 0x80 : 0x00) | (is_last ? 0x40 : 0x00) | (original_type & 0x1F);

    // Body bytes start right after the original 1-byte NAL header
    // (payload[0], already consumed above into the FU header's Type
    // field), offset by every earlier fragment's share.
    memcpy(out + FU_A_HEADER_SIZE, payload + 1 + offset, this_chunk);

    packet->size = RTP_HEADER_SIZE + FU_A_HEADER_SIZE + this_chunk;
    packet->nal_type = original_type;

    return 0;
}
