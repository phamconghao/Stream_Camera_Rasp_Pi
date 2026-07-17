#ifndef __RAW_FRAME_POOL_H__
#define __RAW_FRAME_POOL_H__

#include "raw_frame.h"

// Number of raw_frame_t slots pre-allocated. Only needs to cover the
// in-flight frames between capture and encode (usually 1-2), plus
// slack for jitter; 8 is generous headroom for a single-camera pipeline.
#define RAW_FRAME_POOL_SIZE 8

int raw_frame_pool_init(void);
void raw_frame_pool_cleanup(void);

// Check out a free raw_frame_t (ref_count starts at 1). Returns nullptr
// if the pool is exhausted - caller (camera_capture) must drop the frame
// in that case rather than block, since blocking here would stall the
// libcamera capture callback.
raw_frame_t *raw_frame_pool_acquire(void);

// Give up one reference. Once the last reference is released, the slot
// becomes available again for raw_frame_pool_acquire().
void raw_frame_pool_release(raw_frame_t *frame);

// Add an extra reference to a frame already checked out (for future
// multi-consumer fan-out; unused in the current single-encoder pipeline).
void raw_frame_pool_retain(raw_frame_t *frame);

#endif // __RAW_FRAME_POOL_H__
