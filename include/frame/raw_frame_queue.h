#ifndef __RAW_FRAME_QUEUE_H__
#define __RAW_FRAME_QUEUE_H__

#include "raw_frame.h"

#define RAW_FRAME_QUEUE_SIZE 32

int raw_frame_queue_init(void);
void raw_frame_queue_cleanup(void);
int raw_frame_queue_push(raw_frame_t *frame);
raw_frame_t *raw_frame_queue_pop(void);
int raw_frame_queue_count(void);

/**
 * Wake up any thread blocked in raw_frame_queue_pop() so it can observe
 * shutdown and exit instead of waiting forever. After this call,
 * raw_frame_queue_pop() returns nullptr immediately once the queue is empty.
 */
void raw_frame_queue_shutdown(void);

#endif // __RAW_FRAME_QUEUE_H__