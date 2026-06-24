#ifndef __BCM2835_ENCODER_H__
#define __BCM2835_ENCODER_H__

#include "raw_frame.h"
#include "encoded_frame.h"

int bcm2835_encoder_init(int width, int height);
void bcm2835_encoder_cleanup(void);
int bcm2835_encoder_encode_file(const char *input_file, const char *output_file);
int bcm2835_encoder_encode_frame(raw_frame_t *raw, encoded_frame_t *encoded);

#endif // __BCM2835_ENCODER_H__