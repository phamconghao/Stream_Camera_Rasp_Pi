#ifndef __FRAME_POOL_H__
#define __FRAME_POOL_H__

#include "encoded_frame.h"

/**
 * LEGACY / NOT USED BY THE ACTIVE PIPELINE.
 *
 * An earlier, generic pool design (predates the split into dedicated
 * raw_frame_pool.{h,cpp} and encoded_frame_pool.{h,cpp}). Still compiled
 * (listed in CMakeLists.txt SOURCES) but nothing in main.cpp or any
 * thread calls these functions anymore - the pipeline uses
 * raw_frame_pool_* / encoded_frame_pool_* instead. Safe to delete once
 * you've confirmed nothing external depends on it; kept here only so
 * removing it is a deliberate decision rather than an accidental one.
 */

#define FRAME_POOL_SIZE 32

int frame_pool_init(void);
encoded_frame_t *frame_pool_acquire(void);
void frame_pool_release(encoded_frame_t *frame);
void frame_pool_cleanup(void);

#endif // __FRAME_POOL_H__