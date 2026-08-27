#ifndef __SRTP_SESSION_H__
#define __SRTP_SESSION_H__

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

/**
 * SRTP session management. Turns the 60 bytes of DTLS-SRTP keying
 * material exported via SSL_export_keying_material() (RFC 5764
 * section 4.2) into two libsrtp2 contexts per session: one to encrypt
 * this side's outbound traffic, one to decrypt the peer's inbound
 * traffic (RTCP feedback - NACK/PLI).
 *
 * Uses libsrtp2 rather than hand-rolled AES-CM/HMAC - SRTP's
 * packet-index-dependent keystream derivation is exactly the kind of
 * crypto where a subtle bug silently breaks confidentiality instead
 * of failing loudly.
 */

// RFC 5764 section 4.2's fixed byte layout for the exported keying
// material, specific to the SRTP_AES128_CM_SHA1_80 profile negotiated
// during the DTLS handshake - 16-byte keys, 14-byte salts, client's
// pair first then server's.
struct srtp_derived_keys_t
{
    std::vector<uint8_t> client_write_key;  // 16 bytes - what the peer (DTLS client) uses to encrypt, so what this side uses to decrypt inbound traffic
    std::vector<uint8_t> server_write_key;  // 16 bytes - what this side (DTLS server) uses to encrypt outbound traffic
    std::vector<uint8_t> client_write_salt; // 14 bytes
    std::vector<uint8_t> server_write_salt; // 14 bytes
};

// Splits the 60-byte keying material into the 4 pieces above. Returns
// a struct with all vectors empty if `keying_material` isn't exactly
// 60 bytes.
srtp_derived_keys_t derive_srtp_keys(const std::vector<uint8_t> &keying_material);

// Must be called once, after libsrtp2's own global init - wraps
// srtp_init() plus this module's own state. Returns 0 on success, -1
// on failure.
int srtp_session_manager_init(void);
void srtp_session_manager_cleanup(void);

// Creates this session's two libsrtp2 contexts from `keying_material`
// (see derive_srtp_keys() above) - called once the DTLS handshake
// completes. Returns 0 on success, -1 on a malformed keying_material
// or a libsrtp2 failure.
int srtp_session_create(const std::string &ice_ufrag, const std::vector<uint8_t> &keying_material);

void srtp_session_destroy(const std::string &ice_ufrag);

// Encrypts one RTP packet in place. `buffer` must have at least
// SRTP_MAX_TRAILER_LEN bytes of spare room after `*len` bytes of
// actual RTP data (libsrtp2 appends the auth tag there); `buffer_capacity`
// is checked against that before attempting the encryption. On
// success, `len` is updated to the new (larger) packet length. Returns
// false on any failure, including "no such session" and "not enough
// spare room".
bool srtp_session_protect_rtp(const std::string &ice_ufrag, uint8_t *buffer, size_t buffer_capacity, int *len);

// Decrypts one inbound RTCP (feedback) packet in place, using this
// session's client_write key (the peer's own outbound key).
bool srtp_session_unprotect_rtcp(const std::string &ice_ufrag, uint8_t *buffer, size_t buffer_capacity, int *len);

#endif // __SRTP_SESSION_H__
