#include <pthread.h>
#include <cstdio>
#include <atomic>
#include <unistd.h>
#include <arpa/inet.h>

#include "rtcp_receiver_thread.h"
#include "rtcp_packet.h"
#include "rtcp_receiver_stats.h"
#include "control_channel.h"
#include "time_utils.h"
#include "log.h"

static const char *TAG = "RTCP_RCV";

static pthread_t g_thread;
static std::atomic<bool> g_running(false);
static uint32_t g_reporter_ssrc = 0;
static uint32_t g_video_ssrc = 0;

// Same fixed-interval simplification as rtcp_sender_thread.cpp (see
// that file's comment on RFC 3550 6.2's bandwidth-adaptive interval).
static constexpr int REPORT_INTERVAL_MS = 2000;
static constexpr int POLL_GRANULARITY_MS = 100;

static void *rtcp_receiver_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    int waited_ms = 0;

    while (g_running)
    {
        usleep(POLL_GRANULARITY_MS * 1000);
        waited_ms += POLL_GRANULARITY_MS;

        if (waited_ms < REPORT_INTERVAL_MS)
        {
            continue;
        }

        waited_ms = 0;

        rtcp_rr_t rr;
        rtcp_receiver_stats_build_rr(&rr, g_reporter_ssrc, g_video_ssrc, time_utils_now_us());

        if (control_channel_send_raw(reinterpret_cast<const uint8_t *>(&rr), sizeof(rr)) < 0)
        {
            LOG_WARN(TAG, "failed to send RR");
        }
        else
        {
            uint32_t frac_and_cum = ntohl(rr.block.fraction_lost_and_cumulative_be);
            uint8_t fraction = (frac_and_cum >> 24) & 0xFF;
            uint32_t cumulative = frac_and_cum & 0x00FFFFFF;
            uint32_t jitter = ntohl(rr.block.jitter_be);

            LOG_INFO(TAG, "sent RR: fraction_lost=%u/256 cumulative_lost=%u jitter=%u",
                     fraction, cumulative, jitter);
        }
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

int rtcp_receiver_thread_start(uint32_t reporter_ssrc, uint32_t video_ssrc)
{
    g_reporter_ssrc = reporter_ssrc;
    g_video_ssrc = video_ssrc;
    g_running = true;

    if (pthread_create(&g_thread, nullptr, rtcp_receiver_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        g_running = false;
        return -1;
    }

    return 0;
}

void rtcp_receiver_thread_stop(void)
{
    g_running = false;
    pthread_join(g_thread, nullptr);
}
