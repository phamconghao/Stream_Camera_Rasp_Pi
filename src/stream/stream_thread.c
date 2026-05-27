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
 *                      CLIENT OUTPUT
 * ============================================================
 */

static int client_queue_output(struct client_state *client,
                               const void *buffer,
                               size_t size,
                               unsigned long frame_id)
{
    if (client->out_buf != NULL)
    {
        return 0;
    }

    client->out_buf = malloc(size);

    if (!client->out_buf)
    {
        return -1;
    }

    memcpy(client->out_buf, buffer, size);
    client->out_size = size;
    client->out_offset = 0;
    client->pending_frame_id = frame_id;

    return 0;
}

static int flush_client_output(struct client_state *client)
{
    while (client->out_buf != NULL &&
           client->out_offset < client->out_size)
    {
        ssize_t sent = send(client->fd,
                            client->out_buf + client->out_offset,
                            client->out_size - client->out_offset,
                            MSG_NOSIGNAL);

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

        if (sent == 0)
        {
            return -1;
        }

        client->out_offset += sent;
    }

    if (client->out_buf != NULL)
    {
        if (client->pending_frame_id != 0)
        {
            client->last_frame_id = client->pending_frame_id;
            perf_frame_sent();
        }

        free(client->out_buf);
        client->out_buf = NULL;
        client->out_size = 0;
        client->out_offset = 0;
        client->pending_frame_id = 0;
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

static int prepare_latest_frame(struct client_state *client)
{
    struct shared_frame frame;

    char part_header[256];

    int len;
    size_t total_size;
    unsigned char *out_buf;

    /*
     * Get latest frame
     */

    if (client->out_buf != NULL)
    {
        return 0;
    }

    if (shared_frame_get(&frame) < 0)
    {
        return 0;
    }

    if (frame.frame_id == client->last_frame_id)
    {
        frame_release(frame.buffer_index);
        return 0;
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

    if (len < 0 || (size_t)len >= sizeof(part_header))
    {
        frame_release(frame.buffer_index);

        return -1;
    }

    total_size = (size_t)len + (size_t)frame.size + 2;
    out_buf = malloc(total_size);

    if (!out_buf)
    {
        frame_release(frame.buffer_index);

        return -1;
    }

    memcpy(out_buf, part_header, (size_t)len);
    memcpy(out_buf + len, frame.data, (size_t)frame.size);
    memcpy(out_buf + len + frame.size, "\r\n", 2);

    client->out_buf = out_buf;
    client->out_size = total_size;
    client->out_offset = 0;
    client->pending_frame_id = frame.frame_id;

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

            if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
            {
                if (fd != server_fd)
                {
                    remove_client(fd);
                }

                continue;
            }

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

                    if (client_add(client_fd) < 0)
                    {
                        close(client_fd);
                        continue;
                    }

                    /*
                     * Send HTTP header
                     */

                    char header[] =
                        "HTTP/1.1 200 OK\r\n"
                        "Server: camera-server\r\n"
                        "Cache-Control: no-cache\r\n"
                        "Cache-Control: private\r\n"
                        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";

                    struct client_state *client = client_get(client_fd);

                    if (!client ||
                        client_queue_output(client, header, strlen(header), 0) < 0)
                    {
                        client_remove(client_fd);
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

                        client_remove(client_fd);
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

                struct client_state *client = client_get(fd);

                if (!client)
                {
                    remove_client(fd);
                    continue;
                }

                if (flush_client_output(client) < 0)
                {
                    remove_client(fd);
                    continue;
                }

                if (prepare_latest_frame(client) < 0)
                {
                    remove_client(fd);
                    continue;
                }

                if (flush_client_output(client) < 0)
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
        server_fd = -1;
    }

    if (epoll_fd >= 0)
    {
        close(epoll_fd);
        epoll_fd = -1;
    }
}
