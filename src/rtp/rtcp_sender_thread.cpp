#include <pthread.h>
#include <cstdio>
#include <atomic>
#include <unistd.h>
#include <arpa/inet.h>

#include "rtcp_sender_thread.h"
#include "rtcp_packet.h"
#include "udp_sender.h"
#include "udp_sender_thread.h"
#include "rtp_packetizer_thread.h"
#include "log.h"

static const char *TAG = "RTCP_SND";

static pthread_t g_thread;
static std::atomic<bool> g_running(false);

// RFC 3550 section 6.2 specifies a bandwidth-adaptive interval (RTCP
// traffic capped at ~5% of session bandwidth); this project uses a
// fixed interval instead, matching the same simplification
// loss_reporter_thread already makes for its own reporting cadence -
// good enough for a single-sender/single-receiver LAN stream, not
// meant to scale to large multicast groups the way real RTCP must.
static constexpr int REPORT_INTERVAL_MS = 2000;
static constexpr int POLL_GRANULARITY_MS = 100;

static void *rtcp_sender_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    int waited_ms = 0;

    while (g_running)
    {
        usleep(POLL_GRANULARITY_MS * 1000);
        waited_ms += POLL_GRANULARITY_MS;

        if (waited_ms < REPORT_INTERVAL_MS)
        {
            continue;
        }

        waited_ms = 0;

        uint32_t packets_sent = 0;
        uint32_t octets_sent = 0;
        udp_sender_thread_get_stats(&packets_sent, &octets_sent);

        uint32_t ntp_sec = 0;
        uint32_t ntp_frac = 0;
        rtcp_get_ntp_now(&ntp_sec, &ntp_frac);

        rtcp_sr_t sr;
        sr.v_p_rc = 0x80; // V=2, P=0, RC=0
        sr.packet_type = RTCP_PT_SR;
        sr.length_be = htons(6);
        sr.ssrc_be = htonl(rtp_packetizer_thread_get_ssrc());
        sr.ntp_sec_be = htonl(ntp_sec);
        sr.ntp_frac_be = htonl(ntp_frac);
        sr.rtp_timestamp_be = htonl(rtp_packetizer_thread_get_last_timestamp());
        sr.packet_count_be = htonl(packets_sent);
        sr.octet_count_be = htonl(octets_sent);

        // Sent via udp_sender's already-open socket (same destination
        // as RTP data packets - rtcp-mux) rather than opening a second
        // socket. Safe to call concurrently with udp_sender_thread's
        // own sends: sendto() on one fd from multiple threads just
        // interleaves packets on the wire, no corruption.
        if (udp_sender_send(reinterpret_cast<const uint8_t *>(&sr), sizeof(sr)) < 0)
        {
            LOG_WARN(TAG, "failed to send SR");
        }
        else
        {
            LOG_INFO(TAG, "sent SR: packets=%u octets=%u", packets_sent, octets_sent);
        }
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

int rtcp_sender_thread_start(void)
{
    g_running = true;

    if (pthread_create(&g_thread, nullptr, rtcp_sender_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        g_running = false;
        return -1;
    }

    return 0;
}

void rtcp_sender_thread_stop(void)
{
    // No blocking I/O to wake up here - the loop's own poll of
    // g_running bounds shutdown latency, same as loss_reporter_thread.
    g_running = false;
    pthread_join(g_thread, nullptr);
}
