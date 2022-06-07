/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>

#include "helpers.h"
#include "liburing.h"

#define BUF_SIZE (16 * 4096)

struct test_ctx {
	int real_pipe1[2];
	int real_pipe2[2];
	int real_fd_in;
	int real_fd_out;

	/* fds or for registered files */
	int pipe1[2];
	int pipe2[2];
	int fd_in;
	int fd_out;

	void *buf_in;
	void *buf_out;
};

static unsigned int splice_flags = 0;
static unsigned int sqe_flags = 0;
static int has_splice = 0;
static int has_tee = 0;

static int read_buf(int fd, void *buf, int len)
{
	int ret;

	while (len) {
		ret = read(fd, buf, len);
		if (ret < 0)
			return ret;
		len -= ret;
		buf += ret;
	}
	return 0;
}

static int write_buf(int fd, const void *buf, int len)
{
	int ret;

	while (len) {
		ret = write(fd, buf, len);
		if (ret < 0)
			return ret;
		len -= ret;
		buf += ret;
	}
	return 0;
}

static int check_content(int fd, void *buf, int len, const void *src)
{
	int ret;

	ret = read_buf(fd, buf, len);
	if (ret)
		return ret;

	ret = memcmp(buf, src, len);
	return (ret != 0) ? -1 : 0;
}

static int create_file(const char *filename)
{
	int fd, save_errno;

	fd = open(filename, O_RDWR | O_CREAT, 0644);
	save_errno = errno;
	unlink(filename);
	errno = save_errno;
	return fd;
}

static int init_splice_ctx(struct test_ctx *ctx)
{
	int ret, rnd_fd;

	ctx->buf_in = t_calloc(BUF_SIZE, 1);
	ctx->buf_out = t_calloc(BUF_SIZE, 1);

	ctx->fd_in = create_file(".splice-test-in");
	if (ctx->fd_in < 0) {
		perror("file open");
		return 1;
	}

	ctx->fd_out = create_file(".splice-test-out");
	if (ctx->fd_out < 0) {
		perror("file open");
		return 1;
	}

	/* get random data */
	rnd_fd = open("/dev/urandom", O_RDONLY);
	if (rnd_fd < 0)
		return 1;

	ret = read_buf(rnd_fd, ctx->buf_in, BUF_SIZE);
	if (ret != 0)
		return 1;
	close(rnd_fd);

	/* populate file */
	ret = write_buf(ctx->fd_in, ctx->buf_in, BUF_SIZE);
	if (ret)
		return ret;

	if (pipe(ctx->pipe1) < 0)
		return 1;
	if (pipe(ctx->pipe2) < 0)
		return 1;

	ctx->real_pipe1[0] = ctx->pipe1[0];
	ctx->real_pipe1[1] = ctx->pipe1[1];
	ctx->real_pipe2[0] = ctx->pipe2[0];
	ctx->real_pipe2[1] = ctx->pipe2[1];
	ctx->real_fd_in = ctx->fd_in;
	ctx->real_fd_out = ctx->fd_out;
	return 0;
}

static int do_splice_op(struct io_uring *ring,
			int fd_in, loff_t off_in,
			int fd_out, loff_t off_out,
			unsigned int len,
			__u8 opcode)
{
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret = -1;

	do {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "get sqe failed\n");
			return -1;
		}
		io_uring_prep_splice(sqe, fd_in, off_in, fd_out, off_out,
				     len, splice_flags);
		sqe->flags |= sqe_flags;
		sqe->user_data = 42;
		sqe->opcode = opcode;

		ret = io_uring_submit(ring);
		if (ret != 1) {
			fprintf(stderr, "sqe submit failed: %d\n", ret);
			return ret;
		}

		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "wait completion %d\n", cqe->res);
			return ret;
		}

		if (cqe->res <= 0) {
			io_uring_cqe_seen(ring, cqe);
			return cqe->res;
		}

		len -= cqe->res;
		if (off_in != -1)
			off_in += cqe->res;
		if (off_out != -1)
			off_out += cqe->res;
		io_uring_cqe_seen(ring, cqe);
	} while (len);

	return 0;
}

static int do_splice(struct io_uring *ring,
			int fd_in, loff_t off_in,
			int fd_out, loff_t off_out,
			unsigned int len)
{
	return do_splice_op(ring, fd_in, off_in, fd_out, off_out, len,
			    IORING_OP_SPLICE);
}

static int splice_pipe_to_pipe(struct io_uring *ring, struct test_ctx *ctx)
{
	int ret;

	ret = do_splice(ring, ctx->pipe1[0], -1, ctx->pipe2[1], -1, BUF_SIZE);
	if (ret)
		return ret;

	return check_content(ctx->real_pipe2[0], ctx->buf_out, BUF_SIZE,
				ctx->buf_in);
}

typedef long long ll;
ll usecs(struct timeval tv) {
	return tv.tv_sec * (ll)1000 * 1000 + tv.tv_usec;
}

static int test_splice(struct io_uring *ring, struct test_ctx *ctx)
{
	int ret;
	struct timeval tv_begin, tv_end;
	struct timezone tz;
	ll delta;

	ret = do_splice(ring, ctx->fd_in, 0, ctx->pipe1[1], -1, BUF_SIZE);
	if (ret)
		return ret;

	gettimeofday(&tv_begin, &tz);
	ret = splice_pipe_to_pipe(ring, ctx);
	gettimeofday(&tv_end, &tz);
	if (ret) {
		fprintf(stderr, "splice_pipe_to_pipe failed %i %i\n",
			ret, errno);
		return ret;
	}

	delta = usecs(tv_end) - usecs(tv_begin);
	printf("time spent: %lld usecs\n", delta);

	return 0;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	struct io_uring_params p = { };
	struct test_ctx ctx;
	int ret;
	int reg_fds[6];

	if (argc > 1)
		return 0;

	ret = io_uring_queue_init_params(8, &ring, &p);
	if (ret) {
		fprintf(stderr, "ring setup failed\n");
		return 1;
	}
	if (!(p.features & IORING_FEAT_FAST_POLL)) {
		fprintf(stdout, "No splice support, skipping\n");
		return 0;
	}

	ret = init_splice_ctx(&ctx);
	if (ret) {
		fprintf(stderr, "init failed %i %i\n", ret, errno);
		return 1;
	}

	ret = test_splice(&ring, &ctx);
	if (ret) {
		fprintf(stderr, "basic splice tests failed\n");
		return ret;
	}

	return 0;
}
