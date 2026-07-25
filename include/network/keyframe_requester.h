#ifndef __KEYFRAME_REQUESTER_H__
#define __KEYFRAME_REQUESTER_H__

#include <cstdint>

/**
 * PIPELINE STAGE (receiver side, Phase 18 - packet loss recovery):
 * RTP Depacketizer Thread -> [THIS] -> Network (control channel) -> sender
 *
 * Sends a tiny UDP "please force a keyframe" datagram to the sender
 * whenever rtp_jitter_buffer reports lost packets (see
 * rtp_depacketizer_thread.cpp), so the decoder can recover a clean
 * picture quickly instead of waiting for whatever the next
 * regularly-scheduled IDR happens to be. Received by
 * keyframe_listener_thread on the sender side (network/keyframe_listener_thread.h).
 *
 * Thin wrapper around udp_sender (reused as-is - it's just "send bytes
 * to a fixed UDP destination", nothing RTP-specific), plus rate
 * limiting so a burst of consecutive lost-packet events doesn't flood
 * the sender with redundant requests.
 */

int keyframe_requester_init(const char *sender_ip, uint16_t control_port);
void keyframe_requester_cleanup(void);

// Sends a keyframe request unless one was already sent recently (see
// KEYFRAME_REQUEST_MIN_INTERVAL_US in keyframe_requester.cpp) - safe to
// call on every single lost-packet event without worrying about
// flooding the control channel or the sender re-encoding needlessly
// many keyframes in a row.
void keyframe_requester_request(void);

#endif // __KEYFRAME_REQUESTER_H__
