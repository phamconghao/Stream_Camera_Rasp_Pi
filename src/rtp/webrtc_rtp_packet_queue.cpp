#include <pthread.h>

#include "webrtc_rtp_packet_queue.h"

#define WEBRTC_RTP_PACKET_QUEUE_SIZE 128 // matches webrtc_rtp_packet_pool's size - no point queuing more than the pool can ever hold acquired at once

typedef struct
{
    rtp_packet_t *packets[WEBRTC_RTP_PACKET_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    bool shutting_down;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} webrtc_rtp_packet_queue_ctx_t;

static webrtc_rtp_packet_queue_ctx_t g_queue;

int webrtc_rtp_packet_queue_init(void)
{
    g_queue.head = 0;
    g_queue.tail = 0;
    g_queue.count = 0;
    g_queue.shutting_down = false;

    pthread_mutex_init(&g_queue.lock, nullptr);
    pthread_cond_init(&g_queue.cond, nullptr);

    return 0;
}

void webrtc_rtp_packet_queue_cleanup(void)
{
    pthread_mutex_destroy(&g_queue.lock);
    pthread_cond_destroy(&g_queue.cond);
}

int webrtc_rtp_packet_queue_push(rtp_packet_t *packet)
{
    pthread_mutex_lock(&g_queue.lock);

    if (g_queue.count >= WEBRTC_RTP_PACKET_QUEUE_SIZE)
    {
        pthread_mutex_unlock(&g_queue.lock);
        return -1;
    }

    g_queue.packets[g_queue.tail] = packet;
    g_queue.tail = (g_queue.tail + 1) % WEBRTC_RTP_PACKET_QUEUE_SIZE;
    g_queue.count++;

    pthread_cond_signal(&g_queue.cond);
    pthread_mutex_unlock(&g_queue.lock);

    return 0;
}

rtp_packet_t *webrtc_rtp_packet_queue_pop(void)
{
    pthread_mutex_lock(&g_queue.lock);

    while (g_queue.count == 0 && !g_queue.shutting_down)
    {
        pthread_cond_wait(&g_queue.cond, &g_queue.lock);
    }

    if (g_queue.count == 0 && g_queue.shutting_down)
    {
        pthread_mutex_unlock(&g_queue.lock);
        return nullptr;
    }

    rtp_packet_t *packet = g_queue.packets[g_queue.head];
    g_queue.head = (g_queue.head + 1) % WEBRTC_RTP_PACKET_QUEUE_SIZE;
    g_queue.count--;

    pthread_mutex_unlock(&g_queue.lock);

    return packet;
}

void webrtc_rtp_packet_queue_shutdown(void)
{
    pthread_mutex_lock(&g_queue.lock);
    g_queue.shutting_down = true;
    pthread_cond_broadcast(&g_queue.cond);
    pthread_mutex_unlock(&g_queue.lock);
}
