#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "liburing.h"
#include "test.h"

#define LOOP 30002
#define RING_SIZE (LOOP+10)
off_t offset;
unsigned int whence;

double duration(struct timeval *s, struct timeval *e) {
	long sec = e->tv_sec - s->tv_sec;
	long usec = e->tv_usec - s->tv_usec;

	return sec + usec / 1000000.0;
}

int uring_lseek(struct io_uring *ring, int fd) {
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int i, ret;

	for (i = 0; i < LOOP; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			perror("get sqe fails");
			return 1;
		}

		sqe->opcode = IORING_OP_LSEEK;
		sqe->fd = fd;
		sqe->addr = offset;
		sqe->len = whence;
	}

	ret = io_uring_submit(ring);
	if (ret <= 0) {
		perror("submit sqe fails");
		return 1;
	}

	ret = io_uring_wait_cqes(ring, &cqe, LOOP, NULL, NULL);
	if (ret < 0) {
		perror("wait cqe fails");
		return 1;
	}

	return 0;

}

#define SYS_lseek 8

int sync_lseek(int fd) {
	int i;
	for (i = 0; i < LOOP; i++) {
		int ret;

		ret = syscall(SYS_lseek, fd, offset, whence);
		if (ret == -1) {
			perror("sync llseek error");
			return 1;
		}
	}

	return 0;
}

int main(int argc, char *argv[]) {
	int fd;
	struct timeval tv_begin, tv_end;
	struct io_uring ring;

	fd = open(argv[1], O_RDWR);
	if (fd == -1) {
		perror("open");
		return 1;
	}

	if (!strcmp(argv[2], "iouring")) {
		if (io_uring_queue_init(RING_SIZE, &ring, 0) < 0) {
			perror("io_uring_queue_init");
			return 1;
		}
	}

	offset = atoi(argv[3]);
	switch (atoi(argv[4])) {
	case 0:
		whence = SEEK_SET;
		break;
	case 1:
		whence = SEEK_CUR;
		break;
	case 2:
		whence = SEEK_END;
		break;
	case 3:
		whence = SEEK_DATA;
		break;
	case 4:
		whence = SEEK_HOLE;
	}

	int cnt = 1;
	if (!strcmp(argv[2], "iouring")) {
		gettimeofday(&tv_begin, NULL);
		while(cnt--) {
			if (uring_lseek(&ring, fd))
				return 1;
		}

	} else if (!strcmp(argv[2], "sync")) {
		gettimeofday(&tv_begin, NULL);
		while(cnt--) {
			if (sync_lseek(fd) == -1)
				return 1;
		}
	} else {
		perror("please enter an arg to indicate which case to test");
		return 1;
	}
	gettimeofday(&tv_end, NULL);
	printf("seek mode:%d, offset: %ld\n", whence, offset);
	printf("%f\n", duration(&tv_begin, &tv_end));
	close(fd);
	return 0;
}
