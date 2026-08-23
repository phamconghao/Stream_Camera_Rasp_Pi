#ifndef __DTLS_HANDSHAKE_H__
#define __DTLS_HANDSHAKE_H__

#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>
#include <vector>

/**
 * DTLS handshake (RFC 6347), server role only.
 *
 * One shared UDP socket (ice_agent.h, already validated by ICE)
 * carries STUN, DTLS, and SRTP traffic for every concurrent peer
 * connection, demultiplexed by first-byte content type per RFC 7983.
 * DTLS-typed bytes are handed off here via dtls_handshake_on_packet(),
 * which enqueues them for a per-session handshake thread rather than
 * running OpenSSL calls on the shared recv thread - so one slow peer's
 * handshake can never stall another peer's.
 *
 * Each session's handshake runs against a pair of in-memory BIOs
 * (BIO_s_mem) instead of a real socket: bytes come in via
 * dtls_handshake_on_packet() and go out via the send_fn callback given
 * at session creation, decoupling the handshake state machine from
 * actual socket I/O.
 *
 * Gets a session to "connected" and verifies the peer's certificate
 * fingerprint against what its SDP offer promised (see dtls_cert.h),
 * then hands the exported SRTP keying material to
 * srtp_session_create() (see the end of handshake_thread_func() in
 * dtls_handshake.cpp).
 */

// Builds the ONE shared SSL_CTX every session's SSL* is created from
// (cert/key loaded from dtls_cert.h, SRTP profile enabled via
// SSL_CTX_set_tlsext_use_srtp - RFC 5764). Must be called after
// dtls_cert_init() has already run. Returns 0 on success, -1 on any
// OpenSSL failure.
int dtls_handshake_init(void);

void dtls_handshake_cleanup(void);

// Called once per peer connection, right after
// ice_agent_register_session() - registers the fingerprint this
// session's remote peer is expected to present during its handshake.
// No SSL/handshake object is created yet - that happens lazily on the
// first DTLS packet received for this ice_ufrag.
void dtls_handshake_register_session(
    const std::string &ice_ufrag,
    const std::string &remote_fingerprint_algo,
    const std::string &remote_fingerprint_hex);

void dtls_handshake_unregister_session(const std::string &ice_ufrag);

// Feeds one raw UDP datagram (already identified as DTLS content-type)
// into the session identified by ice_ufrag. Creates that session's
// handshake thread on the first call for a given ice_ufrag (must have
// been registered already, or this call is a harmless no-op).
// `send_fn` is only used at creation time to know how to write bytes
// back to this peer.
void dtls_handshake_on_packet(
    const std::string &ice_ufrag,
    const uint8_t *data,
    size_t size,
    std::function<void(const uint8_t *, size_t)> send_fn);

// True once this session's handshake finished and the peer's
// certificate fingerprint matched what was registered. False for
// "still handshaking", "failed/timed out", or "no such session".
bool dtls_handshake_is_connected(const std::string &ice_ufrag);

#endif // __DTLS_HANDSHAKE_H__
