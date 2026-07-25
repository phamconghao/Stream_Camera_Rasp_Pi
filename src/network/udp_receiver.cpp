#include "udp_receiver.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

#include "log.h"

/**
 * Plain connectionless UDP: one socket() bound to listen_port on
 * INADDR_ANY is all that's needed - RTP/UDP has no handshake, so
 * "receiving" is just repeated recvfrom() calls. The RTP source
 * address (sender's IP:port) is not tracked here since this pipeline
 * currently expects exactly one sender (mirrors udp_sender's single
 * fixed destination on the transmit side).
 */

static const char *TAG = "UDP_RX";

static int g_socket_fd = -1;

int udp_receiver_init(uint16_t listen_port)
{
    g_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_socket_fd < 0)
    {
        LOG_ERROR(TAG, "socket() failed: %s", strerror(errno));
        return -1;
    }

    // Allow quick restart of the receiver on the same port without
    // waiting out TIME_WAIT-style delays (mostly relevant for TCP, but
    // harmless and conventional to set here too).
    int reuse = 1;
    setsockopt(g_socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    listen_addr.sin_port = htons(listen_port);

    if (bind(g_socket_fd, reinterpret_cast<struct sockaddr *>(&listen_addr), sizeof(listen_addr)) < 0)
    {
        LOG_ERROR(TAG, "bind() failed on port %u: %s", listen_port, strerror(errno));
        close(g_socket_fd);
        g_socket_fd = -1;
        return -1;
    }

    LOG_INFO(TAG, "listening on 0.0.0.0:%u", listen_port);

    return 0;
}

int udp_receiver_recv(uint8_t *buffer, size_t max_size)
{
    if (g_socket_fd < 0)
    {
        return -1;
    }

    ssize_t n = recvfrom(g_socket_fd, buffer, max_size, 0, nullptr, nullptr);
    if (n < 0)
    {
        // Expected during shutdown: udp_receiver_cleanup() closes the
        // fd from another thread while this call is blocked, which
        // makes recvfrom() return EBADF - not logged as an error here,
        // udp_receiver_thread distinguishes "shutting down" from a real
        // failure via its own running flag.
        return -1;
    }

    return static_cast<int>(n);
}

void udp_receiver_cleanup(void)
{
    if (g_socket_fd >= 0)
    {
        // shutdown() before close() matters here: closing a fd that
        // another thread is currently blocked on inside recvfrom() does
        // NOT reliably wake that thread up on Linux for sockets (unlike
        // pipes) - it can leave the blocked call hung indefinitely if
        // the kernel-side socket object still has a pending waiter
        // registered from before the close. shutdown(SHUT_RDWR)
        // explicitly aborts any pending recv on this socket and is the
        // POSIX-correct way to unblock another thread's recvfrom() from
        // here; close() alone is not sufficient to guarantee that.
        shutdown(g_socket_fd, SHUT_RDWR);
        close(g_socket_fd);
        g_socket_fd = -1;
    }
}
