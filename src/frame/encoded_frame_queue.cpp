#include <pthread.h>

#include "encoded_frame_queue.h"

/**
 * Same bounded producer/consumer ring buffer pattern as raw_frame_queue.cpp
 * (see that file for the detailed shutdown-mechanism rationale). Producer
 * is encoder_thread, consumer is rtp_packetizer_thread.
 */
typedef struct
{
    encoded_frame_t *frames[ENCODED_FRAME_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    bool shutting_down;      // set by encoded_frame_queue_shutdown(); wakes any blocked pop()
    pthread_mutex_t lock;
    pthread_cond_t cond;
} encoded_frame_queue_ctx_t;

static encoded_frame_queue_ctx_t g_queue;

int encoded_frame_queue_init(void)
{
    g_queue.head = 0;
    g_queue.tail = 0;
    g_queue.count = 0;
    g_queue.shutting_down = false;

    pthread_mutex_init(&g_queue.lock, nullptr);
    pthread_cond_init(&g_queue.cond, nullptr);
    return 0;
}

void encoded_frame_queue_cleanup(void)
{
    pthread_mutex_destroy(&g_queue.lock);
    pthread_cond_destroy(&g_queue.cond);
}

int encoded_frame_queue_push(encoded_frame_t *frame)
{
    pthread_mutex_lock(&g_queue.lock);

    if (g_queue.count >= ENCODED_FRAME_QUEUE_SIZE)
    {
        pthread_mutex_unlock(&g_queue.lock);
        return -1; // Queue is full
    }

    g_queue.frames[g_queue.tail] = frame;
    g_queue.tail = (g_queue.tail + 1) % ENCODED_FRAME_QUEUE_SIZE;
    g_queue.count++;

    pthread_cond_signal(&g_queue.cond);
    pthread_mutex_unlock(&g_queue.lock);
    return 0;
}

// Called by rtp_packetizer_thread. Blocks until encoder_thread pushes a
// frame, or returns nullptr once encoded_frame_queue_shutdown() has been
// called and the queue has drained.
encoded_frame_t *encoded_frame_queue_pop(void)
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

    encoded_frame_t *frame = g_queue.frames[g_queue.head];
    g_queue.head = (g_queue.head + 1) % ENCODED_FRAME_QUEUE_SIZE;
    g_queue.count--;

    pthread_mutex_unlock(&g_queue.lock);
    return frame;
}

void encoded_frame_queue_shutdown(void)
{
    pthread_mutex_lock(&g_queue.lock);
    g_queue.shutting_down = true;
    pthread_cond_broadcast(&g_queue.cond);
    pthread_mutex_unlock(&g_queue.lock);
}

int encoded_frame_queue_count(void)
{
    pthread_mutex_lock(&g_queue.lock);
    int count = g_queue.count;
    pthread_mutex_unlock(&g_queue.lock);
    return count;
}