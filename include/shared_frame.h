#ifndef __SHARED_FRAME_H__
#define __SHARED_FRAME_H__

#include <pthread.h>
#include <time.h>

struct shared_frame
{
    void *data;
    int size;
    int buffer_index;
    unsigned long frame_id;
    struct timespec timestamp;
    pthread_rwlock_t lock;
};

void shared_frame_init(void);
void shared_frame_update(void *data, int size, int buffer_index, struct timespec *ts);
int shared_frame_get(struct shared_frame *out);

extern struct shared_frame g_frame;

#endif /* __SHARED_FRAME_H__ */