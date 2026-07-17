#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "encoded_frame_pool.h"

/**
 * Same fixed-size pool pattern as raw_frame_pool.cpp - see that file
 * for the detailed rationale (avoids per-frame malloc/free, hands out
 * pointers by reference-counted ownership). This pool provides slots
 * for encoded_frame_t (post-encoder H.264 access units) instead of raw
 * YUV420 frames.
 *
 * One difference from raw_frame_pool_acquire(): here, acquire() also
 * resets size/pts_us/sequence to 0 before handing the slot out, so a
 * stale value from this slot's previous use can never leak through if
 * a caller forgets to set one of those fields.
 */
typedef struct
{
    encoded_frame_t frames[ENCODED_FRAME_POOL_SIZE];
    uint8_t *buffers[ENCODED_FRAME_POOL_SIZE];
    int used[ENCODED_FRAME_POOL_SIZE];
    pthread_mutex_t lock;
} encoded_frame_pool_ctx_t;

static encoded_frame_pool_ctx_t g_pool;

int encoded_frame_pool_init(void)
{
    pthread_mutex_init(&g_pool.lock, NULL);

    for (int i = 0; i < ENCODED_FRAME_POOL_SIZE; i++)
    {
        g_pool.buffers[i] = (uint8_t *)malloc(MAX_ENCODED_FRAME_SIZE);
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

void encoded_frame_pool_cleanup(void)
{
    for (int i = 0; i < ENCODED_FRAME_POOL_SIZE; i++)
    {
        free(g_pool.buffers[i]);
        g_pool.buffers[i] = NULL;
    }

    pthread_mutex_destroy(&g_pool.lock);
}

encoded_frame_t *encoded_frame_pool_acquire(void)
{
    pthread_mutex_lock(&g_pool.lock);

    for (int i = 0; i < ENCODED_FRAME_POOL_SIZE; i++)
    {
        if (!g_pool.used[i])
        {
            g_pool.used[i] = 1;
            g_pool.frames[i].size = 0;
            g_pool.frames[i].pts_us = 0;
            g_pool.frames[i].sequence = 0;
            g_pool.frames[i].ref_count = 1;
            pthread_mutex_unlock(&g_pool.lock);
            return &g_pool.frames[i];
        }
    }

    pthread_mutex_unlock(&g_pool.lock);

    return NULL;
}

void encoded_frame_pool_release(encoded_frame_t *frame)
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

    for (int i = 0; i < ENCODED_FRAME_POOL_SIZE; i++)
    {
        if (&g_pool.frames[i] == frame)
        {
            g_pool.used[i] = 0;
            break;
        }
    }

    pthread_mutex_unlock(&g_pool.lock);
}

void encoded_frame_pool_retain(encoded_frame_t *frame)
{
    if (!frame)
    {
        return;
    }

    ++frame->ref_count;
}