#ifndef __ENCODER_THREAD_H__
#define __ENCODER_THREAD_H__

/**
 * PIPELINE STAGE 2 (thread wrapper): Raw Frame Queue -> [THIS] -> Encoded Frame Queue
 *
 * Pops raw_frame_t from raw_frame_queue, runs each through the BCM2835
 * hardware H.264 encoder, and pushes the result into encoded_frame_queue.
 * See encoder_thread.cpp for the full loop.
 */

// Spawn the encoder thread. Returns 0 on success, -1 on pthread_create failure.
int encoder_thread_start(void);

// Signal the thread to stop, wake it if blocked on an empty raw_frame_queue,
// and join it. Blocks until the thread has fully exited.
void encoder_thread_stop(void);

#endif /* __ENCODER_THREAD_H__ */
