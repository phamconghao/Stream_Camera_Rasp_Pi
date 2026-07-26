#ifndef __LOSS_REPORTER_THREAD_H__
#define __LOSS_REPORTER_THREAD_H__

/**
 * PIPELINE STAGE (receiver side, adaptive bitrate - Idea #1):
 * RTP Jitter Buffer stats -> [THIS] -> control_channel -> sender
 *
 * Wakes up periodically (see REPORT_INTERVAL_MS in
 * loss_reporter_thread.cpp), reads rtp_jitter_buffer's cumulative
 * received/lost counters, computes the loss rate over just that
 * interval (not since the stream began - a stale average would react
 * far too slowly to changing network conditions), and reports it to
 * the sender via control_channel_report_loss() so it can adjust its
 * encoding bitrate.
 *
 * Deliberately its own thread rather than folded into
 * rtp_depacketizer_thread: that thread's job is per-packet reassembly
 * and needs to react to loss immediately (keyframe request), while
 * this is a slow, fixed-interval timer unrelated to any single
 * packet - mixing the two would make rtp_depacketizer_thread's loop
 * harder to reason about for no benefit.
 */

int loss_reporter_thread_start(void);
void loss_reporter_thread_stop(void);

#endif // __LOSS_REPORTER_THREAD_H__
