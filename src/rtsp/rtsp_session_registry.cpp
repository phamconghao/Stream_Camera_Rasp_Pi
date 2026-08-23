#include "rtsp_session_registry.h"

#include <pthread.h>
#include <map>
#include <cstdio>
#include <atomic>

#include "time_utils.h"
#include "log.h"

/**
 * In-memory table of active RTSP sessions (RFC 2326's Session ID
 * concept), keyed by a server-generated session_id string. Backs
 * rtsp_server.cpp's SETUP/PLAY/TEARDOWN handlers: SETUP creates an
 * entry here, PLAY/TEARDOWN look one up by the Session header the
 * client sends on every subsequent request, and the reaper thread
 * (see rtsp_server.cpp's reaper_thread_func) periodically calls
 * rtsp_session_registry_reap_orphans() to drop sessions whose client
 * went silent without ever sending TEARDOWN (crashed, network drop).
 *
 * RTSP_MAX_SESSIONS caps concurrent sessions (SETUP is rejected with
 * 453 once full - see rtsp_server.cpp) - a hard-coded small number is
 * fine for a single Pi's single hardware encoder instance, not a
 * design that needs to scale.
 */

static const char *TAG = "RTSP_SESS";

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, rtsp_session_t> g_sessions;

// Monotonically increasing, never reused within one process's
// lifetime (unlike a plain map size or index would be) - so an old
// client retrying a stale/already-torn-down session_id can never
// collide with a brand new session that happens to reuse the same slot.
static std::atomic<uint32_t> g_next_session_number(1);

void rtsp_session_registry_init(void)
{
    pthread_mutex_lock(&g_lock);
    g_sessions.clear();
    pthread_mutex_unlock(&g_lock);

    g_next_session_number = 1;
}

void rtsp_session_registry_cleanup(void)
{
    pthread_mutex_lock(&g_lock);
    g_sessions.clear();
    pthread_mutex_unlock(&g_lock);
}

// Called from handle_setup() (rtsp_server.cpp) - returns "" (an
// otherwise-invalid session_id, since real ones are always 8 hex
// digits) if RTSP_MAX_SESSIONS is already reached, which the caller
// turns into a 453 Not Enough Bandwidth response.
std::string rtsp_session_registry_create(const std::string &client_ip,
                                          uint16_t client_rtp_port,
                                          uint16_t client_rtcp_port)
{
    pthread_mutex_lock(&g_lock);

    if (static_cast<int>(g_sessions.size()) >= RTSP_MAX_SESSIONS)
    {
        pthread_mutex_unlock(&g_lock);
        LOG_WARN(TAG, "SETUP rejected: %d/%d sessions already active", RTSP_MAX_SESSIONS, RTSP_MAX_SESSIONS);
        return "";
    }

    // 8 hex digits from a monotonic counter - human-readable in logs
    // (unlike a UUID) and, per the comment on g_next_session_number
    // above, still unique for the process's whole lifetime.
    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "%08X", g_next_session_number.fetch_add(1));
    std::string session_id(id_buf);

    rtsp_session_t session;
    session.session_id = session_id;
    session.state = rtsp_session_state_t::READY; // not PLAYING yet - that only happens once the client sends PLAY
    session.client_ip = client_ip;
    session.client_rtp_port = client_rtp_port;
    session.client_rtcp_port = client_rtcp_port;
    session.last_activity_us = time_utils_now_us();

    g_sessions[session_id] = session;

    pthread_mutex_unlock(&g_lock);

    LOG_INFO(TAG, "created session %s for %s:%u (rtcp %u) - %d/%d slots used",
             session_id.c_str(), client_ip.c_str(), client_rtp_port, client_rtcp_port,
             static_cast<int>(g_sessions.size()), RTSP_MAX_SESSIONS);

    return session_id;
}

// Also refreshes last_activity_us - a genuine RTSP method (PLAY,
// TEARDOWN) always counts as activity, so this doubles as this
// session's orphan-reaper keepalive (see rtsp_session_registry_reap_orphans()).
bool rtsp_session_registry_set_state(const std::string &session_id, rtsp_session_state_t state)
{
    pthread_mutex_lock(&g_lock);

    auto it = g_sessions.find(session_id);
    if (it == g_sessions.end())
    {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    it->second.state = state;
    it->second.last_activity_us = time_utils_now_us();

    pthread_mutex_unlock(&g_lock);
    return true;
}

bool rtsp_session_registry_get_state(const std::string &session_id, rtsp_session_state_t *out_state)
{
    pthread_mutex_lock(&g_lock);

    auto it = g_sessions.find(session_id);
    if (it == g_sessions.end())
    {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    *out_state = it->second.state;

    pthread_mutex_unlock(&g_lock);
    return true;
}

// Full-record lookup - used e.g. by handle_play() (rtsp_server.cpp)
// to get client_ip/client_rtp_port for registering this session's RTP
// destination with udp_sender's fan-out.
bool rtsp_session_registry_get(const std::string &session_id, rtsp_session_t *out_session)
{
    pthread_mutex_lock(&g_lock);

    auto it = g_sessions.find(session_id);
    if (it == g_sessions.end())
    {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    *out_session = it->second;

    pthread_mutex_unlock(&g_lock);
    return true;
}

// Refreshes last_activity_us WITHOUT changing state - used for RTSP
// methods that don't represent a state transition (e.g. OPTIONS on an
// existing session, or a client's keepalive-style request) but should
// still count as "this client is still alive" for the orphan reaper.
bool rtsp_session_registry_touch(const std::string &session_id)
{
    pthread_mutex_lock(&g_lock);

    auto it = g_sessions.find(session_id);
    if (it == g_sessions.end())
    {
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    it->second.last_activity_us = time_utils_now_us();

    pthread_mutex_unlock(&g_lock);
    return true;
}

bool rtsp_session_registry_remove(const std::string &session_id)
{
    pthread_mutex_lock(&g_lock);

    size_t removed = g_sessions.erase(session_id);
    int remaining = static_cast<int>(g_sessions.size());

    pthread_mutex_unlock(&g_lock);

    if (removed > 0)
    {
        LOG_INFO(TAG, "removed session %s - %d/%d slots used", session_id.c_str(), remaining, RTSP_MAX_SESSIONS);
    }

    return removed > 0;
}

bool rtsp_session_registry_exists(const std::string &session_id)
{
    pthread_mutex_lock(&g_lock);
    bool exists = g_sessions.find(session_id) != g_sessions.end();
    pthread_mutex_unlock(&g_lock);
    return exists;
}

int rtsp_session_registry_count(void)
{
    pthread_mutex_lock(&g_lock);
    int count = static_cast<int>(g_sessions.size());
    pthread_mutex_unlock(&g_lock);
    return count;
}

// Called periodically by rtsp_server.cpp's reaper thread
// (REAPER_INTERVAL_SEC). A session is "orphaned" once
// RTSP_SESSION_TIMEOUT_US has passed since its last genuine RTSP
// request (see rtsp_session_registry_set_state()/_touch() above) -
// this catches clients that crashed or lost network without ever
// sending TEARDOWN, which would otherwise leak a session slot (and,
// for a PLAYING session, keep the pipeline running and RTP flowing to
// a dead address) forever. Returns the FULL removed session records
// (not just their IDs) so the caller can act on state==PLAYING ones
// specifically (dropping their RTP destination, releasing the
// pipeline ref-count - see rtsp_server.cpp's reaper_thread_func).
std::vector<rtsp_session_t> rtsp_session_registry_reap_orphans(void)
{
    uint64_t now = time_utils_now_us();
    std::vector<rtsp_session_t> reaped;

    pthread_mutex_lock(&g_lock);

    for (auto it = g_sessions.begin(); it != g_sessions.end();)
    {
        uint64_t age = now - it->second.last_activity_us;
        if (age > RTSP_SESSION_TIMEOUT_US)
        {
            reaped.push_back(it->second);
            it = g_sessions.erase(it); // map::erase(iterator) returns the next valid iterator - safe to keep looping without a separate ++it
        }
        else
        {
            ++it;
        }
    }

    int remaining = static_cast<int>(g_sessions.size());

    pthread_mutex_unlock(&g_lock);

    // Logging done AFTER releasing g_lock - reaped sessions are
    // already out of the map at this point, so there's no reason to
    // hold the lock (and block other threads' lookups) just to print
    // log lines about them.
    for (const auto &session : reaped)
    {
        LOG_WARN(TAG, "reaped orphaned session %s (no activity for >%llus, was %s)",
                 session.session_id.c_str(),
                 static_cast<unsigned long long>(RTSP_SESSION_TIMEOUT_US / 1000000),
                 session.state == rtsp_session_state_t::PLAYING ? "PLAYING" : "not playing");
    }

    if (!reaped.empty())
    {
        LOG_INFO(TAG, "%d/%d slots used after reaping", remaining, RTSP_MAX_SESSIONS);
    }

    return reaped;
}
