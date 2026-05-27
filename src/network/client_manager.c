#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "client_manager.h"

static struct client_state clients[MAX_CLIENTS];

void client_manager_init(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        clients[i].fd = -1;
        clients[i].last_frame_id = 0;
        clients[i].pending_frame_id = 0;
        clients[i].out_buf = NULL;
        clients[i].out_size = 0;
        clients[i].out_offset = 0;
    }
}

int client_add(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd == -1)
        {
            clients[i].fd = fd;
            clients[i].last_frame_id = 0;
            clients[i].pending_frame_id = 0;
            clients[i].out_buf = NULL;
            clients[i].out_size = 0;
            clients[i].out_offset = 0;

            printf("[CLIENT ADD] fd = %d\n", fd);
            return 0;
        }
    }

    return -1;
}

void client_remove(int fd)
{
    struct client_state *client = client_get(fd);
    if (!client)
    {
        return;
    }
    
    free(client->out_buf);
    client->fd = -1;
    client->last_frame_id = 0;
    client->pending_frame_id = 0;
    client->out_buf = NULL;
    client->out_size = 0;
    client->out_offset = 0;

    printf("[CLIENT REMOVE] fd = %d\n", fd);
}

int client_get_all(int *fds)
{
    int count = 0;

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd != -1)
        {
            fds[count++] = clients[i].fd;
        }
    }

    return count;
}

struct client_state *client_get(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].fd == fd)
        {
            return &clients[i];
        }
    }

    return NULL;
}