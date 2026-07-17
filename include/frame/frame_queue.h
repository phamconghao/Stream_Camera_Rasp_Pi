#ifndef __FRAME_QUEUE_H__
#define __FRAME_QUEUE_H__

#include "encoded_frame.h"
#include <pthread.h>

/**
 * LEGACY / NOT USED BY THE ACTIVE PIPELINE.
 *
 * An earlier, generic queue design (predates the split into dedicated
 * raw_frame_queue.{h,cpp} and encoded_frame_queue.{h,cpp}). Still
 * compiled (listed in CMakeLists.txt SOURCES) but nothing in main.cpp
 * or any thread calls these functions anymore. Also note: unlike the
 * queues actually in use, this one has no shutdown()/wake-up mechanism -
 * if it were ever wired up to a real consumer thread, that thread's
 * stop() would deadlock the same way raw_frame_queue/encoded_frame_queue
 * did before that was fixed (see raw_frame_queue.cpp for the pattern
 * to copy if this is ever revived instead of deleted).
 */

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