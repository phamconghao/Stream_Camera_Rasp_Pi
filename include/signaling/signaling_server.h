#ifndef __SIGNALING_SERVER_H__
#define __SIGNALING_SERVER_H__

#include <cstdint>
#include <string>
#include <functional>

/**
 * ============================================================================
 * PHASE 22.1: WebRTC signaling server - WebSocket control plane
 * ============================================================================
 *
 * WebRTC itself has NO built-in signaling mechanism - two peers need
 * an out-of-band channel to exchange SDP offer/answer and ICE
 * candidates before they can establish a direct connection (see
 * roadmap.md's Phase 22 breakdown). This module IS that channel: a
 * minimal WebSocket (RFC 6455) server, separate from and unrelated to
 * the RTSP TCP server (rtsp/rtsp_server.h) - a browser's
 * RTCPeerConnection talks to this port, not RTSP's.
 *
 * Scope of THIS module: WebSocket handshake + framing + a thin JSON
 * message envelope (see common/json_lite.h) in and out. It does NOT
 * understand SDP, ICE, or any WebRTC semantics - it just delivers
 * whatever JSON text a connected client sends to a callback, and lets
 * that callback send JSON text back to any connected client by
 * client_id. Interpreting "type":"offer" vs "type":"ice-candidate"
 * and doing anything about it is Phase 22.2/22.3's job.
 *
 * One thread per connected client (same accept-loop-plus-per-
 * connection-thread pattern as rtsp_server.cpp), text frames only -
 * WebRTC signaling messages are small JSON blobs, never binary frames
 * and never large enough to need fragmentation across multiple
 * WebSocket frames.
 */

// Called once per complete JSON text message received from a client.
// client_id identifies which connection it came from - pass it back
// into signaling_server_send() to reply to that same client.
// raw_json is the message exactly as received (parsing it, e.g. via
// json_parse_object(), is the callback's job - this module doesn't
// interpret message content).
using signaling_message_handler_t = std::function<void(const std::string &client_id, const std::string &raw_json)>;

// PHASE 22.6.5: called once when a client's WebSocket connection
// closes (browser tab closed, navigated away, or the connection just
// dropped) - client_id is the SAME id that was ever passed to the
// message handler above, so a caller that tracked "this client_id is
// running WebRTC session X" (main.cpp does, keyed by ice_ufrag) can
// tear that session down here. This is currently this project's ONLY
// WebRTC session-end signal (see roadmap.md's Phase 22.6 notes) -
// ICE-level failure/timeout detection isn't implemented, so a peer
// that goes silent without its WebSocket also closing (e.g. the
// signaling connection outlives a dead media path) won't be caught by
// this alone.
using signaling_disconnect_handler_t = std::function<void(const std::string &client_id)>;

int signaling_server_start(uint16_t port, signaling_message_handler_t handler);
void signaling_server_stop(void);

// Registers the disconnect callback - optional; if never called, this
// project just doesn't get notified of disconnects (same as before
// Phase 22.6.5). Must be called before signaling_server_start() to
// guarantee no disconnect is ever missed at startup, though in
// practice a disconnect can't happen before any client has connected
// in the first place.
void signaling_server_set_disconnect_handler(signaling_disconnect_handler_t handler);

// Sends a text frame containing `json` to the client identified by
// client_id. Returns false if that client_id isn't currently
// connected (e.g. it disconnected between when the caller learned its
// client_id and this call) - not a hard error, just means the message
// has nowhere to go anymore.
bool signaling_server_send(const std::string &client_id, const std::string &json);

#endif // __SIGNALING_SERVER_H__
