/*
 * Some sort of Copyright
 */

#ifndef __LIBUNIMSG_SHM__
#define __LIBUNIMSG_SHM__

#include "../common/shm.h"

void shm_init(struct unimsg_shm *control_shm, void *buffers_shm);

void *unimsg_buffer_get_addr(struct unimsg_shm_desc *desc);

int unimsg_buffer_get(struct unimsg_shm_desc *descs, unsigned ndescs);

int unimsg_buffer_put(struct unimsg_shm_desc *descs, unsigned ndescs);

#endif /* __LIBUNIMSG_SHM__ */