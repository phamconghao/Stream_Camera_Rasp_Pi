#ifndef __SIGNALING_SERVER_H__
#define __SIGNALING_SERVER_H__

#include <cstdint>
#include <string>
#include <functional>

/**
 * WebRTC signaling server - a minimal WebSocket (RFC 6455) server used
 * as the out-of-band channel for exchanging SDP offer/answer and ICE
 * candidates with a browser's RTCPeerConnection. Separate from and
 * unrelated to the RTSP TCP server (rtsp/rtsp_server.h).
 *
 * Scope: WebSocket handshake, framing, and a thin JSON message
 * envelope (see common/json_lite.h) in and out. Does not understand
 * SDP/ICE/WebRTC semantics itself - it just delivers whatever JSON
 * text a client sends to a callback, and lets that callback reply to
 * any connected client by client_id.
 *
 * One thread per connected client (same pattern as rtsp_server.cpp's
 * accept loop). Text frames only.
 *
 * PHASE 23.4: `token` is a pre-shared secret every client must present
 * as a `?token=...` query parameter on the initial WebSocket handshake
 * request - checked in do_websocket_handshake() BEFORE any offer/
 * answer parsing or DTLS work begins, since that's the actual point
 * that matters for the DoS concern this closes (see
 * docs-security-threat-model.md section 1.1 row (d)). A missing/wrong
 * token gets a plain HTTP 401, not a WebSocket 101 upgrade.
 *
 * PHASE 24.6: optional TLS (wss://), via `tls_cert_path`/`tls_key_path`
 * (PEM files - e.g. a Let's Encrypt certificate/key pair for real
 * deployments, or a self-signed pair for local testing). When both are
 * non-empty, every accepted TCP connection is wrapped in TLS
 * (SSL_accept()) before the WebSocket handshake even begins - closes
 * the plaintext-token exposure created by Phase 24's direction change
 * (WebRTC viewers no longer required to be on a Tailscale tunnel, so
 * this server is now expected to be reachable from the open internet,
 * where the 23.4 token above would otherwise be a bearer credential
 * sent in the clear). When either path is empty, this server runs
 * exactly as before (plain ws://) - TLS is opt-in, not required to run
 * this project at all (e.g. for LAN-only/Tailscale-only setups where
 * the tunnel itself already provides transport encryption).
 */

// Called once per complete JSON text message received from a client.
// client_id identifies which connection it came from - pass it back
// into signaling_server_send() to reply to that same client.
using signaling_message_handler_t = std::function<void(const std::string &client_id, const std::string &raw_json)>;

// Called once when a client's WebSocket connection closes. Useful for
// tearing down any session state associated with that client_id.
using signaling_disconnect_handler_t = std::function<void(const std::string &client_id)>;

int signaling_server_start(
    uint16_t port, const std::string &token, signaling_message_handler_t handler,
    const std::string &tls_cert_path = "", const std::string &tls_key_path = "");
void signaling_server_stop(void);

// True if this server was started with TLS enabled (both
// tls_cert_path/tls_key_path non-empty) - purely informational, e.g.
// so main.cpp can print "wss://" vs "ws://" in its startup banner
// without duplicating the "both paths non-empty" check itself.
bool signaling_server_is_tls_enabled(void);

// Registers the disconnect callback. Optional - call before
// signaling_server_start() if used.
void signaling_server_set_disconnect_handler(signaling_disconnect_handler_t handler);

// Sends a text frame containing `json` to the client identified by
// client_id. Returns false if that client_id isn't currently
// connected.
bool signaling_server_send(const std::string &client_id, const std::string &json);

#endif // __SIGNALING_SERVER_H__
