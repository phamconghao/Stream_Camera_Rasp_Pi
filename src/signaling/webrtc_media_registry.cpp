#include "webrtc_media_registry.h"

#include <pthread.h>
#include <set>

#include "log.h"

static const char *TAG = "WEBRTC_MEDIA_REG";

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::set<std::string> g_ready_ufrags;

void webrtc_media_registry_add(const std::string &ice_ufrag)
{
    pthread_mutex_lock(&g_lock);
    g_ready_ufrags.insert(ice_ufrag);
    size_t count = g_ready_ufrags.size();
    pthread_mutex_unlock(&g_lock);

    LOG_INFO(TAG, "session ufrag=%s ready for media (%zu active WebRTC viewer(s))", ice_ufrag.c_str(), count);
}

void webrtc_media_registry_remove(const std::string &ice_ufrag)
{
    pthread_mutex_lock(&g_lock);
    size_t removed = g_ready_ufrags.erase(ice_ufrag);
    size_t count = g_ready_ufrags.size();
    pthread_mutex_unlock(&g_lock);

    if (removed > 0)
    {
        LOG_INFO(TAG, "session ufrag=%s removed (%zu active WebRTC viewer(s) left)", ice_ufrag.c_str(), count);
    }
}

bool webrtc_media_registry_has_any(void)
{
    pthread_mutex_lock(&g_lock);
    bool any = !g_ready_ufrags.empty();
    pthread_mutex_unlock(&g_lock);
    return any;
}

std::vector<std::string> webrtc_media_registry_get_all(void)
{
    pthread_mutex_lock(&g_lock);
    std::vector<std::string> result(g_ready_ufrags.begin(), g_ready_ufrags.end());
    pthread_mutex_unlock(&g_lock);
    return result;
}
