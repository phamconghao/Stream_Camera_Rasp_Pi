#include <pthread.h>
#include <cstdio>
#include <atomic>

#include "udp_sender_thread.h"
#include "udp_sender.h"
#include "rtp_packet_pool.h"
#include "rtp_packet_queue.h"
#include "log.h"

static const char *TAG = "UDP_SENDER";

static pthread_t g_udp_thread;

// Owned exclusively by this module, same convention as encoder_thread
// and rtp_packetizer_thread: independent start/stop, no shared flag.
static std::atomic<bool> g_udp_running(false);

static void *udp_sender_thread_func(void *arg)
{
    (void)arg;

    LOG_INFO(TAG, "thread started");

    while (g_udp_running)
    {
        rtp_packet_t *packet = rtp_packet_queue_pop();
        if (!packet)
        {
            continue;
        }

        int sent = udp_sender_send(packet->data, packet->size);
        if (sent < 0)
        {
            LOG_WARN(TAG, "failed to send packet seq=%u", packet->sequence_number);
        }
        else
        {
            LOG_INFO(TAG, "sent seq=%u size=%d", packet->sequence_number, sent);
        }

        rtp_packet_pool_release(packet);
    }

    LOG_INFO(TAG, "thread exit");

    return nullptr;
}

int udp_sender_thread_start(const char *dest_ip, uint16_t dest_port)
{
    if (udp_sender_init(dest_ip, dest_port) < 0)
    {
        return -1;
    }

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
