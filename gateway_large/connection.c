/*
 * Some sort of Copyright
 */

#include "connection.h"
#include "signal.h"
#include "shm.h"

static struct unimsg_ring *pool;
static struct conn *conns;

unsigned conn_get_idx(struct conn *c)
{
	return c - conns;
}

struct conn *conn_from_idx(unsigned idx)
{
	return &conns[idx];
}

void conn_init(struct unimsg_shm *shm)
{
	pool = &shm->conn_pool.r;
	conns = shm->conn_pool.conns;
}

int conn_alloc(struct conn **c, struct conn_id *id)
{
	unsigned idx;
	if (unimsg_ring_dequeue(pool, &idx, 1))
		return -ENOMEM;

	*c = conn_from_idx(idx);
	(*c)->id = *id;

	return 0;
}

static void drain_ring(struct unimsg_ring *r)
{
	struct unimsg_shm_desc desc;

	while (!unimsg_ring_dequeue(r, &desc, 1))
		unimsg_buffer_put(&desc, 1);

	unimsg_ring_reset(r);
}

void conn_free(struct conn *c)
{
	drain_ring(&c->queues[0].r);
	drain_ring(&c->queues[1].r);

	c->id = (struct conn_id){0};
	c->waiting_recv[0] = 0;
	c->waiting_recv[1] = 0;
	c->waiting_send[0] = 0;
	c->waiting_send[1] = 0;
	c->closing = 0;

	unsigned idx = conn_get_idx(c);
	unimsg_ring_enqueue(pool, &idx, 1);
}

void conn_close(struct conn *c, enum conn_side side)
{
	/* Flag the socket as closing, if it already was we need to free it,
	 * otherwise me might need to wake a waiting recv
	 */
	char expected = 0;
	if (__atomic_compare_exchange_n(&c->closing, &expected, 1, 0,
					__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
		unsigned peer_id = side == CONN_SIDE_CLI ?
				   c->id.server_addr : c->id.client_addr;

		/* Wake potental waiting recv */
		unsigned long to_wake = __atomic_load_n(&c->waiting_recv[side],
							__ATOMIC_SEQ_CST);
		if (to_wake)
			signal_send(peer_id, (struct signal *)&to_wake);

		/* Wake potental waiting send */
		to_wake = __atomic_load_n(&c->waiting_send[side ^ 1],
					  __ATOMIC_SEQ_CST);
		if (to_wake)
			signal_send(peer_id, (struct signal *)&to_wake);

	} else {
		conn_free(c);
	}
}

int conn_send(struct conn *c, struct unimsg_shm_desc *descs, unsigned ndescs,
	      enum conn_side side)
{
	if (c->closing)
		return -ECONNRESET;

	int queue = side;
	struct unimsg_ring *r = &c->queues[queue].r;

	if (unimsg_ring_enqueue(r, descs, ndescs))
		return -EAGAIN;

	unsigned long to_wake = __atomic_load_n(&c->waiting_recv[queue],
						__ATOMIC_SEQ_CST);
	if (to_wake) {
		__atomic_store_n(&c->waiting_recv[queue], NULL,
				 __ATOMIC_SEQ_CST);
		signal_send(side == CONN_SIDE_CLI ?
			    c->id.server_addr : c->id.client_addr,
			    (struct signal *)&to_wake);
	}

	return 0;
}

static unsigned dequeue_burst(struct unimsg_ring *r,
			      struct unimsg_shm_desc *descs, unsigned n)
{
	unsigned dequeued = 0;
	while (dequeued < n) {
		if (unimsg_ring_dequeue(r, &descs[dequeued], 1))
			return dequeued;
		dequeued++;
	}

	return dequeued;
}

int conn_recv(struct conn *c, struct unimsg_shm_desc *descs, unsigned *ndescs,
	      enum conn_side side)
{
	/* Flip the direction on the recv side */
	int queue = side ^ 1;
	struct unimsg_ring *r = &c->queues[queue].r;

	unsigned dequeued = dequeue_burst(r, descs, *ndescs);
	if (dequeued == 0) {
		if (c->closing)
			return -ECONNRESET;
		return -EAGAIN;
	}

	unsigned long to_wake = __atomic_load_n(&c->waiting_send[queue],
						__ATOMIC_SEQ_CST);
	if (to_wake) {
		__atomic_store_n(&c->waiting_send[queue], NULL,
				 __ATOMIC_SEQ_CST);
		signal_send(side == CONN_SIDE_CLI ?
			    c->id.server_addr : c->id.client_addr,
			    (struct signal *)&to_wake);
	}

	*ndescs = dequeued;

	return 0;
}