#ifndef __RTSP_SERVER_H__
#define __RTSP_SERVER_H__

#include <cstdint>
#include <string>

/**
 * ============================================================================
 * PHASE 20: RTSP Server (RFC 2326 subset) - TCP control plane
 * ============================================================================
 *
 * Listens on `port` (TCP - distinct from every UDP port elsewhere in
 * this project) for RTSP connections, one thread per connected client.
 * Method subset handled: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN.
 *
 * Also owns a small reaper thread that periodically calls
 * rtsp_session_registry_reap_orphans().
 *
 * PHASE 23.3: `username`/`password` are the one credential pair every
 * request except OPTIONS must authenticate with, via RFC 2617 HTTP
 * Digest Authentication (RTSP reuses it as-is per RFC 2326 section
 * 17). See rtsp_server.cpp's check_digest_auth() for the scheme and
 * docs-security-threat-model.md for why Digest specifically.
 */

int rtsp_server_start(uint16_t port, const std::string &username, const std::string &password);
void rtsp_server_stop(void);

#endif // __RTSP_SERVER_H__
