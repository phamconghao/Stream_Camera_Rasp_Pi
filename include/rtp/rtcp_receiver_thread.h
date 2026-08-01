#ifndef __RTCP_RECEIVER_THREAD_H__
#define __RTCP_RECEIVER_THREAD_H__

#include <cstdint>

/**
 * PIPELINE STAGE (receiver side, Phase 19 - RTCP):
 * rtcp_receiver_stats -> [THIS] -> Network (multiplexed with control_channel)
 *
 * Periodically builds an RTCP Receiver Report (RFC 3550 6.4.2) from
 * rtcp_receiver_stats and sends it via control_channel's already-open
 * socket (same destination as Phase 18's keyframe requests / loss
 * reports - no new port needed). The sender's control_listener_thread
 * demultiplexes RR packets from the ad-hoc control messages by
 * inspecting the packet type byte - see control_listener_thread.cpp.
 *
 * reporter_ssrc: arbitrary identifier for this receiver (distinct from
 * the video stream's own SSRC, which is reported ON, not reported AS).
 * video_ssrc: the SSRC of the stream being reported on - must match
 * rtp_packetizer_thread's RTP_SSRC on the sender side.
 */

int rtcp_receiver_thread_start(uint32_t reporter_ssrc, uint32_t video_ssrc);
void rtcp_receiver_thread_stop(void);

#endif // __RTCP_RECEIVER_THREAD_H__
