#ifndef __ENCODED_FRAME_QUEUE_H__
#define __ENCODED_FRAME_QUEUE_H__

#include "encoded_frame.h"

#define ENCODED_FRAME_QUEUE_SIZE 32

int encoded_frame_queue_init(void);
void encoded_frame_queue_cleanup(void);
int encoded_frame_queue_push(encoded_frame_t *frame);
encoded_frame_t *encoded_frame_queue_pop(void);
int encoded_frame_queue_count(void);

#endif // __RAW_FRAME_QUEUE_H__