/*
 * Some sort of Copyright
 */

#include <fcntl.h>
#include <getopt.h>
#include <linux/limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include "../common/error.h"
#include "../common/jhash.h"
#include "../common/shm.h"

#define DEFAULT_UNIX_SOCK_PATH	 "/tmp/ivshmem_socket"
#define DEFAULT_SHM_PATH 	 "/unimsg_control"
#define DEFAULT_SHM_SIZE	 (8 * 1024 * 1024)
#define SERVER_LISTEN_BACKLOG	 10
#define IVSHMEM_PROTOCOL_VERSION 0
#define SIDECAR_MAX_FILTERS 	 16
#define SIDECAR_SHM_PATH 	 "/unimsg_sidecar_%u"

enum sidecar_verdict {
	SIDECAR_OK,
	SIDECAR_DROP,
};

enum sidecar_filter {
	SIDECAR_FILTER_END,
	REQUEST_LOG_HANDLER,
	HTTP_SPAN_MIDDLEWARE,
	REQUEST_METRIC_HANDLER,
	NEW_TIMEOUT_HANDLER,
	FORWARD_SHIM_HANDLER,
	PROXY_HANDLER,
	REQUEST_APP_METRIC_HANDLER,
	CONCURRENCY_STATE_HANDLER,
	SIDECAR_FILTER_MAX,
};

struct sidecar_config {
	int rx_filetrs_chain[SIDECAR_MAX_FILTERS];
	int tx_filetrs_chain[SIDECAR_MAX_FILTERS];
};

struct sidecar_stats {
	unsigned long request_count;
	unsigned long response_time_ms;
	unsigned long app_request_count;
	unsigned long app_response_time_ms;
	unsigned long queue_depth;
};

struct sidecar_shm {
	struct sidecar_config config;
	struct sidecar_stats stats;
};

struct vm_data {
	int id;
	int sock_fd;
	int event_fd;
	int has_sidecar; /* 0 - to check; 1 - yes; 2 - no */
};

static struct unimsg_shm *shm;
struct vm_data vms[UNIMSG_MAX_VMS];
struct sidecar_shm *sidecar_shms[UNIMSG_MAX_VMS];
static int sock_fd;
static int shm_fd;
static volatile int running = 1;
static struct option long_options[] = {
	{"burst", required_argument, 0, 'b'},
	{"sleep", required_argument, 0, 's'},
	{"threads", required_argument, 0, 't'},
	{0, 0, 0, 0}
};

static void sigint_handler(int signum)
{
	running = 0;
}

static void usage(const char *prog)
{
	// ERROR("  Usage: %s [OPTIONS]\n"
	//       "  Options:\n"
	//       "  -b, --burst	Number of decriptors to send in a burst (default %d)\n"
	//       "  -s, --sleep	Microseconds to sleep between consecutive transmissions (default %d)\n"
	//       "  -t, --threads	Number of threads to use (default %d)\n",
	//       prog, DEFAULT_BURST, DEFAULT_SLEEP, DEFAULT_THREADS);

	exit(EXIT_FAILURE);
}

static void parse_command_line(int argc, char **argv)
{
	// int option_index, c;

	// for (;;) {
	// 	c = getopt_long(argc, argv, "b:s:t:", long_options, &option_index);
	// 	if (c == -1) {
	// 		break;
	// 	}

	// 	switch (c) {
	// 	case 'b':
	// 		opt_burst = atoi(optarg);
	// 		break;
	// 	case 's':
	// 		opt_sleep = atoi(optarg);
	// 		break;
	// 	case 't':
	// 		opt_threads = atoi(optarg);
	// 		break;
	// 	default:
	// 		usage(argv[0]);
	// 	}
	// }
}

static void shm_init(struct unimsg_shm *shm)
{
	/* Fill the header */
	shm->hdr.vms_info_off = (void *)&shm->vms_info - (void *)shm;
	shm->hdr.vms_info_sz = UNIMSG_MAX_VMS;
	shm->hdr.gw_backlog_off = (void *)&shm->gw_backlog - (void *)shm;
	shm->hdr.signal_off = (void *)&shm->signal_queues - (void *)shm;
	shm->hdr.signal_sz = sizeof(struct signal);
	shm->hdr.listen_sock_map_off =
		(void *)&shm->listen_sock_map - (void *)shm;
	shm->hdr.listen_socks_off =
		(void *)&shm->listen_sock_map.socks - (void *)shm;
	shm->hdr.listen_sock_sz = sizeof(struct listen_sock);
	shm->hdr.conn_pool_off = (void *)&shm->conn_pool - (void *)shm;
	shm->hdr.conn_conns_off = (void *)&shm->conn_pool.conns - (void *)shm;
	shm->hdr.conn_sz = sizeof(struct conn);
	shm->hdr.conn_queue_sz = sizeof(struct sock_queue);
	shm->hdr.shm_buffers_off = (void *)&shm->shm_pool - (void *)shm;

	/* Initialize routing table */
	for (unsigned i = 0; i < UNIMSG_MAX_VMS; i++)
		shm->vms_info.rt_buckets[i] = UNIMSG_MAX_VMS;

	/* Initialize GW backlog */
	struct unimsg_ring *r = &shm->gw_backlog.r;
	r->esize = sizeof(unsigned);
	r->size = BACKLOG_QUEUE_SIZE;
	r->flags = UNIMSG_RING_F_SC;

	/* Initialize signals */
	for (int i = 0; i < UNIMSG_MAX_VMS; i++) {
		shm->signal_queues[i].r.esize = sizeof(struct signal);
		shm->signal_queues[i].r.size = SIGNAL_QUEUE_SIZE;
		shm->signal_queues[i].r.flags = UNIMSG_RING_F_SC;
	}

	/* Initialize listen sock */
	shm->listen_sock_map.size = UNIMSG_MAX_LISTEN_SOCKS;
	shm->listen_sock_map.freelist_head = 0;
	for (int i = 0; i < UNIMSG_MAX_LISTEN_SOCKS; i++) {
		struct unimsg_ring *r = &shm->listen_sock_map.socks[i].backlog;
		r->esize = sizeof(unsigned);
		r->size = BACKLOG_QUEUE_SIZE;
		r->flags = UNIMSG_RING_F_SC;
		shm->listen_sock_map.socks[i].freelist_next = i + 1;
		shm->listen_sock_map.buckets[i].head = UNIMSG_MAX_LISTEN_SOCKS;
	}

	/* Initialize connections */
	shm->conn_pool.r.esize = sizeof(unsigned);
	shm->conn_pool.r.size = UNIMSG_MAX_CONNS;
	shm->conn_pool.r.flags = 0;
	for (unsigned i = 0; i < UNIMSG_MAX_CONNS; i++) {
		unimsg_ring_enqueue(&shm->conn_pool.r, &i, 1);
		shm->conn_pool.conns[i].queues[0].r.esize
				= sizeof(struct unimsg_shm_desc);
		shm->conn_pool.conns[i].queues[1].r.esize
				= sizeof(struct unimsg_shm_desc);
		shm->conn_pool.conns[i].queues[0].r.size = SOCK_QUEUE_SIZE;
		shm->conn_pool.conns[i].queues[1].r.size = SOCK_QUEUE_SIZE;
		shm->conn_pool.conns[i].queues[0].r.flags
				= UNIMSG_RING_F_SP | UNIMSG_RING_F_SC;
		shm->conn_pool.conns[i].queues[1].r.flags
				= UNIMSG_RING_F_SP | UNIMSG_RING_F_SC;
	}

	/* Initialize the shm buffer pool */
	shm->shm_pool.r.esize = sizeof(unsigned);
	shm->shm_pool.r.size = UNIMSG_BUFFERS_COUNT;
	shm->shm_pool.r.flags = 0;
	for (unsigned i = 0; i < UNIMSG_BUFFERS_COUNT; i++)
		unimsg_ring_enqueue(&shm->shm_pool.r, &i, 1);
}

static int shm_register_vm(struct vms_info *info, unsigned id, uint32_t addr)
{
	info->vm_info[id].addr = addr;

	/* Update the routing table */
	unsigned *bucket = &info->rt_buckets[jhash(&addr, sizeof(addr), 0)
					     % UNIMSG_MAX_VMS];
	unsigned curr_id = *bucket;
	while (curr_id != UNIMSG_MAX_VMS) {
		if (info->vm_info[curr_id].addr == addr)
			return -EEXIST;
		curr_id = info->vm_info[curr_id].rt_bkt_next;
	}

	info->vm_info[id].rt_bkt_next = *bucket;
	*bucket = id;

	return 0;
}

static void shm_unregister_vm(struct vms_info *info, unsigned id)
{
	uint32_t addr = info->vm_info[id].addr;

	/* Remove the VM from the routing table bucket */
	unsigned *bucket = &info->rt_buckets[jhash(&addr, sizeof(addr), 0)
					     % UNIMSG_MAX_VMS];
	unsigned curr_id = *bucket;
	if (curr_id == id) {
		*bucket = info->vm_info[id].rt_bkt_next;

	} else {
		/* Look for the preceding element in the bucket and make it
		 * point to the successor
		 */
		while (curr_id != UNIMSG_MAX_VMS
		       && info->vm_info[curr_id].rt_bkt_next != id)
			curr_id = info->vm_info[curr_id].rt_bkt_next;

		if (curr_id == UNIMSG_MAX_VMS)
			ERROR("Unregistering unknown VM");

		info->vm_info[curr_id].rt_bkt_next
			= info->vm_info[id].rt_bkt_next;
	}
}

static void init_sidecar_shm(unsigned id)
{
	char shm_path[PATH_MAX];
	sprintf(shm_path, SIDECAR_SHM_PATH, id);

	int shm_fd = shm_open(shm_path, O_RDWR, S_IRWXU);
	if (shm_fd < 0) {
		if (errno == ENOENT) {
			vms[id].has_sidecar = 2;
			return;
		} else {
			SYSERROR("Error opening sidecar shared memory");
		}
	}

	struct sidecar_shm *shm =
		(struct sidecar_shm *)mmap(0, sizeof(*shm),
					   PROT_READ | PROT_WRITE, MAP_SHARED,
					   shm_fd, 0);
	if (!shm)
		SYSERROR("Error mapping sidecar shared memory");

	sidecar_shms[id] = shm;

	close(shm_fd);
}

static void scrape_sidecar_stats()
{
	struct sidecar_shm *shm;

	printf("Sidecar stats:\n");
	for (int id = 1; id < UNIMSG_MAX_VMS; id++) {
		if (vms[id].id == -1 || vms[id].has_sidecar == 2)
			continue;

		if (vms[id].has_sidecar == 0) {
			init_sidecar_shm(id);
			if (vms[id].has_sidecar == 2)
				continue;
		}

		shm = sidecar_shms[id];

		printf("%u: request count = %lu, response time ms = %lu, "
		       "app request count = %lu, app response time ms = %lu, "
		       "queue depth = %lu\n", id, shm->stats.request_count,
		       shm->stats.response_time_ms,
		       shm->stats.app_request_count,
		       shm->stats.app_response_time_ms, shm->stats.queue_depth);
	}

	printf("\n");
}

static int ivshmem_server_sendmsg(int sock_fd, int64_t peer_id, int fd)
{
	int ret;
	struct msghdr msg;
	struct iovec iov[1];
	union {
		struct cmsghdr cmsg;
		char control[CMSG_SPACE(sizeof(int))];
	} msg_control;
	struct cmsghdr *cmsg;

	iov[0].iov_base = &peer_id;
	iov[0].iov_len = sizeof(peer_id);

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	/* if fd is specified, add it in a cmsg */
	if (fd >= 0) {
		memset(&msg_control, 0, sizeof(msg_control));
		msg.msg_control = &msg_control;
		msg.msg_controllen = sizeof(msg_control);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
	}

	ret = sendmsg(sock_fd, &msg, MSG_NOSIGNAL);
	if (ret <= 0) {
		fprintf(stderr, "Error sending a message\n");
		return -1;
	}

	return 0;
}

static void ivshmem_server_free_peer(int id)
{
	/* Advertise the deletion to other peers */
	for (int i = 0; i < UNIMSG_MAX_VMS; i++) {
		if (vms[i].id >= 0 && vms[i].id != id)
			ivshmem_server_sendmsg(vms[i].sock_fd, id, -1);
	}

	close(vms[id].sock_fd);
	close(vms[id].event_fd);
	vms[id].id = -1;

	if (sidecar_shms[id]) {
		munmap(sidecar_shms[id], sizeof(*sidecar_shms[id]));
		sidecar_shms[id] = NULL;
	}

	printf("Peer %d unregistered\n", id);
}

static int ivshmem_server_handle_new_conn()
{
	struct sockaddr_un unaddr;
	socklen_t unaddr_len;
	int newfd;
	int id;
	int ret;

	/* accept the incoming connection */
	unaddr_len = sizeof(unaddr);
	newfd = accept(sock_fd, (struct sockaddr *)&unaddr, &unaddr_len);

	if (newfd < 0)
		SYSERROR("Error accepting connection");

	for (id = 0; id < UNIMSG_MAX_VMS; id++) {
		if (vms[id].id == -1) {
			vms[id].id = id;
			vms[id].sock_fd = newfd;
			vms[id].has_sidecar = 0;
			break;
		}
	}

	if (id == UNIMSG_MAX_VMS) {
		fprintf(stderr, "Reached maximum VMs count\n");
		close(newfd);
		return 0;
	}

	/* VM addresses are currently built as 10.0.0.<id> */
	/* TODO: read from file */
	if (shm_register_vm(&shm->vms_info, id, 0xa | (id << 24)))
		ERROR("Error registering VM to shared memory");

	/* Handle a single vector for now */
	vms[id].event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (vms[id].event_fd < 0)
		SYSERROR("Error creating eventfd");

	/* Send our protocol version first */
	ret = ivshmem_server_sendmsg(vms[id].sock_fd, IVSHMEM_PROTOCOL_VERSION,
				     -1);
	if (ret < 0)
		SYSERROR("Cannot send version");

	/* Send the peer id to the client */
	ret = ivshmem_server_sendmsg(vms[id].sock_fd, id, -1);
	if (ret < 0)
		SYSERROR("Cannot send peer id");

	/* Send the shm_fd */
	ret = ivshmem_server_sendmsg(vms[id].sock_fd, -1, shm_fd);
	if (ret < 0)
		SYSERROR("Cannot send shm fd");

	/* Advertise the peers to each other */
	for (int i = 0; i < UNIMSG_MAX_VMS; i++) {
		if (vms[i].id != -1) {
			ivshmem_server_sendmsg(vms[i].sock_fd, id,
					       vms[id].event_fd);
			if (vms[i].id != id)
				ivshmem_server_sendmsg(vms[id].sock_fd, i,
						       vms[i].event_fd);
		}
	}

	printf("New peer registered with id %d\n", id);

	return 0;
}

static void *poll_events(void *arg)
{
	(void)arg;

	struct pollfd fds[UNIMSG_MAX_VMS + 1];
	int nfds;
	int ret;
	int active_vms[UNIMSG_MAX_VMS];
	int active_vms_count;

	while (running) {
		fds[0].fd = sock_fd;
		fds[0].events = POLLIN;
		nfds = 1;

		active_vms_count = 0;
		for (int i = 0; i < UNIMSG_MAX_VMS; i++) {
			if (vms[i].id >= 0) {
				fds[nfds].fd = vms[i].sock_fd;
				fds[nfds].events = 0;
				active_vms[active_vms_count++] = vms[i].id;
				nfds++;
			}
		}

		ret = poll(fds, nfds, 1000);
		if (ret < 0)
			SYSERROR("Error polling file descriptors");

		if (ret > 0) {
			if (fds[0].revents != 0) {
				if (fds[0].revents == POLLIN) {
					ivshmem_server_handle_new_conn();
					ret--;
				} else {
					ERROR("Received unexpected event on "
					      "listening socket");
				}
			}

			for (int i = 1; i < nfds; i++) {
				if (fds[i].revents != 0) {
					/* ivshmem clients are not supposed to
					 * do anything expect closing the
					 * connection on VM termination, handle
					 * any event by closing the client
					 * connection
					 */
					ivshmem_server_free_peer(
							active_vms[i - 1]);

					shm_unregister_vm(&shm->vms_info,
							  active_vms[i - 1]);

					if (--ret == 0)
						break;
				}
			}
		}
	}

	return NULL;
}

int main(int argc, char *argv[])
{
	parse_command_line(argc, argv);

	/* Setup shared memory */

	shm_fd = shm_open(DEFAULT_SHM_PATH, O_RDWR | O_CREAT | O_TRUNC,
			  S_IRWXU);
	if (shm_fd < 0)
		SYSERROR("Error opening shared memory");

	if (ftruncate(shm_fd, DEFAULT_SHM_SIZE))
		SYSERROR("Error setting shared memory size");

	shm = (struct unimsg_shm *)mmap(0, DEFAULT_SHM_SIZE,
					PROT_READ | PROT_WRITE, MAP_SHARED,
					shm_fd, 0);
	if (!shm)
		SYSERROR("Error mapping shared memory");

	shm_init(shm);

	for (int i = 0; i < UNIMSG_MAX_VMS; i++)
		vms[i].id = -1;

	/* Setup AF_UNIX socket */
	unlink(DEFAULT_UNIX_SOCK_PATH);
	sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock_fd < 0)
		SYSERROR("Error creating socket");

	struct sockaddr_un s_un;
	s_un.sun_family = AF_UNIX;
	strncpy(s_un.sun_path, DEFAULT_UNIX_SOCK_PATH, 108);
	if (bind(sock_fd, (struct sockaddr *)&s_un, sizeof(s_un)))
		SYSERROR("Error binding socket to to %s", s_un.sun_path);

	if (listen(sock_fd, SERVER_LISTEN_BACKLOG))
		SYSERROR("Error listening on socket");

	pthread_t events_poll_thread;
	if (pthread_create(&events_poll_thread, NULL,  poll_events, NULL))
		SYSERROR("Error creating events polling thread");

	/* Start the VMs */

	/* Run some benchmark */

	struct sigaction sigact = { .sa_handler = sigint_handler };
	if (sigaction(SIGINT, &sigact, NULL))
		SYSERROR("Error setting SIGINT handler");

	printf("Unimsg manager running, hit Ctrl+C to stop\n");
	while(running) {
		sleep(1);
		scrape_sidecar_stats();
	}

	pthread_join(events_poll_thread, NULL);
	unlink(DEFAULT_UNIX_SOCK_PATH);
	close(sock_fd);
	close(shm_fd);

	return EXIT_SUCCESS;
}
