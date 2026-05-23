#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <time.h>

#include "stream_thread.h"
#include "client_thread.h"

extern volatile int running;

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

        printf("[NEW CLIENT]\n");

        int *pclient = malloc(sizeof(int));

        *pclient = client_fd;
        pthread_t tid;
        pthread_create(&tid, NULL, client_thread_func, pclient);

        pthread_detach(tid);
    }

    close(server_fd);

    return NULL;
}
