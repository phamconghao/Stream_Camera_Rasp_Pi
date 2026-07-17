#ifndef __H264_WRITER_H__
#define __H264_WRITER_H__

/**
 * ALTERNATE CONSUMER OF encoded_frame_queue - NOT USED IN THE CURRENT
 * LIVE PIPELINE (its start call is commented out in main.cpp).
 *
 * Writes the H.264 elementary stream straight to a local file
 * (stream.h264) instead of feeding the RTP path. Useful for local
 * recording/debugging (e.g. verifying the encoder's output with
 * `ffplay stream.h264` without needing the network stack at all), but
 * would need to run alongside (or instead of) rtp_packetizer_thread -
 * both are independent consumers competing for the same
 * encoded_frame_queue, so running both at once means each encoded
 * frame goes to only ONE of them, not both (whichever pop()s it first).
 */

int h264_writer_start();
void h264_writer_stop();

#endif // __H264_WRITER_H__
