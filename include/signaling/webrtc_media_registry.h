#ifndef __WEBRTC_MEDIA_REGISTRY_H__
#define __WEBRTC_MEDIA_REGISTRY_H__

#include <cstdint>
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
 *
 * Also carries each session's negotiated H.264 RTP payload type
 * (webrtc_sdp_offer_t::h264_payload_type, parsed from that browser's
 * own offer - see webrtc_sdp.h) - browsers assign payload type numbers
 * themselves and different browsers/sessions can pick different
 * numbers for H.264, so this project's fixed internal PT (used for the
 * RTSP path and for building each session's SDP answer) cannot be
 * assumed to match what any given WebRTC viewer actually expects on
 * the wire. webrtc_sender_thread.cpp reads this back to patch each
 * outgoing packet's payload type field per session before encrypting.
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

// Records the H.264 payload type this session's browser negotiated -
// called once, at offer time, before the session is necessarily
// "ready" (see webrtc_media_registry_add() above), so this is
// independent of the ready-set itself. Safe to call before or after
// webrtc_media_registry_add() for the same ufrag.
void webrtc_media_registry_set_payload_type(const std::string &ice_ufrag, uint8_t payload_type);

// Returns the payload type recorded via _set_payload_type() for this
// ufrag, or 0 if none was ever recorded (e.g. unknown ufrag).
uint8_t webrtc_media_registry_get_payload_type(const std::string &ice_ufrag);

#endif // __WEBRTC_MEDIA_REGISTRY_H__
