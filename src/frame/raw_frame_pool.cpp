#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "raw_frame_pool.h"

/**
 * Fixed-size, pre-allocated pool of raw_frame_t buffers.
 *
 * Why a pool instead of malloc/free per frame: the capture callback runs
 * on libcamera's own thread and must not block on the heap allocator or
 * on other threads for long; a pool lets it grab a pre-allocated buffer
 * in O(RAW_FRAME_POOL_SIZE) with just a mutex, and avoids fragmentation
 * from continuously allocating/freeing ~460KB (640x480 YUV420) buffers.
 *
 * Ownership model: whoever holds a `raw_frame_t*` returned by acquire()
 * owns it until they call release(). Frames are handed across threads by
 * pointer only (see raw_frame_queue) - the actual pixel data is never
 * copied between pipeline stages, only the pointer/ref-count is shared.
 */
typedef struct
{
    raw_frame_t frames[RAW_FRAME_POOL_SIZE];   // fixed slots; a raw_frame_t's lifetime = this whole program run
    uint8_t *buffers[RAW_FRAME_POOL_SIZE];     // backing pixel storage for frames[i].data, allocated once at init
    int used[RAW_FRAME_POOL_SIZE];             // used[i] = 1 while frames[i] is checked out to the pipeline
    pthread_mutex_t lock;                      // guards used[] and the linear scan in acquire()/release()
} raw_frame_pool_ctx_t;

static raw_frame_pool_ctx_t g_pool;

// Allocate all RAW_FRAME_POOL_SIZE buffers up front (once, at startup).
// After this, the steady-state pipeline never calls malloc/free for
// frame data again - only pointer hand-off between threads.
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

// Called by the capture thread for every new frame from the camera.
// Linear scan for a free slot - RAW_FRAME_POOL_SIZE is small (a handful
// of frames), so this is cheap and simpler than a free-list.
// Returns nullptr if the pool is exhausted (downstream stages are too
// slow to keep up and every slot is still checked out) - caller must
// handle this by dropping the frame, not by blocking.
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

// Decrements ref_count; only actually returns the slot to the pool once
// ref_count reaches 0. This lets a frame be "borrowed" by more than one
// consumer at a time (see raw_frame_pool_retain) without a use-after-free
// race - whichever consumer finishes last is the one that frees it.
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

// Take an extra reference on a frame that's already checked out. Not
// currently called anywhere in the active pipeline (each frame today
// has exactly one consumer at a time), but kept available for future
// fan-out cases (e.g. feeding the same raw frame to two encoders).
void raw_frame_pool_retain(raw_frame_t *frame)
{
    if (!frame)
    {
        return;
    }

    ++frame->ref_count;
}
