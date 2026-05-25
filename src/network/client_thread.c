#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

#include "client_thread.h"
#include "shared_frame.h"
#include "v4l2_capture.h"
#include "frame_pool.h"
#include "perf_stats.h"

extern volatile int running;

static int send_all(int fd, const void *buf, size_t size)
{
    size_t total = 0;
    const char *p = buf;

    while (total < size)
    {
        ssize_t n = write(fd, p + total, size - total);

        if (n <= 0)
        {
            return -1;
        }

        total += n;
    }

    return 0;
}

static void send_http_header(int client_fd)
{
    char hreader[] =
        "HTTP/1.1 200 OK\r\n"
        "Server: camera-server\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "\r\n";

    send_all(client_fd, hreader, strlen(hreader));
    
    printf("[HTTP HEADER SENT]\n");
}

void *client_thread_func(void *arg)
{
    int client_fd = *(int *)arg;

    free(arg);

    printf("[CLIENT THREAD START]\n");

    int flag = 1;

    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    send_http_header(client_fd);

    unsigned long last_frame_id = 0;

    while (running)
    {
        struct shared_frame frame;
        struct shared_frame latest;

        if (shared_frame_get(&frame) < 0)
        {
            usleep(100);
            continue;
        }

        if (frame.frame_id == last_frame_id)
        {
            frame_release(frame.buffer_index);
            usleep(1000);
            continue;
        }

        last_frame_id = frame.frame_id;

        char part_header[256];

        int len = sprintf(part_header,
                          "--frame\r\n"
                          "Content-Type: image/jpeg\r\n"
                          "Content-Length: %d\r\n"
                          "\r\n",
                          frame.size);
        
        if (shared_frame_get(&latest) == 0)
        {
            /**
             * New frame already arrived
             */
            if (latest.frame_id != frame.frame_id)
            {
                printf("[DROP STALE FRAME] current = %lu latest = %lu\n", frame.frame_id, latest.frame_id);
                frame_release(frame.buffer_index);
                frame_release(latest.buffer_index);
                perf_frame_dropped();
                continue;
            }

            frame_release(latest.buffer_index);
        }

        if (send_all(client_fd, part_header, len) < 0)
        {
            frame_release(frame.buffer_index);
            printf("[SEND FAILED]\n");
            goto disconnect;
        }

        if (send_all(client_fd, frame.data, frame.size) < 0)
        {
            frame_release(frame.buffer_index);
            printf("[SEND FAILED]\n");
            goto disconnect;
        }

        if (send_all(client_fd, "\r\n\r\n", 4) < 0)
        {
            frame_release(frame.buffer_index);
            printf("[SEND FAILED]\n");
            goto disconnect;
        }

        printf("[CLIENT] frame=%lu size=%d\n", frame.frame_id, frame.size);

        perf_frame_sent();

        struct timespec now;

        clock_gettime(CLOCK_MONOTONIC, &now);

        double latency_ms = (now.tv_sec - frame.timestamp.tv_sec) * 1000.0 
                            + (now.tv_nsec - frame.timestamp.tv_nsec) / 1000000.0;

        printf("[LATENCY]: %.2f ms\n", latency_ms);
        
        
        frame_release(frame.buffer_index);
    }

    printf("Browser disconnected\n");

    close(client_fd);

    return NULL;

disconnect:
    printf("[CLIENT DISCONNECTED]\n");
    close(client_fd);
    return NULL;
}