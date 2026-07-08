#ifndef __LOG_H__
#define __LOG_H__

enum class log_level_t
{
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
};

/**
 * Thread-safe logger. Prints "HH:MM:SS.mmm [LEVEL] [tag] message" to
 * stderr. Prefer the LOG_INFO/LOG_WARN/LOG_ERROR macros below over
 * calling this directly.
 */
void log_msg(log_level_t level, const char *tag, const char *fmt, ...);

#define LOG_INFO(tag, fmt, ...)  log_msg(log_level_t::LOG_LEVEL_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...)  log_msg(log_level_t::LOG_LEVEL_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...) log_msg(log_level_t::LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)

#endif // __LOG_H__
