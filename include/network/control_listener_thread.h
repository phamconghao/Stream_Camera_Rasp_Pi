#ifndef __CONTROL_LISTENER_THREAD_H__
#define __CONTROL_LISTENER_THREAD_H__

#include <cstdint>
#include <string>

/**
 * PIPELINE STAGE (sender side, control channel): Network -> [THIS] ->
 * bcm2835_encoder_force_keyframe() / bcm2835_encoder_set_bitrate()
 *
 * Binds a UDP socket on `control_port` and listens for control messages
 * sent by control_channel (receiver side, see network/control_channel.h):
 *   - keyframe request: forces an IDR on the next frame.
 *   - loss report (adaptive bitrate): maps the reported loss rate to a
 *     target bitrate tier and applies it live via V4L2, if it actually
 *     changed from the current tier.
 *
 * Independent thread, own running flag, same start/stop convention as
 * every other thread in this project. Renamed from keyframe_listener_thread
 * once it grew a second message type - same module, broader job.
 *
 * PHASE 23.2: `control_secret` must match the value control_channel.cpp
 * on the receiver side was given - every keyframe-request/loss-report
 * datagram is HMAC-SHA256-verified against it before being acted on
 * (see docs-security-threat-model.md).
 */

int control_listener_thread_start(uint16_t control_port, const std::string &control_secret);
void control_listener_thread_stop(void);

#endif // __CONTROL_LISTENER_THREAD_H__
