#include <pthread.h>

#include "rtp_packet_queue.h"

/**
 * Same bounded producer/consumer ring buffer pattern as raw_frame_queue.cpp
 * (see that file for the detailed shutdown-mechanism rationale).
 * Producer is rtp_packetizer_thread, consumer is udp_sender_thread.
 */
#define RTP_PACKET_QUEUE_SIZE 512

typedef struct
{
    rtp_packet_t *packets[RTP_PACKET_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    bool shutting_down;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} rtp_packet_queue_ctx_t;

static rtp_packet_queue_ctx_t g_queue;

int rtp_packet_queue_init(void)
{
    g_queue.head = 0;
    g_queue.tail = 0;
    g_queue.count = 0;
    g_queue.shutting_down = false;

    pthread_mutex_init(&g_queue.lock, nullptr);
    pthread_cond_init(&g_queue.cond, nullptr);

    return 0;
}

void rtp_packet_queue_cleanup(void)
{
    pthread_mutex_destroy(&g_queue.lock);
    pthread_cond_destroy(&g_queue.cond);
}

int rtp_packet_queue_push(rtp_packet_t *packet)
{
    pthread_mutex_lock(&g_queue.lock);

    if (g_queue.count >= RTP_PACKET_QUEUE_SIZE)
    {
        pthread_mutex_unlock(&g_queue.lock);
        return -1;
    }

    g_queue.packets[g_queue.tail] = packet;
    g_queue.tail = (g_queue.tail + 1) % RTP_PACKET_QUEUE_SIZE;
    g_queue.count++;

    pthread_cond_signal(&g_queue.cond);
    pthread_mutex_unlock(&g_queue.lock);

    return 0;
}

rtp_packet_t *rtp_packet_queue_pop(void)
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
    g_queue.head = (g_queue.head + 1) % RTP_PACKET_QUEUE_SIZE;
    g_queue.count--;

    pthread_mutex_unlock(&g_queue.lock);

    return packet;
}

void rtp_packet_queue_shutdown(void)
{
    pthread_mutex_lock(&g_queue.lock);
    g_queue.shutting_down = true;
    pthread_cond_broadcast(&g_queue.cond);
    pthread_mutex_unlock(&g_queue.lock);
}