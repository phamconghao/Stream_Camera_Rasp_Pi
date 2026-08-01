#include "rtcp_receiver_stats.h"

#include <pthread.h>
#include <cmath>
#include <arpa/inet.h>

#include "rtp_jitter_buffer.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Extended sequence number tracking (RFC 3550 section A.1's approach,
// simplified: we don't need full RTP-source-validation state machine
// here, just cycle counting for the "extended highest sequence number
// received" field).
static bool g_seq_initialized = false;
static uint16_t g_last_seq = 0;
static uint16_t g_cycles = 0; // increments each time seq wraps 0xFFFF -> 0x0000

// Interarrival jitter (RFC 3550 section 6.4.1): a running estimate,
// updated on every packet, of how much arrival spacing deviates from
// the ideal spacing implied by the packets' own RTP timestamps.
static bool g_have_prev_transit = false;
static double g_prev_transit = 0.0;
static double g_jitter = 0.0;

// RTP clock rate this project uses for H.264 (see RTP_CLOCK in
// rtp_packetizer_thread.cpp) - needed to convert our monotonic arrival
// clock (microseconds) into the same units as RTP timestamps so the
// two can be subtracted meaningfully.
static constexpr double RTP_CLOCK_HZ = 90000.0;

// LSR/DLSR: remembers the most recent SR we've seen, so a later RR can
// report how long ago that was.
static bool g_have_last_sr = false;
static uint32_t g_last_sr_mid32 = 0; // middle 32 bits of that SR's NTP timestamp
static uint64_t g_last_sr_local_recv_us = 0;

// For "fraction lost since last RR": remembers the cumulative
// received/lost counts as of the previous rtcp_receiver_stats_build_rr()
// call, mirroring the same delta pattern loss_reporter_thread.cpp uses.
static uint32_t g_prev_report_received = 0;
static uint32_t g_prev_report_lost = 0;

void rtcp_receiver_stats_init(void)
{
    pthread_mutex_lock(&g_lock);

    g_seq_initialized = false;
    g_last_seq = 0;
    g_cycles = 0;
    g_have_prev_transit = false;
    g_prev_transit = 0.0;
    g_jitter = 0.0;
    g_have_last_sr = false;
    g_last_sr_mid32 = 0;
    g_last_sr_local_recv_us = 0;
    g_prev_report_received = 0;
    g_prev_report_lost = 0;

    pthread_mutex_unlock(&g_lock);
}

void rtcp_receiver_stats_on_packet(uint16_t seq, uint32_t rtp_timestamp, uint64_t arrival_us)
{
    pthread_mutex_lock(&g_lock);

    // Cycle detection: a large backward jump in the raw 16-bit sequence
    // number (e.g. 65530 -> 3) means it wrapped around, so bump the
    // cycle counter. A large forward jump the OTHER way (e.g. after a
    // resync) is not treated as un-wrapping - matches the same
    // "only count forward wraps" simplification jitter buffers
    // typically use, since spurious un-wraps are far rarer in practice
    // than the normal wrap-forward case this project actually hits.
    if (g_seq_initialized)
    {
        int16_t diff = static_cast<int16_t>(seq - g_last_seq);
        if (diff < 0 && seq < g_last_seq && (g_last_seq - seq) > 0x8000)
        {
            g_cycles++;
        }
    }
    else
    {
        g_seq_initialized = true;
    }

    g_last_seq = seq;

    // Interarrival jitter (RFC 3550 6.4.1):
    //   transit(i) = arrival(i) - timestamp(i)      [both in RTP clock units]
    //   D = transit(i) - transit(i-1)
    //   J += (|D| - J) / 16
    double arrival_rtp_units = (static_cast<double>(arrival_us) / 1000000.0) * RTP_CLOCK_HZ;
    double transit = arrival_rtp_units - static_cast<double>(rtp_timestamp);

    if (g_have_prev_transit)
    {
        double d = transit - g_prev_transit;
        if (d < 0)
        {
            d = -d;
        }

        g_jitter += (d - g_jitter) / 16.0;
    }

    g_prev_transit = transit;
    g_have_prev_transit = true;

    pthread_mutex_unlock(&g_lock);
}

void rtcp_receiver_stats_on_sr(uint32_t ntp_sec, uint32_t ntp_frac, uint64_t local_recv_us)
{
    pthread_mutex_lock(&g_lock);

    // LSR per RFC 3550 is "the middle 32 bits of the NTP timestamp" -
    // i.e. the low 16 bits of the seconds field concatenated with the
    // high 16 bits of the fractional field.
    g_last_sr_mid32 = ((ntp_sec & 0xFFFF) << 16) | ((ntp_frac >> 16) & 0xFFFF);
    g_last_sr_local_recv_us = local_recv_us;
    g_have_last_sr = true;

    pthread_mutex_unlock(&g_lock);
}

void rtcp_receiver_stats_build_rr(rtcp_rr_t *out, uint32_t reporter_ssrc, uint32_t video_ssrc, uint64_t now_us)
{
    uint32_t received = 0;
    uint32_t lost = 0;
    rtp_jitter_buffer_get_stats(&received, &lost);

    pthread_mutex_lock(&g_lock);

    uint32_t received_delta = received - g_prev_report_received;
    uint32_t lost_delta = lost - g_prev_report_lost;
    g_prev_report_received = received;
    g_prev_report_lost = lost;

    uint32_t expected_delta = received_delta + lost_delta;
    uint8_t fraction_lost = 0;
    if (expected_delta > 0)
    {
        uint32_t frac = (lost_delta * 256) / expected_delta;
        fraction_lost = static_cast<uint8_t>(frac > 255 ? 255 : frac);
    }

    // Cumulative lost is a signed 24-bit field per RFC 3550 - clamp to
    // its range rather than silently wrapping, since `lost` here is a
    // uint32_t running total that could in principle exceed 2^23 on an
    // extremely long-running, extremely lossy session.
    uint32_t cumulative_lost = lost;
    if (cumulative_lost > 0x7FFFFF)
    {
        cumulative_lost = 0x7FFFFF;
    }

    uint32_t highest_seq_ext = (static_cast<uint32_t>(g_cycles) << 16) | g_last_seq;

    uint32_t jitter_units = static_cast<uint32_t>(g_jitter);

    uint32_t lsr = 0;
    uint32_t dlsr = 0;
    if (g_have_last_sr)
    {
        lsr = g_last_sr_mid32;

        // DLSR units are 1/65536 second (RFC 3550 6.4.2): delay = time
        // elapsed, in our own monotonic clock, since we received that
        // SR. now_us is passed in by the caller (rtcp_receiver_thread)
        // right before sending, so this stays accurate regardless of
        // how long build_rr() itself takes to run.
        uint64_t elapsed_us = (now_us > g_last_sr_local_recv_us) ? (now_us - g_last_sr_local_recv_us) : 0;
        double elapsed_sec = static_cast<double>(elapsed_us) / 1000000.0;
        dlsr = static_cast<uint32_t>(elapsed_sec * 65536.0);
    }

    out->v_p_rc = 0x80 | 1; // V=2, P=0, RC=1
    out->packet_type = RTCP_PT_RR;
    out->length_be = htons(7);
    out->ssrc_be = htonl(reporter_ssrc);
    out->block.ssrc_be = htonl(video_ssrc);
    out->block.fraction_lost_and_cumulative_be =
        htonl((static_cast<uint32_t>(fraction_lost) << 24) | cumulative_lost);
    out->block.highest_seq_ext_be = htonl(highest_seq_ext);
    out->block.jitter_be = htonl(jitter_units);
    out->block.lsr_be = htonl(lsr);
    out->block.dlsr_be = htonl(dlsr);

    pthread_mutex_unlock(&g_lock);
}
