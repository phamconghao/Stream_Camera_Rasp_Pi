#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "raw_frame_pool.h"

typedef struct
{
    raw_frame_t frames[RAW_FRAME_POOL_SIZE];
    uint8_t *buffers[RAW_FRAME_POOL_SIZE];
    int used[RAW_FRAME_POOL_SIZE];
    pthread_mutex_t lock;
} raw_frame_pool_ctx_t;

static raw_frame_pool_ctx_t g_pool;

int raw_frame_pool_init(void)
{
    pthread_mutex_init(&g_pool.lock, NULL);

    for (int i = 0; i < RAW_FRAME_POOL_SIZE; i++)
    {
        g_pool.buffers[i] = (uint8_t *)malloc(MAX_RAW_FRAME_SIZE);
        if (!g_pool.buffers[i])
        {
            return -1;
        }

        g_pool.frames[i].data = g_pool.buffers[i];
        g_pool.frames[i].size = 0;
        g_pool.frames[i].pts_us = 0;
        g_pool.frames[i].sequence = 0;
        g_pool.frames[i].ref_count = 0;
        g_pool.used[i] = 0;
    }

    return 0;
}

void raw_frame_pool_cleanup(void)
{
    for (int i = 0; i < RAW_FRAME_POOL_SIZE; i++)
    {
        free(g_pool.buffers[i]);
        g_pool.buffers[i] = NULL;
    }

    pthread_mutex_destroy(&g_pool.lock);
}

raw_frame_t *raw_frame_pool_acquire(void)
{
    pthread_mutex_lock(&g_pool.lock);

    for (int i = 0; i < RAW_FRAME_POOL_SIZE; i++)
    {
        if (!g_pool.used[i])
        {
            g_pool.used[i] = 1;
            g_pool.frames[i].ref_count = 1;
            pthread_mutex_unlock(&g_pool.lock);
            return &g_pool.frames[i];
        }
    }

    pthread_mutex_unlock(&g_pool.lock);

    return NULL;
}

void raw_frame_pool_release(raw_frame_t *frame)
{
    if (!frame)
    {
        return;
    }

    int ref = --frame->ref_count;
    if (ref > 0)
    {
        return;
    }

    pthread_mutex_lock(&g_pool.lock);

    for (int i = 0; i < RAW_FRAME_POOL_SIZE; i++)
    {
        if (&g_pool.frames[i] == frame)
        {
            g_pool.used[i] = 0;
            break;
        }
    }

    pthread_mutex_unlock(&g_pool.lock);
}

void raw_frame_pool_retain(raw_frame_t *frame)
{
    if (!frame)
    {
        return;
    }

    ++frame->ref_count;
}