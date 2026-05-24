#include <stdio.h>

#include "frame_pool.h"
#include "v4l2_capture.h"

static struct frame_slot slots[BUFFER_COUNT];

void frame_pool_init()
{
    for (int i = 0; i < BUFFER_COUNT; i++)
    {
        slots[i].ref_count = 0;
        pthread_mutex_init(&slots[i].lock, NULL);
    }

    printf("[FRAME POOL INIT]\n");
}

void frame_acquire(int index)
{
    pthread_mutex_lock(&slots[index].lock);
    slots[index].ref_count++;

    printf("[ACQUIRE] buffer=%d ref=%d\n", index, slots[index].ref_count);

    pthread_mutex_unlock(&slots[index].lock);
}

void frame_release(int index)
{
    int should_qbuf = 0;
    pthread_mutex_lock(&slots[index].lock);
    slots[index].ref_count--;

    printf("[RELEASE] buffer=%d ref=%d\n", index, slots[index].ref_count);

    if (slots[index].ref_count == 0)
    {
        should_qbuf = 1;
    }

    pthread_mutex_unlock(&slots[index].lock);

    if (should_qbuf)
    {
        printf("[REQUEUE BUFFER] %d\n", index);

        capture_release_frame(index);
    }
}