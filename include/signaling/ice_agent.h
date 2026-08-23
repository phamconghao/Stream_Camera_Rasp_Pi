#ifndef __ICE_AGENT_H__
#define __ICE_AGENT_H__

#include <cstdint>
#include <cstddef>
#include <string>

/**
 * PHASE 22.3.4: ICE connectivity check - the RESPONDER side only.
 *
 * One UDP socket, shared across every concurrent WebRTC peer
 * connection (differentiated by the ice_ufrag embedded in each STUN
 * request's USERNAME attribute - see stun_message.h), that answers
 * STUN Binding Requests (RFC 8445 section 7.2) from browsers with a
 * Binding Success Response.
 *
 * IMPORTANT SIMPLIFICATION (documented, not hidden): a fully spec-
 * compliant ICE agent sends connectivity checks in BOTH directions -
 * this project only implements the responder half (answering the
 * browser's checks), not initiating checks of its own toward the
 * browser's candidate. In practice this is normally sufficient: the
 * browser (as the ICE controlling agent in a typical offer/answer
 * exchange) is the side that nominates a candidate pair, and it does
 * so based on checks IT sends and receives a valid response to - which
 * this module provides. build_stun_binding_request() in
 * stun_message.h exists and is ready to use if bidirectional checks
 * turn out to be necessary for real-world NAT traversal robustness,
 * but wiring that up is left for a later pass once basic
 * connectivity is confirmed working end to end.
 */

int ice_agent_start(uint16_t port);
void ice_agent_stop(void);

// Registers (or replaces) the (ice_ufrag, ice_pwd) pair for one
// WebRTC peer connection - called right after generate_ice_credentials()
// in main.cpp's handle_offer(), so ice_agent can validate that
// session's incoming STUN requests once the browser starts sending
// them (which can be almost immediately after receiving the SDP
// answer - well before Phase 22.3.3's own ice-candidate message might
// even arrive).
void ice_agent_register_session(const std::string &ice_ufrag, const std::string &ice_pwd);

// Removed once a session ends (mirrors the pattern rtsp_session_registry.h
// and udp_sender.h's per-session add/remove calls use elsewhere in
// this project) - called from main.cpp's on_signaling_disconnect()
// (Phase 22.6.5), the signaling-WebSocket-closed session-teardown path.
void ice_agent_unregister_session(const std::string &ice_ufrag);

// PHASE 22.6.2: sends raw bytes to the address this session's ICE
// connectivity check nominated (see ice_agent.cpp's g_nominated_pairs -
// the reverse of that map, ufrag -> address, populated at the same
// moment a STUN check first succeeds). Used by webrtc_sender_thread.cpp
// (Phase 22.6.4) to deliver SRTP-encrypted RTP - this project's
// existing STUN responder logic and this new send path share the
// exact same UDP socket, which is what makes a single ICE-nominated
// address usable for media in the first place (RFC 8445's whole point:
// the pair that passed connectivity checks IS the media path).
// Returns false if no address has been nominated for this ufrag yet
// (ICE hasn't completed) or the sendto() itself failed.
bool ice_agent_send_to_peer(const std::string &ice_ufrag, const uint8_t *data, size_t size);

#endif // __ICE_AGENT_H__
