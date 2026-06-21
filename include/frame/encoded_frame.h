#ifndef __ENCODED_FRAME_H__
#define __ENCODED_FRAME_H__

#include <stdint.h>
#include <stddef.h>
#include <atomic>

#define MAX_ENCODED_FRAME_SIZE (1024 * 1024)

typedef struct
{
    uint8_t *data;
    size_t size;
    uint64_t pts_us;
    uint32_t sequence;
    std::atomic<int> ref_count;
} encoded_frame_t;

#endif  // __ENCODED_FRAME_H__