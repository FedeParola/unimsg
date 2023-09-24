/*
 * Some sort of Copyright
 */

#include "signal.h"
#include "../common/error.h"

static struct signal_queue *signal_queues;
static int *peers_fds;

void signal_init(struct unimsg_shm *shm, int *fds)
{
	signal_queues = shm->signal_queues;
	peers_fds = fds;
}

void signal_send(unsigned vm_id, struct signal *signal)
{
	struct signal_queue *q = &signal_queues[vm_id];
	while (unimsg_ring_enqueue(&q->r, signal, 1));

	int need_wakeup = __atomic_load_n(&q->need_wakeup, __ATOMIC_SEQ_CST);
	if (need_wakeup &&
	    __atomic_compare_exchange_n(&q->need_wakeup, &need_wakeup, 0, 0,
					__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
		uint64_t val = 1;
		if (write(peers_fds[vm_id], &val, sizeof(val)) != sizeof(val))
			SYSERROR("Error sending signal to VM %u\n", vm_id);
	}
}