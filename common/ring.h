/*
 * Some sort of Copyright
 */

#ifndef __LIBUNIMSG_RING__
#define __LIBUNIMSG_RING__

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define CACHE_LINE_SIZE 64
#define __cache_aligned __attribute__((aligned(CACHE_LINE_SIZE)))

#define UNIMSG_RING_F_SP 0x1
#define UNIMSG_RING_F_SC 0x2

#define MASK (r->size - 1)

struct unimsg_ring_headtail {
	volatile uint32_t head;
	volatile uint32_t tail;
};

struct unimsg_ring {
	unsigned size;
	unsigned esize;
	int flags;
	char pad0 __cache_aligned;
	struct unimsg_ring_headtail prod __cache_aligned;
	char pad1 __cache_aligned;
	struct unimsg_ring_headtail cons __cache_aligned;
	char pad2 __cache_aligned;
	char objs[] __cache_aligned;
};

static __always_inline int
unimsg_ring_enqueue_sp(struct unimsg_ring *r, const void *objs, unsigned n)
{
	uint32_t cons = __atomic_load_n(&r->cons.tail, __ATOMIC_SEQ_CST /*__ATOMIC_ACQUIRE*/);
	if (r->prod.tail - cons + n > r->size)
		return -EAGAIN;

	char *firstobj = r->objs + (r->prod.tail & MASK) * r->esize;
	memcpy(firstobj, objs, n * r->esize);

	__atomic_store_n(&r->prod.tail, r->prod.tail + n, __ATOMIC_SEQ_CST /*__ATOMIC_RELEASE*/);

	return 0;
}

static __always_inline int
unimsg_ring_enqueue_mp(struct unimsg_ring *r, const void *objs, unsigned n)
{
	uint32_t old_head, cons;

	old_head = __atomic_load_n(&r->prod.head, __ATOMIC_RELAXED);
	do {
		cons = __atomic_load_n(&r->cons.tail, __ATOMIC_SEQ_CST /*__ATOMIC_ACQUIRE*/);
		if (old_head - cons + n > r->size)
			return -EAGAIN;
	} while (!__atomic_compare_exchange_n(&r->prod.head, &old_head,
					      old_head + n, 0, __ATOMIC_SEQ_CST,
					      __ATOMIC_SEQ_CST));

	char *firstobj = r->objs + (old_head & MASK) * r->esize;
	memcpy(firstobj, objs, n * r->esize);

	while (__atomic_load_n(&r->prod.tail, __ATOMIC_RELAXED) != old_head)
		__builtin_ia32_pause();

	__atomic_store_n(&r->prod.tail, r->prod.tail + n, __ATOMIC_SEQ_CST /*__ATOMIC_RELEASE*/);

	return 0;
}

static __always_inline int
unimsg_ring_enqueue(struct unimsg_ring *r, const void *objs, unsigned n)
{
	if (r->flags & UNIMSG_RING_F_SP)
		return unimsg_ring_enqueue_sp(r, objs, n);
	else
		return unimsg_ring_enqueue_mp(r, objs, n);
}

static __always_inline int
unimsg_ring_dequeue_sc(struct unimsg_ring *r, void *objs, unsigned n)
{
	uint32_t prod = __atomic_load_n(&r->prod.tail, __ATOMIC_SEQ_CST /*__ATOMIC_ACQUIRE*/);
	if (prod - r->cons.tail < n)
		return -EAGAIN;

	char *firstobj = r->objs + (r->cons.tail & MASK) * r->esize;
	memcpy(objs, firstobj, n * r->esize);

	__atomic_store_n(&r->cons.tail, r->cons.tail + n, __ATOMIC_SEQ_CST /*__ATOMIC_RELEASE*/);

	return 0;
}

static __always_inline int
unimsg_ring_dequeue_mc(struct unimsg_ring *r, void *objs, unsigned n)
{
	uint32_t old_head, prod;

	old_head = __atomic_load_n(&r->cons.head, __ATOMIC_RELAXED);
	do {
		prod = __atomic_load_n(&r->prod.tail, __ATOMIC_SEQ_CST /*__ATOMIC_ACQUIRE*/);
		if (prod - old_head < n)
			return -EAGAIN;
	} while (!__atomic_compare_exchange_n(&r->cons.head, &old_head,
					      old_head + n, 0, __ATOMIC_SEQ_CST,
					      __ATOMIC_SEQ_CST));

	char *firstobj = r->objs + (old_head & MASK) * r->esize;
	memcpy(objs, firstobj, n * r->esize);

	while (__atomic_load_n(&r->cons.tail, __ATOMIC_RELAXED) != old_head)
		__builtin_ia32_pause();

	__atomic_store_n(&r->cons.tail, r->cons.tail + n, __ATOMIC_SEQ_CST /*__ATOMIC_RELEASE*/);

	return 0;
}

static __always_inline int
unimsg_ring_dequeue(struct unimsg_ring *r, void *objs, unsigned n)
{
	if (r->flags & UNIMSG_RING_F_SC)
		return unimsg_ring_dequeue_sc(r, objs, n);
	else
		return unimsg_ring_dequeue_mc(r, objs, n);
}

static inline void unimsg_ring_reset(struct unimsg_ring *r)
{
	r->cons = (struct unimsg_ring_headtail){0};
	r->prod = (struct unimsg_ring_headtail){0};
}

static inline size_t unimsg_ring_objs_memsize(const struct unimsg_ring *r)
{
	return r->esize * r->size;
}

static inline size_t unimsg_ring_memsize(const struct unimsg_ring *r)
{
	return sizeof(*r) + r->esize * r->size;
}

#undef MASK

#endif /* __LIBUNIMSG_RING__ */
