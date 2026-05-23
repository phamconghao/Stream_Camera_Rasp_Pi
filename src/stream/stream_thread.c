#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <time.h>

#include "stream_thread.h"
#include "frame_queue.h"
#include "v4l2_capture.h"

extern volatile int running;

extern struct frame_queue frame_q;

static int send_all(int fd,
                    const void *buf,
                    size_t size)
{
    size_t total = 0;

    const char *p = buf;

    while (total < size)
    {
        ssize_t n =
            write(fd,
                  p + total,
                  size - total);

        if (n <= 0)
        {
            perror("write");

            return -1;
        }

        total += n;
    }

    return 0;
}

static void dump_jpeg_header(void *data)
{
    unsigned char *p = data;

    printf("[JPEG HEADER] %02X %02X %02X %02X\n",
           p[0],
           p[1],
           p[2],
           p[3]);
}

void *stream_thread_func(void *arg)
{
    (void)arg;

    int server_fd =
        socket(AF_INET,
               SOCK_STREAM,
               0);

    int opt = 1;

    setsockopt(server_fd,
               SOL_SOCKET,
               SO_REUSEADDR,
               &opt,
               sizeof(opt));

    struct sockaddr_in addr;

    memset(&addr,
           0,
           sizeof(addr));

    addr.sin_family = AF_INET;

    addr.sin_port = htons(8080);

    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd,
             (struct sockaddr *)&addr,
             sizeof(addr)) < 0)
    {
        perror("bind");

        return NULL;
    }

    if (listen(server_fd,
               5) < 0)
    {
        perror("listen");

        return NULL;
    }

    printf("Waiting for browser...\n");

    while (running)
    {
        int client_fd =
            accept(server_fd,
                   NULL,
                   NULL);

        if (client_fd < 0)
        {
            perror("accept");

            continue;
        }

        printf("Browser connected\n");

        char request[1024];

        int req_len =
            read(client_fd,
                 request,
                 sizeof(request) - 1);

        if (req_len <= 0)
        {
            close(client_fd);

            continue;
        }

        request[req_len] = '\0';

        printf("\n=== HTTP REQUEST ===\n");
        printf("%s\n",
               request);

        char header[] =
            "HTTP/1.1 200 OK\r\n"
            "Server: camera-server\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-cache\r\n"
            "Pragma: no-cache\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
            "\r\n";

        if (send_all(client_fd,
                     header,
                     strlen(header)) < 0)
        {
            close(client_fd);

            continue;
        }

        printf("[HTTP HEADER SENT]\n");

        while (running)
        {
            struct frame frame;

            queue_pop(&frame_q,
                      &frame);

            printf("[QUEUE POP] ptr=%p size=%d index=%d\n",
                   frame.data,
                   frame.size,
                   frame.buffer_index);

            dump_jpeg_header(frame.data);

            char part_header[256];

            int len = sprintf(part_header,
                        "--frame\r\n"
                        "Content-Type: image/jpeg\r\n"
                        "Content-Length: %d\r\n"
                        "\r\n",
                        frame.size);

            if (send_all(client_fd,
                         part_header,
                         len) < 0)
            {
                capture_release_frame(
                    frame.buffer_index);

                break;
            }

            if (send_all(client_fd,
                         frame.data,
                         frame.size) < 0)
            {
                capture_release_frame(
                    frame.buffer_index);

                break;
            }

            if (send_all(client_fd,
                         "\r\n\r\n",
                         4) < 0)
            {
                capture_release_frame(
                    frame.buffer_index);

                break;
            }

            printf("[FRAME SENT] size=%d\n",
                   frame.size);

            struct timespec now;

            clock_gettime(CLOCK_MONOTONIC, &now);

            double latency_ms = (now.tv_sec - frame.timestamp.tv_sec) * 1000.0 
                                + (now.tv_nsec - frame.timestamp.tv_nsec) / 1000000.0;

            printf("Latency: %.2f ms\n", latency_ms);

            capture_release_frame(frame.buffer_index);
        }

        printf("Browser disconnected\n");

        close(client_fd);
    }

    close(server_fd);

    return NULL;
}
