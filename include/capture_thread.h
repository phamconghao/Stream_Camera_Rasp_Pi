#ifndef __CAPTURE_THREAD_H__
#define __CAPTURE_THREAD_H__

#include "frame_queue.h"

int capture_thread_start(struct frame_queue *queue);
void capture_thread_stop();

#endif // __CAPTURE_THREAD_H__
