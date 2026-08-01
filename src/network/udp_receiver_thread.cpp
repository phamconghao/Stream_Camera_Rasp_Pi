#include <pthread.h>
#include <cstdio>
#include <atomic>
#include <arpa/inet.h>

#include "udp_receiver_thread.h"
#include "udp_receiver.h"
#include "rtp_packet_pool.h"
#include "rtp_depacketizer.h"
#include "rtp_jitter_buffer.h"
#include "rtcp_packet.h"
#include "rtcp_receiver_stats.h"
#include "time_utils.h"
#include "log.h"

/**
 * ============================================================================
 * PIPELINE STAGE (first stage, receive side): Network -> [THIS] -> RTP Jitter Buffer
 * ============================================================================
 *
 * Loop, once per UDP datagram:
 *   0. Phase 19 (RTCP, rtcp-mux): if this datagram looks like an RTCP
 *      SR (RFC 5761 - same port as RTP data, told apart by the packet
 *      type byte), hand it to rtcp_receiver_stats and go straight back
 *      to recvfrom() - it never touches the jitter buffer.
 *   1. Acquire a free rtp_packet_t from rtp_packet_pool.
 *   2. recvfrom() straight into a scratch buffer, then
 *      rtp_depacketize_header() parses the RTP header fields and
 *      copies the raw bytes into the packet.
 *   3. Feed rtcp_receiver_stats with this packet's sequence/timestamp/
 *      arrival time, for its jitter and extended-sequence-number
 *      tracking (used later when building an RR).
 *   4. Push into rtp_jitter_buffer for sequence reordering - this
 *      thread never blocks waiting on ordering itself, that's entirely
 *      rtp_depacketizer_thread's job on the pop side.
 */

static const char *TAG = "UDP_RX_THREAD";

static pthread_t g_udp_rx_thread;
static std::atomic<bool> g_udp_rx_running(false);

static void *udp_receiver_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    uint8_t scratch[RTP_MAX_PACKET_SIZE];

    while (g_udp_rx_running)
    {
        int n = udp_receiver_recv(scratch, sizeof(scratch));
        if (n <= 0)
        {
            // n < 0: transient recv error, or (during shutdown) the
            // socket was shut down/closed out from under us by
            // udp_receiver_thread_stop(). n == 0: a shutdown() call
            // (see udp_receiver_cleanup()) makes a pending recvfrom()
            // return 0 rather than an error - treat the same way.
            // Either case: re-check the running flag and exit cleanly
            // if it's now false.
            continue;
        }

        // Phase 19 (RTCP, rtcp-mux): the sender's rtcp_sender_thread
        // periodically injects SR packets into this same socket/port
        // (RFC 5761) instead of using a separate port. Recognize and
        // divert them here so they never reach the jitter buffer -
        // they use RTP/RTCP's version-2 header shape, not our H.264
        // payload format, and would otherwise be misparsed as a
        // corrupt RTP data packet.
        uint64_t arrival_us = time_utils_now_us();

        if (rtcp_is_sr(scratch, static_cast<size_t>(n)))
        {
            const rtcp_sr_t *sr = reinterpret_cast<const rtcp_sr_t *>(scratch);
            rtcp_receiver_stats_on_sr(ntohl(sr->ntp_sec_be), ntohl(sr->ntp_frac_be), arrival_us);
            LOG_INFO(TAG, "received RTCP SR");
            continue;
        }

        rtp_packet_t *packet = rtp_packet_pool_acquire();
        if (!packet)
        {
            LOG_WARN(TAG, "rtp packet pool empty, dropping datagram");
            continue;
        }

        if (rtp_depacketize_header(scratch, static_cast<size_t>(n), packet) < 0)
        {
            LOG_WARN(TAG, "malformed/truncated RTP packet (%d bytes), dropping", n);
            rtp_packet_pool_release(packet);
            continue;
        }

        rtcp_receiver_stats_on_packet(packet->sequence_number, packet->timestamp, arrival_us);

        // Ownership of `packet` passes to the jitter buffer here - it
        // either buffers it for rtp_depacketizer_thread to pop later,
        // or releases it back to the pool itself (duplicate/too late).
        rtp_jitter_buffer_push(packet);
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

int udp_receiver_thread_start(uint16_t listen_port)
{
    if (udp_receiver_init(listen_port) < 0)
    {
        return -1;
    }

    g_udp_rx_running = true;

    if (pthread_create(&g_udp_rx_thread, nullptr, udp_receiver_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        udp_receiver_cleanup();
        g_udp_rx_running = false;
        return -1;
    }

    return 0;
}

void udp_receiver_thread_stop(void)
{
    g_udp_rx_running = false;

    // Closing the socket is what actually unblocks a thread parked in
    // recvfrom() - unlike the other stages there's no queue/condvar to
    // signal here, the blocking call itself is the socket read.
    udp_receiver_cleanup();

    pthread_join(g_udp_rx_thread, nullptr);
}
