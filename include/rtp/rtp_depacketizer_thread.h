#ifndef __RTP_DEPACKETIZER_THREAD_H__
#define __RTP_DEPACKETIZER_THREAD_H__

#include <cstdint>

/**
 * PIPELINE STAGE (thread wrapper): RTP Jitter Buffer -> [THIS] -> Encoded Frame Queue
 *
 * Spawns the thread that drains rtp_jitter_buffer (already
 * sequence-ordered, see rtp_jitter_buffer.h), feeds each packet through
 * rtp_depacketizer, and pushes completed access units into
 * encoded_frame_queue for decoder_thread to consume. Also mirrors each
 * completed access unit into circular_h264_writer (Idea #4 - bounded,
 * rotating "dashcam" storage; see writer/circular_h264_writer.h).
 *
 * recording_dir: directory circular_h264_writer writes segments into.
 * Pass nullptr to disable recording entirely (still does everything
 * else - decoding, playback - normally).
 */
int rtp_depacketizer_thread_start(const char *recording_dir = nullptr,
                                   int segment_duration_sec = 60,
                                   int max_segments = 10);
void rtp_depacketizer_thread_stop(void);

/**
 * Idea #3 (monitoring dashboard): cumulative count of access units
 * (i.e. decoded video frames) completed since rtp_depacketizer_thread_start().
 * Used by loss_reporter_thread to compute an fps figure for stats.json.
 */
uint32_t rtp_depacketizer_thread_get_completed_count(void);

#endif // __RTP_DEPACKETIZER_THREAD_H__
