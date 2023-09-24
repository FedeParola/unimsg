/*
 * Some sort of Copyright
 */

#include <errno.h>
#include <string.h>
#include "connection.h"
#include "jhash.h"
#include "listen_sock.h"
#include "signal.h"

static struct listen_sock_map *map;

/* Copied from Unikraft */
static inline void spin_lock(struct __spinlock *lock)
{
	register int locked = 1;

	__asm__ __volatile__(
		"1:	mov	(%0), %%eax\n"	/* read current value */
		"	test	%%eax, %%eax\n"	/* check if locked */
		"	jz	3f\n"		/* if not locked, try get it */
		"2:	pause\n"		/* is locked, hint spinning */
		"	jmp	1b\n"		/* retry */
		"3:	lock; cmpxchg %1, (%0)\n" /* try to acquire spinlock */
		"	jnz	2b\n"		/* if unsuccessful, retry */
		:
		: "r" (&lock->lock), "r" (locked)
		: "eax");
}

static inline void spin_unlock(struct __spinlock *lock)
{
	__atomic_store_n(&lock->lock, 0, __ATOMIC_SEQ_CST);
}

void listen_sock_init(struct unimsg_shm *shm)
{
	map = &shm->listen_sock_map;
}

int listen_sock_lookup_acquire(uint32_t addr, uint16_t port,
			       struct listen_sock **s)
{
	struct listen_sock_id key = {
		.addr = addr,
		.port = port,
	};

	struct bucket *bkt =
			&map->buckets[jhash(&key, sizeof(key), 0) % map->size];
	spin_lock(&bkt->lock);

	struct listen_sock *curr = NULL;
	unsigned next = bkt->head;
	while (next != map->size) {
		curr = &map->socks[next];
		if (!memcmp(&key, &curr->key, sizeof(key)))
			break;
		next = curr->next;
	}

	if (!curr || next == map->size) {
		spin_unlock(&bkt->lock);
		return -ENOENT;
	}

	curr->refcount++;
	spin_unlock(&bkt->lock);
	*s = curr;

	return 0;
}

static void listen_sock_free(struct listen_sock *s)
{
	/* Perform the socket cleanup, close all the pending connections
	 * in the backlog and free the socket
	 */
	unsigned cs_idx;
	while (!unimsg_ring_dequeue(&s->backlog, &cs_idx, 1))
		conn_close(conn_from_idx(cs_idx), CONN_SIDE_SRV);

	s->next = map->size;
	s->prev = map->size;
	s->bucket = map->size;
	s->key = (struct listen_sock_id){0};
	s->waiting_accept = 0;
	unimsg_ring_reset(&s->backlog);

	spin_lock(&map->freelist_lock);
	s->freelist_next = map->freelist_head;
	map->freelist_head = s - map->socks;
	spin_unlock(&map->freelist_lock);
}

void listen_sock_release(struct listen_sock *s)
{
	/* We still use the bucket's lock even if the socket might no longer be
	 * in the bucket
	 */
	struct bucket *bkt = &map->buckets[s->bucket];
	spin_lock(&bkt->lock);
	s->refcount--;
	spin_unlock(&bkt->lock);

	if (s->refcount == 0)
		listen_sock_free(s);	
}

int listen_sock_send_conn(struct listen_sock *s, struct conn *c)
{
	unsigned conn_idx = conn_get_idx(c);
	if (unimsg_ring_enqueue(&s->backlog, &conn_idx, 1))
		return -EAGAIN;

	unsigned long to_wake = __atomic_load_n(&s->waiting_accept,
						__ATOMIC_ACQUIRE);
	if (to_wake &&
	    __atomic_compare_exchange_n(&s->waiting_accept, &to_wake, NULL, 0,
					__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
		signal_send(s->key.addr, (struct signal *)&to_wake);

	return 0;
}