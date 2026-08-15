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
 * PHASE 22.1 (WebRTC signaling): the WebSocket handshake (RFC 6455
 * section 1.3) requires computing
 * base64(SHA1(client's Sec-WebSocket-Key + a fixed GUID)) for the
 * Sec-WebSocket-Accept response header. This is NOT a
 * security-sensitive use of SHA-1 (it's not protecting a secret or
 * defending against a real adversary - it's just the protocol's way
 * of confirming both sides speak WebSocket, per spec), so a small
 * hand-rolled implementation is fine here - unlike DTLS/SRTP later in
 * Phase 22, which use OpenSSL/libsrtp2 precisely because THAT crypto
 * genuinely needs to be correct against an adversary.
 */
std::array<uint8_t, 20> sha1(const uint8_t *data, size_t size);
std::array<uint8_t, 20> sha1(const std::string &data);

#endif // __SHA1_H__
