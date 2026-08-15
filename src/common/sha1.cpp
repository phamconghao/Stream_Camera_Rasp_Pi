#include "sha1.h"

#include <cstring>
#include <vector>

namespace
{

uint32_t rotl32(uint32_t value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

} // namespace

// Textbook SHA-1: pad the message (RFC 3174 section 4), process it in
// 64-byte blocks through the 80-round compression function, and
// serialize the 5 resulting 32-bit words big-endian. No shortcuts
// taken - correctness matters even for a non-adversarial use (see
// sha1.h's header comment), a wrong digest here just breaks the
// WebSocket handshake outright rather than silently.
std::array<uint8_t, 20> sha1(const uint8_t *data, size_t size)
{
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    // Padding: a single 0x80 byte, then zero bytes, then the
    // original bit-length as a big-endian 64-bit integer, such that
    // the total length is a multiple of 64 bytes.
    std::vector<uint8_t> msg(data, data + size);
    uint64_t bit_length = static_cast<uint64_t>(size) * 8;

    msg.push_back(0x80);
    while (msg.size() % 64 != 56)
    {
        msg.push_back(0x00);
    }

    for (int i = 7; i >= 0; i--)
    {
        msg.push_back(static_cast<uint8_t>((bit_length >> (i * 8)) & 0xFF));
    }

    for (size_t block = 0; block < msg.size(); block += 64)
    {
        uint32_t w[80];

        for (int i = 0; i < 16; i++)
        {
            w[i] = (static_cast<uint32_t>(msg[block + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[block + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[block + i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(msg[block + i * 4 + 3]);
        }

        for (int i = 16; i < 80; i++)
        {
            w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        for (int i = 0; i < 80; i++)
        {
            uint32_t f;
            uint32_t k;

            if (i < 20)
            {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            }
            else if (i < 40)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            }
            else if (i < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }

            uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<uint8_t, 20> digest;
    uint32_t words[5] = {h0, h1, h2, h3, h4};

    for (int i = 0; i < 5; i++)
    {
        digest[i * 4] = static_cast<uint8_t>((words[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<uint8_t>((words[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<uint8_t>((words[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<uint8_t>(words[i] & 0xFF);
    }

    return digest;
}

std::array<uint8_t, 20> sha1(const std::string &data)
{
    return sha1(reinterpret_cast<const uint8_t *>(data.data()), data.size());
}
