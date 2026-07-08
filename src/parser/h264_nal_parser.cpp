#include "h264_nal_parser.h"

// Find Start Code Helper
static int find_start_code(uint8_t *data, size_t size, size_t offset)
{
    for (size_t i = offset; i + 3 < size; i++)
    {
        if (data[i] == 0x00 && data[i + 1] == 0x00)
        {
            if (data[i + 2] == 0x01)
            {
                return i;
            }

            if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01)
            {
                return i;
            }
        }
    }

    return -1;
}

const char *h264_nal_type_string(uint8_t type)
{
    switch (type)
    {
        case 1:
            return "P";

        case 5:
            return "IDR";

        case 7:
            return "SPS";

        case 8:
            return "PPS";

        default:
            return "OTHER";
    }
}

void h264_nal_parser_init(h264_nal_parser_t *parser, uint8_t *data, size_t size)
{
    parser->data = data;
    parser->size = size;
    parser->pos = 0;
}

bool h264_nal_parser_next(h264_nal_parser_t *parser, h264_nal_t *out_nal)
{
    int start = find_start_code(parser->data, parser->size, parser->pos);
    if (start < 0)
    {
        return false;
    }

    size_t prefix_size = (parser->data[start + 2] == 0x01) ? 3 : 4;
    size_t nal_start = start + prefix_size;

    int next = find_start_code(parser->data, parser->size, nal_start);
    size_t nal_end = (next < 0) ? parser->size : (size_t)next;

    out_nal->data = &parser->data[nal_start];
    out_nal->size = nal_end - nal_start;
    out_nal->nal_type = out_nal->data[0] & 0x1F;

    parser->pos = (next < 0) ? parser->size : (size_t)next;

    return true;
}

int h264_split_nals(uint8_t *data, size_t size, std::vector<h264_nal_t> &nals)
{
    nals.clear();

    h264_nal_parser_t parser;
    h264_nal_parser_init(&parser, data, size);

    h264_nal_t nal;
    while (h264_nal_parser_next(&parser, &nal))
    {
        nals.push_back(nal);
    }

    return nals.size();
}
