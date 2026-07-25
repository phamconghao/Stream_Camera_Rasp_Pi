#include "keyframe_requester.h"

#include "udp_sender.h"
#include "time_utils.h"
#include "log.h"

static const char *TAG = "KF_REQ";

static bool g_initialized = false;
static uint64_t g_last_request_us = 0;

// Don't send more than one request every 500ms: a burst of loss events
// during one bad patch of network should trigger a single recovery
// keyframe, not a flood of them - each keyframe is much larger than a
// P-frame, so requesting too many in a row would congest the link
// further and defeat the purpose.
static constexpr uint64_t KEYFRAME_REQUEST_MIN_INTERVAL_US = 500 * 1000;

// Single-byte magic value keyframe_listener_thread checks for on the
// sender side - not a general protocol, just enough to filter out
// random garbage/port-scan traffic hitting the control port.
static constexpr uint8_t KEYFRAME_REQUEST_MAGIC = 0xA5;

int keyframe_requester_init(const char *sender_ip, uint16_t control_port)
{
    if (udp_sender_init(sender_ip, control_port) < 0)
    {
        return -1;
    }

    g_initialized = true;
    g_last_request_us = 0;

    return 0;
}

void keyframe_requester_cleanup(void)
{
    if (g_initialized)
    {
        udp_sender_cleanup();
        g_initialized = false;
    }
}

void keyframe_requester_request(void)
{
    if (!g_initialized)
    {
        return;
    }

    uint64_t now = time_utils_now_us();
    if (g_last_request_us != 0 && now - g_last_request_us < KEYFRAME_REQUEST_MIN_INTERVAL_US)
    {
        LOG_INFO(TAG, "suppressing keyframe request (rate limited)");
        return;
    }

    uint8_t magic = KEYFRAME_REQUEST_MAGIC;
    if (udp_sender_send(&magic, sizeof(magic)) < 0)
    {
        LOG_WARN(TAG, "failed to send keyframe request");
        return;
    }

    g_last_request_us = now;
    LOG_INFO(TAG, "sent keyframe request to sender");
}
