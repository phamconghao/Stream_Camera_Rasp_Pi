#ifndef __CLIENT_MANAGER_H__
#define __CLIENT_MANAGER_H__

#include <stddef.h>

#define MAX_CLIENTS 32

struct client_state
{
    int fd;
    unsigned long last_frame_id;
    unsigned long pending_frame_id;
    unsigned char *out_buf;
    size_t out_size;
    size_t out_offset;
};

void client_manager_init(void);
int client_add(int fd);
void client_remove(int fd);
int client_get_all(int *fds);
struct client_state *client_get(int fd);

#endif /* __CLIENT_MANAGER_H__ */