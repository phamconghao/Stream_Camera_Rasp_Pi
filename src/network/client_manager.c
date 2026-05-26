#include <stdio.h>
#include <string.h>

#include "client_manager.h"

static int clients[MAX_CLIENTS];

void client_manager_init(void)
{
    memset(clients, -1, sizeof(clients));
}

int client_add(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i] == -1)
        {
            clients[i] = fd;

            printf("[CLIENT ADD] fd = %d\n", fd);
            return 0;
        }
    }

    return -1;
}

void client_remove(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i] == fd)
        {
            clients[i] = -1;

            printf("[CLIENT REMOVE] fd = %d\n", fd);
            return;
        }
            
    }
}

int client_get_all(int *fds)
{
    int count = 0;

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i] != -1)
        {
            fds[count++] = clients[i];
        }
    }

    return count;
}