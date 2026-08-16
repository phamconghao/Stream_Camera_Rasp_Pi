#ifndef __DTLS_HANDSHAKE_H__
#define __DTLS_HANDSHAKE_H__

#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>
#include <vector>

/**
 * ============================================================================
 * PHASE 22.4: DTLS handshake (RFC 6347), server role only.
 * ============================================================================
 *
 * ARCHITECTURE (see roadmap.md's Phase 22.4 discussion for the
 * shared-thread-vs-separate-thread tradeoff this resolves): ONE
 * shared UDP socket (ice_agent.cpp's, already validated by ICE - see
 * ice_agent.h) carries STUN, DTLS, and eventually SRTP traffic for
 * every concurrent peer connection, demultiplexed by first-byte
 * content type per RFC 7983. ice_agent.cpp owns that demux point in
 * its single recv() loop; DTLS-typed bytes are handed off here via
 * dtls_handshake_on_packet(), which enqueues them for a PER-SESSION
 * handshake thread rather than running OpenSSL calls on ice_agent's
 * shared recv thread - so one slow/malicious peer's handshake can
 * never stall STUN responses (or another peer's own handshake).
 *
 * Each session's handshake runs against a pair of in-memory BIOs
 * (BIO_s_mem) instead of a real socket - this project supplies bytes
 * in (from the network, via dtls_handshake_on_packet) and pulls bytes
 * out (to send back over the network, via the send_fn callback given
 * at session creation) itself, entirely decoupled from any actual
 * socket I/O. This is what makes "N independent per-session threads
 * sharing one physical socket" possible without each thread needing
 * its own socket.
 *
 * SCOPE: gets a DTLS session to "connected" and verifies the peer's
 * certificate fingerprint against what the SDP offer promised
 * (Phase 22.2.2's parsed fingerprint - the actual MITM-prevention
 * mechanism here, see dtls_cert.h). Exporting/using the resulting
 * SRTP keying material to actually encrypt/decrypt RTP is Phase 22.5,
 * not this phase - dtls_handshake_get_srtp_keying_material() exists
 * ready for that, but nothing calls it yet.
 */

// Builds the ONE shared SSL_CTX every session's SSL* is created from
// (cert/key loaded from dtls_cert.h, SRTP profile enabled via
// SSL_CTX_set_tlsext_use_srtp - RFC 5764). Must be called after
// dtls_cert_init() has already run. Returns 0 on success, -1 on any
// OpenSSL failure.
int dtls_handshake_init(void);

void dtls_handshake_cleanup(void);

// Called once per WebRTC peer connection, right after
// ice_agent_register_session() (main.cpp's handle_offer) - registers
// the fingerprint this session's remote peer is EXPECTED to present
// during its handshake (from parse_webrtc_sdp_offer()'s offer.
// fingerprint_algo/fingerprint_hex). No SSL/handshake object is
// created yet at this point - that happens lazily, on the first DTLS
// packet actually received for this ice_ufrag (see
// dtls_handshake_on_packet()), since nothing about a handshake can
// start before the peer sends its ClientHello.
void dtls_handshake_register_session(
    const std::string &ice_ufrag,
    const std::string &remote_fingerprint_algo,
    const std::string &remote_fingerprint_hex);

void dtls_handshake_unregister_session(const std::string &ice_ufrag);

// Feeds one raw UDP datagram (already identified by ice_agent.cpp as
// DTLS content-type, per RFC 7983) into the session identified by
// ice_ufrag. Creates that session's handshake thread on the FIRST
// call for a given ice_ufrag (must have been registered already via
// dtls_handshake_register_session(), or this call is a no-op -
// logged, not fatal, since a stray DTLS packet for an unknown session
// is unremarkable network noise, not a caller bug). `send_fn` is only
// used at creation time (to know how to write bytes back to this
// peer, i.e. sendto() on ice_agent's socket to the address the
// original STUN check nominated) - ignored on subsequent calls, since
// the session is already bound to that address.
void dtls_handshake_on_packet(
    const std::string &ice_ufrag,
    const uint8_t *data,
    size_t size,
    std::function<void(const uint8_t *, size_t)> send_fn);

// True once this session's handshake finished AND the peer's
// certificate fingerprint matched what was registered - i.e. this
// project can trust the channel. False for "still handshaking",
// "failed/timed out", or "no such session".
bool dtls_handshake_is_connected(const std::string &ice_ufrag);

// Phase 22.5 prep: once dtls_handshake_is_connected() is true, this
// returns the SRTP keying material exported from the completed DTLS
// session via SSL_export_keying_material() (RFC 5764 section 4.2,
// label "EXTRACTOR-dtls_srtp") - the raw bytes SRTP's key derivation
// (not implemented until Phase 22.5) turns into actual AES-CM keys
// and salts for both directions. Returns false (leaving `out` alone)
// if the session isn't connected yet or doesn't exist.
bool dtls_handshake_get_srtp_keying_material(const std::string &ice_ufrag, std::vector<uint8_t> &out);

#endif // __DTLS_HANDSHAKE_H__
