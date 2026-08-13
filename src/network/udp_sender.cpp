#include "udp_sender.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <cstring>
#include <cerrno>
#include <map>
#include <vector>
#include <utility>

#include "log.h"

/**
 * Plain connectionless UDP: one socket() shared by every destination -
 * UDP has no handshake/connection state, so fanning out to N clients
 * is just N sendto() calls on the same fd, not N sockets.
 *
 * Destination set is keyed by RTSP session_id and guarded by g_lock,
 * since it's mutated from RTSP connection threads (handle_play/
 * handle_teardown in rtsp_server.cpp) and the orphan reaper thread,
 * while being read from udp_sender_thread's send loop and
 * rtcp_sender_thread concurrently. See udp_sender.h for the full
 * fan-out picture.
 */

static const char *TAG = "UDP";

static int g_socket_fd = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::map<std::string, struct sockaddr_in> g_dests; // session_id -> destination address

int udp_sender_init(void)
{
    g_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_socket_fd < 0)
    {
        LOG_ERROR(TAG, "socket() failed: %s", strerror(errno));
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    g_dests.clear();
    pthread_mutex_unlock(&g_lock);

    LOG_INFO(TAG, "socket opened (destinations added per-session)");

    return 0;
}

int udp_sender_add_dest(const std::string &session_id, const char *dest_ip, uint16_t dest_port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(dest_port);

    if (inet_pton(AF_INET, dest_ip, &addr.sin_addr) != 1)
    {
        LOG_ERROR(TAG, "invalid destination IP for session %s: %s", session_id.c_str(), dest_ip);
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    g_dests[session_id] = addr;
    size_t count = g_dests.size();
    pthread_mutex_unlock(&g_lock);

    LOG_INFO(TAG, "added dest for session %s -> %s:%u (%zu destination(s) now)",
             session_id.c_str(), dest_ip, dest_port, count);

    return 0;
}

bool udp_sender_remove_dest(const std::string &session_id)
{
    pthread_mutex_lock(&g_lock);
    size_t removed = g_dests.erase(session_id);
    size_t count = g_dests.size();
    pthread_mutex_unlock(&g_lock);

    if (removed > 0)
    {
        LOG_INFO(TAG, "removed dest for session %s (%zu destination(s) left)", session_id.c_str(), count);
    }

    return removed > 0;
}

size_t udp_sender_dest_count(void)
{
    pthread_mutex_lock(&g_lock);
    size_t count = g_dests.size();
    pthread_mutex_unlock(&g_lock);
    return count;
}

// Fire-and-forget per destination: UDP gives no delivery guarantee, so
// a failed/lost packet to any one client just means one dropped RTP
// packet on the wire for THAT client - normal and expected for
// real-time video, and does not affect delivery to the others.
int udp_sender_send(const uint8_t *data, size_t size)
{
    if (g_socket_fd < 0)
    {
        return -1;
    }

    // Snapshot the destination list under the lock, then send outside
    // it - sendto() on a connectionless UDP socket can't block on a
    // peer, but there's no reason to hold g_lock (and therefore stall
    // udp_sender_add_dest()/udp_sender_remove_dest() from RTSP threads)
    // for the duration of N syscalls.
    pthread_mutex_lock(&g_lock);
    std::vector<std::pair<std::string, struct sockaddr_in>> dests(g_dests.begin(), g_dests.end());
    pthread_mutex_unlock(&g_lock);

    int ok_count = 0;

    for (const auto &entry : dests)
    {
        ssize_t sent = sendto(g_socket_fd, data, size, 0,
                               reinterpret_cast<const struct sockaddr *>(&entry.second),
                               sizeof(entry.second));

        if (sent < 0)
        {
            LOG_WARN(TAG, "sendto() failed for session %s: %s", entry.first.c_str(), strerror(errno));
        }
        else
        {
            ok_count++;
        }
    }

    return ok_count;
}

void udp_sender_cleanup(void)
{
    if (g_socket_fd >= 0)
    {
        close(g_socket_fd);
        g_socket_fd = -1;
    }

    pthread_mutex_lock(&g_lock);
    g_dests.clear();
    pthread_mutex_unlock(&g_lock);
}
