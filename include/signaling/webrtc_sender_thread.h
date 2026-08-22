#ifndef __WEBRTC_SENDER_THREAD_H__
#define __WEBRTC_SENDER_THREAD_H__

/**
 * PIPELINE STAGE (thread wrapper, final stage): WebRTC RTP Packet Queue -> [THIS] -> SRTP encrypt -> Network
 *
 * PHASE 22.6.4: consumes webrtc_rtp_packet_queue.h (Phase 22.6.1's
 * copy of the same stream udp_sender_thread sends over RTSP) and fans
 * each packet out to every WebRTC viewer currently ready to receive
 * media (webrtc_media_registry.h, Phase 22.6.3), SRTP-encrypting a
 * separate copy per viewer (srtp_session_protect_rtp(), Phase
 * 22.5.3 - each viewer has its own DTLS-derived keys, so one
 * ciphertext can never be shared across more than one destination)
 * and delivering it over ice_agent's UDP socket
 * (ice_agent_send_to_peer(), Phase 22.6.2) rather than udp_sender's -
 * WebRTC media travels over the same ICE-nominated path its DTLS
 * handshake and STUN checks already used, not a separate port.
 *
 * Mirrors udp_sender_thread.h's role for the RTSP path, but is a
 * genuinely separate thread/module: different queue, different
 * destinations (per-session, discovered via the registry rather than
 * a fixed per-RTSP-session destination list), different wire format
 * (SRTP ciphertext, not plaintext RTP).
 */
int webrtc_sender_thread_start(void);
void webrtc_sender_thread_stop(void);

#endif // __WEBRTC_SENDER_THREAD_H__
