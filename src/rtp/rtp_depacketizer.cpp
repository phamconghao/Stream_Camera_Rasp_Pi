#include <arpa/inet.h>
#include <cstring>

#include "rtp_depacketizer.h"
#include "log.h"

/**
 * Mirror image of rtp_packetizer.cpp's build_rtp_header()/FU-A packing -
 * see that file's comments for the exact wire layout this undoes.
 */

static const char *TAG = "DEPKT";

void rtp_depacketizer_state_init(rtp_depacketizer_state_t *state)
{
    state->in_progress = false;
    state->reconstructed_nal_header = 0;
}

int rtp_depacketize_header(const uint8_t *data, size_t size, rtp_packet_t *out_packet)
{
    if (!data || !out_packet || size < RTP_HEADER_SIZE || size > RTP_MAX_PACKET_SIZE)
    {
        return -1;
    }

    // byte 0: version/padding/extension/CSRC count - not validated
    // beyond size, since this pipeline's sender never sets padding,
    // extension, or CSRCs (see build_rtp_header() in rtp_packetizer.cpp).
    out_packet->marker = (data[1] & 0x80) != 0;
    out_packet->payload_type = data[1] & 0x7F;

    uint16_t seq;
    memcpy(&seq, data + 2, sizeof(seq));
    out_packet->sequence_number = ntohs(seq);

    uint32_t ts;
    memcpy(&ts, data + 4, sizeof(ts));
    out_packet->timestamp = ntohl(ts);

    uint32_t ssrc;
    memcpy(&ssrc, data + 8, sizeof(ssrc));
    out_packet->ssrc = ntohl(ssrc);

    memcpy(out_packet->data, data, size);
    out_packet->size = size;

    return 0;
}

static const uint8_t ANNEXB_START_CODE[4] = {0x00, 0x00, 0x00, 0x01};

// Appends a fresh Annex-B start code + `nal_header` + `body`/`body_size`
// to out_frame->data, i.e. begins a brand new NAL unit in the
// reassembled bitstream. Returns false (and leaves out_frame untouched)
// if there isn't enough room left in the pool-owned buffer.
static bool append_new_nal(encoded_frame_t *out_frame, uint8_t nal_header,
                            const uint8_t *body, size_t body_size)
{
    size_t needed = sizeof(ANNEXB_START_CODE) + 1 + body_size;

    if (out_frame->size + needed > MAX_ENCODED_FRAME_SIZE)
    {
        LOG_WARN(TAG, "encoded frame buffer full, dropping NAL (size=%zu)", body_size);
        return false;
    }

    memcpy(out_frame->data + out_frame->size, ANNEXB_START_CODE, sizeof(ANNEXB_START_CODE));
    out_frame->size += sizeof(ANNEXB_START_CODE);

    out_frame->data[out_frame->size] = nal_header;
    out_frame->size += 1;

    memcpy(out_frame->data + out_frame->size, body, body_size);
    out_frame->size += body_size;

    return true;
}

// Appends `body`/`body_size` bytes onto the NAL currently being built
// (no start code, no new header - just more of the same NAL's body,
// used for FU-A fragments after the first).
static bool append_to_current_nal(encoded_frame_t *out_frame, const uint8_t *body, size_t body_size)
{
    if (out_frame->size + body_size > MAX_ENCODED_FRAME_SIZE)
    {
        LOG_WARN(TAG, "encoded frame buffer full, dropping FU-A continuation (size=%zu)", body_size);
        return false;
    }

    memcpy(out_frame->data + out_frame->size, body, body_size);
    out_frame->size += body_size;

    return true;
}

bool rtp_depacketizer_process_packet(rtp_depacketizer_state_t *state,
                                      const rtp_packet_t *packet,
                                      encoded_frame_t *out_frame)
{
    if (!state || !packet || !out_frame)
    {
        return false;
    }

    const uint8_t *payload = packet->data + RTP_HEADER_SIZE;
    size_t payload_size = packet->size - RTP_HEADER_SIZE;

    if (payload_size < 1)
    {
        LOG_WARN(TAG, "empty RTP payload seq=%u", packet->sequence_number);
        return false;
    }

    uint8_t payload_header = payload[0];
    uint8_t rtp_nal_type = payload_header & 0x1F;

    if (rtp_nal_type == RTP_NAL_TYPE_FU_A)
    {
        // RFC 6184 5.8: [FU indicator][FU header][fragment body...]
        if (payload_size < 2)
        {
            LOG_WARN(TAG, "truncated FU-A packet seq=%u", packet->sequence_number);
            return false;
        }

        uint8_t fu_indicator = payload[0];
        uint8_t fu_header = payload[1];
        bool start = (fu_header & 0x80) != 0;
        bool end = (fu_header & 0x40) != 0;
        uint8_t original_type = fu_header & 0x1F;

        const uint8_t *body = payload + 2;
        size_t body_size = payload_size - 2;

        if (start)
        {
            // Reconstruct the original 1-byte NAL header from the FU
            // indicator's F/NRI bits plus the FU header's original type.
            state->reconstructed_nal_header = static_cast<uint8_t>((fu_indicator & 0xE0) | (original_type & 0x1F));
            state->in_progress = true;

            if (!append_new_nal(out_frame, state->reconstructed_nal_header, body, body_size))
            {
                state->in_progress = false;
            }
        }
        else if (state->in_progress)
        {
            append_to_current_nal(out_frame, body, body_size);
        }
        else
        {
            // A continuation/end fragment arrived without ever seeing
            // its start fragment (e.g. the start fragment was declared
            // lost by the jitter buffer, and rtp_depacketizer_thread
            // should have called rtp_depacketizer_reset() at that
            // point). Nothing valid to append.
            LOG_WARN(TAG, "FU-A continuation without start, seq=%u - ignoring", packet->sequence_number);
        }

        if (end)
        {
            state->in_progress = false;
        }
    }
    else
    {
        // RFC 6184 5.6: Single NAL Unit Packet - the whole payload IS
        // the NAL (header byte + body), copied through unchanged.
        state->in_progress = false;
        append_new_nal(out_frame, payload[0], payload + 1, payload_size - 1);
    }

    return packet->marker;
}

void rtp_depacketizer_reset(rtp_depacketizer_state_t *state, encoded_frame_t *out_frame)
{
    state->in_progress = false;
    state->reconstructed_nal_header = 0;

    if (out_frame)
    {
        out_frame->size = 0;
    }
}
