#include <stdlib.h>
#include <string.h>

#include "frame_queue.h"

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
    q->tail = (q->tail + 1) % FRAME_QUEUE_SIZE;
    q->count++;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

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