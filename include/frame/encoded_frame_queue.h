#ifndef __ENCODED_FRAME_QUEUE_H__
#define __ENCODED_FRAME_QUEUE_H__

#include "encoded_frame.h"

#define ENCODED_FRAME_QUEUE_SIZE 32

int encoded_frame_queue_init(void);
void encoded_frame_queue_cleanup(void);
int encoded_frame_queue_push(encoded_frame_t *frame);
encoded_frame_t *encoded_frame_queue_pop(void);
int encoded_frame_queue_count(void);

/**
 * Wake up any thread blocked in encoded_frame_queue_pop() so it can
 * observe shutdown and exit instead of waiting forever. After this call,
 * encoded_frame_queue_pop() returns nullptr immediately once the queue
 * is empty.
 */
void encoded_frame_queue_shutdown(void);

#endif // __RAW_FRAME_QUEUE_H__