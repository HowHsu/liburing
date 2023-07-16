#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <dirent.h>
#include "liburing.h"
#include "test.h"

struct linux_dirent64 {
	int64_t		d_ino;    /* 64-bit inode number */
	int64_t		d_off;    /* 64-bit offset to next structure */
	unsigned short	d_reclen; /* Size of this dirent */
	unsigned char	d_type;   /* File type */
	char		d_name[]; /* Filename (null-terminated) */
};

#define LOOP 30002
#define RING_SIZE (LOOP+10)
char buf[64];

double duration(struct timeval *s, struct timeval *e) {
	long sec = e->tv_sec - s->tv_sec;
	long usec = e->tv_usec - s->tv_usec;

	return sec + usec / 1000000.0;
}

int uring_getdents(struct io_uring *ring, int fd) {
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int i, ret;

	for (i = 0; i < LOOP; i++) {
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			perror("get sqe fails");
			return 1;
		}

		sqe->opcode = IORING_OP_GETDENTS;
		sqe->fd = fd;
		sqe->addr = (unsigned long long)buf;
		sqe->len = sizeof(buf);
		sqe->user_data = i;
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

int sync_getdents(int fd) {
	int i;
	for (i = 0; i < LOOP; i++) {
		int nread;

		nread = syscall(217, fd, buf, sizeof(buf));
		if (nread == -1) {
			perror("getdents64");
			return 1;
		}

		if (nread == 0) {
			break;
		}
	}

	return 0;
}

int main(int argc, char *argv[]) {
	int fd;
	struct timeval tv_begin, tv_end;
	struct io_uring ring;

	fd = open(argv[1], O_RDONLY | O_DIRECTORY);
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

	int cnt = 1;
	if (!strcmp(argv[2], "iouring")) {
		gettimeofday(&tv_begin, NULL);
		while(cnt--) {
			if (uring_getdents(&ring, fd)) {
				perror("error uring_getdents");
				return 1;
			}
		}

	} else if (!strcmp(argv[2], "sync")) {
		gettimeofday(&tv_begin, NULL);
		while(cnt--) {
			if (sync_getdents(fd)) {
				perror("error sync getdents");
				return 1;
			}
		}
	} else {
		perror("please enter an arg to indicate which case to test");
		return 1;
	}
	gettimeofday(&tv_end, NULL);
	printf("%f\n", duration(&tv_begin, &tv_end));
	close(fd);
	return 0;
}
