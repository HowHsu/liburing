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

int main(int argc, char *argv[])
{
	int count;
	ll delta;
	struct io_uring ring;
	int ret, l, loop = 1, depth = 1;
	struct timeval tv_begin, tv_end;
	struct timezone tz;
	struct io_uring_fixed_worker_arg new_count[2] = {
		{1, 10000000,},
		{0, 0,},
	};

	if (argc < 4) {
		return 1;
	}
	loop = atoi(argv[1]); depth = atoi(argv[2]); count = atoi(argv[3]);
	ret = io_uring_queue_init(10010, &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}
	new_count[0].nr_workers = count;
	ret = io_uring_register_iowq_fixed_workers(&ring, new_count);
	if (ret) {
		fprintf(stderr, "fixed workers registration failed: %d\n", ret);
		return 1;
	}
	l = loop;
	gettimeofday(&tv_begin, &tz);
	while(loop--)
		test_single_nop(&ring, depth);
	gettimeofday(&tv_end, &tz);
	delta = usecs(tv_end) - usecs(tv_begin);

	printf("time spent: %lld usecs\n", delta);
	printf("IOPS: %lld\n", (ll)l * depth * 1000000 / delta);
	return 0;
}
