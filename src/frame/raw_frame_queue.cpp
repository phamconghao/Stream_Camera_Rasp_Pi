#include <pthread.h>

#include "raw_frame_queue.h"

/**
 * Thread-safe ring buffer carrying raw_frame_t POINTERS (zero-copy) from
 * the capture thread (producer) to the encoder thread (consumer).
 *
 * This is a classic bounded producer/consumer queue: mutex + condvar,
 * push() signals one waiter, pop() blocks until something is available.
 *
 * Shutdown note: pop() blocks indefinitely on an empty queue via
 * pthread_cond_wait. Without `shutting_down`, a consumer thread stuck
 * waiting here would never wake up once its producer stops - so any
 * thread's stop() function MUST call raw_frame_queue_shutdown() before
 * pthread_join(), or that join will hang forever. See encoder_thread.cpp.
 */
typedef struct
{
    raw_frame_t *frames[RAW_FRAME_QUEUE_SIZE];
    int head;               // index to pop from next
    int tail;                // index to push into next
    int count;               // number of items currently queued
    bool shutting_down;      // set by raw_frame_queue_shutdown(); wakes any blocked pop()
    pthread_mutex_t lock;
    pthread_cond_t cond;     // signaled on push, broadcast on shutdown
} raw_frame_queue_ctx_t;

static raw_frame_queue_ctx_t g_queue;

int raw_frame_queue_init(void)
{
    g_queue.head = 0;
    g_queue.tail = 0;
    g_queue.count = 0;
    g_queue.shutting_down = false;

    pthread_mutex_init(&g_queue.lock, nullptr);
    pthread_cond_init(&g_queue.cond, nullptr);
    return 0;
}

void raw_frame_queue_cleanup(void)
{
    pthread_mutex_destroy(&g_queue.lock);
    pthread_cond_destroy(&g_queue.cond);
}

// Called by camera_capture's libcamera callback for every captured frame.
// Non-blocking: if the encoder can't keep up and the queue is full, the
// frame is dropped (returns -1) rather than stalling the camera callback.
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

// Called by encoder_thread's loop. Blocks until a frame is available OR
// raw_frame_queue_shutdown() has been called and the queue has drained
// (in which case it returns nullptr so the caller's while-loop can exit).
raw_frame_t *raw_frame_queue_pop(void)
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

    raw_frame_t *frame = g_queue.frames[g_queue.head];
    g_queue.head = (g_queue.head + 1) % RAW_FRAME_QUEUE_SIZE;
    g_queue.count--;

    pthread_mutex_unlock(&g_queue.lock);
    return frame;
}

// Wakes up any thread currently blocked in pop() so it can observe
// shutdown and return nullptr instead of waiting forever. Call this from
// encoder_thread_stop() (or any future consumer's stop function) before
// pthread_join().
void raw_frame_queue_shutdown(void)
{
    pthread_mutex_lock(&g_queue.lock);
    g_queue.shutting_down = true;
    pthread_cond_broadcast(&g_queue.cond);
    pthread_mutex_unlock(&g_queue.lock);
}

// Diagnostic/monitoring helper - current number of queued frames.
int raw_frame_queue_count(void)
{
    pthread_mutex_lock(&g_queue.lock);
    int count = g_queue.count;
    pthread_mutex_unlock(&g_queue.lock);
    return count;
}
