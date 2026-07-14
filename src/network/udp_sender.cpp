#include "udp_sender.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

#include "log.h"

static const char *TAG = "UDP";

static int g_socket_fd = -1;
static struct sockaddr_in g_dest_addr;

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
