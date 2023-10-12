/*
 * Some sort of Copyright
 */

#include <stddef.h>
#include <stdio.h>
#include "gwmap.h"

struct fd_pair fd_map[MAX_CONNECTIONS];

void initialize_fd_map()
{
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		fd_map[i].hostfd = -1;
		fd_map[i].conn = NULL;
	}
}

void add_fd_pair(struct fd_pair *map, int hostfd, struct conn *conn,
		 enum conn_side local_side, int connected)
{
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (map[i].hostfd == -1 && map[i].conn == NULL) {
			map[i].hostfd = hostfd;
			map[i].conn = conn;
			map[i].local_side = local_side;
			map[i].connected = connected;
			return;
		}
	}
	printf("Map is full, cannot add new pair\n");
}

void remove_fd_pair(struct fd_pair *map, int hostfd)
{
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (map[i].hostfd == hostfd) {
			map[i].hostfd = -1;
			map[i].conn = NULL;
			return;
		}
	}
	printf("Pair not found in map\n");
}

struct fd_pair *get_pair_from_hostfd(struct fd_pair *map, int hostfd)
{
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (map[i].hostfd == hostfd)
			return &map[i];
	}

	return NULL;
}

struct fd_pair *get_pair_from_conn(struct fd_pair *map, struct conn *conn)
{
	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (map[i].conn == conn)
			return &map[i];
	}

	return NULL;
}