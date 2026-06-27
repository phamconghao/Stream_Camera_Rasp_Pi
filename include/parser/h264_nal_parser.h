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

int h264_split_nals(uint8_t *data, size_t size, std::vector<h264_nal_t> &nals);
const char *h264_nal_type_string(uint8_t nal_type);

#endif // __H264_NAL_PARSER_H__