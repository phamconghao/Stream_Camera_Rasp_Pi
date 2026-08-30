#ifndef __WEBRTC_SESSION_STATS_H__
#define __WEBRTC_SESSION_STATS_H__

#include <cstdint>
#include <string>
#include <vector>

/**
 * Per-WebRTC-viewer stats for the admin dashboard (connected duration,
 * frames sent, packet loss/jitter as reported by the browser's own
 * RTCP Receiver Reports) - deliberately kept separate from
 * webrtc_media_registry.h, whose documented job is the RTP fan-out
 * list webrtc_sender_thread.cpp iterates every packet. This module is
 * a pure read-mostly side table for a much less frequent consumer (the
 * dashboard's ~2s poll), fed by two different writer subsystems: the
 * RTP send path (frames_sent) and the RTCP receive path (loss/jitter).
 *
 * Same ice_ufrag identity as webrtc_media_registry, same lifecycle
 * (created alongside webrtc_media_registry_add(), destroyed alongside
 * webrtc_media_registry_remove() - see dtls_handshake.cpp), but no
 * shared storage or locking with that module.
 */

struct webrtc_session_stats_snapshot_t
{
    std::string ice_ufrag;
    uint64_t connected_at_us = 0;

    // Incremented once per outgoing marker-bit (last-packet-of-frame)
    // RTP packet successfully sent to this session - see
    // webrtc_sender_thread.cpp. This is "frames this project sent",
    // not "frames the browser actually decoded" (this project has no
    // visibility into the latter) - the dashboard computes an
    // approximate fps by diffing this value between two polls.
    uint64_t frames_sent = 0;

    // False until this session's first RTCP Receiver Report arrives -
    // the dashboard shows "-" for loss/jitter until then rather than a
    // misleading 0%.
    bool has_rtcp_rr = false;
    uint8_t last_fraction_lost = 0;     // RFC 3550 6.4.2: 8-bit fixed point, /256 = loss rate since the peer's last RR
    uint32_t cumulative_lost = 0;       // low 24 bits of the RR field, same extraction as control_listener_thread.cpp's handle_rtcp_rr() (not sign-extended, matching that precedent)
    uint32_t last_jitter_rtp_units = 0; // RFC 3550 6.4.1 formula, 90kHz clock units (this project's RTP clock rate)
    uint64_t last_rr_at_us = 0;
};

// Called from dtls_handshake.cpp right alongside
// webrtc_media_registry_add()/_remove() - starts/stops tracking this
// ufrag. _on_connect records `connected_at_us` as "now".
void webrtc_session_stats_on_connect(const std::string &ice_ufrag);
void webrtc_session_stats_on_disconnect(const std::string &ice_ufrag);

// Called from webrtc_sender_thread.cpp once per successfully-sent
// marker-bit packet for this ufrag. No-op (not an error) if the ufrag
// isn't currently tracked - e.g. a send racing against disconnect.
void webrtc_session_stats_record_frame_sent(const std::string &ice_ufrag);

// Called from ice_agent.cpp's SRTCP handling when the decrypted RTCP
// packet is a Receiver Report (rtcp_is_rr()) for this ufrag.
void webrtc_session_stats_record_rtcp_rr(
    const std::string &ice_ufrag, uint8_t fraction_lost,
    uint32_t cumulative_lost, uint32_t jitter_rtp_units);

// Snapshot of every currently-tracked session, for the dashboard's
// periodic poll. Returned by value, same "don't hold the lock while
// the caller works" pattern as webrtc_media_registry_get_all().
std::vector<webrtc_session_stats_snapshot_t> webrtc_session_stats_get_all(void);

#endif // __WEBRTC_SESSION_STATS_H__
