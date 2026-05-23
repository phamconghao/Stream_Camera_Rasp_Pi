#include "stream_thread.h"
#include "v4l2_capture.h"
#include "frame_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <pthread.h>

#define PORT 8080

static pthread_t stream_tid;
extern volatile int running;
extern struct frame_queue frame_q;

static int send_all(int fd, const void *buf, size_t size);

static void dump_jpeg_header(void *data)
{
    unsigned char *p = data;
    printf("[JPEG HEADER] %02X %02X %02X %02X\n", p[0], p[1], p[2], p[3]);
}

// ==============================================
//                  STREAM THREAD
// ==============================================
static void *stream_thread_func(void *arg)
{
    (void)arg;
    int server_fd;
    int client_fd;

    struct sockaddr_in addr;

    // ---------------------
    // Create socket
    // ---------------------
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        perror("socket");
        return NULL;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    // ---------------------
    // Bind 
    // ---------------------
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        return NULL;
    }

    // ---------------------
    // Listen 
    // ---------------------
    listen(server_fd, 5);
    printf("HTTP server listening on port %d\n", PORT);

    // ---------------------
    // SERVER LOOP 
    // ---------------------
    while (running)
    {
        printf("Waiting for browser...\n");
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }

        printf("Browser connected\n");

        char request[1024];
        int req_len = read(client_fd, request, sizeof(request) - 1);

        if (req_len <= 0)
        {
            close(client_fd);
            continue;
        }
        request[req_len] = '\0';

        printf("\n==== HTTP REQUEST ====\n");
        printf("%s\n", request);

        // Send HTTP header
        char header[] = 
            "HTTP/1.1 200 OK\r\n"
            "Server: camera-server\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-cache\r\n"
            "Cache-Control: private\r\n"
            "Pragma: no-cache\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
            "\r\n";
        
        printf("\n==== HTTP RESPONSE HEADER ====\n");
        printf("%s\n", header);

        if (send_all(client_fd, header, strlen(header)) > 0)
        {
            close(client_fd);
            continue;
        }
        printf("[HTTP HEADER SENT]\n");

        // ========================================
        // STREAM LOOP
        // ========================================
        while (running)
        {
            struct frame frame;

            // -------------------------------
            // Get frame from queue
            // -------------------------------
            queue_pop(&frame_q, &frame);
            printf("[QUEUE POP]\n");
            printf("ptr     = %p\n", frame.data);
            printf("size    = %d\n", frame.size);
            printf("index   = %d\n", frame.buffer_index);

            dump_jpeg_header(frame.data);
            char part_header[256];
            int len = sprintf(part_header,
                              "--frame\r\n"
                              "Content-Type: image/jpeg\r\n"
                              "\r\n");
            
            printf("\n==== FRAME HEADER ====\n");
            printf("%s\n", part_header);
            
            // Send header
            if (send_all(client_fd, part_header, len) < 0)
            {
                capture_release_frame(frame.buffer_index);
                printf("[ERROR] frame header send failed\n");
                break;
            }

            printf("[FRAME HEADER SENT]\n");

            // Send JPEG
            if (send_all(client_fd, frame.data, frame.size) < 0)
            {
                capture_release_frame(frame.buffer_index);
                printf("[ERROR] jpeg send failed\n");
                break;
            }
            
            printf("[JPEG SENT] size = %d\n", frame.size);

            // End frame
            if (send_all(client_fd, "\r\n", 2) < 0)
            {
                capture_release_frame(frame.buffer_index);
                printf("[ERROR] frame ending send failed\n");
                break;
            }

            printf("[FRAME END SENT]\n");

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double latency_ms = (now.tv_sec - frame.timestamp.tv_sec) * 1000.0 +
                                (now.tv_nsec - frame.timestamp.tv_nsec) / 1000000.0;

            printf("Latency: %.2f ms\n", latency_ms);
            capture_release_frame(frame.buffer_index);

            printf("[BUFFER RELEASED] index = %d\n", frame.buffer_index);

            
        }

        close(client_fd);
        printf("Client socket closed\n");

    }

    close(server_fd);

    return NULL;
}

// ==============================================
//                  START 
// ==============================================
int stream_thread_start(struct frame_queue *queue)
{
    frame_q = *queue;
    running = 1;

    if (pthread_create(&stream_tid, NULL, stream_thread_func, NULL) != 0)
    {
        perror("pthread_create");
        return -1;
    }

    return 0;
}

// ==============================================
//                  STOP 
// ==============================================
void stream_thread_stop()
{
    running = 0;
    pthread_join(stream_tid, NULL);
}

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

