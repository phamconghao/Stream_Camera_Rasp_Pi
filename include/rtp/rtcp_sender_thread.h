#ifndef __RTCP_SENDER_THREAD_H__
#define __RTCP_SENDER_THREAD_H__

/**
 * PIPELINE STAGE (sender side, Phase 19 - RTCP):
 * udp_sender_thread stats + rtp_packetizer_thread SSRC/timestamp -> [THIS] -> Network (rtcp-mux, shared with RTP data)
 *
 * Periodically builds an RTCP Sender Report (RFC 3550 6.4.1) and sends
 * it via the SAME socket udp_sender_thread already uses for RTP data
 * (RFC 5761 "rtcp-mux" - no new port needed). The receiver's
 * udp_receiver_thread demultiplexes SR packets from ordinary RTP data
 * by inspecting the packet type byte - see udp_receiver_thread.cpp.
 *
 * Independent thread, own running flag, same start/stop convention as
 * every other thread in this project.
 */

int rtcp_sender_thread_start(void);
void rtcp_sender_thread_stop(void);

#endif // __RTCP_SENDER_THREAD_H__
