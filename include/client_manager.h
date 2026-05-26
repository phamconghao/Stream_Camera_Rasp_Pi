#ifndef __CLIENT_MANAGER_H__
#define __CLIENT_MANAGER_H__

#define MAX_CLIENTS 32

void client_manager_init(void);
int client_add(int fd);
void client_remove(int fd);
int client_get_all(int *fds);

#endif /* __CLIENT_MANAGER_H__ */