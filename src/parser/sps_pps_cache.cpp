#include "sps_pps_cache.h"

#include <pthread.h>

#include "log.h"

static const char *TAG = "SPS_PPS_CACHE";

// Guards both g_sps and g_pps together (not one lock per vector) -
// simpler, and contention is a non-issue here: writes only happen
// once per SPS/PPS NAL (a handful of times a second at most), reads
// happen once per DESCRIBE/offer, never in a hot per-packet path.
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

// assign() (not append) - each new SPS NAL fully replaces whatever
// was cached before, since only the MOST RECENT SPS/PPS matters (see
// sps_pps_cache.h's header comment on why a copy, not a pointer, is
// kept here).
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

// Returns a COPY (std::vector's copy constructor, under the lock) -
// not a reference or pointer into g_sps, since the writer thread could
// otherwise overwrite it out from under the caller the instant the
// lock is released.
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
