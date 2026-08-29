#include "auth_failure_log.h"

#include <pthread.h>
#include <map>

#include "time_utils.h"
#include "log.h"

static const char *TAG = "AUTH_FAIL";

// Past this many failures from the same (source, client_ip) pair
// within AUTH_FAILURE_BLOCK_WINDOW_US, that pair is blocked for
// AUTH_FAILURE_BLOCK_DURATION_US. Deliberately generous (10 failures,
// not e.g. 3) - a real client legitimately retrying a mistyped RTSP
// password or a stale/expired signaling token a couple of times
// shouldn't get itself blocked; this is meant to catch sustained
// guessing, not the occasional typo.
static constexpr int AUTH_FAILURE_BLOCK_THRESHOLD = 10;
static constexpr uint64_t AUTH_FAILURE_BLOCK_WINDOW_US = 30ULL * 1000000ULL;   // 30s
static constexpr uint64_t AUTH_FAILURE_BLOCK_DURATION_US = 60ULL * 1000000ULL; // 60s

struct failure_state_t
{
    int count = 0;
    uint64_t window_start_us = 0;
    uint64_t blocked_until_us = 0;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, failure_state_t> g_state; // key: "<source>|<client_ip>"

static std::string make_key(const char *source, const std::string &client_ip)
{
    return std::string(source) + "|" + client_ip;
}

void auth_failure_log(const char *source, const std::string &client_ip, const char *reason)
{
    LOG_WARN(TAG, "source=%s client=%s reason=\"%s\"",
             source, client_ip.empty() ? "-" : client_ip.c_str(), reason);

    if (client_ip.empty())
    {
        return; // nothing to key a per-client block on - see header comment
    }

    uint64_t now = time_utils_now_us();
    std::string key = make_key(source, client_ip);
    bool just_blocked = false;
    uint64_t block_duration_for_log = 0;

    pthread_mutex_lock(&g_lock);

    failure_state_t &state = g_state[key];
    if (state.window_start_us == 0 || now - state.window_start_us > AUTH_FAILURE_BLOCK_WINDOW_US)
    {
        state.window_start_us = now;
        state.count = 0;
    }
    state.count++;

    if (state.count >= AUTH_FAILURE_BLOCK_THRESHOLD)
    {
        state.blocked_until_us = now + AUTH_FAILURE_BLOCK_DURATION_US;
        // Reset so the block, once it expires, requires a fresh full
        // threshold of failures to trigger again - not just one more.
        state.count = 0;
        state.window_start_us = 0;
        just_blocked = true;
        block_duration_for_log = AUTH_FAILURE_BLOCK_DURATION_US;
    }

    pthread_mutex_unlock(&g_lock);

    if (just_blocked)
    {
        LOG_WARN(TAG, "source=%s client=%s temporarily blocked for %llus after %d consecutive failures",
                 source, client_ip.c_str(),
                 static_cast<unsigned long long>(block_duration_for_log / 1000000),
                 AUTH_FAILURE_BLOCK_THRESHOLD);
    }
}

bool auth_failure_is_blocked(const char *source, const std::string &client_ip)
{
    if (client_ip.empty())
    {
        return false;
    }

    std::string key = make_key(source, client_ip);
    uint64_t now = time_utils_now_us();

    pthread_mutex_lock(&g_lock);
    auto it = g_state.find(key);
    bool blocked = (it != g_state.end()) && (now < it->second.blocked_until_us);
    pthread_mutex_unlock(&g_lock);

    return blocked;
}
