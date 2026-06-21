#ifndef __ENCODED_FRAME_POOL_H__
#define __ENCODED_FRAME_POOL_H__

#include "encoded_frame.h"

#define ENCODED_FRAME_POOL_SIZE 16

int encoded_frame_pool_init(void);
void encoded_frame_pool_cleanup(void);
encoded_frame_t *encoded_frame_pool_acquire(void);
void encoded_frame_pool_release(encoded_frame_t *frame);
void encoded_frame_pool_retain(encoded_frame_t *frame);

#endif // __ENCODED_FRAME_POOL_H__