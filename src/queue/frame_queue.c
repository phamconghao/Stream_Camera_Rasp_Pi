#include "frame_queue.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ===========================================
//                  INIT
// ===========================================
void queue_init(struct frame_queue *q)
{
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->pushed_frames = 0;
    q->popped_frames = 0;
    q->dropped_frames = 0;

    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    
    /*
    for (int i = 0; i < QUEUE_SIZE; i++)
    {
        q->frames[i].data = malloc(MAX_FRAME_SIZE);

        if (!q->frames[i].data)
        {
            printf("malloc failed\n");
            exit(1);
        }
    }
    */
}

// ===========================================
//                  PUSH 
// ===========================================
int queue_push(struct frame_queue *q, void *data, int size, int buffer_index, struct timespec *ts)
{
    pthread_mutex_lock(&q->mutex);

    // ---------------------------------
    // Queue full
    // Drop oldest frame
    // ---------------------------------
    if (q->count == QUEUE_SIZE)
    {
        printf("[QUEUE FULL] dropping oldest frame\n");

        q->head = (q->head + 1) % QUEUE_SIZE;
        q->count--;
        q->dropped_frames++;
    }

    /*
    if (size > MAX_FRAME_SIZE)
    {
        printf("Frame too large\n");
        pthread_mutex_unlock(&q->mutex);

        return -1;
    }
    */

    // ---------------------------------
    // Copy frame 
    // ---------------------------------
    q->frames[q->tail].data = data;
    q->frames[q->tail].size = size;
    q->frames[q->tail].buffer_index = buffer_index;
    q->frames[q->tail].timestamp = *ts;

    printf("\n[QUEUE STORE]\n");
    printf("slot    = %d\n", q->tail);
    printf("ptr     = %p\n", data);
    printf("size    = %d\n", size);
    printf("index   = %d\n", buffer_index);

    // ---------------------------------
    // Update ring buffer 
    // ---------------------------------
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;
    q->pushed_frames++;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);

    return 0;
}

// ===========================================
//                  POP 
// ===========================================
int queue_pop(struct frame_queue *q, struct frame *out)
{
    pthread_mutex_lock(&q->mutex);

    while (q->count == 0)
    {
        pthread_cond_wait(&q->cond, &q->mutex);
    }

    // ---------------------------------
    // Copy frame out 
    // ---------------------------------
    *out = q->frames[q->head];

    printf("\n[QUEUE LOAD]\n");
    printf("slot    = %d\n", q->head);
    printf("ptr     = %p\n", out->data);
    printf("size    = %d\n", out->size);
    printf("index   = %d\n", out->buffer_index);

    // ---------------------------------
    // Update ring buffer 
    // ---------------------------------
    q->head = (q->head + 1) % QUEUE_SIZE;

    q->count--;
    q->popped_frames++;

    pthread_mutex_unlock(&q->mutex);

    return 0;
}

void queue_print_stats(struct frame_queue *q)
{
    pthread_mutex_lock(&q->mutex);
    printf("\n==================== STATS ==================\n");
    printf("Queue count     = %d\n", q->count);
    printf("Frames pushed   = %lu\n", q->pushed_frames);
    printf("Frames popped   = %lu\n", q->popped_frames);
    printf("Frames dropped  = %lu\n", q->dropped_frames);
    printf("===============================================\n");

    pthread_mutex_unlock(&q->mutex);
}
