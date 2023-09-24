
/*
 * Some sort of Copyright
 */
#include <stddef.h>
#include <stdio.h>
#include "gwmap.h"

struct fd_pair fd_map[MAX_CONNECTIONS];

void initialize_fd_map() {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        fd_map[i].hostfd = -1;
        fd_map[i].connection = NULL;
    }
}

void add_fd_pair(struct fd_pair *map, int hostfd, struct conn *connection)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++)
    {
        if (map[i].hostfd == -1 && map[i].connection == NULL)
        {
            map[i].hostfd = hostfd;
            map[i].connection = connection;
            return;
        }
    }
    printf("Map is full, cannot add new pair\n");
}

void remove_fd_pair(struct fd_pair *map, int hostfd)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++)
    {
        if (map[i].hostfd == hostfd)
        {
            map[i].hostfd = -1;
            map[i].connection = NULL;
            return;
        }
    }
    printf("Pair not found in map\n");
}

int get_hostfd(struct fd_pair *map, struct conn *connection)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++)
    {
        if (map[i].connection == connection)
        {
            return map[i].hostfd;
        }
    }
    return -1;
}

struct conn* get_clientfd(struct fd_pair *map, int hostfd)
{
    for (int i = 0; i < MAX_CONNECTIONS; i++)
    {
        if (map[i].hostfd == hostfd)
        {
            return map[i].connection;
        }
    }
    return NULL;
}