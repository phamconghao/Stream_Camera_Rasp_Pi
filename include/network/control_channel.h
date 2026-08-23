#ifndef __CONTROL_CHANNEL_H__
#define __CONTROL_CHANNEL_H__

#include <cstdint>
#include <cstddef>

/**
 * PIPELINE STAGE (receiver side, control channel):
 * RTP Depacketizer Thread / loss_reporter_thread -> [THIS] -> Network -> sender
 *
 * Sends control messages to the sender's control_listener_thread
 * (network/control_listener_thread.h):
 *   - keyframe request: called from rtp_depacketizer_thread.cpp whenever
 *     rtp_jitter_buffer reports lost packets.
 *   - loss report: called periodically from loss_reporter_thread.cpp
 *     with the observed loss rate, driving the sender's adaptive bitrate.
 *
 * Thin wrapper around udp_sender (reused as-is - it's just "send bytes
 * to a fixed UDP destination", nothing RTP-specific). Renamed from
 * keyframe_requester once it grew a second message type.
 */

int control_channel_init(const char *sender_ip, uint16_t control_port);
void control_channel_cleanup(void);

// Sends a keyframe request unless one was already sent recently (see
// KEYFRAME_REQUEST_MIN_INTERVAL_US in control_channel.cpp) - safe to
// call on every single lost-packet event without flooding the sender.
void control_channel_request_keyframe(void);

// Reports the current loss rate, in parts-per-thousand (e.g. 50 = 5.0%),
// to the sender for its adaptive-bitrate decision. Not rate-limited
// internally - loss_reporter_thread already only calls this on its own
// fixed interval (see loss_reporter_thread.cpp).
void control_channel_report_loss(uint32_t loss_permille);

/**
 * Sends raw bytes as-is via this module's already-open socket - used
 * by rtcp_receiver_thread to send RTCP RR packets over the same
 * destination as the ad-hoc keyframe-request/loss-report messages
 * above (multiplexed, no new port). Returns the same as
 * udp_sender_send(): bytes sent, or -1 on failure.
 */
int control_channel_send_raw(const uint8_t *data, size_t size);

#endif // __CONTROL_CHANNEL_H__
