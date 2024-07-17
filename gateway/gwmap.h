/*
 * Some sort of Copyright
 */

#ifndef __LIBUNIMSG_GWMAP__
#define __LIBUNIMSG_GWMAP__

#include <stddef.h>
#include "../common/shm.h"

struct conn;
struct fd_pair {
	int hostfd;
	struct conn *conn;
	enum conn_side local_side;
	/* Whether the connection to the remote host is established or
	 * pending
	 */
	int connected;
	int remote_busy;
	/* Pending message to send to remote end */
	struct rte_mbuf *pending_remotes;
};

#define MAX_CONNECTIONS 512
extern struct fd_pair fd_map[MAX_CONNECTIONS];

void initialize_fd_map();
void add_fd_pair(struct fd_pair *map, int hostfd, struct conn *conn,
		 enum conn_side local_side, int connected);
void remove_fd_pair(struct fd_pair *map, int hostfd);
struct fd_pair *get_pair_from_hostfd(struct fd_pair *map, int hostfd);
struct fd_pair *get_pair_from_conn(struct fd_pair *map, struct conn *conn);

#endif /* __LIBUNIMSG_GWMAP__ */
