#ifndef __ICE_AGENT_H__
#define __ICE_AGENT_H__

#include <cstdint>
#include <cstddef>
#include <string>

/**
 * ICE connectivity check - responder side, plus (Phase 24.3) a
 * one-shot STUN client used only for self-discovery. One UDP socket,
 * shared across every concurrent WebRTC peer connection (differentiated
 * by the ice_ufrag embedded in each STUN request's USERNAME attribute -
 * see stun_message.h), that answers STUN Binding Requests (RFC 8445
 * section 7.2) with a Binding Success Response. Once a pair is
 * nominated, the same socket also carries the peer's SRTCP feedback
 * and this side's outbound SRTP media (ice_agent_send_to_peer()).
 *
 * Simplification: only the responder half of PEER connectivity checks
 * is implemented (answering the browser's checks) - this side never
 * initiates a connectivity check of its own toward the browser's
 * candidate. In a typical offer/answer exchange the browser is the
 * ICE controlling agent and nominates the pair based on checks it
 * sends, which this module answers. build_stun_binding_request() in
 * stun_message.h is available if bidirectional checks are ever
 * needed. The STUN *client* role this module DOES implement
 * (ice_agent_get_public_address() below) is a different, unrelated
 * STUN usage - a one-time query to a public STUN server at startup to
 * learn this project's own address, unauthenticated per RFC 5389's
 * basic Binding usage, not a per-peer connectivity check.
 *
 * Requests arriving on this socket from an unrecognized session or
 * failing MESSAGE-INTEGRITY are tracked per source IP via
 * auth_failure_log.h (Phase 23.5's shared sink, extended here under
 * source "ICE") - repeated failures from the same IP get temporarily
 * blocked before any further parsing, the same protection RTSP and
 * WebSocket signaling already had. Added specifically because Phase
 * 24's direction change means this UDP port is now expected to be
 * reachable from the public internet (24.5), not just LAN/Tailscale.
 */

int ice_agent_start(uint16_t port);
void ice_agent_stop(void);

// The Pi's own public (server-reflexive) address, as learned via a
// one-time STUN discovery exchange run synchronously inside
// ice_agent_start() (Phase 24.3) - before the receive thread starts,
// using the exact same UDP socket/port that ICE connectivity checks
// and media both use afterward, so the mapping is guaranteed to match
// what a browser's checks will actually hit. Configurable via
// STUN_SERVER_HOST/STUN_SERVER_PORT env vars (defaults to Google's
// public STUN server - swap this to a self-hosted coturn instance's
// STUN listener once Phase 24.4 stands one up, removing the
// third-party dependency).
struct ice_public_address_t
{
    std::string public_ip;
    uint16_t public_port = 0;

    // RFC 8839 section 5.1's raddr/rport - the local address this
    // mapping was learned FROM (via getsockname() on the connected
    // discovery socket), needed to build a valid srflx candidate line
    // (see ice_candidate.h's make_server_reflexive_candidate()).
    std::string base_ip;
    uint16_t base_port = 0;
};

// Returns true and fills `out` if discovery succeeded at startup.
// Returns false (leaving `out` untouched) if it failed or hasn't run
// yet - e.g. no route to the STUN server, or it never responded.
// Callers (main.cpp) should treat false as "no srflx candidate this
// session", not a fatal error - the host candidates from
// get_local_host_candidates() are unaffected either way.
bool ice_agent_get_public_address(ice_public_address_t &out);

// Registers (or replaces) the (ice_ufrag, ice_pwd) pair for one peer
// connection, so incoming STUN requests for that session can be
// validated as soon as they arrive.
void ice_agent_register_session(const std::string &ice_ufrag, const std::string &ice_pwd);

// Removed once a session ends.
void ice_agent_unregister_session(const std::string &ice_ufrag);

// Sends raw bytes to the address this session's ICE connectivity
// check nominated. Returns false if no address has been nominated for
// this ufrag yet, or the send itself failed.
bool ice_agent_send_to_peer(const std::string &ice_ufrag, const uint8_t *data, size_t size);

#endif // __ICE_AGENT_H__
