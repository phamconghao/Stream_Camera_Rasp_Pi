#ifndef __SHA1_H__
#define __SHA1_H__

#include <cstdint>
#include <cstddef>
#include <array>
#include <string>

/**
 * Minimal SHA-1 (FIPS 180-1) implementation - single input, single
 * 20-byte digest output, no incremental/streaming API.
 *
 * Used for the WebSocket handshake (RFC 6455 section 1.3), which
 * requires computing base64(SHA1(client's Sec-WebSocket-Key + a fixed
 * GUID)) for the Sec-WebSocket-Accept response header. This is not a
 * security-sensitive use of SHA-1 - it's just the protocol's way of
 * confirming both sides speak WebSocket - so a small hand-rolled
 * implementation is fine here, unlike DTLS/SRTP elsewhere in this
 * project, which use OpenSSL/libsrtp2 because that crypto genuinely
 * needs to be correct against an adversary.
 */
std::array<uint8_t, 20> sha1(const uint8_t *data, size_t size);
std::array<uint8_t, 20> sha1(const std::string &data);

#endif // __SHA1_H__
