/*
 * Some sort of Copyright
 */

#ifndef __SHM__
#define __SHM__

#include <stdint.h>
#include <unistd.h>
#include "ring.h"

#define UNIMSG_BUFFERS_PATH "/dev/hugepages/unimsg_buffers"
#define UNIMSG_MAX_VMS 16
#define PAGE_SIZE (4 * 1024)
#define UNIMSG_BUFFER_SIZE PAGE_SIZE
#define UNIMSG_BUFFERS_COUNT 2048
#define SIGNAL_QUEUE_SIZE 256
#define BACKLOG_QUEUE_SIZE 128
#define SOCK_QUEUE_SIZE 128
#define UNIMSG_MAX_LISTEN_SOCKS 1024
#define UNIMSG_MAX_CONNS 1024
#define UNIMSG_RT_SIZE 1024
#define UNIMSG_MAX_DESCS_BULK 16

struct signal {
	unsigned long target_thread;
};

struct signal_queue {
	int need_wakeup;
	struct unimsg_ring r;
	struct signal items[SIGNAL_QUEUE_SIZE];
};

struct backlog_queue {
	unsigned items[BACKLOG_QUEUE_SIZE];
	unsigned cons;
	unsigned prod;
	unsigned count;
	unsigned char available[BACKLOG_QUEUE_SIZE];
};

struct unimsg_shm_desc {
	void *addr;
	unsigned size;
	unsigned off;
	unsigned idx;
};

struct listen_sock_id {
	uint32_t addr;
	uint16_t port;
};

struct listen_sock {
	uint32_t next;
	uint32_t prev;
	uint32_t freelist_next;
	uint32_t bucket;
	unsigned refcount;
	struct listen_sock_id key;
	unsigned long waiting_accept;
	struct unimsg_ring backlog;
	unsigned items[BACKLOG_QUEUE_SIZE];
};

struct __spinlock {
	volatile int lock;
};

struct bucket {
	uint32_t head;
	struct __spinlock lock;
};

struct listen_sock_map {
	unsigned size;
	uint32_t freelist_head;
	struct __spinlock freelist_lock;
	struct bucket buckets[UNIMSG_MAX_LISTEN_SOCKS];
	struct listen_sock socks[UNIMSG_MAX_LISTEN_SOCKS];
};

enum conn_side {
	CONN_SIDE_CLI = 0,
	CONN_SIDE_SRV = 1,
};

struct conn_id {
	unsigned client_id;
	uint32_t client_addr;
	uint16_t client_port;
	unsigned server_id;
	uint32_t server_addr;
	uint16_t server_port;
};

typedef uint32_t idxpool_token_t;

struct sock_queue {
	struct unimsg_ring r;
	struct unimsg_shm_desc items[SOCK_QUEUE_SIZE];
};

struct conn {
	struct conn_id id;
	unsigned long waiting_recv[2];
	unsigned long waiting_send[2];
	char closing;
	struct sock_queue queues[2];
};

struct conn_pool {
	struct unimsg_ring r;
	unsigned items[UNIMSG_MAX_CONNS];
	struct conn conns[UNIMSG_MAX_CONNS];
};

struct unimsg_shm_header {
	unsigned long gw_backlog_off;
	unsigned long rt_off;
	unsigned long rt_sz;
	unsigned long signal_off;
	unsigned long signal_sz;
	unsigned long listen_sock_map_off;
	unsigned long listen_socks_off;
	unsigned long listen_sock_sz;
	unsigned long conn_pool_off;
	unsigned long conn_conns_off;
	unsigned long conn_sz;
	unsigned long conn_queue_sz;
	unsigned long shm_buffers_off;
};

struct shm_buffer_pool {
	struct unimsg_ring r;
	unsigned items[UNIMSG_BUFFERS_COUNT];
};

struct gw_backlog {
	struct unimsg_ring r;
	unsigned items[BACKLOG_QUEUE_SIZE];
};

struct route {
	uint32_t addr;
	unsigned peer_id;
};

/* TODO: use a hash table and handle concurrent updates and reads */
struct routing_table {
	struct route routes[UNIMSG_RT_SIZE];
};

struct unimsg_shm {
	struct unimsg_shm_header hdr;
	struct gw_backlog gw_backlog;
	struct routing_table rt;
	struct signal_queue signal_queues[UNIMSG_MAX_VMS];
	struct listen_sock_map listen_sock_map;
	struct conn_pool conn_pool;
	struct shm_buffer_pool shm_pool;
};

#endif