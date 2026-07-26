#ifndef __CONTROL_PROTOCOL_H__
#define __CONTROL_PROTOCOL_H__

#include <cstdint>

/**
 * Wire format shared by both sides of the small control channel between
 * camera_receiver and camera_app (Phase 18 packet-loss recovery + the
 * adaptive-bitrate extension). Every message starts with a 1-byte type
 * tag so control_listener_thread can tell them apart on one socket:
 *
 *   CONTROL_MSG_KEYFRAME_REQUEST - 1 byte total, no payload.
 *     "Force an IDR on the next encoded frame" (see
 *     bcm2835_encoder_force_keyframe()).
 *
 *   CONTROL_MSG_LOSS_REPORT - 5 bytes total (control_loss_report_t).
 *     Reports the receiver's observed loss rate since the last report,
 *     as parts-per-thousand (so 50 = 5.0% loss). The sender maps this
 *     to a target bitrate tier - see control_listener_thread.cpp.
 *
 * This is intentionally minimal and NOT RFC 3550 RTCP - seePhase 19 on
 * the roadmap for a real RTCP implementation later; this exists to get
 * loss recovery + adaptive bitrate working now with the smallest
 * reasonable amount of code. Both sides must agree on this exact format
 * since it isn't self-describing or versioned.
 */

enum : uint8_t
{
    CONTROL_MSG_KEYFRAME_REQUEST = 0xA5,
    CONTROL_MSG_LOSS_REPORT = 0xB6,
};

#pragma pack(push, 1)
struct control_loss_report_t
{
    uint8_t type; // = CONTROL_MSG_LOSS_REPORT
    uint32_t loss_permille_be; // network byte order (htonl/ntohl) - packets lost per 1000 received
};
#pragma pack(pop)

#endif // __CONTROL_PROTOCOL_H__
