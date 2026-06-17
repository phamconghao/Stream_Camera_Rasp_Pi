#ifndef __RAW_FRAME_QUEUE_H__
#define __RAW_FRAME_QUEUE_H__

#include "raw_frame.h"

#define RAW_FRAME_QUEUE_SIZE 32

int raw_frame_queue_init(void);
void raw_frame_queue_cleanup(void);
int raw_frame_queue_push(raw_frame_t *frame);
raw_frame_t *raw_frame_queue_pop(void);
int raw_frame_queue_count(void);

#endif // __RAW_FRAME_QUEUE_H__