#ifndef __BCM2835_DECODER_H__
#define __BCM2835_DECODER_H__

#include "encoded_frame.h"
#include "raw_frame.h"

/**
 * PIPELINE STAGE: Encoded Frame Queue -> [decoder_thread + THIS] -> Raw Frame Queue
 *
 * Public API for the BCM2835 hardware H.264 decoder (V4L2 M2M, mirror
 * image of bcm2835_encoder.h on the sender side). See
 * bcm2835_decoder.cpp for the full explanation of buffer flow.
 */

// One-time setup: opens the device, negotiates H.264-in/YUV420-out,
// allocates and mmaps buffers, starts streaming. Returns 0/-1.
int bcm2835_decoder_init(int width, int height);

// Closes the device file descriptor.
void bcm2835_decoder_cleanup(void);

// Main pipeline entry point, called once per access unit by
// decoder_thread. Blocking: feeds `encoded` in and waits for the
// hardware to fill `raw` with one decoded YUV420 frame.
int bcm2835_decoder_decode_frame(encoded_frame_t *encoded, raw_frame_t *raw);

#endif // __BCM2835_DECODER_H__
