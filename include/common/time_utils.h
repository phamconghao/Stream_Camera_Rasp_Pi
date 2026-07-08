#ifndef __TIME_UTILS_H__
#define __TIME_UTILS_H__

#include <cstdint>
#include <cstddef>
#include <chrono>
#include <ctime>
#include <cstdio>

/**
 * Monotonic microsecond clock. Use this (not wall-clock time) for RTP
 * timestamps, PTS/DTS, and any frame timing/duration math, since it can
 * never jump backwards (unlike system time, which can be adjusted by NTP).
 */
inline uint64_t time_utils_now_us(void)
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

/**
 * Wall-clock time formatted as "HH:MM:SS.mmm", for human-readable log
 * lines. buf must be at least 13 bytes.
 */
inline void time_utils_format_now(char *buf, size_t buf_size)
{
    using namespace std::chrono;

    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::time_t t = system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);

    std::snprintf(buf, buf_size, "%02d:%02d:%02d.%03d",
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<int>(ms.count()));
}

#endif // __TIME_UTILS_H__
