/*
 * Some sort of Copyright
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <ff_config.h>
#include <ff_api.h>
#include <ff_veth.h>
#include "connection.h"
#include "listen_sock.h"
#include "signal.h"
#include "shm.h"
#include "../common/error.h"
#include "../common/jhash.h"
#include "../common/shm.h"
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <sys/ioctl.h>
#include "gwmap.h"
#define GATEWAY_ID 0
#define IVSHMEM_PROT_VERSION 0
#define MGR_SOCK_PATH "/tmp/ivshmem_socket"
#define CONTROL_SHM_SIZE (sizeof(struct unimsg_shm))
#define DEFAULT_SIZE 64 
#define RAND_UPPER_LIMIT 2
#define RAND_LOWER_LIMIT 0
/* Max number of consecutive connections the gateway can accept from local
 * hosts before moving to other tasks
 */
#define ACCEPT_BURST 8

extern struct unimsg_ring *rte_mempool_unimsg_ring;
static int peers_fds[UNIMSG_MAX_VMS];
static struct unimsg_shm *control_shm;
static void *buffers_shm;
static pthread_t mgr_handler_t;
static volatile int stop = 0;

/* f-stack thread variables*/
int kq;
int sockfd;
#define MAX_EVENTS 512
struct kevent kevSet;
struct kevent events[MAX_EVENTS];
static int opt_size = DEFAULT_SIZE;
#define NB_SOCKETS 8
extern struct rte_mempool *pktmbuf_pool[NB_SOCKETS];

/* Setup Connection to multiple applications 
running on different port with own identity*/
struct app_ident{
	uint32_t adr; 
	uint16_t port;
}; 
struct app_ident app_idents[] = {
	{ .adr = 0x0100000a /* 10.0.0.1 in nbo */, .port = 5000 },
	{ .adr = 0x0200000a /* 10.0.0.2 in nbo */, .port = 6000 }, 
	{ .adr = 0x0300000a /* 10.0.0.3 in nbo */, .port = 7000 },
};

/* Cached mempool infos */
static unsigned long mp_base_addr;
static size_t mp_tot_esize;
static size_t mp_hdr_size;

static void ivshmem_client_read_one_msg(int sock_fd, int64_t *index, int *fd)
{
	int ret;
	struct msghdr msg;
	struct iovec iov[1];
	union {
		struct cmsghdr cmsg;
		char control[CMSG_SPACE(sizeof(int))];
	} msg_control;
	struct cmsghdr *cmsg;

	iov[0].iov_base = index;
	iov[0].iov_len = sizeof(*index);

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_control = &msg_control;
	msg.msg_controllen = sizeof(msg_control);

	ret = recvmsg(sock_fd, &msg, 0);
	if (ret == 0)
		SYSERROR("Lost connection to manager");
	else if (ret < sizeof(*index))
		SYSERROR("Error receiving data from manager");

	*fd = -1;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_len != CMSG_LEN(sizeof(int))
		    || cmsg->cmsg_level != SOL_SOCKET
		    || cmsg->cmsg_type != SCM_RIGHTS)
			continue;

		memcpy(fd, CMSG_DATA(cmsg), sizeof(*fd));
	}
}

void *mgr_notification_handler(void *arg)
{
	int rc;
	int sock = (int)(unsigned long)arg;

	struct pollfd pfd = {0};
	pfd.fd = sock;
	pfd.events = POLLIN;

	while (!stop) {
		rc = poll(&pfd, 1, 1000);
		if (rc < 0)
			SYSERROR("Error polling manager connection");
		if (rc == 0)
			continue;

		if (pfd.revents & ~POLLIN)
			ERROR("Error polling manager connection");

		if (!(pfd.revents & POLLIN))
			continue;

		int64_t val;
		int fd;
		ivshmem_client_read_one_msg(sock, &val, &fd);
		if (fd > 0) {
			/* New peer */
			if (peers_fds[val] != 0) {
				/* Unimsg allows a single fd per peer */
				ERROR("Notification for existing peer");
			}

			peers_fds[val] = fd;
			printf("Peer %ld registered\n", val);
		
		} else {
			/* Peer disconnected */
			peers_fds[val] = 0;
			printf("Peer %ld unregistered\n", val);
		}
	}

	close(sock);
}

static void *setup_manager_connection()
{
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0)
		SYSERROR("Error creating socket");

	printf("Connecting to Unimsg manager...\n");

	struct sockaddr_un addr;
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, MGR_SOCK_PATH);
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)))
		SYSERROR("Error connecting to manager");

	printf("Connected to Unimsg manager\n");
	printf("Receiving configuration...\n");

	int64_t val;
	int aux_fd;

	/* Receive protocol version IVSHMEM_PROT_VERSION */
	ivshmem_client_read_one_msg(sock, &val, &aux_fd);
	if (val != IVSHMEM_PROT_VERSION)
		ERROR("Received unexpected procol version %lu", val);

	/* Receive id GATEWAY_ID */
	ivshmem_client_read_one_msg(sock, &val, &aux_fd);
	if (val != GATEWAY_ID)
		ERROR("Expected id %d, received %ld", GATEWAY_ID, val);

	/* Receive control shm fd */
	ivshmem_client_read_one_msg(sock, &val, &aux_fd);
	if (val != -1 || aux_fd < 0)
		ERROR("Error receiving control shm");

	void *shm = mmap(0, CONTROL_SHM_SIZE, PROT_READ | PROT_WRITE,
			 MAP_SHARED, aux_fd, 0);
	if (!shm)
		SYSERROR("Error mapping control shm");

	/* Receive interrupt configuration */
	ivshmem_client_read_one_msg(sock, &val, &aux_fd);
	if (val != GATEWAY_ID)
		ERROR("Gateway should be the first connecting to manager");
	if (aux_fd < 0)
		ERROR("Error receiving interrupt configuration");

	peers_fds[val] = aux_fd;

	/* Create a thread to handle nofitications from manager */
	if (pthread_create(&mgr_handler_t, NULL, mgr_notification_handler,
			   (void *)(unsigned long)sock))
		SYSERROR("Error creating thread");

	printf("Configuration completed\n");
	printf("Manager connection established\n");

	return shm;
}

static void *get_buffers_shm()
{
	int fd = open(UNIMSG_BUFFERS_PATH, O_RDWR);
	if (fd < 0)
		SYSERROR("Error opening unimsg buffers file");

	void *shm = mmap(0, UNIMSG_BUFFER_SIZE * UNIMSG_BUFFERS_COUNT,
			 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (!shm)
		SYSERROR("Error mapping unimsg buffers");

	close(fd);

	/* Read a byte to map the first page in the page table */
	*(volatile char *)shm;

	return shm;
}

static int peer_lookup(uint32_t addr, unsigned *peer_id)
{
	unsigned id = control_shm->vms_info.rt_buckets[
		jhash(&addr, sizeof(addr), 0) % UNIMSG_MAX_VMS];
	while (id != UNIMSG_MAX_VMS) {
		if (control_shm->vms_info.vm_info[id].addr == addr)
			break;
		id = control_shm->vms_info.vm_info[id].rt_bkt_next;
	}

	if (id == UNIMSG_MAX_VMS)
		return -ENOENT;

	*peer_id = id;

	return 0;
}

static int connect_to_peer(uint32_t addr, uint16_t port, struct conn **c)
{
	int rc;

	unsigned peer_id;
	if (peer_lookup(addr, &peer_id))
		return -ENETUNREACH;

	struct listen_sock *ls;
	rc = listen_sock_lookup_acquire(addr, port, &ls);
	if (rc)
		return rc;

	struct conn_id id;
	/* TODO: replace with addr and port of remote client */
	id.client_id = 0;
	id.client_addr = 100; /* Addr of the remote client */
	id.client_port = 100; /* Port of the remote client */
	id.server_id = peer_id;
	id.server_addr = addr;
	id.server_port = port;
	rc = conn_alloc(c, &id);
	if (rc)
		goto err_release_ls;

	rc = listen_sock_send_conn(ls, *c, peer_id);
	if (rc)
		goto err_free_conn;

	listen_sock_release(ls);

	return 0;

err_free_conn:
	conn_free(*c);
err_release_ls:
	listen_sock_release(ls);
	return rc;
}

static int accept_local_connection()
{
	int rc;
	unsigned idx;

	rc = unimsg_ring_dequeue(&control_shm->gw_backlog.r, &idx, 1);
	if (rc == -EAGAIN)
		return rc;
	if (rc)
		ERROR("Error accepting local  connection: %s\n", strerror(-rc));

	struct conn *c = conn_from_idx(idx);

	int sockfd = ff_socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
		SYSERROR("Error creating socket");

	int on = 1;
	if (ff_ioctl(sockfd, FIONBIO, &on) < 0)
		SYSERROR("Error setting non blocking");

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(c->id.server_port);
	addr.sin_addr.s_addr = c->id.server_addr;

	/* Start connection establishment to remote peer */
	if (ff_connect(sockfd, (struct linux_sockaddr *)&addr, sizeof(addr))
	    < 0) {
		if (errno != EINPROGRESS)
			SYSERROR("Error connecting to remote host");
	}

	/* A WRITE event notifies connection established */
	struct kevent kev;
	EV_SET(&kev, sockfd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
	if (ff_kevent(kq, &kev, 1, NULL, 0, NULL) < 0)
		SYSERROR("Error updating kevents");

	add_fd_pair(fd_map, sockfd, c, CONN_SIDE_SRV, 0);

	return 0;
}

static unsigned buffer_get_idx(void *addr)
{
	return (addr - buffers_shm) / UNIMSG_BUFFER_SIZE;
}

static void *buffer_get_addr(struct unimsg_shm_desc *desc)
{
	desc->addr = buffers_shm + UNIMSG_BUFFER_SIZE * desc->idx + desc->off;
	return desc->addr;
}

static unsigned buffer_get_offset(struct unimsg_shm_desc *desc)
{
	desc->off = (unsigned long)desc->addr % UNIMSG_BUFFER_SIZE;
	
	return desc->off;
}

static void cache_mempool_infos()
{
	struct rte_mempool *mp = pktmbuf_pool[rte_socket_id()];

	struct rte_mempool_memhdr *memhdr = STAILQ_FIRST(&mp->mem_list);
	if (!memhdr)
		ERROR("Cannot find base addr of mempool");

	mp_base_addr = (unsigned long)memhdr->addr;
	mp_tot_esize = mp->header_size + mp->elt_size + mp->trailer_size;
	mp_hdr_size = mp->header_size;
}

static struct rte_mbuf *get_rte_mb_from_buffer(struct unimsg_shm_desc *desc)
{
	struct rte_mbuf *mb = (struct rte_mbuf *)(desc->idx * mp_tot_esize
						  + mp_hdr_size + mp_base_addr);

	rte_pktmbuf_reset(mb);
	mb->data_off = desc->off;
	mb->data_len = desc->size;
	mb->pkt_len = mb->data_len;

	return mb;
}

static void print_msg(char *msg, unsigned length)
{
	for (unsigned i = 0; i < length; i++)
		printf("%c", msg[i]);
}

int loop(void *arg)
{
	/* Wait for events to happen */
	int nevents = ff_kevent(kq, NULL, 0, events, MAX_EVENTS, NULL);
	if (nevents < 0)
		SYSERROR("Error waiting for events");

	for (int i = 0; i < nevents; ++i) {
		struct kevent event = events[i];
		int clientfd = (int)event.ident;

		if (event.flags & EV_EOF) {
			/* Handle disconnect */
			ff_close(clientfd);
			struct fd_pair *pair = get_pair_from_hostfd(fd_map,
								    clientfd);
			conn_close(pair->conn, pair->local_side);
			remove_fd_pair(fd_map, clientfd);

		} else if (clientfd == sockfd) {
			int available = (int)event.data;
			do {
				int nclientfd = ff_accept(clientfd, NULL, NULL);
				if (nclientfd < 0) {
					printf("ff_accept failed:%d, %s\n",
					       errno, strerror(errno));
					break;
				}

				/* Add to event list */
				EV_SET(&kevSet, nclientfd, EVFILT_READ, EV_ADD,
				       0, 0, NULL);

				if (ff_kevent(kq, &kevSet, 1, NULL, 0, NULL)
				    < 0) {
					printf("ff_kevent error:%d, %s\n",
					       errno, strerror(errno));
					return -1;
				}

				available--;
				/* Random selection of radiobox server*/
				// int rd = (rand() % (RAND_UPPER_LIMIT - RAND_LOWER_LIMIT + 1)) + RAND_LOWER_LIMIT;
				int rd = 0;
				/* Initiate connection to radiobox server */ 
				struct conn *cn;
				int ret = connect_to_peer(app_idents[rd].adr,
							  app_idents[rd].port,
							  &cn);
				if (ret) {
					ERROR("connect_to_peer failed: %s\n",
					      strerror(-ret));
					continue;
				}	
				/* Add the pair to the map */
				add_fd_pair(fd_map, nclientfd, cn,
					    CONN_SIDE_CLI, 1);

			} while (available);

		} else if (event.filter == EVFILT_READ) {
			void *mb;
			/* TODO: replace size limit with mb number limit, 3072
			 * is just a raw estimate of the space available in a
			 * shm buffer
			 */
			ssize_t readlen = ff_read(clientfd, &mb,
						  UNIMSG_MAX_DESCS_BULK * 3072);

			struct fd_pair *pair = get_pair_from_hostfd(fd_map,
								    clientfd);
			if (!pair)
				ERROR("No connection found\n");

			/* Map mbuf chain to shm descriptors array */
			struct unimsg_shm_desc descs[UNIMSG_MAX_DESCS_BULK];
			unsigned ndescs = 0;
			void *curr = mb;
			while (curr) {
				void *next = curr;
				ff_next_mbuf(&next, &descs[ndescs].addr,
					     &descs[ndescs].size);
				descs[ndescs].idx =
					buffer_get_idx(descs[ndescs].addr);
				buffer_get_offset(&descs[ndescs]);
				ndescs++;

				ff_mbuf_detach_rte(curr);

				curr = next;
			}
			/* This frees the whole mbuf chain */
			ff_mbuf_free(mb);

			int rc = conn_send(pair->conn, descs, ndescs,
					   pair->local_side);
			if (rc) {
				if (rc == -ECONNRESET) {
					unimsg_buffer_put(descs, ndescs);
					ff_close(clientfd);
					conn_close(pair->conn,
						   pair->local_side);
					remove_fd_pair(fd_map, clientfd);
				} else if (rc == -EAGAIN) {
					/* The ring is full, we drop the packet
					 * and let TCP handle retransmission
					 */
					unimsg_buffer_put(descs, ndescs);
				} else {
					unimsg_buffer_put(descs, ndescs);
					ERROR("Error sending desc: %s\n",
					      strerror(-rc));
				}
			}

		} else if (event.filter == EVFILT_WRITE) {
			/* Connection to remote host established */
			struct kevent kevs[2];
			EV_SET(&kevs[0], clientfd, EVFILT_WRITE, EV_DELETE, 0,
			       0, NULL);
			EV_SET(&kevs[1], clientfd, EVFILT_READ, EV_ADD, 0, 0,
			       NULL);
			if (ff_kevent(kq, kevs, 2, NULL, 0, NULL) < 0)
				SYSERROR("Error updating kevents");

			struct fd_pair *pair = get_pair_from_hostfd(fd_map,
								    clientfd);
			if (!pair)
				ERROR("No connection found");
			pair->connected = 1;

		} else {
			printf("unknown event: %8.8X\n", event.flags);
		}
	}

	/* Handle new connections from local hosts */
	for (int i = 0; i < ACCEPT_BURST; i++) {
		if (accept_local_connection())
			break;
	}

	for (int i = 0; i < MAX_CONNECTIONS; i++) {
		if (fd_map[i].hostfd == -1 || !fd_map[i].connected)
			continue;

		/* Read from the connection */
		struct conn *c = fd_map[i].conn;
		enum conn_side side = fd_map[i].local_side;
		struct unimsg_shm_desc desc[UNIMSG_MAX_DESCS_BULK];
		unsigned ndescs = UNIMSG_MAX_DESCS_BULK;
		int rc;
		rc = conn_recv(c, desc, &ndescs, side);
		if (rc) {
			if (rc == -ECONNRESET) {
				ff_close(fd_map[i].hostfd);
				conn_close(c, side);
				remove_fd_pair(fd_map, fd_map[i].hostfd);
				continue;
			} else if (rc == -EAGAIN) {
				/* If rc == -EAGAIN the peer has nothing to send
				 * and we can proceed with the next iteration of
				 * the loop
				 */
				continue;
			} else {
				ERROR("Error sending desc: %s\n",
				      strerror(-rc));
			}
		}

		/* Chain the messages */
		void *head = NULL, *tail = NULL;
		size_t tot_len = 0;
		for (unsigned k = 0; k < ndescs; k++) {
			char *msg = buffer_get_addr(&desc[k]);
			tot_len += desc[k].size;

			struct rte_mbuf *m = get_rte_mb_from_buffer(&desc[k]);
			if (!m)
				ERROR("Cannot map shm buffer to rte_mbuf\n");

			tail = ff_mbuf_get(tail, (void *)m, msg, m->data_len);
			if (!head)
				head = tail;
		}

		/* Send the chain */
		if (ff_write(fd_map[i].hostfd, head, tot_len) < 0)
			SYSERROR("Error sending msg to remote host");
	}
}

static void sigint_handler(int signum)
{
	stop = 1;
}

void gateway_start()
{
	/* f-stack configuration */
	kq = ff_kqueue();
	if (kq < 0)
		SYSERROR("Error creating kqueue");

	sockfd = ff_socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0)
		SYSERROR("Error creating socket");
	
	/* Set non blocking */
	int on = 1;
	if (ff_ioctl(sockfd, FIONBIO, &on) < 0)
		SYSERROR("Error setting non blocking");
	
	struct sockaddr_in my_addr;
	bzero(&my_addr, sizeof(my_addr));
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(80);
	my_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (ff_bind(sockfd, (struct linux_sockaddr *)&my_addr, sizeof(my_addr))
	    < 0)
		SYSERROR("Error binding socket");

	if (ff_listen(sockfd, MAX_EVENTS) < 0)
		SYSERROR("Error listening socket");

	EV_SET(&kevSet, sockfd, EVFILT_READ, EV_ADD, 0, 0, NULL);
	if (ff_kevent(kq, &kevSet, 1, NULL, 0, NULL) < 0)
		SYSERROR("Error registering kevent");

}

int main(int argc, char *argv[])
{
	control_shm = setup_manager_connection();
	buffers_shm = get_buffers_shm();

	/* Configure the ring for the rte mempool */
	/* TODO: this is awful, find a best API to set the ring */
	rte_mempool_unimsg_ring = &control_shm->shm_pool.r;

	ff_init(argc, argv, buffers_shm, UNIMSG_BUFFERS_COUNT,
		UNIMSG_BUFFER_SIZE);

	cache_mempool_infos();
	
	shm_init(control_shm, buffers_shm);
	signal_init(control_shm, peers_fds);
	conn_init(control_shm);
	listen_sock_init(control_shm);

	/* It looks like F-stack doesn't provide a mechanism to cleanly exit the
	 * main loop, let Ctrl-C terminate the app directly for now
	 */
	// struct sigaction sigact = { .sa_handler = sigint_handler };
	// if (sigaction(SIGINT, &sigact, NULL))
	// 	SYSERROR("Error setting SIGINT handler");

	/* intialize the fd_map*/
	initialize_fd_map();
	gateway_start();
	ff_run(loop, NULL);	
	pthread_join(mgr_handler_t, NULL);

	return 0;
}