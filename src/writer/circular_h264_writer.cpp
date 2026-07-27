#include "circular_h264_writer.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#include "time_utils.h"
#include "log.h"

static const char *TAG = "CIRC_WRITER";

static std::string g_dir;
static int g_segment_duration_sec = 60;
static int g_max_segments = 10;
static FILE *g_current_file = nullptr;
static uint64_t g_segment_start_us = 0;
static std::deque<std::string> g_segment_paths; // oldest first, so front() = next to delete

static std::string make_segment_path()
{
    time_t now = time(nullptr);
    char buf[512];
    snprintf(buf, sizeof(buf), "%s/segment_%ld.h264", g_dir.c_str(), static_cast<long>(now));
    return std::string(buf);
}

// Closes the current segment (if any), opens a fresh one, and deletes
// the oldest segment(s) if we're now over the retention limit.
static void rotate_segment(void)
{
    if (g_current_file)
    {
        fclose(g_current_file);
        g_current_file = nullptr;
    }

    std::string path = make_segment_path();
    g_current_file = fopen(path.c_str(), "wb");
    if (!g_current_file)
    {
        LOG_WARN(TAG, "failed to open segment file %s", path.c_str());
        return;
    }

    g_segment_paths.push_back(path);
    g_segment_start_us = time_utils_now_us();

    LOG_INFO(TAG, "started new segment: %s", path.c_str());

    while (static_cast<int>(g_segment_paths.size()) > g_max_segments)
    {
        const std::string &oldest = g_segment_paths.front();

        if (remove(oldest.c_str()) == 0)
        {
            LOG_INFO(TAG, "deleted old segment: %s", oldest.c_str());
        }
        else
        {
            // Not fatal - e.g. the user already deleted it by hand.
            // Still drop it from our tracking so we don't retry forever.
            LOG_WARN(TAG, "failed to delete old segment: %s", oldest.c_str());
        }

        g_segment_paths.pop_front();
    }
}

int circular_h264_writer_init(const char *dir, int segment_duration_sec, int max_segments)
{
    g_dir = dir;
    g_segment_duration_sec = segment_duration_sec;
    g_max_segments = max_segments;
    g_segment_paths.clear();

    // Create the directory if it doesn't exist yet; ignore "already
    // exists" (errno == EEXIST) - anything else means we likely can't
    // write there anyway, which rotate_segment()'s fopen() below will
    // then report.
    mkdir(dir, 0755);

    rotate_segment();

    return g_current_file ? 0 : -1;
}

void circular_h264_writer_write(const uint8_t *data, size_t size)
{
    if (!g_current_file || size == 0)
    {
        return;
    }

    uint64_t elapsed_us = time_utils_now_us() - g_segment_start_us;
    if (elapsed_us >= static_cast<uint64_t>(g_segment_duration_sec) * 1000000ULL)
    {
        rotate_segment();
        if (!g_current_file)
        {
            return;
        }
    }

    fwrite(data, 1, size, g_current_file);
    fflush(g_current_file);
}

void circular_h264_writer_cleanup(void)
{
    if (g_current_file)
    {
        fclose(g_current_file);
        g_current_file = nullptr;
    }
}
