#include "log.h"
#include "time_utils.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

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

    return "?????";
}

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
