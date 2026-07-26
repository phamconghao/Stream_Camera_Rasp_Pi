#include <pthread.h>
#include <cstdio>
#include <atomic>
#include <unistd.h>

#include "loss_reporter_thread.h"
#include "rtp_jitter_buffer.h"
#include "control_channel.h"
#include "log.h"

static const char *TAG = "LOSS_RPT";

static pthread_t g_thread;
static std::atomic<bool> g_running(false);

// How often to compute and send a loss report. Short enough to react
// to changing network conditions within a couple seconds, long enough
// that the control channel isn't chattier than the RTP stream itself.
static constexpr int REPORT_INTERVAL_MS = 2000;

// Polling granularity for checking g_running - keeps shutdown latency
// low (at most this long) without needing a dedicated condvar just to
// interrupt a timer sleep.
static constexpr int POLL_GRANULARITY_MS = 100;

static void *loss_reporter_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    uint32_t prev_received = 0;
    uint32_t prev_lost = 0;
    rtp_jitter_buffer_get_stats(&prev_received, &prev_lost);

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

        uint32_t received = 0;
        uint32_t lost = 0;
        rtp_jitter_buffer_get_stats(&received, &lost);

        uint32_t received_delta = received - prev_received;
        uint32_t lost_delta = lost - prev_lost;
        prev_received = received;
        prev_lost = lost;

        uint32_t total_delta = received_delta + lost_delta;
        if (total_delta == 0)
        {
            // Nothing arrived this interval at all (stream paused, or
            // camera not running yet) - nothing meaningful to report.
            continue;
        }

        uint32_t loss_permille = (lost_delta * 1000) / total_delta;

        LOG_INFO(TAG, "interval: received=%u lost=%u -> %u.%u%% loss",
                 received_delta, lost_delta, loss_permille / 10, loss_permille % 10);

        control_channel_report_loss(loss_permille);
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

int loss_reporter_thread_start(void)
{
    g_running = true;

    if (pthread_create(&g_thread, nullptr, loss_reporter_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        g_running = false;
        return -1;
    }

    return 0;
}

void loss_reporter_thread_stop(void)
{
    // No blocking I/O to wake up here (unlike the queue/socket-based
    // threads elsewhere) - the loop's own POLL_GRANULARITY_MS poll of
    // g_running is what bounds shutdown latency, so a plain join is
    // enough.
    g_running = false;
    pthread_join(g_thread, nullptr);
}
