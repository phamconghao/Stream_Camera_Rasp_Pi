#ifndef __RTP_DEPACKETIZER_THREAD_H__
#define __RTP_DEPACKETIZER_THREAD_H__

/**
 * PIPELINE STAGE (thread wrapper): RTP Jitter Buffer -> [THIS] -> Encoded Frame Queue
 *
 * Spawns the thread that drains rtp_jitter_buffer (already
 * sequence-ordered, see rtp_jitter_buffer.h), feeds each packet through
 * rtp_depacketizer, and pushes completed access units into
 * encoded_frame_queue for decoder_thread to consume.
 */

int rtp_depacketizer_thread_start(void);
void rtp_depacketizer_thread_stop(void);

#endif // __RTP_DEPACKETIZER_THREAD_H__
