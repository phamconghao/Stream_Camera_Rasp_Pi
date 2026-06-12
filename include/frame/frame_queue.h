#ifndef __FRAME_QUEUE_H__
#define __FRAME_QUEUE_H__

#include "encoded_frame.h"
#include <pthread.h>

#define FRAME_QUEUE_SIZE 64

typedef struct
{
    encoded_frame_t *frames[FRAME_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} frame_queue_t;

int frame_queue_init(frame_queue_t *q);
int frame_queue_push(frame_queue_t *q, encoded_frame_t *frame);
encoded_frame_t *frame_queue_pop(frame_queue_t *q);
void frame_queue_destroy(frame_queue_t *q);

#endif // __FRAME_QUEUE_H__