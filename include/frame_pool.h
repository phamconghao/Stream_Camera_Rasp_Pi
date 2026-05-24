#ifndef __FRAME_POOL_H__
#define __FRAME_POOL_H__

#include <pthread.h>

#define BUFFER_COUNT 4

struct frame_slot
{
    int ref_count;
    pthread_mutex_t lock;
};

void frame_pool_init();
void frame_acquire(int index);
void frame_release(int index);

#endif /* __FRAME_POOL_H__ */