#ifndef __WEBRTC_SENDER_THREAD_H__
#define __WEBRTC_SENDER_THREAD_H__

/**
 * PIPELINE STAGE (thread wrapper, final stage): WebRTC RTP Packet Queue -> [THIS] -> SRTP encrypt -> Network
 *
 * Consumes webrtc_rtp_packet_queue.h and fans each packet out to
 * every WebRTC viewer currently ready to receive media
 * (webrtc_media_registry.h), SRTP-encrypting a separate copy per
 * viewer (srtp_session_protect_rtp() - each viewer has its own
 * DTLS-derived keys) and delivering it over ice_agent's UDP socket
 * (ice_agent_send_to_peer()) - WebRTC media travels over the same
 * ICE-nominated path its DTLS handshake and STUN checks already used.
 *
 * Mirrors udp_sender_thread.h's role for the RTSP path, but is a
 * separate thread/module: different queue, different destinations
 * (per-session, discovered via the registry), different wire format
 * (SRTP ciphertext, not plaintext RTP).
 */
int webrtc_sender_thread_start(void);
void webrtc_sender_thread_stop(void);

#endif // __WEBRTC_SENDER_THREAD_H__
