#ifndef __CONTROL_PROTOCOL_H__
#define __CONTROL_PROTOCOL_H__

#include <cstdint>

/**
 * Wire format shared by both sides of the small control channel between
 * camera_receiver and camera_app - packet-loss recovery and the
 * adaptive-bitrate extension. Every message starts with a 1-byte type
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
 * This is intentionally minimal and not RFC 3550 RTCP - a fuller RTCP
 * implementation exists separately (see rtp/rtcp_*). This exists to
 * get loss recovery and adaptive bitrate working with the smallest
 * reasonable amount of code. Both sides must agree on this exact
 * format since it isn't self-describing or versioned.
 *
 * PHASE 23.2: both message types above are followed on the wire by a
 * trailing CONTROL_MSG_HMAC_SIZE-byte HMAC-SHA256 (see common/hmac.h),
 * computed over exactly the bytes shown above (the un-suffixed
 * message), keyed with a shared secret both executables are started
 * with. control_listener_thread.cpp verifies this before acting on
 * either message type - see docs-security-threat-model.md for why.
 * RTCP RR packets relayed via control_channel_send_raw() are
 * deliberately NOT covered by this - see that function's comment.
 */

#include <cstddef>

enum : uint8_t
{
    CONTROL_MSG_KEYFRAME_REQUEST = 0xA5,
    CONTROL_MSG_LOSS_REPORT = 0xB6,
};

// HMAC-SHA256 digest size (see common/hmac.h) - the trailing MAC length
// on both authenticated message types above.
static constexpr size_t CONTROL_MSG_HMAC_SIZE = 32;

#pragma pack(push, 1)
struct control_loss_report_t
{
    uint8_t type; // = CONTROL_MSG_LOSS_REPORT
    uint32_t loss_permille_be; // network byte order (htonl/ntohl) - packets lost per 1000 received
};
#pragma pack(pop)

#endif // __CONTROL_PROTOCOL_H__
