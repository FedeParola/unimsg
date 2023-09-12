/*
 * Some sort of Copyright
 */

#ifndef __LIBUNIMSG_GWMAP__
#define __LIBUNIMSG_GWMAP__
#include <stddef.h>
struct conn;
struct fd_pair
{
    int hostfd;
    struct conn *connection;
};

#define MAX_CONNECTIONS 10
extern struct fd_pair fd_map[MAX_CONNECTIONS];

void initialize_fd_map();
void add_fd_pair(struct fd_pair *map, int hostfd, struct conn *connection);
void remove_fd_pair(struct fd_pair *map, int hostfd);
int get_hostfd(struct fd_pair *map, struct conn *connection);
struct conn* get_clientfd(struct fd_pair *map, int hostfd);
#endif /* __LIBUNIMSG_GWMAP__ */