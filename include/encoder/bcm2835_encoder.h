#ifndef __BCM2835_ENCODER_H__
#define __BCM2835_ENCODER_H__

#include "raw_frame.h"
#include "encoded_frame.h"

/**
 * PIPELINE STAGE 2: Raw Frame Queue -> [encoder_thread + THIS] -> Encoded Frame Queue
 *
 * Public API for the BCM2835 hardware H.264 encoder (/dev/video11,
 * V4L2 M2M). See bcm2835_encoder.cpp for the full explanation of how
 * V4L2 M2M OUTPUT/CAPTURE queues work.
 */

// One-time setup: opens the device, negotiates YUV420-in/H.264-out,
// allocates and mmaps buffers, starts streaming. Returns 0/-1.
int bcm2835_encoder_init(int width, int height);

// Closes the device file descriptor.
void bcm2835_encoder_cleanup(void);

// Debug/offline helper: encode one raw YUV420 file to an H.264 file,
// bypassing the live pipeline entirely. Not used by main.cpp.
int bcm2835_encoder_encode_file(const char *input_file, const char *output_file);

// Main pipeline entry point, called once per frame by encoder_thread.
// Blocking: feeds `raw` in and waits for the hardware to fill `encoded`.
int bcm2835_encoder_encode_frame(raw_frame_t *raw, encoded_frame_t *encoded);

/**
 * Forces the NEXT encoded frame to be an IDR/keyframe, via the
 * V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME control. Safe to call from a
 * different thread than the one calling bcm2835_encoder_encode_frame() -
 * used when a receiver signals lost packets and needs a fresh keyframe
 * to recover instead of waiting for the next regularly-scheduled one.
 * Returns 0 on success, -1 on ioctl failure.
 */
int bcm2835_encoder_force_keyframe(void);

/**
 * Changes the encoder's target bitrate live, via
 * V4L2_CID_MPEG_VIDEO_BITRATE. Called when a receiver reports a loss
 * rate high enough to warrant backing off, or low enough to step back
 * up. Safe to call from a different thread than
 * bcm2835_encoder_encode_frame(), same reasoning as
 * bcm2835_encoder_force_keyframe(). Returns 0/-1.
 */
int bcm2835_encoder_set_bitrate(uint32_t bitrate_bps);

#endif // __BCM2835_ENCODER_H__
