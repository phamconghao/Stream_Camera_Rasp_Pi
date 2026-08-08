#include "rtsp_session_registry.h"

#include <pthread.h>
#include <map>
#include <cstdio>
#include <atomic>

#include "time_utils.h"
#include "log.h"

static const char *TAG = "RTSP_SESS";

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, rtsp_session_t> g_sessions;
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

    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "%08X", g_next_session_number.fetch_add(1));
    std::string session_id(id_buf);

    rtsp_session_t session;
    session.session_id = session_id;
    session.state = rtsp_session_state_t::READY;
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

std::vector<rtsp_session_t> rtsp_session_registry_get_playing(void)
{
    std::vector<rtsp_session_t> result;

    pthread_mutex_lock(&g_lock);

    for (const auto &pair : g_sessions)
    {
        if (pair.second.state == rtsp_session_state_t::PLAYING)
        {
            result.push_back(pair.second);
        }
    }

    pthread_mutex_unlock(&g_lock);

    return result;
}

int rtsp_session_registry_reap_orphans(void)
{
    uint64_t now = time_utils_now_us();
    std::vector<std::string> to_remove;

    pthread_mutex_lock(&g_lock);

    for (const auto &pair : g_sessions)
    {
        uint64_t age = now - pair.second.last_activity_us;
        if (age > RTSP_SESSION_TIMEOUT_US)
        {
            to_remove.push_back(pair.first);
        }
    }

    for (const auto &id : to_remove)
    {
        g_sessions.erase(id);
    }

    int remaining = static_cast<int>(g_sessions.size());

    pthread_mutex_unlock(&g_lock);

    for (const auto &id : to_remove)
    {
        LOG_WARN(TAG, "reaped orphaned session %s (no activity for >%llus)",
                 id.c_str(), static_cast<unsigned long long>(RTSP_SESSION_TIMEOUT_US / 1000000));
    }

    if (!to_remove.empty())
    {
        LOG_INFO(TAG, "%d/%d slots used after reaping", remaining, RTSP_MAX_SESSIONS);
    }

    return static_cast<int>(to_remove.size());
}
