#ifndef __ADMIN_SESSION_H__
#define __ADMIN_SESSION_H__

#include <string>

/**
 * In-memory admin login session store. A session is created once
 * admin_http_server.cpp's POST /login handler has verified
 * ADMIN_USERNAME/ADMIN_PASSWORD, and is what the login cookie (and the
 * admin-only WebSocket message types in main.cpp - see "force-keyframe"/
 * "get-viewers") both point back to for authorization. Deliberately
 * separate from webrtc_media_registry.h/dtls_handshake.cpp's session
 * concept - those track WebRTC *media* sessions (per ice_ufrag); this
 * tracks *admin login* sessions (per random session id), an entirely
 * different identity and lifecycle.
 *
 * No persistence across a process restart - matches this project's
 * existing in-memory-only registries (ice_agent's g_sessions,
 * webrtc_media_registry's g_ready_ufrags). A restart simply logs
 * everyone out, which is an acceptable trade-off for a single-admin
 * home camera - not a distributed multi-instance auth system.
 */

// Creates a new session for `username`, returns its id (64 lowercase
// hex chars, from 32 bytes of OpenSSL RAND_bytes - same construction
// as rtsp_server.cpp's Digest-auth nonce, just twice the length since
// this needs to resist guessing for weeks, not one TCP connection).
// `remember_me` selects the session's lifetime: ~30 days if true,
// ~24h if false (bounds memory growth from a browser tab left open
// without "remember me" checked). Returns "" if RAND_bytes fails
// (should be unreachable in practice) - caller must treat that as a
// hard login failure, never fall back to a predictable id.
std::string admin_session_create(const std::string &username, bool remember_me);

// True if `session_id` names a currently valid (existing, unexpired)
// session. Lazily erases the entry if it's found but expired. False
// (no state change) for an empty or unknown id.
bool admin_session_is_valid(const std::string &session_id);

// Removes a session (logout). No-op if `session_id` is unknown.
void admin_session_destroy(const std::string &session_id);

#endif // __ADMIN_SESSION_H__
