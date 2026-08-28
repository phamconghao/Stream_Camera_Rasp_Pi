#include "control_channel.h"
#include "control_protocol.h"

#include <arpa/inet.h>
#include <cstring>

#include "udp_sender.h"
#include "time_utils.h"
#include "hmac.h"
#include "log.h"

static const char *TAG = "CTRL_CHAN";

static bool g_initialized = false;
static uint64_t g_last_keyframe_request_us = 0;
static std::string g_control_secret;

// Don't send more than one keyframe request every 500ms: a burst of
// loss events during one bad patch of network should trigger a single
// recovery keyframe, not a flood of them - each keyframe is much larger
// than a P-frame, so requesting too many in a row would congest the
// link further and defeat the purpose.
static constexpr uint64_t KEYFRAME_REQUEST_MIN_INTERVAL_US = 500 * 1000;

// udp_sender.cpp is shared plumbing that fans out to a set of keyed
// destinations (see udp_sender.h) rather than a single fixed one -
// the RTSP sender side (rtsp_server.cpp) needs that for multi-client
// RTP fan-out. This control channel is still strictly point-to-point
// (receiver -> the one sender it's paired with), so it just registers
// its single destination under a fixed key.
static const char *CONTROL_DEST_KEY = "control_channel";

int control_channel_init(const char *sender_ip, uint16_t control_port, const std::string &control_secret)
{
    if (udp_sender_init() < 0)
    {
        return -1;
    }

    if (udp_sender_add_dest(CONTROL_DEST_KEY, sender_ip, control_port) < 0)
    {
        udp_sender_cleanup();
        return -1;
    }

    g_control_secret = control_secret;
    g_initialized = true;
    g_last_keyframe_request_us = 0;

    return 0;
}

void control_channel_cleanup(void)
{
    if (g_initialized)
    {
        udp_sender_cleanup();
        g_initialized = false;
        g_control_secret.clear();
    }
}

void control_channel_request_keyframe(void)
{
    if (!g_initialized)
    {
        return;
    }

    uint64_t now = time_utils_now_us();
    if (g_last_keyframe_request_us != 0 &&
        now - g_last_keyframe_request_us < KEYFRAME_REQUEST_MIN_INTERVAL_US)
    {
        LOG_INFO(TAG, "suppressing keyframe request (rate limited)");
        return;
    }

    // PHASE 23.2: message is the 1-byte type tag followed by a
    // CONTROL_MSG_HMAC_SIZE-byte HMAC-SHA256 over that byte, keyed with
    // g_control_secret - control_listener_thread.cpp verifies this
    // before acting on the request.
    uint8_t buf[1 + CONTROL_MSG_HMAC_SIZE];
    buf[0] = CONTROL_MSG_KEYFRAME_REQUEST;

    std::vector<uint8_t> mac = hmac_sha256(g_control_secret, buf, 1);
    std::memcpy(buf + 1, mac.data(), CONTROL_MSG_HMAC_SIZE);

    if (udp_sender_send(buf, sizeof(buf)) < 0)
    {
        LOG_WARN(TAG, "failed to send keyframe request");
        return;
    }

    g_last_keyframe_request_us = now;
    LOG_INFO(TAG, "sent keyframe request to sender");
}

void control_channel_report_loss(uint32_t loss_permille)
{
    if (!g_initialized)
    {
        return;
    }

    control_loss_report_t report;
    report.type = CONTROL_MSG_LOSS_REPORT;
    report.loss_permille_be = htonl(loss_permille);

    // PHASE 23.2: same scheme as control_channel_request_keyframe()
    // above - HMAC-SHA256 over the report struct's bytes, appended
    // after it.
    uint8_t buf[sizeof(control_loss_report_t) + CONTROL_MSG_HMAC_SIZE];
    std::memcpy(buf, &report, sizeof(report));

    std::vector<uint8_t> mac = hmac_sha256(g_control_secret, buf, sizeof(report));
    std::memcpy(buf + sizeof(report), mac.data(), CONTROL_MSG_HMAC_SIZE);

    if (udp_sender_send(buf, sizeof(buf)) < 0)
    {
        LOG_WARN(TAG, "failed to send loss report");
        return;
    }

    LOG_INFO(TAG, "reported loss rate %u.%u%% to sender", loss_permille / 10, loss_permille % 10);
}

int control_channel_send_raw(const uint8_t *data, size_t size)
{
    if (!g_initialized)
    {
        return -1;
    }

    return udp_sender_send(data, size);
}
