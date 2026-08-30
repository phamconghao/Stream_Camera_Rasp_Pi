#include "admin_session.h"

#include <pthread.h>
#include <map>

#include <openssl/rand.h>

#include "time_utils.h"
#include "log.h"

static const char *TAG = "ADMIN_SESSION";

static constexpr size_t SESSION_ID_RAW_BYTES = 32; // -> 64 hex chars
static constexpr uint64_t REMEMBER_ME_TTL_US = 30ULL * 24 * 60 * 60 * 1000000ULL; // 30 days
static constexpr uint64_t DEFAULT_TTL_US = 24ULL * 60 * 60 * 1000000ULL;          // 24 hours

struct admin_session_t
{
    std::string username;
    uint64_t expires_at_us;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, admin_session_t> g_sessions;

std::string admin_session_create(const std::string &username, bool remember_me)
{
    unsigned char raw[SESSION_ID_RAW_BYTES];
    if (RAND_bytes(raw, sizeof(raw)) != 1)
    {
        // Same reasoning as rtsp_server.cpp's generate_nonce_hex(): a
        // predictable/empty session id would let an attacker skip
        // guessing entirely, so fail loudly and hand out nothing
        // rather than something weak.
        LOG_ERROR(TAG, "RAND_bytes failed generating admin session id - refusing to log in");
        return "";
    }

    static const char *HEX = "0123456789abcdef";
    std::string id;
    id.reserve(sizeof(raw) * 2);
    for (unsigned char b : raw)
    {
        id += HEX[(b >> 4) & 0x0F];
        id += HEX[b & 0x0F];
    }

    uint64_t now = time_utils_now_us();
    uint64_t ttl = remember_me ? REMEMBER_ME_TTL_US : DEFAULT_TTL_US;

    pthread_mutex_lock(&g_lock);
    g_sessions[id] = admin_session_t{username, now + ttl};
    pthread_mutex_unlock(&g_lock);

    LOG_INFO(TAG, "session created for user=%s (remember_me=%d)", username.c_str(), remember_me ? 1 : 0);

    return id;
}

bool admin_session_is_valid(const std::string &session_id)
{
    if (session_id.empty())
    {
        return false;
    }

    uint64_t now = time_utils_now_us();

    pthread_mutex_lock(&g_lock);
    auto it = g_sessions.find(session_id);
    if (it == g_sessions.end())
    {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    if (it->second.expires_at_us <= now)
    {
        // Expired - erase it now rather than waiting for some future
        // cleanup pass, same "lazy eviction on next lookup" approach
        // control_listener_thread.cpp's auth_failure_log.cpp uses for
        // its own time-windowed state.
        g_sessions.erase(it);
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    pthread_mutex_unlock(&g_lock);
    return true;
}

void admin_session_destroy(const std::string &session_id)
{
    pthread_mutex_lock(&g_lock);
    g_sessions.erase(session_id);
    pthread_mutex_unlock(&g_lock);
}
