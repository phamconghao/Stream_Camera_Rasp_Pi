#include <pthread.h>
#include <cstdio>
#include <atomic>
#include <arpa/inet.h>

#include "control_listener_thread.h"
#include "control_protocol.h"
#include "udp_receiver.h"
#include "bcm2835_encoder.h"
#include "time_utils.h"
#include "log.h"

static const char *TAG = "CTRL_LISTEN";

static pthread_t g_thread;
static std::atomic<bool> g_running(false);
static std::atomic<uint32_t> g_keyframe_requests_total(0);

// Idea #3 (monitoring dashboard): where the sender's live stats get
// written every time a control message is handled, for dashboard.html
// to poll via fetch(). See loss_reporter_thread.cpp for the matching
// receiver-side file.
static constexpr const char *STATS_PATH = "sender_stats.json";

/**
 * Bitrate tiers for the adaptive-bitrate extension. Deliberately coarse
 * (3 steps) and hysteresis-free simplicity over a smooth/continuous
 * curve: a real congestion-control algorithm (e.g. something GCC-like)
 * is a whole project on its own, and isn't what Phase 18/this idea is
 * about - the goal here is "visibly react to bad network conditions
 * within a couple seconds", not optimal rate-distortion tuning.
 */
static constexpr uint32_t BITRATE_HIGH_BPS = 2000000; // loss < 1%
static constexpr uint32_t BITRATE_MEDIUM_BPS = 1000000; // 1% <= loss < 5%
static constexpr uint32_t BITRATE_LOW_BPS = 500000; // loss >= 5%

static constexpr uint32_t LOSS_PERMILLE_MEDIUM_THRESHOLD = 10; // 1.0%
static constexpr uint32_t LOSS_PERMILLE_LOW_THRESHOLD = 50;    // 5.0%

enum class bitrate_tier_t
{
    UNKNOWN, // haven't applied any tier yet - first report always applies
    HIGH,
    MEDIUM,
    LOW,
};

static bitrate_tier_t g_current_tier = bitrate_tier_t::UNKNOWN;

static bitrate_tier_t tier_for_loss(uint32_t loss_permille)
{
    if (loss_permille >= LOSS_PERMILLE_LOW_THRESHOLD)
    {
        return bitrate_tier_t::LOW;
    }

    if (loss_permille >= LOSS_PERMILLE_MEDIUM_THRESHOLD)
    {
        return bitrate_tier_t::MEDIUM;
    }

    return bitrate_tier_t::HIGH;
}

static uint32_t bps_for_tier(bitrate_tier_t tier)
{
    switch (tier)
    {
        case bitrate_tier_t::LOW:
            return BITRATE_LOW_BPS;
        case bitrate_tier_t::MEDIUM:
            return BITRATE_MEDIUM_BPS;
        case bitrate_tier_t::HIGH:
        case bitrate_tier_t::UNKNOWN:
        default:
            return BITRATE_HIGH_BPS;
    }
}

static const char *tier_name(bitrate_tier_t tier)
{
    switch (tier)
    {
        case bitrate_tier_t::LOW:
            return "LOW";
        case bitrate_tier_t::MEDIUM:
            return "MEDIUM";
        case bitrate_tier_t::HIGH:
            return "HIGH";
        default:
            return "UNKNOWN";
    }
}

static void write_stats_json(void)
{
    FILE *f = fopen(STATS_PATH, "w");
    if (!f)
    {
        return;
    }

    fprintf(f,
            "{\n"
            "  \"role\": \"sender\",\n"
            "  \"bitrate_bps\": %u,\n"
            "  \"bitrate_tier\": \"%s\",\n"
            "  \"keyframe_requests_total\": %u,\n"
            "  \"updated_at_us\": %llu\n"
            "}\n",
            bps_for_tier(g_current_tier), tier_name(g_current_tier),
            g_keyframe_requests_total.load(),
            static_cast<unsigned long long>(time_utils_now_us()));

    fclose(f);
}

static void handle_loss_report(const control_loss_report_t *report)
{
    uint32_t loss_permille = ntohl(report->loss_permille_be);
    bitrate_tier_t new_tier = tier_for_loss(loss_permille);

    LOG_INFO(TAG, "loss report: %u.%u%% -> tier %s",
             loss_permille / 10, loss_permille % 10, tier_name(new_tier));

    if (new_tier == g_current_tier)
    {
        // Already at the right tier - skip the ioctl rather than
        // reapply the same bitrate on every single report (these come
        // in periodically, most of the time nothing needs to change).
        return;
    }

    uint32_t bps = bps_for_tier(new_tier);

    if (bcm2835_encoder_set_bitrate(bps) == 0)
    {
        LOG_INFO(TAG, "bitrate changed: %s -> %s (%u bps)",
                 tier_name(g_current_tier), tier_name(new_tier), bps);
        g_current_tier = new_tier;
    }
    else
    {
        LOG_WARN(TAG, "failed to apply bitrate tier %s", tier_name(new_tier));
    }

    write_stats_json();
}

static void *control_listener_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    uint8_t scratch[16]; // largest message today (control_loss_report_t) is 5 bytes; generous headroom

    while (g_running)
    {
        int n = udp_receiver_recv(scratch, sizeof(scratch));
        if (n <= 0)
        {
            // n < 0: transient recv error, or (during shutdown) the
            // socket was shut down/closed out from under us by
            // control_listener_thread_stop(). n == 0: a shutdown() call
            // (see udp_receiver_cleanup()) makes a pending recvfrom()
            // return 0 rather than an error - treat the same way.
            // Either case: re-check the running flag and exit cleanly
            // if it's now false.
            continue;
        }

        if (n >= 1 && scratch[0] == CONTROL_MSG_KEYFRAME_REQUEST)
        {
            LOG_INFO(TAG, "keyframe request received - forcing IDR on next frame");
            bcm2835_encoder_force_keyframe();
            g_keyframe_requests_total++;
            write_stats_json();
        }
        else if (n == static_cast<int>(sizeof(control_loss_report_t)) &&
                 scratch[0] == CONTROL_MSG_LOSS_REPORT)
        {
            handle_loss_report(reinterpret_cast<const control_loss_report_t *>(scratch));
        }
        else
        {
            // Ignore anything else (stray traffic, port scans, a
            // truncated/malformed loss report) rather than acting on
            // unrecognized input.
            LOG_WARN(TAG, "ignoring unrecognized control datagram (%d bytes)", n);
        }
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

int control_listener_thread_start(uint16_t control_port)
{
    if (udp_receiver_init(control_port) < 0)
    {
        return -1;
    }

    g_current_tier = bitrate_tier_t::UNKNOWN;
    g_running = true;

    if (pthread_create(&g_thread, nullptr, control_listener_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        udp_receiver_cleanup();
        g_running = false;
        return -1;
    }

    return 0;
}

void control_listener_thread_stop(void)
{
    g_running = false;

    // Closing (after shutdown()) the socket is what actually unblocks a
    // thread parked in recvfrom() - same pattern as udp_receiver_thread_stop().
    udp_receiver_cleanup();

    pthread_join(g_thread, nullptr);
}
