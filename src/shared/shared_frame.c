#include <string.h>
#include <stdio.h>

#include "shared_frame.h"
#include "frame_pool.h"

struct shared_frame g_frame;
void shared_frame_init(void)
{
    memset(&g_frame, 0, sizeof(g_frame));

    pthread_rwlock_init(&g_frame.lock,
                        NULL);

    printf("[SHARED FRAME INIT]\n");
}

void shared_frame_update(void *data,
                         int size,
                         int buffer_index,
                         struct timespec *ts)
{
    int old_index = -1;
    int has_old_frame = 0;

    pthread_rwlock_wrlock(&g_frame.lock);

    /**
     * Save old frame ownership
     */
    if (g_frame.data != NULL)
    {
        old_index = g_frame.buffer_index;
        has_old_frame = 1;
    }

    /**
     * Overwrite latest frame
     */
    g_frame.data = data;

    g_frame.size = size;

    g_frame.buffer_index = buffer_index;

    g_frame.frame_id++;

    g_frame.timestamp = *ts;

    printf("[FRAME UPDATE] id=%lu ptr=%p size=%d buffer_index=%d\n",
           g_frame.frame_id,
           data,
           size,
           buffer_index);

    pthread_rwlock_unlock(&g_frame.lock);

    /**
     * Release old frame AFTER unlock
     */
    if (has_old_frame)
    {
        printf("[RELEASE OLD FRAME] index = %d\n", old_index);
        frame_release(old_index);
    }
}

int shared_frame_get(struct shared_frame *out)
{
    pthread_rwlock_rdlock(&g_frame.lock);

    if (g_frame.data == NULL)
    {
        pthread_rwlock_unlock(&g_frame.lock);

        return -1;
    }
    
    frame_acquire(g_frame.buffer_index);
    *out = g_frame;

    pthread_rwlock_unlock(&g_frame.lock);

    return 0;
}