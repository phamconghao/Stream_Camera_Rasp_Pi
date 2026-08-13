#include <pthread.h>
#include <cstdio>
#include <atomic>

#include "udp_sender_thread.h"
#include "udp_sender.h"
#include "rtp_packet.h"
#include "rtp_packet_pool.h"
#include "rtp_packet_queue.h"
#include "log.h"

/**
 * ============================================================================
 * PIPELINE STAGE (final stage): RTP Packet Queue -> [THIS] -> Network -> RTSP/RTP Client
 * ============================================================================
 *
 * The simplest thread in the pipeline: pop a finished RTP packet, hand
 * its raw bytes to udp_sender_send() (plain sendto()), release it back
 * to the pool. No re-transmission, no ACKs - this is UDP, loss is
 * expected and tolerated by the video pipeline as a whole.
 */

static const char *TAG = "UDP_SENDER";

static pthread_t g_udp_thread;

// Owned exclusively by this module, same convention as encoder_thread
// and rtp_packetizer_thread: independent start/stop, no shared flag.
static std::atomic<bool> g_udp_running(false);

// Phase 19 (RTCP): cumulative counts rtcp_sender_thread reads to fill
// in an SR's packet_count/octet_count fields (RFC 3550 6.4.1 - octet
// count excludes the RTP header, payload bytes only). Reset only at
// udp_sender_thread_start(), matching "since this session began"
// semantics real RTCP expects.
static std::atomic<uint32_t> g_packets_sent(0);
static std::atomic<uint32_t> g_octets_sent(0);

static void *udp_sender_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    while (g_udp_running)
    {
        // Blocks until rtp_packetizer_thread pushes a packet, or
        // returns nullptr once rtp_packet_queue_shutdown() has fired.
        rtp_packet_t *packet = rtp_packet_queue_pop();
        if (!packet)
        {
            continue;
        }

        // udp_sender_send() fans this one packet out to every
        // currently-PLAYING session's destination and returns how many
        // of those sendto() calls succeeded. 0 is not an error here -
        // it just means the pipeline is momentarily running with no
        // registered destination yet (e.g. between pipeline_controller
        // starting it and handle_play() calling udp_sender_add_dest() -
        // see rtsp_server.cpp) - so packets/octets counters, which feed
        // RTCP SR fields, only advance on an actual delivery.
        int dests_ok = udp_sender_send(packet->data, packet->size);
        if (dests_ok < 0)
        {
            LOG_WARN(TAG, "failed to send packet seq=%u", packet->sequence_number);
        }
        else if (dests_ok > 0)
        {
            LOG_INFO(TAG, "sent seq=%u size=%d to %d dest(s)", packet->sequence_number, packet->size, dests_ok);
            g_packets_sent++;
            g_octets_sent += static_cast<uint32_t>(packet->size - RTP_HEADER_SIZE);
        }

        rtp_packet_pool_release(packet);
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

// Opens the (destination-less, for now) UDP socket (see udp_sender_init)
// and THEN spawns the thread - if the socket fails to open, this
// returns -1 without starting a thread that would have nothing to send
// with. Destinations are added afterwards, per RTSP session, via
// udp_sender_add_dest() (called from rtsp_server.cpp's handle_play()).
int udp_sender_thread_start(void)
{
    if (udp_sender_init() < 0)
    {
        return -1;
    }

    g_packets_sent = 0;
    g_octets_sent = 0;
    g_udp_running = true;

    if (pthread_create(&g_udp_thread, nullptr, udp_sender_thread_func, nullptr) != 0)
    {
        perror("pthread_create");
        udp_sender_cleanup();
        g_udp_running = false;
        return -1;
    }

    return 0;
}

void udp_sender_thread_stop(void)
{
    g_udp_running = false;

    // Wake the thread up if it's blocked waiting for a packet, otherwise
    // pthread_join below would hang forever once the RTP packetizer
    // thread has stopped producing packets.
    rtp_packet_queue_shutdown();

    pthread_join(g_udp_thread, nullptr);

    udp_sender_cleanup();
}

void udp_sender_thread_get_stats(uint32_t *out_packets_sent, uint32_t *out_octets_sent)
{
    if (out_packets_sent)
    {
        *out_packets_sent = g_packets_sent;
    }

    if (out_octets_sent)
    {
        *out_octets_sent = g_octets_sent;
    }
}
