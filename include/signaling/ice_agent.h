#ifndef __ICE_AGENT_H__
#define __ICE_AGENT_H__

#include <cstdint>
#include <cstddef>
#include <string>

/**
 * ICE connectivity check - responder side. One UDP socket, shared
 * across every concurrent WebRTC peer connection (differentiated by
 * the ice_ufrag embedded in each STUN request's USERNAME attribute -
 * see stun_message.h), that answers STUN Binding Requests (RFC 8445
 * section 7.2) with a Binding Success Response. Once a pair is
 * nominated, the same socket also carries the peer's SRTCP feedback
 * and this side's outbound SRTP media (ice_agent_send_to_peer()).
 *
 * Simplification: only the responder half is implemented (answering
 * the browser's checks) - this side never initiates checks of its
 * own toward the browser's candidate. In a typical offer/answer
 * exchange the browser is the ICE controlling agent and nominates the
 * pair based on checks it sends, which this module answers.
 * build_stun_binding_request() in stun_message.h is available if
 * bidirectional checks are ever needed.
 */

int ice_agent_start(uint16_t port);
void ice_agent_stop(void);

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
