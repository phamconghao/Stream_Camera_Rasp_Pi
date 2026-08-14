#include "sps_pps_cache.h"

#include <pthread.h>

#include "log.h"

static const char *TAG = "SPS_PPS_CACHE";

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<uint8_t> g_sps;
static std::vector<uint8_t> g_pps;

void sps_pps_cache_init(void)
{
    pthread_mutex_lock(&g_lock);
    g_sps.clear();
    g_pps.clear();
    pthread_mutex_unlock(&g_lock);
}

void sps_pps_cache_cleanup(void)
{
    pthread_mutex_lock(&g_lock);
    g_sps.clear();
    g_pps.clear();
    pthread_mutex_unlock(&g_lock);
}

void sps_pps_cache_set_sps(const uint8_t *data, size_t size)
{
    pthread_mutex_lock(&g_lock);
    g_sps.assign(data, data + size);
    pthread_mutex_unlock(&g_lock);

    LOG_INFO(TAG, "cached SPS (%zu bytes)", size);
}

void sps_pps_cache_set_pps(const uint8_t *data, size_t size)
{
    pthread_mutex_lock(&g_lock);
    g_pps.assign(data, data + size);
    pthread_mutex_unlock(&g_lock);

    LOG_INFO(TAG, "cached PPS (%zu bytes)", size);
}

std::vector<uint8_t> sps_pps_cache_get_sps(void)
{
    pthread_mutex_lock(&g_lock);
    std::vector<uint8_t> copy = g_sps;
    pthread_mutex_unlock(&g_lock);
    return copy;
}

std::vector<uint8_t> sps_pps_cache_get_pps(void)
{
    pthread_mutex_lock(&g_lock);
    std::vector<uint8_t> copy = g_pps;
    pthread_mutex_unlock(&g_lock);
    return copy;
}

bool sps_pps_cache_has_both(void)
{
    pthread_mutex_lock(&g_lock);
    bool both = !g_sps.empty() && !g_pps.empty();
    pthread_mutex_unlock(&g_lock);
    return both;
}
