#include <pthread.h>
#include <cstdio>
#include <atomic>
#include <unistd.h>

#include "loss_reporter_thread.h"
#include "rtp_jitter_buffer.h"
#include "rtp_depacketizer_thread.h"
#include "control_channel.h"
#include "time_utils.h"
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

// Idea #3 (monitoring dashboard): where the receiver's live stats get
// written every REPORT_INTERVAL_MS, for dashboard.html to poll via
// fetch(). Plain JSON written by hand (snprintf) rather than pulling
// in a JSON library - the schema is small and fixed, not worth the
// dependency.
static constexpr const char *STATS_PATH = "receiver_stats.json";

static void write_stats_json(uint32_t loss_permille, double fps)
{
    FILE *f = fopen(STATS_PATH, "w");
    if (!f)
    {
        return;
    }

    fprintf(f,
            "{\n"
            "  \"role\": \"receiver\",\n"
            "  \"loss_permille\": %u,\n"
            "  \"fps\": %.1f,\n"
            "  \"decoded_frames_total\": %u,\n"
            "  \"updated_at_us\": %llu\n"
            "}\n",
            loss_permille, fps,
            rtp_depacketizer_thread_get_completed_count(),
            static_cast<unsigned long long>(time_utils_now_us()));

    fclose(f);
}

static void *loss_reporter_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    uint32_t prev_received = 0;
    uint32_t prev_lost = 0;
    rtp_jitter_buffer_get_stats(&prev_received, &prev_lost);

    uint32_t prev_completed = rtp_depacketizer_thread_get_completed_count();

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

        uint32_t completed = rtp_depacketizer_thread_get_completed_count();
        uint32_t completed_delta = completed - prev_completed;
        prev_completed = completed;
        double fps = completed_delta / (REPORT_INTERVAL_MS / 1000.0);

        uint32_t total_delta = received_delta + lost_delta;
        if (total_delta == 0)
        {
            // Nothing arrived this interval at all (stream paused, or
            // camera not running yet) - nothing meaningful to report,
            // but still refresh stats.json so the dashboard shows 0
            // fps/loss rather than going stale.
            write_stats_json(0, 0.0);
            continue;
        }

        uint32_t loss_permille = (lost_delta * 1000) / total_delta;

        LOG_INFO(TAG, "interval: received=%u lost=%u -> %u.%u%% loss, %.1f fps",
                 received_delta, lost_delta, loss_permille / 10, loss_permille % 10, fps);

        write_stats_json(loss_permille, fps);
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
