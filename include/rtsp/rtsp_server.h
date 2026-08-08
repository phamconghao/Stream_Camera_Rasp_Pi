#ifndef __RTSP_SERVER_H__
#define __RTSP_SERVER_H__

#include <cstdint>

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
 */

int rtsp_server_start(uint16_t port);
void rtsp_server_stop(void);

#endif // __RTSP_SERVER_H__
