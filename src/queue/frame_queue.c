#include <stdio.h>
#include <string.h>

#include "frame_queue.h"

void queue_init(struct frame_queue *q)
{
    memset(q,
           0,
           sizeof(*q));

    pthread_mutex_init(&q->mutex,
                       NULL);

    pthread_cond_init(&q->cond,
                      NULL);
}

int queue_push(struct frame_queue *q,
               void *data,
               int size,
               int buffer_index,
               struct timespec *ts)
{
    pthread_mutex_lock(&q->mutex);

    if (q->count == QUEUE_SIZE)
    {
        printf("[QUEUE FULL] dropping frame\n");

        q->head =
            (q->head + 1) % QUEUE_SIZE;

        q->count--;

        q->dropped_frames++;
    }

    struct frame *f =
        &q->frames[q->tail];

    f->data = data;

    f->size = size;

    f->buffer_index = buffer_index;

    f->timestamp = *ts;

    printf("[QUEUE STORE] slot=%d ptr=%p size=%d index=%d\n",
           q->tail,
           data,
           size,
           buffer_index);

    q->tail =
        (q->tail + 1) % QUEUE_SIZE;

    q->count++;

    q->pushed_frames++;

    pthread_cond_signal(&q->cond);

    pthread_mutex_unlock(&q->mutex);

    return 0;
}

int queue_pop(struct frame_queue *q,
              struct frame *out)
{
    pthread_mutex_lock(&q->mutex);

    while (q->count == 0)
    {
        pthread_cond_wait(&q->cond,
                          &q->mutex);
    }

    *out =
        q->frames[q->head];

    printf("[QUEUE LOAD] slot=%d ptr=%p size=%d index=%d\n",
           q->head,
           out->data,
           out->size,
           out->buffer_index);

    q->head =
        (q->head + 1) % QUEUE_SIZE;

    q->count--;

    q->popped_frames++;

    pthread_mutex_unlock(&q->mutex);

    return 0;
}

void queue_print_stats(struct frame_queue *q)
{
    pthread_mutex_lock(&q->mutex);

    printf("\n============== STATS ==============\n");

    printf("Queue count     : %d\n",
           q->count);

    printf("Frames pushed   : %lu\n",
           q->pushed_frames);

    printf("Frames popped   : %lu\n",
           q->popped_frames);

    printf("Frames dropped  : %lu\n",
           q->dropped_frames);

    printf("===================================\n");

    pthread_mutex_unlock(&q->mutex);
}
