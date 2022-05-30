/* SPDX-License-Identifier: MIT */
/*
 * Description: test io_uring poll async handling
 *
 */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>

#include "liburing.h"

static int test_poll_async(int order)
{
	struct io_uring ring;
	int pipe1[2];
	struct io_uring_sqe *sqe;
	int i, ret;

	if (pipe(pipe1) != 0) {
		perror("pipe");
		return 1;
	}

	ret = io_uring_queue_init((1 << (order + 5)), &ring, 0);
	if (ret) {
		fprintf(stderr, "ring setup failed: %d\n", ret);
		return 1;
	}

	for (i = 0; i < (1 << order); i++) {
		sqe = io_uring_get_sqe(&ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			return 1;
		}

		io_uring_prep_poll_add(sqe, pipe1[0], POLLIN);
		sqe->flags |= IOSQE_ASYNC;
		sqe->user_data = i;
	}
	ret = io_uring_submit(&ring);
	if (ret <= 0) {
		fprintf(stderr, "sqe submit failed\n");
		return 1;
	}

	close(pipe1[0]);
	close(pipe1[1]);
	io_uring_queue_exit(&ring);
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc > 1)
		return 0;

	ret = test_poll_async(3);
	if (ret) {
		fprintf(stderr, "test_poll_async failed\n");
		return -1;
	}

	return 0;
}
