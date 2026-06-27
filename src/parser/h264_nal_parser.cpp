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

int h264_split_nals(uint8_t *data, size_t size, std::vector<h264_nal_t> &nals)
{
    nals.clear();
    size_t pos = 0;
    
    while (true)
    {
        int start = find_start_code(data, size, pos);
        if (start < 0)
        {
            break;
        }

        int next = find_start_code(data, size, start + 4);
        size_t nal_start;
        size_t prefix_size;

        if (data[start + 2] == 0x01)
        {
            prefix_size = 3;
        }
        else
        {
            prefix_size = 4;
        }

        nal_start = start + prefix_size;

        size_t nal_end = (next < 0) ? size : (size_t)next;

        h264_nal_t nal;
        nal.data = &data[nal_start];
        nal.size = nal_end - nal_start;
        nal.nal_type = nal.data[0] & 0x1F;
        nals.push_back(nal);

        if (next < 0)
        {
            break;
        }

        pos = next;
    }

    return nals.size();
}