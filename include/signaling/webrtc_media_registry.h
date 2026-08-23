#ifndef __WEBRTC_MEDIA_REGISTRY_H__
#define __WEBRTC_MEDIA_REGISTRY_H__

#include <string>
#include <vector>

/**
 * Tracks which WebRTC sessions (by ice_ufrag) are currently ready to
 * receive encrypted media - i.e. their SRTP context has been created,
 * so srtp_session_protect_rtp()/ice_agent_send_to_peer() can both
 * succeed. This is the fan-out list webrtc_sender_thread.cpp iterates
 * for every packet, parallel to how rtsp_session_registry.h's PLAYING
 * sessions serve that role for RTSP fan-out.
 *
 * Entries are added right after a session's SRTP context is created,
 * and removed on session teardown or process shutdown. One known gap:
 * an ICE-level failure/timeout that leaves the signaling connection
 * open isn't currently detected, so that case doesn't trigger removal.
 */

void webrtc_media_registry_add(const std::string &ice_ufrag);
void webrtc_media_registry_remove(const std::string &ice_ufrag);

// Cheap check for whether any session is ready - lets callers skip
// extra work when no one is watching via WebRTC.
bool webrtc_media_registry_has_any(void);

// Snapshot of every currently-ready ufrag, for fan-out. Returned by
// value so the caller can iterate it without holding this registry's
// internal lock for the duration.
std::vector<std::string> webrtc_media_registry_get_all(void);

#endif // __WEBRTC_MEDIA_REGISTRY_H__
