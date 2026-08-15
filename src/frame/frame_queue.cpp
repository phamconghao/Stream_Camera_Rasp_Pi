#include <stdlib.h>
#include <string.h>

#include "frame_queue.h"

/**
 * UNUSED GENERIC SCAFFOLDING - same status as frame_pool.cpp: not
 * called anywhere in the actual pipeline (raw_frame_queue.cpp/
 * encoded_frame_queue.cpp are what's really used). Kept only as a
 * remaining build source; a candidate for future cleanup.
 *
 * A textbook bounded ring buffer with condition-variable blocking:
 * frame_queue_pop() blocks the calling thread until something is
 * pushed, frame_queue_push() fails (rather than blocking) once the
 * ring is full.
 */

int frame_queue_init(frame_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);

    return 0;
}

int frame_queue_push(frame_queue_t *q, encoded_frame_t *frame)
{
    pthread_mutex_lock(&q->mutex);

    if (q->count == FRAME_QUEUE_SIZE)
    {
        pthread_mutex_unlock(&q->mutex);
        return -1; // Queue full
    }

    q->frames[q->tail] = frame;
    q->tail = (q->tail + 1) % FRAME_QUEUE_SIZE; // wrap around - ring buffer, not a growing list
    q->count++;

    pthread_cond_signal(&q->cond); // wake one blocked frame_queue_pop() caller, if any
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

// Blocks (via condition variable, not busy-polling) until an item is
// available - the while-loop around pthread_cond_wait() guards
// against spurious wakeups, which POSIX condition variables are
// explicitly allowed to produce.
encoded_frame_t *frame_queue_pop(frame_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);

    while (q->count == 0)
    {
        pthread_cond_wait(&q->cond, &q->mutex);
    }

    encoded_frame_t *frame = q->frames[q->head];
    q->head = (q->head + 1) % FRAME_QUEUE_SIZE;
    q->count--;

    pthread_mutex_unlock(&q->mutex);
    return frame;
}

void frame_queue_destroy(frame_queue_t *q)
{
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}