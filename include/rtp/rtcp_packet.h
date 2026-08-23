#ifndef __RTCP_PACKET_H__
#define __RTCP_PACKET_H__

#include <cstdint>
#include <cstddef>

/**
 * RTCP (RFC 3550) - Sender Report / Receiver Report wire format.
 *
 * Implements just the two most valuable RTCP packet types for this
 * project - Sender Report (SR) and Receiver Report (RR) - rather than
 * the full RFC (no SDES/BYE/APP, no compound-packet bundling, no
 * bandwidth-adaptive reporting interval per section 6.2). This is
 * enough to interoperate with real RTCP-aware tools (Wireshark,
 * ffprobe, VLC's stream stats) for the fields that actually matter
 * here: loss, jitter, and clock sync between sender and receiver.
 *
 * Both packet types share RTP's version-2 header pattern, which is how
 * they're told apart from ordinary RTP/H.264 data packets when
 * multiplexed onto the same port (RFC 5761 "rtcp-mux") - see
 * udp_receiver_thread.cpp (SR arriving alongside RTP data) and
 * control_listener_thread.cpp (RR arriving alongside the ad-hoc
 * control-channel messages).
 *
 * Common RTCP header (4 bytes), same shape for every packet type:
 *   byte 0: V(2 bits)=2, P(1 bit)=0, RC/SC(5 bits)=count of report blocks
 *   byte 1: PT (packet type: 200=SR, 201=RR)
 *   bytes 2-3: length, in 32-bit words minus one (i.e. total_bytes/4 - 1)
 */

static constexpr uint8_t RTCP_PT_SR = 200;
static constexpr uint8_t RTCP_PT_RR = 201;

#pragma pack(push, 1)

/**
 * RTCP Sender Report (RFC 3550 section 6.4.1), with zero report blocks
 * (RC=0) - this project's sender never itself receives an RTP stream
 * from the other side, so it has nothing to report ON; it only reports
 * about its OWN outgoing stream via the fixed sender-info fields below.
 * 28 bytes total.
 */
struct rtcp_sr_t
{
    uint8_t v_p_rc;         // 0x80 | 0 = V=2, P=0, RC=0
    uint8_t packet_type;    // RTCP_PT_SR
    uint16_t length_be;     // = 6 (7 32-bit words - 1)
    uint32_t ssrc_be;       // sender's SSRC (matches the RTP stream's SSRC)
    uint32_t ntp_sec_be;    // NTP timestamp, seconds since 1900-01-01 (wall clock, for cross-stream sync)
    uint32_t ntp_frac_be;   // NTP timestamp, fractional seconds (1/2^32 units)
    uint32_t rtp_timestamp_be; // RTP timestamp corresponding to the NTP time above (same units as RTP packets - 90kHz here)
    uint32_t packet_count_be;  // total RTP data packets sent so far this session
    uint32_t octet_count_be;   // total RTP payload bytes sent so far this session (header excluded)
};

/**
 * One reception report block within an RR (RFC 3550 section 6.4.2),
 * describing what the receiver observed about ONE source's stream
 * (identified by its SSRC). 24 bytes.
 */
struct rtcp_report_block_t
{
    uint32_t ssrc_be;                  // SSRC of the source being reported on (our video stream's SSRC)
    uint32_t fraction_lost_and_cumulative_be; // top byte: fraction lost (8-bit fixed point) since last RR; low 24 bits: signed cumulative packets lost
    uint32_t highest_seq_ext_be;       // cycle count (top 16 bits) + highest sequence number received (low 16 bits)
    uint32_t jitter_be;                // interarrival jitter estimate, RTP timestamp units (RFC 3550 6.4.1 formula)
    uint32_t lsr_be;                   // middle 32 bits of the NTP timestamp from the last SR received (0 if none yet)
    uint32_t dlsr_be;                  // delay since that SR, in 1/65536 second units (0 if none yet)
};

/**
 * RTCP Receiver Report (RFC 3550 section 6.4.2), with exactly one
 * report block (RC=1) - this project has exactly one video SSRC to
 * report on. 32 bytes total.
 */
struct rtcp_rr_t
{
    uint8_t v_p_rc;         // 0x80 | 1 = V=2, P=0, RC=1
    uint8_t packet_type;    // RTCP_PT_RR
    uint16_t length_be;     // = 7 (8 32-bit words - 1)
    uint32_t ssrc_be;       // SSRC identifying the receiver sending this report (arbitrary, distinct from the video SSRC)
    rtcp_report_block_t block;
};

#pragma pack(pop)

// True if `data` looks like an RTCP SR/RR packet (V=2 pattern in byte 0
// plus a recognized PT byte) rather than ordinary RTP data - used to
// demultiplex a shared socket. Deliberately conservative: only
// recognizes the two packet types this project actually sends.
inline bool rtcp_is_sr(const uint8_t *data, size_t size)
{
    return size >= sizeof(rtcp_sr_t) && (data[0] >> 6) == 2 && data[1] == RTCP_PT_SR;
}

inline bool rtcp_is_rr(const uint8_t *data, size_t size)
{
    return size >= sizeof(rtcp_rr_t) && (data[0] >> 6) == 2 && data[1] == RTCP_PT_RR;
}

// Converts the current wall-clock time to NTP 64-bit timestamp form
// (seconds since 1900-01-01 + fractional part), as required by SR's
// ntp_sec/ntp_frac fields.
void rtcp_get_ntp_now(uint32_t *out_ntp_sec, uint32_t *out_ntp_frac);

#endif // __RTCP_PACKET_H__
