#ifndef __YUV_WRITER_H__
#define __YUV_WRITER_H__

/**
 * PIPELINE STAGE (final stage, receive side): Raw Frame Queue -> [THIS] -> received.yuv
 *
 * Simplest consumer in the receiver pipeline: pops one decoded YUV420
 * frame at a time and appends its raw bytes to a local file, so the
 * whole receive path can be verified offline, e.g.:
 *   ffplay -f rawvideo -pixel_format yuv420p -video_size 640x480 received.yuv
 * (paired with received.h264, the pre-decode elementary stream dumped
 * by rtp_depacketizer_thread - see rtp_depacketizer_thread.cpp).
 */

int yuv_writer_start(const char *output_path);
void yuv_writer_stop(void);

#endif // __YUV_WRITER_H__
