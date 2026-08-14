#ifndef __H264_NAL_PARSER_H__
#define __H264_NAL_PARSER_H__

#include <stdint.h>
#include <stddef.h>

/**
 * ============================================================================
 * PIPELINE STAGE: Encoded Frame -> [THIS] -> RTP Packetizer
 * ============================================================================
 *
 * Splits one Annex-B H.264 access unit (as produced by the hardware
 * encoder, may contain several NAL units back-to-back - e.g. SPS, PPS,
 * then a slice) into individual NAL units, each stripped of its Annex-B
 * start code (00 00 01 or 00 00 00 01) so callers get just the raw NAL
 * (starting with the 1-byte NAL header, e.g. 0x67 = SPS, 0x65 = IDR slice).
 */
typedef struct
{
    uint8_t *data;      // pointer INTO the original access unit buffer (no copy) - start code already stripped
    size_t size;        // bytes of this NAL (header + RBSP), not counting the start code
    uint8_t nal_type;   // low 5 bits of the NAL header byte (see h264_nal_type_string below)
    bool is_last_nal;   // true for the final NAL in this access unit - rtp_packetizer uses this to set the RTP marker bit
} h264_nal_t;

/**
 * Streaming/iterator-style NAL parser. Scans an Annex-B buffer one NAL
 * at a time without allocating a std::vector, which matters once
 * RTP packetization needs to fragment/consume NALs one by one
 * (FU-A, STAP-A) rather than materialize the whole list up front.
 *
 * Usage:
 *   h264_nal_parser_t parser;
 *   h264_nal_parser_init(&parser, frame->data, frame->size);
 *   h264_nal_t nal;
 *   while (h264_nal_parser_next(&parser, &nal)) { ... }
 */
typedef struct
{
    uint8_t *data;
    size_t size;
    size_t pos;
} h264_nal_parser_t;

void h264_nal_parser_init(h264_nal_parser_t *parser, uint8_t *data, size_t size);
bool h264_nal_parser_next(h264_nal_parser_t *parser, h264_nal_t *out_nal);

const char *h264_nal_type_string(uint8_t nal_type);

#endif // __H264_NAL_PARSER_H__
