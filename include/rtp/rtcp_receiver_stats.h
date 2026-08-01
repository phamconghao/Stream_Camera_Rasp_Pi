#ifndef __RTCP_RECEIVER_STATS_H__
#define __RTCP_RECEIVER_STATS_H__

#include <cstdint>
#include "rtcp_packet.h"

/**
 * Receiver-side bookkeeping needed to fill in an RTCP Receiver Report
 * (see rtcp_packet.h). Pure logic, no threading/sockets - fed by
 * udp_receiver_thread (per-packet, for jitter/sequence tracking and
 * incoming SR timestamps) and read by rtcp_receiver_thread (once per
 * reporting interval, to build the RR to send).
 */

void rtcp_receiver_stats_init(void);

/**
 * Call once per RTP DATA packet received (not RTCP), with its raw
 * 16-bit sequence number, 32-bit RTP timestamp, and this receiver's own
 * arrival time (monotonic, same clock as time_utils_now_us()). Updates
 * the extended highest-sequence/cycle count and the running
 * interarrival jitter estimate (RFC 3550 section 6.4.1's formula).
 */
void rtcp_receiver_stats_on_packet(uint16_t seq, uint32_t rtp_timestamp, uint64_t arrival_us);

/**
 * Call when an RTCP SR is received from the sender, so a later RR can
 * fill in LSR (last SR timestamp) and DLSR (delay since then).
 */
void rtcp_receiver_stats_on_sr(uint32_t ntp_sec, uint32_t ntp_frac, uint64_t local_recv_us);

/**
 * Builds a fresh Receiver Report reflecting everything observed since
 * the last call to this function: fraction lost is relative to that
 * last call; cumulative lost, extended highest sequence, and jitter are
 * running totals/estimates as RFC 3550 requires. `now_us` (same
 * monotonic clock as time_utils_now_us()) is used to compute DLSR -
 * pass the time right before sending so the field stays accurate.
 */
void rtcp_receiver_stats_build_rr(rtcp_rr_t *out, uint32_t reporter_ssrc, uint32_t video_ssrc, uint64_t now_us);

#endif // __RTCP_RECEIVER_STATS_H__
