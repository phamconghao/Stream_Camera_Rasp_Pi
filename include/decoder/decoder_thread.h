#ifndef __DECODER_THREAD_H__
#define __DECODER_THREAD_H__

/**
 * PIPELINE STAGE (thread wrapper): Encoded Frame Queue -> [THIS] -> Raw Frame Queue
 *
 * Mirror image of encoder_thread.h. Pops access units, calls
 * bcm2835_decoder to turn them into YUV420 frames, pushes the results
 * into raw_frame_queue for yuv_writer_thread to write to disk.
 */

int decoder_thread_start(int width, int height);
void decoder_thread_stop(void);

#endif // __DECODER_THREAD_H__
