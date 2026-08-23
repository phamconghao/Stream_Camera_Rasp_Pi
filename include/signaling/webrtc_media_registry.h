#ifndef __WEBRTC_MEDIA_REGISTRY_H__
#define __WEBRTC_MEDIA_REGISTRY_H__

#include <string>
#include <vector>

/**
 * PHASE 22.6.3: which WebRTC sessions (by ice_ufrag) are currently
 * ready to receive encrypted media - i.e. their SRTP session (Phase
 * 22.5.3's srtp_session_create()) has been created, so
 * srtp_session_protect_rtp()/ice_agent_send_to_peer() can both
 * succeed for them right now. This is the "who do we fan out to"
 * list webrtc_sender_thread.cpp (Phase 22.6.4) iterates every time it
 * has a packet to send - the multi-client answer for the WebRTC path,
 * parallel to how rtsp_session_registry.h's PLAYING sessions serve
 * that role for Phase 20's RTSP fan-out.
 *
 * Both ADD and REMOVE are wired up: dtls_handshake.cpp calls
 * webrtc_media_registry_add() right after a session's SRTP context is
 * created, and webrtc_media_registry_remove() from BOTH
 * dtls_handshake_unregister_session() (Phase 22.6.5's session-end
 * path - see signaling_server.h's disconnect handler and main.cpp's
 * on_signaling_disconnect()) and dtls_handshake_cleanup() (process
 * shutdown). The one documented gap (see signaling_server.h): an
 * ICE-level failure/timeout that leaves the signaling WebSocket open
 * isn't currently detected, so that specific case wouldn't trigger a
 * remove - everything else (browser closes the tab, navigates away,
 * or the signaling connection drops) does.
 */

void webrtc_media_registry_add(const std::string &ice_ufrag);
void webrtc_media_registry_remove(const std::string &ice_ufrag);

// Cheap check for whether ANY session is ready - used by
// rtp_packetizer_thread.cpp (Phase 22.6.1) to skip the extra
// copy-into-webrtc-queue work entirely when no one is watching via
// WebRTC, without needing to know anything else about this registry.
bool webrtc_media_registry_has_any(void);

// Snapshot of every currently-ready ufrag - webrtc_sender_thread.cpp
// (Phase 22.6.4) iterates this once per packet to fan out to every
// active viewer. Returned by value (a copy) so the caller can iterate
// it without holding this registry's internal lock for the duration.
std::vector<std::string> webrtc_media_registry_get_all(void);

#endif // __WEBRTC_MEDIA_REGISTRY_H__
