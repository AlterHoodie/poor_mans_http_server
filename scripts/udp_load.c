/*
 * High-rate UDP tick sender using sendmmsg(2).
 *
 * Build:
 *   cc -O3 -Wall -o udp_load scripts/udp_load.c
 *
 * Examples:
 *   ./udp_load 192.168.29.36 9000 2000000
 *   ./udp_load 192.168.29.36 9000 2000000 --procs 2
 *
 * Tick layout matches include/tick.h (28 bytes).
 * Server: ./build/server -l 4 -n 4 -- udp   (no echo)
 *
 * With multiple processes, ignore server "gaps" — seq ranges interleave on the wire.
 * Compare server total_ticks vs packets sent for real loss.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { TICK_SIZE = 28, BATCH = 64, SNDBUF = 4 * 1024 * 1024 };

struct __attribute__((packed)) tick {
	uint64_t seq;
	uint64_t ts_ns;
	uint32_t symbol_id;
	int32_t price;
	int32_t qty;
};

static uint64_t nsec_now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int send_batch(int fd, struct mmsghdr *msgs, struct tick *ticks,
		      uint64_t start_seq, int n, const struct sockaddr_in *dst)
{
	for (int i = 0; i < n; i++) {
		ticks[i].seq = start_seq + (uint64_t)i;
		ticks[i].ts_ns = nsec_now();
		ticks[i].symbol_id = 1;
		ticks[i].price = 10050 + (int32_t)ticks[i].seq;
		ticks[i].qty = 100;
		msgs[i].msg_hdr.msg_name = (void *)dst;
		msgs[i].msg_hdr.msg_namelen = sizeof(*dst);
		msgs[i].msg_len = TICK_SIZE;
	}
	return sendmmsg(fd, msgs, (unsigned int)n, 0);
}

static uint64_t run_sender(const char *host, int port, uint64_t count,
			   uint64_t seq_start)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		exit(1);
	}

	int sndbuf = SNDBUF;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

	struct sockaddr_in dst = {0};
	dst.sin_family = AF_INET;
	dst.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) {
		fprintf(stderr, "bad host: %s\n", host);
		exit(1);
	}

	struct tick ticks[BATCH];
	struct iovec iov[BATCH];
	struct mmsghdr msgs[BATCH];

	for (int i = 0; i < BATCH; i++) {
		iov[i].iov_base = &ticks[i];
		iov[i].iov_len = TICK_SIZE;
		msgs[i].msg_hdr.msg_iov = &iov[i];
		msgs[i].msg_hdr.msg_iovlen = 1;
		msgs[i].msg_hdr.msg_control = NULL;
		msgs[i].msg_hdr.msg_controllen = 0;
		msgs[i].msg_hdr.msg_flags = 0;
	}

	uint64_t sent = 0;
	uint64_t seq = seq_start;

	while (sent < count) {
		int n = (int)((count - sent) < (uint64_t)BATCH ? (count - sent) : BATCH);
		int rc = send_batch(fd, msgs, ticks, seq, n, &dst);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			perror("sendmmsg");
			break;
		}
		if (rc == 0)
			break;
		sent += (uint64_t)rc;
		seq += (uint64_t)rc;
	}

	close(fd);
	return sent;
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr,
			"usage: %s <host> <port> <count> [--procs N]\n",
			argv[0]);
		return 1;
	}

	const char *host = argv[1];
	int port = atoi(argv[2]);
	uint64_t total = strtoull(argv[3], NULL, 10);
	int procs = 1;

	for (int i = 4; i < argc; i++) {
		if (strcmp(argv[i], "--procs") == 0 && i + 1 < argc) {
			procs = atoi(argv[++i]);
			if (procs < 1)
				procs = 1;
		}
	}

	uint64_t t0 = nsec_now();

	if (procs == 1) {
		uint64_t sent = run_sender(host, port, total, 1);
		double sec = (nsec_now() - t0) / 1e9;
		printf("sent %llu ticks (%d bytes) to %s:%d in %.3fs (%.0f ticks/s)\n",
		       (unsigned long long)sent, TICK_SIZE, host, port, sec,
		       sec > 0 ? sent / sec : 0.0);
		return sent == total ? 0 : 1;
	}

	uint64_t per = total / (uint64_t)procs;
	uint64_t rem = total % (uint64_t)procs;
	uint64_t seq = 1;
	pid_t *kids = calloc((size_t)procs, sizeof(pid_t));
	if (!kids) {
		perror("calloc");
		return 1;
	}

	for (int p = 0; p < procs; p++) {
		uint64_t n = per + (p < (int)rem ? 1 : 0);
		uint64_t start = seq;
		seq += n;
		pid_t pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}
		if (pid == 0) {
			uint64_t sent = run_sender(host, port, n, start);
			_exit(sent == n ? 0 : 1);
		}
		kids[p] = pid;
	}

	for (int p = 0; p < procs; p++)
		waitpid(kids[p], NULL, 0);

	free(kids);

	double sec = (nsec_now() - t0) / 1e9;
	printf(
		"sent %llu ticks (%d bytes) to %s:%d in %.3fs (%.0f ticks/s) across %d process(es)\n",
		(unsigned long long)total, TICK_SIZE, host, port, sec,
		sec > 0 ? total / sec : 0.0, procs);
	printf("ignore server gaps with --procs > 1; compare total_ticks vs sent.\n");
	return 0;
}
