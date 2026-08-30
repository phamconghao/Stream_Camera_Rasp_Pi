#include "webrtc_session_stats.h"

#include <pthread.h>
#include <map>

#include "time_utils.h"
#include "log.h"

static const char *TAG = "WEBRTC_SESSION_STATS";

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, webrtc_session_stats_snapshot_t> g_sessions;

void webrtc_session_stats_on_connect(const std::string &ice_ufrag)
{
    webrtc_session_stats_snapshot_t stats;
    stats.ice_ufrag = ice_ufrag;
    stats.connected_at_us = time_utils_now_us();

    pthread_mutex_lock(&g_lock);
    g_sessions[ice_ufrag] = stats;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO(TAG, "tracking stats for ufrag=%s", ice_ufrag.c_str());
}

void webrtc_session_stats_on_disconnect(const std::string &ice_ufrag)
{
    pthread_mutex_lock(&g_lock);
    g_sessions.erase(ice_ufrag);
    pthread_mutex_unlock(&g_lock);
}

void webrtc_session_stats_record_frame_sent(const std::string &ice_ufrag)
{
    pthread_mutex_lock(&g_lock);
    auto it = g_sessions.find(ice_ufrag);
    if (it != g_sessions.end())
    {
        it->second.frames_sent++;
    }
    pthread_mutex_unlock(&g_lock);
}

void webrtc_session_stats_record_rtcp_rr(
    const std::string &ice_ufrag, uint8_t fraction_lost,
    uint32_t cumulative_lost, uint32_t jitter_rtp_units)
{
    pthread_mutex_lock(&g_lock);
    auto it = g_sessions.find(ice_ufrag);
    if (it != g_sessions.end())
    {
        it->second.has_rtcp_rr = true;
        it->second.last_fraction_lost = fraction_lost;
        it->second.cumulative_lost = cumulative_lost;
        it->second.last_jitter_rtp_units = jitter_rtp_units;
        it->second.last_rr_at_us = time_utils_now_us();
    }
    pthread_mutex_unlock(&g_lock);
}

std::vector<webrtc_session_stats_snapshot_t> webrtc_session_stats_get_all(void)
{
    std::vector<webrtc_session_stats_snapshot_t> result;

    pthread_mutex_lock(&g_lock);
    result.reserve(g_sessions.size());
    for (const auto &pair : g_sessions)
    {
        result.push_back(pair.second);
    }
    pthread_mutex_unlock(&g_lock);

    return result;
}
