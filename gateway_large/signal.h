/*
 * Some sort of Copyright
 */

#ifndef __LIBUNIMSG_SIGNAL__
#define __LIBUNIMSG_SIGNAL__

#include "../common/shm.h"

void signal_init(struct unimsg_shm *shm, int *fds);
void signal_send(unsigned vm_id, struct signal *signal);

#endif /* __LIBUNIMSG_SIGNAL__ */