#ifndef __CIRCULAR_H264_WRITER_H__
#define __CIRCULAR_H264_WRITER_H__

#include <cstdint>
#include <cstddef>

/**
 * PIPELINE STAGE (receiver side, Idea #4 - dashcam-style recording):
 * RTP Depacketizer Thread -> [THIS] -> disk (rotating segments)
 *
 * "Dashcam" storage for the reassembled H.264 elementary stream:
 * writes into a new segment file every `segment_duration_sec`, and
 * once more than `max_segments` exist, deletes the oldest one - so
 * disk usage stays bounded (roughly max_segments * segment_duration_sec
 * worth of video) instead of growing forever the way a single flat
 * dump file would.
 *
 * Segment files are named "<dir>/segment_<unix_timestamp>.h264" so
 * they sort chronologically by filename. Each segment is a raw
 * Annex-B elementary stream (no container), playable directly with
 * e.g. `ffplay segment_1234567890.h264`.
 *
 * Not a thread of its own - called directly from
 * rtp_depacketizer_thread's loop each time an access unit completes,
 * same as the old single-file debug dump it replaces.
 */

// Creates `dir` if it doesn't already exist, opens the first segment.
// Returns 0 on success, -1 on failure (e.g. can't create/write to dir).
int circular_h264_writer_init(const char *dir, int segment_duration_sec, int max_segments);

// Appends one access unit's bytes to the current segment, rotating to
// a new segment file first if segment_duration_sec has elapsed since
// the current one was opened.
void circular_h264_writer_write(const uint8_t *data, size_t size);

void circular_h264_writer_cleanup(void);

#endif // __CIRCULAR_H264_WRITER_H__
