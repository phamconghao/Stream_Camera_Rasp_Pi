#include "log.h"
#include "time_utils.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

/**
 * Minimal printf-style logger, output goes to stderr (see the
 * LOG_INFO/LOG_WARN/LOG_ERROR macros in log.h, which pass the calling
 * file's TAG through automatically). No log file, no log rotation, no
 * severity filtering - every level always prints. Global mutex keeps
 * a single log line from different threads (every module in this
 * project logs from its own thread) from interleaving mid-line.
 */

static std::mutex g_log_mutex;

static const char *level_str(log_level_t level)
{
    switch (level)
    {
        case log_level_t::LOG_LEVEL_INFO:
            return "INFO ";

        case log_level_t::LOG_LEVEL_WARN:
            return "WARN ";

        case log_level_t::LOG_LEVEL_ERROR:
            return "ERROR";
    }

    return "?????"; // unreachable for any currently-defined log_level_t, kept for defensiveness against a future enum value
}

// Format: "HH:MM:SS.mmm [LEVEL] [TAG] message\n" - the mutex covers
// the entire call (timestamp read through the trailing newline) so
// one thread's full log line is guaranteed to print as a contiguous
// block, never spliced with another thread's line.
void log_msg(log_level_t level, const char *tag, const char *fmt, ...)
{
    char time_buf[16];
    time_utils_format_now(time_buf, sizeof(time_buf));

    std::lock_guard<std::mutex> lock(g_log_mutex);

    std::fprintf(stderr, "%s [%s] [%s] ", time_buf, level_str(level), tag);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);

    std::fprintf(stderr, "\n");
}
