#ifndef __RAW_FRAME_POOL_H__
#define __RAW_FRAME_POOL_H__

#include "raw_frame.h"

#define RAW_FRAME_POOL_SIZE 8

int raw_frame_pool_init(void);
void raw_frame_pool_cleanup(void);
raw_frame_t *raw_frame_pool_acquire(void);
void raw_frame_pool_release(raw_frame_t *frame);
void raw_frame_pool_retain(raw_frame_t *frame);

#endif // __RAW_FRAME_POOL_H__