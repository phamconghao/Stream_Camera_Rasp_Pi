#ifndef __FRAME_POOL_H__
#define __FRAME_POOL_H__

#include "encoded_frame.h"

#define FRAME_POOL_SIZE 32

int frame_pool_init(void);
encoded_frame_t *frame_pool_acquire(void);
void frame_pool_release(encoded_frame_t *frame);
void frame_pool_cleanup(void);

#endif // __FRAME_POOL_H__