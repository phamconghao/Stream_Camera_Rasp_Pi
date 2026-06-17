#ifndef __RAW_FRAME_H__
#define __RAW_FRAME_H__

#include <stdint.h>
#include <stddef.h>
#include <atomic>

#define MAX_RAW_FRAME_SIZE (640 * 480 * 3 / 2)

typedef struct
{
    uint8_t *data;
    size_t size;
    uint64_t pts_us;
    uint32_t sequence;
    std::atomic<int> ref_count;
} raw_frame_t;

#endif // __RAW_FRAME_H__