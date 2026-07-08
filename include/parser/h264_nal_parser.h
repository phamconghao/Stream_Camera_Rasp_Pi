#ifndef __H264_NAL_PARSER_H__
#define __H264_NAL_PARSER_H__

#include <stdint.h>
#include <stddef.h>
#include <vector>

typedef struct
{
    uint8_t *data;
    size_t size;
    uint8_t nal_type;
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

/**
 * Legacy vector-based API. Still used by h264_writer.cpp. New code
 * (e.g. rtp_packetizer_thread) should prefer the iterator API above.
 */
int h264_split_nals(uint8_t *data, size_t size, std::vector<h264_nal_t> &nals);
const char *h264_nal_type_string(uint8_t nal_type);

#endif // __H264_NAL_PARSER_H__