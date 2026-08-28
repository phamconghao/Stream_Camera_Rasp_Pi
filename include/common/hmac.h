#ifndef __HMAC_H__
#define __HMAC_H__

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

/**
 * PHASE 23.2: minimal HMAC-SHA256 wrapper (OpenSSL, RFC 2104/6234).
 * Used to authenticate the control channel's UDP control messages (see
 * network/control_channel.cpp / network/control_listener_thread.cpp)
 * with a shared secret both camera_app and camera_receiver are started
 * with, so a forged UDP datagram sent directly to the control-channel
 * port - bypassing the legitimate control_channel.cpp client entirely -
 * gets rejected rather than acted on. See docs-security-threat-model.md
 * section 1.3 for why HMAC-SHA256 specifically was chosen for this
 * asset.
 *
 * Deliberately a tiny, purpose-built wrapper - like stun_message.cpp's
 * own hmac_sha1() for STUN MESSAGE-INTEGRITY - rather than a general
 * crypto utility module. SHA-256 here (not STUN's SHA-1) since this is
 * a new, unrelated wire format with no interop constraint pulling it
 * toward SHA-1.
 */

// Computes HMAC-SHA256(key, data[0..size)) - always 32 bytes.
std::vector<uint8_t> hmac_sha256(const std::string &key, const uint8_t *data, size_t size);

// Recomputes HMAC-SHA256(key, data[0..size)) and compares it against
// mac[0..mac_size) using a constant-time comparison (OpenSSL's
// CRYPTO_memcmp), not memcmp/==, so a mismatch doesn't return faster on
// an earlier differing byte - a plain memcmp's early-exit is a timing
// side channel an attacker could use to brute-force the correct MAC
// one byte at a time. Returns false immediately (no comparison, so no
// timing signal either) if mac_size doesn't match the expected 32
// bytes.
bool hmac_sha256_verify(const std::string &key, const uint8_t *data, size_t size,
                         const uint8_t *mac, size_t mac_size);

#endif // __HMAC_H__
