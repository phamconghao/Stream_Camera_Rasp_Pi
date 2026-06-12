#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "frame_pool.h"

typedef struct
{
    encoded_frame_t frames[FRAME_POOL_SIZE];
    uint8_t *buffers[FRAME_POOL_SIZE];
    int used[FRAME_POOL_SIZE];
    pthread_mutex_t lock;
} frame_pool_ctx_t;

static frame_pool_ctx_t g_pool;

int frame_pool_init(void)
{
    pthread_mutex_init(&g_pool.lock, NULL);

    for (int i = 0; i < FRAME_POOL_SIZE; i++)
    {
        g_pool.buffers[i] = (uint8_t *)malloc(MAX_ENCODED_FRAME_SIZE);

        if (!g_pool.buffers[i])
        {
            return -1;
        }

        g_pool.frames[i].data = g_pool.buffers[i];
    }

    return 0;
}

encoded_frame_t *frame_pool_acquire(void)
{
    pthread_mutex_lock(&g_pool.lock);

    for (int i = 0; i < FRAME_POOL_SIZE; i++)
    {
        if (!g_pool.used[i])
        {
            g_pool.used[i] = 1;
            g_pool.frames[i].ref_count.store(1);
            pthread_mutex_unlock(&g_pool.lock);
            return &g_pool.frames[i];
        }
    }

    pthread_mutex_unlock(&g_pool.lock);
    return NULL;
}

void frame_pool_release(encoded_frame_t *frame)
{
    if (!frame)
    {
        return;
    }

    if (frame->ref_count.fetch_sub(1) != 1)
    {
        return;
    }

    pthread_mutex_lock(&g_pool.lock);
    int index = frame - g_pool.frames;

    if (index >= 0 && index < FRAME_POOL_SIZE)
    {
        g_pool.used[index] = 0;
    }

    pthread_mutex_unlock(&g_pool.lock);
}

void frame_pool_cleanup(void)
{
    for (int i = 0; i < FRAME_POOL_SIZE; i++)
    {
        free(g_pool.buffers[i]);
    }

    pthread_mutex_destroy(&g_pool.lock);
}