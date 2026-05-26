#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "stream_thread.h"
#include "shared_frame.h"
#include "frame_pool.h"
#include "client_manager.h"
#include "perf_stats.h"

#define PORT 8080
#define MAX_EVENTS 32

static volatile int running = 1;

static int server_fd = -1;

static int epoll_fd = -1;

/*
 * ============================================================
 *                      NON BLOCKING
 * ============================================================
 */

static int make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
    {
        perror("fcntl F_GETFL");
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("fcntl F_SETFL");
        return -1;
    }

    return 0;
}

/*
 * ============================================================
 *                      SEND ALL
 * ============================================================
 */

static int send_all(int fd, const void *buffer, size_t size)
{
    const char *ptr = buffer;

    while (size > 0)
    {
        ssize_t sent = send(fd,  ptr, size, MSG_NOSIGNAL);

        if (sent < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            if (errno == EAGAIN ||
                errno == EWOULDBLOCK)
            {
                return 0;
            }

            return -1;
        }

        ptr += sent;

        size -= sent;
    }

    return 0;
}

/*
 * ============================================================
 *                  REMOVE CLIENT
 * ============================================================
 */

static void remove_client(int client_fd)
{
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);

    client_remove(client_fd);

    close(client_fd);

    printf("[CLIENT DISCONNECTED] fd=%d\n", client_fd);
}

/*
 * ============================================================
 *                  SEND FRAME
 * ============================================================
 */

static int send_latest_frame(int client_fd)
{
    struct shared_frame frame;

    struct shared_frame latest;

    char part_header[256];

    int len;

    /*
     * Get latest frame
     */

    if (shared_frame_get(&frame) < 0)
    {
        return 0;
    }

    /*
     * Acquire ownership
     */

    frame_acquire(frame.buffer_index);

    /*
     * Check stale frame
     */

    if (shared_frame_get(&latest) == 0)
    {
        if (latest.frame_id != frame.frame_id)
        {
            printf("[DROP STALE FRAME] current=%lu latest=%lu\n",
                   frame.frame_id,
                   latest.frame_id);

            perf_frame_dropped();

            frame_release(frame.buffer_index);

            return 0;
        }
    }

    /*
     * Multipart header
     */

    len = snprintf(part_header,
                   sizeof(part_header),
                   "--frame\r\n"
                   "Content-Type: image/jpeg\r\n"
                   "Content-Length: %d\r\n\r\n",
                   frame.size);

    /*
     * Send multipart header
     */

    if (send_all(client_fd,
                 part_header,
                 len) < 0)
    {
        frame_release(frame.buffer_index);

        return -1;
    }

    /*
     * Send JPEG payload
     */

    if (send_all(client_fd,
                 frame.data,
                 frame.size) < 0)
    {
        frame_release(frame.buffer_index);

        return -1;
    }

    /*
     * Send multipart tail
     */

    if (send_all(client_fd,
                 "\r\n\r\n",
                 4) < 0)
    {
        frame_release(frame.buffer_index);

        return -1;
    }

    perf_frame_sent();

    frame_release(frame.buffer_index);

    return 0;
}

/*
 * ============================================================
 *                  STREAM THREAD
 * ============================================================
 */

void *stream_thread_func(void *arg)
{
    (void)arg;

    struct sockaddr_in addr;

    struct epoll_event ev;

    struct epoll_event events[MAX_EVENTS];

    client_manager_init();

    /*
     * Create server socket
     */

    server_fd = socket(AF_INET,
                       SOCK_STREAM,
                       0);

    if (server_fd < 0)
    {
        perror("socket");
        return NULL;
    }

    /*
     * Reuse address
     */

    int opt = 1;

    setsockopt(server_fd,
               SOL_SOCKET,
               SO_REUSEADDR,
               &opt,
               sizeof(opt));

    /*
     * Non-blocking socket
     */

    make_nonblocking(server_fd);

    /*
     * Bind
     */

    memset(&addr,
           0,
           sizeof(addr));

    addr.sin_family = AF_INET;

    addr.sin_addr.s_addr = INADDR_ANY;

    addr.sin_port = htons(PORT);

    if (bind(server_fd,
             (struct sockaddr *)&addr,
             sizeof(addr)) < 0)
    {
        perror("bind");

        close(server_fd);

        return NULL;
    }

    /*
     * Listen
     */

    if (listen(server_fd,
               32) < 0)
    {
        perror("listen");

        close(server_fd);

        return NULL;
    }

    printf("[STREAM SERVER] port=%d\n",
           PORT);

    /*
     * Create epoll
     */

    epoll_fd = epoll_create1(0);

    if (epoll_fd < 0)
    {
        perror("epoll_create1");

        close(server_fd);

        return NULL;
    }

    /*
     * Register server socket
     */

    ev.events = EPOLLIN;

    ev.data.fd = server_fd;

    if (epoll_ctl(epoll_fd,
                  EPOLL_CTL_ADD,
                  server_fd,
                  &ev) < 0)
    {
        perror("epoll_ctl");

        close(server_fd);

        close(epoll_fd);

        return NULL;
    }

    /*
     * Event loop
     */

    while (running)
    {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 100);

        if (nfds < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("epoll_wait");

            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;

            /*
             * New client
             */

            if (fd == server_fd)
            {
                while (1)
                {
                    int client_fd = accept(server_fd, NULL, NULL);

                    if (client_fd < 0)
                    {
                        if (errno == EAGAIN ||
                            errno == EWOULDBLOCK)
                        {
                            break;
                        }

                        perror("accept");

                        break;
                    }

                    printf("[NEW CLIENT] fd=%d\n", client_fd);

                    make_nonblocking(client_fd);

                    client_add(client_fd);

                    /*
                     * Send HTTP header
                     */

                    char header[] =
                        "HTTP/1.1 200 OK\r\n"
                        "Server: camera-server\r\n"
                        "Cache-Control: no-cache\r\n"
                        "Cache-Control: private\r\n"
                        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";

                    if (send_all(client_fd, header, strlen(header)) < 0)
                    {
                        close(client_fd);
                        continue;
                    }

                    /*
                     * Register client socket
                     */

                    struct epoll_event cev;

                    cev.events = EPOLLOUT;

                    cev.data.fd = client_fd;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev) < 0)
                    {
                        perror("epoll_ctl client");

                        close(client_fd);

                        continue;
                    }
                }
            }
            else
            {
                /*
                 * Writable client socket
                 */

                if (send_latest_frame(fd) < 0)
                {
                    remove_client(fd);
                }
            }
        }
    }

    /*
     * Cleanup
     */

    close(server_fd);

    close(epoll_fd);

    printf("[STREAM THREAD EXIT]\n");

    return NULL;
}

/*
 * ============================================================
 *                  STOP THREAD
 * ============================================================
 */

void stream_thread_stop(void)
{
    running = 0;

    if (server_fd >= 0)
    {
        close(server_fd);
    }

    if (epoll_fd >= 0)
    {
        close(epoll_fd);
    }
}