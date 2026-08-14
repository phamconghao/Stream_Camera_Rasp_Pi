#include "base64.h"

static const char *BASE64_ALPHABET =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

// Standard 3-bytes-in -> 4-chars-out base64, padded with '=' when the
// input length isn't a multiple of 3 - same encoding every SDP
// implementation expects for sprop-parameter-sets.
std::string base64_encode(const std::vector<uint8_t> &data)
{
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 3 <= data.size())
    {
        uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);

        out += BASE64_ALPHABET[(triple >> 18) & 0x3F];
        out += BASE64_ALPHABET[(triple >> 12) & 0x3F];
        out += BASE64_ALPHABET[(triple >> 6) & 0x3F];
        out += BASE64_ALPHABET[triple & 0x3F];

        i += 3;
    }

    size_t remaining = data.size() - i;
    if (remaining == 1)
    {
        uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
        out += BASE64_ALPHABET[(triple >> 18) & 0x3F];
        out += BASE64_ALPHABET[(triple >> 12) & 0x3F];
        out += "==";
    }
    else if (remaining == 2)
    {
        uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
        out += BASE64_ALPHABET[(triple >> 18) & 0x3F];
        out += BASE64_ALPHABET[(triple >> 12) & 0x3F];
        out += BASE64_ALPHABET[(triple >> 6) & 0x3F];
        out += "=";
    }

    return out;
}
