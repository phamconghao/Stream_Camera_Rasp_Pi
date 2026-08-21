#ifndef __SRTP_SESSION_H__
#define __SRTP_SESSION_H__

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

/**
 * ============================================================================
 * PHASE 22.5 (steps 1-3): SRTP session management.
 * ============================================================================
 *
 * Turns the 60 bytes of DTLS-SRTP keying material Phase 22.4's
 * dtls_handshake.cpp exports (SSL_export_keying_material(), RFC 5764
 * section 4.2) into two libsrtp2 contexts per WebRTC session: one to
 * ENCRYPT this project's own outbound traffic, one to DECRYPT the
 * browser's inbound traffic (RTCP feedback - NACK/PLI - since this
 * project only ever sends video and never receives RTP, per
 * webrtc_sdp.cpp's a=sendonly).
 *
 * SCOPE BOUNDARY (documented, not hidden - see roadmap.md's Phase
 * 22.5/22.6 split): this module provides the CRYPTO MECHANISM only.
 * srtp_session_protect_rtp() is ready to call, but nothing in this
 * phase calls it with real camera RTP yet - wiring actual live video
 * through this encrypted path (fanning it out to WebRTC viewers
 * alongside Phase 20's existing RTSP fan-out) is Phase 22.6's "end to
 * end integration" job, since that requires a WebRTC-specific sender
 * path parallel to udp_sender.cpp, not just this crypto layer.
 *
 * Uses libsrtp2 (this project's second crypto library dependency
 * after OpenSSL) rather than hand-rolled AES-CM/HMAC, per this
 * phase's design decision (roadmap.md's Phase 22 breakdown) - SRTP's
 * packet-index-dependent keystream derivation is exactly the kind of
 * crypto where a subtle hand-rolled bug silently breaks
 * confidentiality rather than just failing loudly.
 */

// RFC 5764 section 4.2's fixed byte layout for the exported keying
// material, specific to the SRTP_AES128_CM_SHA1_80 profile this
// project negotiates (dtls_handshake.cpp's SRTP_PROFILES) - 16-byte
// keys, 14-byte salts, client's pair first then server's.
struct srtp_derived_keys_t
{
    std::vector<uint8_t> client_write_key;  // 16 bytes - what the browser (DTLS client) uses to ENCRYPT, so what this project uses to DECRYPT inbound traffic from it
    std::vector<uint8_t> server_write_key;  // 16 bytes - what this project (DTLS server, a=setup:passive) uses to ENCRYPT outbound traffic
    std::vector<uint8_t> client_write_salt; // 14 bytes
    std::vector<uint8_t> server_write_salt; // 14 bytes
};

// Splits the 60-byte keying material into the 4 pieces above. Returns
// a struct with all vectors empty if `keying_material` isn't exactly
// 60 bytes (the only length SRTP_AES128_CM_SHA1_80 ever produces) -
// callers should treat that as "this session's material is unusable",
// not attempt a partial derivation.
srtp_derived_keys_t derive_srtp_keys(const std::vector<uint8_t> &keying_material);

// Must be called once, after libsrtp2's own global init - wraps
// srtp_init() (the library's required one-time setup) plus this
// module's own state. Returns 0 on success, -1 on failure.
int srtp_session_manager_init(void);
void srtp_session_manager_cleanup(void);

// Creates this session's two libsrtp2 contexts from `keying_material`
// (see derive_srtp_keys() above) - called once DTLS handshake
// completes (dtls_handshake.cpp's Phase 22.4 connected=true path).
// Returns 0 on success, -1 on a malformed keying_material or a
// libsrtp2 failure.
int srtp_session_create(const std::string &ice_ufrag, const std::vector<uint8_t> &keying_material);

void srtp_session_destroy(const std::string &ice_ufrag);

// Encrypts one RTP packet in place. `buffer` must have at least
// SRTP_MAX_TRAILER_LEN bytes of spare room after `*len` bytes of
// actual RTP data (libsrtp2 appends the auth tag there) -
// `buffer_capacity` is checked against that before attempting the
// encryption, rather than trusting the caller. On success, `len` is
// updated to the new (larger) packet length including the appended
// tag. Returns false (leaving `len`/`buffer` untouched) on any
// failure, including "no such session" and "not enough spare room".
bool srtp_session_protect_rtp(const std::string &ice_ufrag, uint8_t *buffer, size_t buffer_capacity, int *len);

// Decrypts one inbound RTCP (feedback) packet in place - same
// in-place/capacity-checked convention as srtp_session_protect_rtp(),
// but using this session's client_write key (the browser's own
// outbound key) since this is INCOMING data being verified/decrypted,
// not outgoing data being encrypted.
bool srtp_session_unprotect_rtcp(const std::string &ice_ufrag, uint8_t *buffer, size_t buffer_capacity, int *len);

#endif // __SRTP_SESSION_H__
