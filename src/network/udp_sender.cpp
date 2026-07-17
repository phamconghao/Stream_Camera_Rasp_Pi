#include "udp_sender.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

#include "log.h"

/**
 * Plain connectionless UDP: one socket() + one fixed destination
 * (set once in udp_sender_init) is all that's needed, since UDP has no
 * handshake/connection state. Each rtp_packet_t is one independent
 * datagram - no fragmentation/reassembly here (that's why
 * RTP_MAX_PAYLOAD_SIZE in rtp_packet.h is kept safely under the
 * Ethernet MTU).
 *
 * Destination is currently fixed/hardcoded (passed in from main.cpp's
 * command-line args) - a real RTSP server would instead learn the
 * actual client IP/port per-session from the RTSP SETUP request.
 */

static const char *TAG = "UDP";

static int g_socket_fd = -1;
static struct sockaddr_in g_dest_addr; // destination address, resolved once here and reused for every sendto()

int udp_sender_init(const char *dest_ip, uint16_t dest_port)
{
    g_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_socket_fd < 0)
    {
        LOG_ERROR(TAG, "socket() failed: %s", strerror(errno));
        return -1;
    }

    memset(&g_dest_addr, 0, sizeof(g_dest_addr));
    g_dest_addr.sin_family = AF_INET;
    g_dest_addr.sin_port = htons(dest_port);

    if (inet_pton(AF_INET, dest_ip, &g_dest_addr.sin_addr) != 1)
    {
        LOG_ERROR(TAG, "invalid destination IP: %s", dest_ip);
        close(g_socket_fd);
        g_socket_fd = -1;
        return -1;
    }

    LOG_INFO(TAG, "sending to %s:%u", dest_ip, dest_port);

    return 0;
}

// Fire-and-forget: UDP gives no delivery guarantee, so a failed/lost
// packet here just means one dropped RTP packet on the wire - normal
// and expected for real-time video (the receiver tolerates some loss;
// RTCP, not yet implemented, would report loss stats back).
int udp_sender_send(const uint8_t *data, size_t size)
{
    if (g_socket_fd < 0)
    {
        return -1;
    }

    ssize_t sent = sendto(g_socket_fd, data, size, 0,
                           reinterpret_cast<struct sockaddr *>(&g_dest_addr),
                           sizeof(g_dest_addr));

    if (sent < 0)
    {
        LOG_WARN(TAG, "sendto() failed: %s", strerror(errno));
        return -1;
    }

    return static_cast<int>(sent);
}

void udp_sender_cleanup(void)
{
    if (g_socket_fd >= 0)
    {
        close(g_socket_fd);
        g_socket_fd = -1;
    }
}
