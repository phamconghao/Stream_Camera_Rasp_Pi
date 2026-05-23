#ifndef __STREAM_THREAD_H__
#define __STREAM_THREAD_H__

#include "frame_queue.h"

int stream_thread_start(struct frame_queue *queue);
void stream_thread_stop();

#endif  // __STREAM_THREAD_H__
