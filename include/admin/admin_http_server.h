#ifndef __ADMIN_HTTP_SERVER_H__
#define __ADMIN_HTTP_SERVER_H__

#include <cstdint>
#include <string>

/**
 * Plain HTTP/1.1 server (no TLS - see docs-security-threat-model.md
 * for why that's an accepted trade-off, same "only reach this over
 * Tailscale/LAN" precedent Phase 23/24 already established) whose only
 * job is: log an admin in, and serve the admin dashboard HTML with the
 * real WebRTC signaling token baked in so a human never has to see or
 * paste it. Deliberately a separate module/port from
 * signaling_server.cpp (WebSocket, port 8765) - see admin_http_server.cpp's
 * header comment for the routes it serves.
 *
 * The dashboard's own WebSocket connection (for video + the two
 * privileged admin commands) goes to the EXISTING signaling server on
 * `signaling_port`, completely unchanged - this module only needs that
 * port number and the token to inject them into the HTML it serves.
 */

int admin_http_server_start(
    uint16_t port,
    const std::string &admin_username,
    const std::string &admin_password,
    const std::string &signaling_token,
    uint16_t signaling_port);

void admin_http_server_stop(void);

#endif // __ADMIN_HTTP_SERVER_H__
