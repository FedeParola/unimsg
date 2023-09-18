/*
 * Some sort of Copyright
 */

#ifndef __LIBUNIMSG_CONNECTION__
#define __LIBUNIMSG_CONNECTION__

#include "../common/shm.h"

void conn_init(struct unimsg_shm *shm);

unsigned conn_get_idx(struct conn *c);

struct conn *conn_from_idx(unsigned idx);

int conn_alloc(struct conn **c, struct conn_id *id);

void conn_free(struct conn *c);

void conn_close(struct conn *c, enum conn_side side);

int conn_send(struct conn *c, struct unimsg_shm_desc *descs, unsigned ndescs,
	      enum conn_side side);

int conn_recv(struct conn *c, struct unimsg_shm_desc *descs, unsigned *ndescs,
	      enum conn_side side);

#endif /* __LIBUNIMSG_CONNECTION__ */