#include "stun_message.h"

#include <cstring>
#include <random>
#include <arpa/inet.h>
#include <openssl/hmac.h>

namespace
{

constexpr uint32_t STUN_MAGIC_COOKIE = 0x2112A442;
constexpr uint16_t STUN_ATTR_USERNAME = 0x0006;
constexpr uint16_t STUN_ATTR_MESSAGE_INTEGRITY = 0x0008;
constexpr uint16_t STUN_ATTR_XOR_MAPPED_ADDRESS = 0x0020;
constexpr uint16_t STUN_ATTR_FINGERPRINT = 0x8028;
constexpr uint32_t STUN_FINGERPRINT_XOR = 0x5354554E;
constexpr size_t STUN_HEADER_SIZE = 20;

void append_u16(std::vector<uint8_t> &out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void append_u32(std::vector<uint8_t> &out, uint32_t value)
{
    for (int i = 3; i >= 0; i--)
    {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

uint16_t read_u16(const uint8_t *p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t read_u32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Pads an attribute value with zero bytes up to the next 4-byte
// boundary, per RFC 5389 section 15 - every STUN attribute value's
// on-wire length is padded even though its declared Length field
// isn't.
void append_padded(std::vector<uint8_t> &out, const uint8_t *data, size_t size)
{
    out.insert(out.end(), data, data + size);
    while (out.size() % 4 != 0)
    {
        out.push_back(0);
    }
}

// Rewrites the 2-byte message-length field in the STUN header (bytes
// 2-3) to reflect everything appended after the header so far - must
// be called (with the right running total) before each of
// MESSAGE-INTEGRITY/FINGERPRINT is computed, since both cover "the
// message as it will be sent, length field included" per RFC 5389
// sections 15.4/15.5.
void patch_length(std::vector<uint8_t> &msg, size_t body_length)
{
    msg[2] = static_cast<uint8_t>((body_length >> 8) & 0xFF);
    msg[3] = static_cast<uint8_t>(body_length & 0xFF);
}

// Standard CRC-32 (polynomial 0xEDB88320, the same one zlib/PNG/PKZIP
// use) - table-based for speed, table built once on first use. Not
// security-sensitive (see stun_message.h's header comment - this is a
// demultiplexing checksum, not an authentication mechanism), so a
// hand-rolled implementation is fine here the same way sha1.h's
// WebSocket-handshake hash is.
uint32_t crc32(const uint8_t *data, size_t size)
{
    static uint32_t table[256];
    static bool table_built = false;

    if (!table_built)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
            {
                c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        table_built = true;
    }

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++)
    {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

std::vector<uint8_t> hmac_sha1(const std::string &key, const uint8_t *data, size_t size)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    HMAC(EVP_sha1(),
         key.data(), static_cast<int>(key.size()),
         data, size,
         digest, &digest_len);

    return std::vector<uint8_t>(digest, digest + digest_len);
}

void append_message_integrity(std::vector<uint8_t> &msg, const std::string &ice_pwd)
{
    // Length field must already include this attribute's own 24 bytes
    // (4-byte attribute header + 20-byte HMAC-SHA1 value) before the
    // HMAC is computed over the message - see patch_length()'s comment.
    patch_length(msg, msg.size() - STUN_HEADER_SIZE + 24);

    std::vector<uint8_t> hmac = hmac_sha1(ice_pwd, msg.data(), msg.size());

    append_u16(msg, STUN_ATTR_MESSAGE_INTEGRITY);
    append_u16(msg, static_cast<uint16_t>(hmac.size()));
    msg.insert(msg.end(), hmac.begin(), hmac.end());
}

void append_fingerprint(std::vector<uint8_t> &msg)
{
    // Same principle as MESSAGE-INTEGRITY: the length field must
    // include FINGERPRINT's own 8 bytes before computing the CRC over
    // the message (RFC 5389 section 15.5).
    patch_length(msg, msg.size() - STUN_HEADER_SIZE + 8);

    uint32_t crc = crc32(msg.data(), msg.size()) ^ STUN_FINGERPRINT_XOR;

    append_u16(msg, STUN_ATTR_FINGERPRINT);
    append_u16(msg, 4);
    append_u32(msg, crc);
}

std::vector<uint8_t> stun_message_header(uint16_t message_type, const uint8_t transaction_id[12])
{
    std::vector<uint8_t> msg;
    append_u16(msg, message_type);
    append_u16(msg, 0); // length patched in later, once attributes are known
    append_u32(msg, STUN_MAGIC_COOKIE);
    msg.insert(msg.end(), transaction_id, transaction_id + 12);
    return msg;
}

} // namespace

stun_parsed_message_t parse_stun_message(const uint8_t *data, size_t size)
{
    stun_parsed_message_t result;

    if (size < STUN_HEADER_SIZE)
    {
        return result; // valid stays false
    }

    result.message_type = read_u16(data);
    uint16_t body_length = read_u16(data + 2);
    uint32_t magic = read_u32(data + 4);

    if (magic != STUN_MAGIC_COOKIE)
    {
        return result; // not a STUN message (or an old RFC 3489 one this project doesn't support)
    }

    memcpy(result.transaction_id, data + 8, 12);

    if (size < STUN_HEADER_SIZE + body_length)
    {
        return result; // truncated - declared body longer than what we actually received
    }

    size_t pos = STUN_HEADER_SIZE;
    size_t end = STUN_HEADER_SIZE + body_length;

    while (pos + 4 <= end)
    {
        uint16_t attr_type = read_u16(data + pos);
        uint16_t attr_len = read_u16(data + pos + 2);
        size_t value_start = pos + 4;

        if (value_start + attr_len > end)
        {
            break; // malformed attribute - stop parsing, keep whatever was already found
        }

        if (attr_type == STUN_ATTR_USERNAME)
        {
            result.username.assign(reinterpret_cast<const char *>(data + value_start), attr_len);
        }
        else if (attr_type == STUN_ATTR_MESSAGE_INTEGRITY)
        {
            result.has_message_integrity = true;
        }
        else if (attr_type == STUN_ATTR_FINGERPRINT && attr_len == 4)
        {
            result.has_fingerprint = true;

            // FINGERPRINT must be the last attribute and covers
            // everything before it (RFC 5389 section 15.5) - verify it
            // right here since we already have the exact byte range.
            uint32_t received_crc = read_u32(data + value_start);
            uint32_t computed_crc = crc32(data, pos) ^ STUN_FINGERPRINT_XOR;

            if (received_crc != computed_crc)
            {
                return result; // valid stays false - tampered or corrupted packet
            }
        }

        // Attribute values are padded to a 4-byte boundary on the wire
        // (see append_padded()) even though attr_len reflects the
        // unpadded length - advance past the padding too.
        size_t padded_len = (attr_len + 3) & ~static_cast<size_t>(3);
        pos = value_start + padded_len;
    }

    result.valid = true;
    return result;
}

bool stun_verify_message_integrity(const uint8_t *data, size_t size, const std::string &ice_pwd)
{
    if (size < STUN_HEADER_SIZE)
    {
        return false;
    }

    uint16_t body_length = read_u16(data + 2);
    size_t end = STUN_HEADER_SIZE + body_length;
    if (size < end)
    {
        return false;
    }

    // Find the MESSAGE-INTEGRITY attribute and recompute the HMAC over
    // everything before it, with the header's length field
    // temporarily patched to what it was AT THE TIME the HMAC was
    // originally computed (i.e. covering up to and including this
    // attribute, per RFC 5389 section 15.4 - see append_message_integrity()).
    size_t pos = STUN_HEADER_SIZE;
    while (pos + 4 <= end)
    {
        uint16_t attr_type = read_u16(data + pos);
        uint16_t attr_len = read_u16(data + pos + 2);
        size_t value_start = pos + 4;

        if (value_start + attr_len > end)
        {
            return false;
        }

        if (attr_type == STUN_ATTR_MESSAGE_INTEGRITY && attr_len == 20)
        {
            std::vector<uint8_t> prefix(data, data + pos);
            patch_length(prefix, pos - STUN_HEADER_SIZE + 24);

            std::vector<uint8_t> expected = hmac_sha1(ice_pwd, prefix.data(), prefix.size());
            return expected.size() == 20 && memcmp(expected.data(), data + value_start, 20) == 0;
        }

        size_t padded_len = (attr_len + 3) & ~static_cast<size_t>(3);
        pos = value_start + padded_len;
    }

    return false; // no MESSAGE-INTEGRITY attribute found
}

std::vector<uint8_t> build_stun_binding_response(
    const uint8_t transaction_id[12],
    const std::string &mapped_ip,
    uint16_t mapped_port,
    const std::string &ice_pwd)
{
    std::vector<uint8_t> msg = stun_message_header(STUN_BINDING_RESPONSE_SUCCESS, transaction_id);

    // XOR-MAPPED-ADDRESS (RFC 5389 section 15.2): family/port/address
    // XORed with the magic cookie (and, for IPv6, the transaction ID
    // too - IPv4-only here since this project doesn't do IPv6).
    struct in_addr addr;
    inet_pton(AF_INET, mapped_ip.c_str(), &addr);
    uint32_t addr_host = ntohl(addr.s_addr);

    uint16_t xor_port = static_cast<uint16_t>(mapped_port ^ (STUN_MAGIC_COOKIE >> 16));
    uint32_t xor_addr = addr_host ^ STUN_MAGIC_COOKIE;

    append_u16(msg, STUN_ATTR_XOR_MAPPED_ADDRESS);
    append_u16(msg, 8);
    msg.push_back(0); // reserved
    msg.push_back(0x01); // family: IPv4
    append_u16(msg, xor_port);
    append_u32(msg, xor_addr);

    append_message_integrity(msg, ice_pwd);
    append_fingerprint(msg);

    return msg;
}

std::vector<uint8_t> build_stun_binding_request(
    const std::string &username,
    const std::string &ice_pwd,
    uint8_t out_transaction_id[12])
{
    std::random_device rd;
    for (int i = 0; i < 12; i++)
    {
        out_transaction_id[i] = static_cast<uint8_t>(rd() & 0xFF);
    }

    std::vector<uint8_t> msg = stun_message_header(STUN_BINDING_REQUEST, out_transaction_id);

    std::vector<uint8_t> username_bytes(username.begin(), username.end());
    append_u16(msg, STUN_ATTR_USERNAME);
    append_u16(msg, static_cast<uint16_t>(username_bytes.size()));
    append_padded(msg, username_bytes.data(), username_bytes.size());

    append_message_integrity(msg, ice_pwd);
    append_fingerprint(msg);

    return msg;
}
