#include <cstring>
#include <pthread.h>

#include "webrtc_rtp_packet_pool.h"

// Smaller than rtp_packet_pool's 512 - this path only exists while at
// least one WebRTC viewer is connected (webrtc_media_registry.h gates
// it), a scenario expected to be rare/occasional relative to the
// always-on RTSP path, so less buffering headroom is an acceptable
// tradeoff for using less memory in the common "no WebRTC viewers"
// case (this array is still allocated even when unused, since it's a
// static/global, not dynamically sized).
#define WEBRTC_RTP_PACKET_POOL_SIZE 128

typedef struct
{
    rtp_packet_t packets[WEBRTC_RTP_PACKET_POOL_SIZE];
    bool used[WEBRTC_RTP_PACKET_POOL_SIZE];
    pthread_mutex_t lock;
} webrtc_rtp_packet_pool_ctx_t;

static webrtc_rtp_packet_pool_ctx_t g_pool;

int webrtc_rtp_packet_pool_init(void)
{
    pthread_mutex_init(&g_pool.lock, nullptr);

    for (int i = 0; i < WEBRTC_RTP_PACKET_POOL_SIZE; i++)
    {
        std::memset(&g_pool.packets[i], 0, sizeof(g_pool.packets[i]));
        g_pool.used[i] = false;
    }

    return 0;
}

void webrtc_rtp_packet_pool_cleanup(void)
{
    pthread_mutex_destroy(&g_pool.lock);
}

rtp_packet_t *webrtc_rtp_packet_pool_acquire(void)
{
    pthread_mutex_lock(&g_pool.lock);

    for (int i = 0; i < WEBRTC_RTP_PACKET_POOL_SIZE; i++)
    {
        if (!g_pool.used[i])
        {
            g_pool.used[i] = true;
            std::memset(&g_pool.packets[i], 0, sizeof(g_pool.packets[i]));
            pthread_mutex_unlock(&g_pool.lock);
            return &g_pool.packets[i];
        }
    }

    pthread_mutex_unlock(&g_pool.lock);
    return nullptr;
}

void webrtc_rtp_packet_pool_release(rtp_packet_t *packet)
{
    if (!packet)
    {
        return;
    }

    pthread_mutex_lock(&g_pool.lock);

    for (int i = 0; i < WEBRTC_RTP_PACKET_POOL_SIZE; i++)
    {
        if (&g_pool.packets[i] == packet)
        {
            g_pool.used[i] = false;
            break;
        }
    }

    pthread_mutex_unlock(&g_pool.lock);
}
