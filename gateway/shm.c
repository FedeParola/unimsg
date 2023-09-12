/*
 * Some sort of Copyright
 */

#include <stdlib.h>
#include "shm.h"

#define UNIMSG_BUFFER_HEADROOM 128

static struct unimsg_ring *pool;
static void *buffers;

void shm_init(struct unimsg_shm *control_shm, void *buffers_shm)
{
	pool = &control_shm->shm_pool.r;
	buffers = buffers_shm;
}

void *unimsg_buffer_get_addr(struct unimsg_shm_desc *desc)
{
	return desc->idx * UNIMSG_BUFFER_SIZE + (void *)buffers + desc->off;
}

int unimsg_buffer_get(struct unimsg_shm_desc *descs, unsigned ndescs)
{
	if (!descs || ndescs > UNIMSG_MAX_DESCS_BULK)
		return -EINVAL;
	if (ndescs == 0)
		return 0;

	unsigned idx[UNIMSG_MAX_DESCS_BULK];
	if (unimsg_ring_dequeue(pool, idx, ndescs))
		return -ENOMEM;
	for (unsigned i = 0; i < ndescs; i++) {
		descs[i].size = UNIMSG_BUFFER_SIZE - UNIMSG_BUFFER_HEADROOM;
		descs[i].off = UNIMSG_BUFFER_HEADROOM;
		descs[i].idx = idx[i];
		descs[i].addr = unimsg_buffer_get_addr(&descs[i]);
	}

	return 0;
}

int unimsg_buffer_put(struct unimsg_shm_desc *descs, unsigned ndescs)
{
	if (!descs || ndescs > UNIMSG_MAX_DESCS_BULK)
		return -EINVAL;
	if (ndescs == 0)
		return 0;

	unsigned idx[UNIMSG_MAX_DESCS_BULK];
	for (unsigned i = 0; i < ndescs; i++) {
		idx[i] = descs[i].idx;
		void *addr = (void *)buffers + idx[i] * UNIMSG_BUFFER_SIZE;
		memset(addr, 0, UNIMSG_BUFFER_SIZE);
	}

	if (unimsg_ring_enqueue(pool, idx, ndescs)) {
		fprintf(stderr, "Detected freeing of unknown shm buffer\n");
		exit(EXIT_FAILURE);
	}

	return 0;
}