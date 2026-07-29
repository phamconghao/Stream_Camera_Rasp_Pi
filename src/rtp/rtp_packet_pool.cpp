#include <cstring>
#include <pthread.h>

#include "rtp_packet_pool.h"

/**
 * 512 slots is generous: at 30fps with typically 1-3 RTP packets per
 * frame (one per NAL, mostly just the slice), that's several seconds of
 * buffering headroom if udp_sender_thread stalls momentarily, without
 * needing a huge amount of memory (each rtp_packet_t is ~1.4KB, so
 * 512 * ~1.4KB =~ 700KB total - trivial on a Pi 4).
 */
#define RTP_PACKET_POOL_SIZE 512

typedef struct
{
    rtp_packet_t packets[RTP_PACKET_POOL_SIZE];
    bool used[RTP_PACKET_POOL_SIZE];
    pthread_mutex_t lock;
} rtp_packet_pool_ctx_t;

static rtp_packet_pool_ctx_t g_pool;

int rtp_packet_pool_init()
{
    pthread_mutex_init(&g_pool.lock, nullptr);

    for (int i = 0; i < RTP_PACKET_POOL_SIZE; i++)
    {
        std::memset(&g_pool.packets[i], 0, sizeof(g_pool.packets[i]));
        g_pool.used[i] = false;
    }

    return 0;
}

void rtp_packet_pool_cleanup()
{
    pthread_mutex_destroy(&g_pool.lock);
}

rtp_packet_t *rtp_packet_pool_acquire()
{
    pthread_mutex_lock(&g_pool.lock);

    for (int i = 0; i < RTP_PACKET_POOL_SIZE; i++)
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

void rtp_packet_pool_release(rtp_packet_t *packet)
{
    if (!packet)
    {
        return;
    }

    pthread_mutex_lock(&g_pool.lock);

    for (int i = 0; i < RTP_PACKET_POOL_SIZE; i++)
    {
        if (&g_pool.packets[i] == packet)
        {
            g_pool.used[i] = false;
            break;
        }
    }

    pthread_mutex_unlock(&g_pool.lock);
}
