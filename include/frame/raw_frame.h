#ifndef __RAW_FRAME_H__
#define __RAW_FRAME_H__

#include <stdint.h>
#include <stddef.h>
#include <atomic>

/**
 * ============================================================================
 * PIPELINE STAGE: Camera -> [Capture Thread] -> Raw Frame Pool/Queue -> Encoder
 * ============================================================================
 *
 * raw_frame_t represents ONE uncompressed video frame straight out of the
 * camera (YUV420 planar, 640x480), before H.264 encoding.
 *
 * Lifecycle:
 *   1. camera_capture's libcamera callback copies a captured frame's bytes
 *      into a raw_frame_t acquired from raw_frame_pool.
 *   2. It is pushed (by pointer only, zero-copy) into raw_frame_queue.
 *   3. encoder_thread pops it, feeds `data`/`size` into the hardware
 *      H.264 encoder (bcm2835_encoder), then releases it back to the pool.
 */

// 640x480 YUV420 (4:2:0): Y plane is w*h bytes, U and V planes are
// (w/2)*(h/2) bytes each -> total = w*h + 2*(w*h/4) = w*h*3/2.
#define MAX_RAW_FRAME_SIZE (640 * 480 * 3 / 2)

typedef struct
{
    uint8_t *data;          // YUV420 pixel data, MAX_RAW_FRAME_SIZE bytes, owned by raw_frame_pool
    size_t size;            // actual bytes filled in `data` for this frame (<= MAX_RAW_FRAME_SIZE)
    uint64_t pts_us;        // capture timestamp in microseconds (monotonic clock, see time_utils.h)
    uint32_t sequence;      // monotonically increasing frame counter, assigned at capture time
    std::atomic<int> ref_count; // pool bookkeeping: 0 = free/available, >0 = in use by the pipeline
} raw_frame_t;

#endif // __RAW_FRAME_H__
