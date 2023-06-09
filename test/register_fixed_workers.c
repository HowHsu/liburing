/* SPDX-License-Identifier: MIT */
/*
 * Description:
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/time.h>

#include "liburing.h"

typedef long long ll;
ll usecs(struct timeval tv) {
	return tv.tv_sec * (ll)1000 * 1000 + tv.tv_usec;
}

static int test_single_nop(struct io_uring *ring, int depth)
{
	int i;
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;

	for (i = 0; i < depth; i++) {
		sqe = io_uring_get_sqe(ring);
		io_uring_prep_nop(sqe);
		sqe->flags |= IOSQE_ASYNC;
	}
	io_uring_submit(ring);
	for(i = 0; i < depth; i++) {
		io_uring_wait_cqe(ring, &cqe);
		io_uring_cqe_seen(ring, cqe);
	}
	return 0;
}

struct io_uring_fixed_worker_arg {
	int nr_workers;
	int resv;
	long long resv2[3];
};

int main(int argc, char *argv[])
{
	int new_count, i;
	ll delta;
	int tmp, fixed;
	struct io_uring ring;
	int ret, l, loop = 1, depth = 1;
	struct timeval tv_begin, tv_end;
	struct timezone tz;
	struct io_uring_fixed_worker_arg count[2] = {
		{1, 0, {0, 0, 0}},
		{0, 1, {0, 0, 0}},
	};

	if (argc < 5) {
		return 1;
	}
	fixed = atoi(argv[1]); loop = atoi(argv[2]); depth = atoi(argv[3]); new_count = atoi(argv[4]);
	ret = io_uring_queue_init(10010, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}
	if (fixed) {
		count[0].nr_workers = new_count;
		ret = io_uring_register_iowq_fixed_workers(&ring, (void *)count);
		if (ret) {
			fprintf(stderr, "fixed workers registration failed: %d\n", ret);
			return 1;
		}
	} else {
		unsigned values[2];
		values[0] = values[1] = 3;
		ret = io_uring_register_iowq_max_workers(&ring, values);
	}

	l = loop;
	gettimeofday(&tv_begin, &tz);
	while(loop--)
		test_single_nop(&ring, depth);
	gettimeofday(&tv_end, &tz);
	delta = usecs(tv_end) - usecs(tv_begin);

	printf("time spent: %lld usecs\n", delta);
	if (delta)
		printf("IOPS: %lld\n", (ll)l * depth * 1000000 / delta);
	if (fixed) {
		sleep(5);
		ret = io_uring_unregister_iowq_fixed_workers(&ring);
		if (ret) {
			fprintf(stderr, "fixed workers unregistration failed: %d\n", ret);
			return 1;
		}
	}
	sleep(5);
	return 0;
}
