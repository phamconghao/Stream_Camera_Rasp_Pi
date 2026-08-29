#ifndef __AUTH_FAILURE_LOG_H__
#define __AUTH_FAILURE_LOG_H__

#include <string>

/**
 * PHASE 23.5: one shared sink for every authentication failure across
 * the three sources Phase 23 added auth to - control channel HMAC
 * (23.2), RTSP Digest (23.3), and WebSocket signaling token (23.4).
 * Two things this buys over each module logging its own way:
 *
 *   1. `grep AUTH_FAIL` finds every rejected auth attempt in this
 *      project regardless of which subsystem produced it, in one
 *      consistent `source=... client=... reason="..."` shape.
 *   2. Optional per-(source, client_ip) temporary blocking: past
 *      AUTH_FAILURE_BLOCK_THRESHOLD failures from the same client_ip
 *      against the same source within AUTH_FAILURE_BLOCK_WINDOW_US,
 *      that pair is blocked for AUTH_FAILURE_BLOCK_DURATION_US - see
 *      auth_failure_is_blocked() below.
 *
 * client_ip may be "" - the control channel (23.2) has no per-datagram
 * source address available today (see control_listener_thread.cpp's
 * own comment on this), so it can only ever log, never be
 * IP-blocked here. RTSP (23.3) and signaling (23.4) both already have
 * a real client_ip from their TCP accept() call, so both get the full
 * logging + blocking behavior.
 *
 * This intentionally does NOT replace control_listener_thread.cpp's
 * own 23.2 rate-limited summarization (see its log_auth_failure()) -
 * that logic exists specifically because raw UDP has no TCP-handshake
 * cost gating how fast an attacker can send forged datagrams, unlike
 * RTSP/signaling's TCP-based paths. control_listener_thread.cpp still
 * decides WHEN to emit a line (once per its own ~5s window); this
 * module only standardizes the format of that line once it does.
 */

// Logs one authentication failure in the shared format, and - if
// client_ip is non-empty - counts it toward that (source, client_ip)
// pair's temporary-block threshold (see the header comment above).
void auth_failure_log(const char *source, const std::string &client_ip, const char *reason);

// Returns true if (source, client_ip) is currently within a temporary
// block window set by auth_failure_log() above. Always false for an
// empty client_ip. Callers that DO have a client_ip (rtsp_server.cpp,
// signaling_server.cpp) should check this BEFORE doing any further
// work on a request/connection, not just before logging - the point
// is to cheaply short-circuit a client that's already shown it's not
// presenting valid credentials, not just to log that it happened
// again.
bool auth_failure_is_blocked(const char *source, const std::string &client_ip);

#endif // __AUTH_FAILURE_LOG_H__
