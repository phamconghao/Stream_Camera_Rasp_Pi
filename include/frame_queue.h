#ifndef __FRAME_QUEUE_H__
#define __FRAME_QUEUE_H__

#include <pthread.h>
#include <time.h>

#define QUEUE_SIZE 4

struct frame
{
    void *data;

    int size;

    int buffer_index;

    struct timespec timestamp;
};

struct frame_queue
{
    struct frame frames[QUEUE_SIZE];

    int head;

    int tail;

    int count;

    pthread_mutex_t mutex;

    pthread_cond_t cond;

    unsigned long pushed_frames;

    unsigned long popped_frames;

    unsigned long dropped_frames;
};

void queue_init(struct frame_queue *q);

int queue_push(struct frame_queue *q,
               void *data,
               int size,
               int buffer_index,
               struct timespec *ts);

int queue_pop(struct frame_queue *q,
              struct frame *out);

void queue_print_stats(struct frame_queue *q);

#endif
