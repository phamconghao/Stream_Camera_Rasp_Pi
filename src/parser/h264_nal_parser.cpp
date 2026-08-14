#include "h264_nal_parser.h"

// Scans for the next Annex-B start code (00 00 01, the 3-byte form, or
// 00 00 00 01, the 4-byte form) at or after `offset`. Returns the byte
// index where the 00 00 (01|00 01) sequence begins, or -1 if none found
// before the end of the buffer.
// Find Start Code Helper
static int find_start_code(uint8_t *data, size_t size, size_t offset)
{
    for (size_t i = offset; i + 3 < size; i++)
    {
        if (data[i] == 0x00 && data[i + 1] == 0x00)
        {
            if (data[i + 2] == 0x01)
            {
                return static_cast<int>(i);
            }

            if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01)
            {
                return static_cast<int>(i);
            }
        }
    }

    return -1;
}

// Human-readable label for a NAL type, used only in log lines. Common
// H.264 NAL types: 1 = non-IDR slice (P/B frame), 5 = IDR slice
// (keyframe), 7 = SPS (Sequence Parameter Set), 8 = PPS (Picture
// Parameter Set). SPS/PPS carry decoder config and are normally sent
// once at the start of the stream and again before each IDR.
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

// Reset the iterator to scan `data`/`size` from the beginning.
void h264_nal_parser_init(h264_nal_parser_t *parser, uint8_t *data, size_t size)
{
    parser->data = data;
    parser->size = size;
    parser->pos = 0;
}

// Advance to the next NAL unit. Returns false once there are no more
// NALs left in the buffer (out_nal is left untouched in that case).
//
// Each call: finds the current NAL's start code, determines whether it's
// the 3-byte or 4-byte form (by checking if byte[start+2] is already the
// 0x01 terminator), computes where the NAL's actual payload begins
// (nal_start = start + prefix_size), then searches for the START of the
// *next* NAL (which also marks the END of this one) to compute this
// NAL's size. If no further start code is found, this NAL runs to the
// end of the buffer and is_last_nal is set true.
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
    out_nal->is_last_nal = (next < 0);

    parser->pos = (next < 0) ? parser->size : (size_t)next;

    return true;
}
