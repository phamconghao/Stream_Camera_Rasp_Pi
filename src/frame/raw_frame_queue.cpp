#include <pthread.h>

#include "raw_frame_queue.h"

typedef struct
{
    raw_frame_t *frames[RAW_FRAME_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} raw_frame_queue_ctx_t;

static raw_frame_queue_ctx_t g_queue;

int raw_frame_queue_init(void)
{
    g_queue.head = 0;
    g_queue.tail = 0;
    g_queue.count = 0;

    pthread_mutex_init(&g_queue.lock, nullptr);
    pthread_cond_init(&g_queue.cond, nullptr);
    return 0;
}

void raw_frame_queue_cleanup(void)
{
    pthread_mutex_destroy(&g_queue.lock);
    pthread_cond_destroy(&g_queue.cond);
}

int raw_frame_queue_push(raw_frame_t *frame)
{
    pthread_mutex_lock(&g_queue.lock);

    if (g_queue.count >= RAW_FRAME_QUEUE_SIZE)
    {
        pthread_mutex_unlock(&g_queue.lock);
        return -1; // Queue is full
    }

    g_queue.frames[g_queue.tail] = frame;
    g_queue.tail = (g_queue.tail + 1) % RAW_FRAME_QUEUE_SIZE;
    g_queue.count++;

    pthread_cond_signal(&g_queue.cond);
    pthread_mutex_unlock(&g_queue.lock);
    return 0;
}

raw_frame_t *raw_frame_queue_pop(void)
{
    pthread_mutex_lock(&g_queue.lock);

    while (g_queue.count == 0)
    {
        pthread_cond_wait(&g_queue.cond, &g_queue.lock);
    }

    raw_frame_t *frame = g_queue.frames[g_queue.head];
    g_queue.head = (g_queue.head + 1) % RAW_FRAME_QUEUE_SIZE;
    g_queue.count--;

    pthread_mutex_unlock(&g_queue.lock);
    return frame;
}

int raw_frame_queue_count(void)
{
    pthread_mutex_lock(&g_queue.lock);
    int count = g_queue.count;
    pthread_mutex_unlock(&g_queue.lock);
    return count;
}