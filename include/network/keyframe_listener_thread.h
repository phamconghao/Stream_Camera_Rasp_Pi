#ifndef __KEYFRAME_LISTENER_THREAD_H__
#define __KEYFRAME_LISTENER_THREAD_H__

#include <cstdint>

/**
 * PIPELINE STAGE (sender side, Phase 18 - packet loss recovery):
 * Network (control channel) -> [THIS] -> bcm2835_encoder_force_keyframe()
 *
 * Binds a UDP socket on `control_port` and listens for keyframe-request
 * datagrams sent by keyframe_requester (receiver side, see
 * network/keyframe_requester.h) whenever the receiver's jitter buffer
 * detects lost packets. On receiving one, forces the encoder to emit an
 * IDR on the next frame - this bounds how long the receiver has to wait
 * to recover a decodable picture after loss, instead of waiting for
 * whatever the next regularly-scheduled keyframe happens to be.
 *
 * Independent thread, own running flag, same start/stop convention as
 * every other thread in this project.
 */

int keyframe_listener_thread_start(uint16_t control_port);
void keyframe_listener_thread_stop(void);

#endif // __KEYFRAME_LISTENER_THREAD_H__
