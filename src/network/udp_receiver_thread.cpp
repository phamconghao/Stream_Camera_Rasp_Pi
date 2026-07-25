#include <pthread.h>
#include <cstdio>
#include <atomic>

#include "udp_receiver_thread.h"
#include "udp_receiver.h"
#include "rtp_packet_pool.h"
#include "rtp_depacketizer.h"
#include "rtp_jitter_buffer.h"
#include "log.h"

/**
 * ============================================================================
 * PIPELINE STAGE (first stage, receive side): Network -> [THIS] -> RTP Jitter Buffer
 * ============================================================================
 *
 * Loop, once per UDP datagram:
 *   1. Acquire a free rtp_packet_t from rtp_packet_pool.
 *   2. recvfrom() straight into a scratch buffer, then
 *      rtp_depacketize_header() parses the RTP header fields and
 *      copies the raw bytes into the packet.
 *   3. Push into rtp_jitter_buffer for sequence reordering - this
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
        if (n < 0)
        {
            // Either a transient recv error or (during shutdown) the
            // socket was closed out from under us by
            // udp_receiver_thread_stop() - either way, re-check the
            // running flag and exit cleanly if it's now false.
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
