/*
 * Some sort of Copyright
 */

#ifndef __LIBUNIMSG_LISTENING_SOCK__
#define __LIBUNIMSG_LISTENING_SOCK__

#include "../common/shm.h"

/**
 * Sets up access to the listening socks map in shared memory.
 * @param shmh Pointer to the shared memory header
 * @return 0 on success, a negative errno value otherwise
*/
void listen_sock_init(struct unimsg_shm *shm);

int listen_sock_lookup_acquire(uint32_t addr, uint16_t port,
			       struct listen_sock **s);

void listen_sock_release(struct listen_sock *s);

int listen_sock_send_conn(struct listen_sock *s, struct conn *c);

#endif /* __LIBUNIMSG_LISTENING_SOCK__ */