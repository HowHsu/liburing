#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/limits.h>
#include <linux/unistd.h>
#include <sys/vfs.h>
#include "liburing.h"
#include "test.h"

enum t_test_result {
	T_EXIT_PASS   = 0,
	T_EXIT_FAIL   = 1,
	T_EXIT_SKIP   = 77,
};

#define NAME_LEN 10
//#define BATCH 1000
#define BATCH 15
#define RING_SIZE BATCH+10

int N = 1000;
bool *visited;
char **filenames;
char buf[BATCH][1024];
int cnt;

struct linux_dirent64 {
	ino64_t        d_ino;
	off64_t        d_off;
	unsigned short d_reclen;
	unsigned char  d_type;
	char           d_name[];
};

bool check_xfs(char *dirname) {
	struct statfs fs_info;

	if (statfs(dirname, &fs_info) == -1) {
		fprintf(stderr, "statfs fails\n");
		return T_EXIT_FAIL;
	}
	if (fs_info.f_type != 0x58465342) // XFS_MAGIC
		fprintf(stderr, "Warning: The filesystem is not XFS\n");

	return 0;
}

int open_dir(int *dir_fd) {
	*dir_fd = open(".", O_RDONLY | O_DIRECTORY);
	if (*dir_fd == -1) {
		fprintf(stderr, "open directory fails\n");
		return T_EXIT_FAIL;
	}

	return 0;
}

int create_files() {
	int i;

	filenames = (char **)calloc(N, sizeof(char *));
	if (!filenames) {
		fprintf(stderr, "filenames allocation fails: %d\n", errno);
		return T_EXIT_FAIL;
	}
	for (i = 0; i < N; i++) {
		filenames[i] = (char *)malloc(NAME_LEN * sizeof(char));
		if (!filenames[i]) {
			fprintf(stderr, "filenames[%d] allocation fails: %d\n",
				i, errno);
			goto err;
		}
	}
	for (i = 0; i < N; i++) {
		filenames[i][0] = 0;
		sprintf(filenames[i], "%d", i);
		int fd = open(filenames[i], O_CREAT | O_WRONLY, 0644);
		if (fd == -1) {
			fprintf(stderr, "create file fails\n");
			i = N - 1;
			goto err;
		}
		close(fd);
	}

	return 0;

err:
	for(; i >= 0; i--)
		free(filenames[i]);

	free(filenames);
	return T_EXIT_FAIL;
}

int handle_result(int index, int nread) {
	struct linux_dirent64 *de;

	for (int pos = 0; pos < nread;) {
		de = (struct linux_dirent64 *)(buf[index] + pos);

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
			pos += de->d_reclen;
			continue;
		}
		int fn = atoi(de->d_name);
		if (fn >= N || fn < 0) {
			fprintf(stderr, "file not in the list: %s\n", de->d_name);
			return T_EXIT_FAIL;
		}
		if (visited[fn]) {
			fprintf(stderr, "file has already been visited\n");
			return T_EXIT_FAIL;
		}
		visited[fn] = true;
		cnt++;
		pos += de->d_reclen;
	}

	return 0;
}

int uring_getdents(struct io_uring *ring, int dir_fd) {
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	int i, ret;
	int error = 0;
	bool eof = false;

	visited = (bool *)calloc(N, sizeof(bool));
	cnt = 0;
	while (!error && !eof) {
		for (i = 0; i < BATCH; i++) {
			sqe = io_uring_get_sqe(ring);
			if (!sqe) {
				fprintf(stderr, "get sqe fails\n");
				return T_EXIT_FAIL;
			}

			io_uring_prep_getdents(sqe, dir_fd, buf[i], sizeof(buf[i]));
			sqe->user_data = i;
		}

		ret = io_uring_submit(ring);
		if (ret <= 0) {
			fprintf(stderr, "submit sqe fails\n");
			return T_EXIT_FAIL;
		}

		for (i = 0; i < BATCH; i++) {
			ret = io_uring_wait_cqe(ring, &cqe);
			if (ret < 0) {
				fprintf(stderr, "wait cqe fails: %d\n", ret);
				return T_EXIT_FAIL;
			}

			if (!error && cqe->res < 0) {
				fprintf(stderr, "getdents returns error: %d\n", errno);
				error = errno;
			}

			/*
			 * something is wrong but we still need to reap all cqes.
			 */
			if (error)
				continue;

			/*
			 * we meet the end of the file, but remember we are doing
			 * async IO, this may not be the last cqe. Keep reaping
			 * rest requests.
			 */
			if (!cqe->res)
				eof = true;

			ret = handle_result(cqe->user_data, cqe->res);
			if (ret == T_EXIT_FAIL)
				error = ret;
			io_uring_cq_advance(ring, 1);
		}
	}

	if (error)
		return T_EXIT_FAIL;

	if (cnt != N) {
		fprintf(stderr, "not all files found\n");
		return T_EXIT_FAIL;
	}

	return 0;
}

int delete_files_and_directory(const char *dirname) {
	int ret = chdir("..");
	if (ret) {
		fprintf(stderr, "chdir fails\n");
		return T_EXIT_FAIL;
	}
	for (int i = 0; i < N; i++) {
		char filepath[NAME_LEN + strlen(dirname) + 1];
		snprintf(filepath, sizeof(filepath), "%s/%s", dirname, filenames[i]);
		if (unlink(filepath) == -1) {
			fprintf(stderr, "unlink fails: %d\n", errno);
			return T_EXIT_FAIL;
		}
	}
	if (rmdir(dirname) == -1) {
		fprintf(stderr, "rmdir fails: %d\n", errno);
		return T_EXIT_FAIL;
	}

	return 0;
}

int main(int argc, char *argv[]) {
	char dirname[256] = "testdir";
	int ret = 0, ret2;
	struct io_uring ring;

	if (argc >= 2) {
		sprintf(dirname, "%s/testdir", argv[1]);
	}
	if (argc >= 3) {
		N = atoi(argv[2]);
		if (!N) {
			fprintf(stderr, "please give a integer with non-zero value\n");
			return T_EXIT_FAIL;
		}
	}

	if (io_uring_queue_init(RING_SIZE, &ring, 0) < 0) {
		fprintf(stderr, "io_uring_queue_init\n");
		return T_EXIT_FAIL;
	}

	// Create the directory
	if (mkdir(dirname, 0755) == -1) {
		fprintf(stderr, "mkdir fails: %d\n", errno);
		return T_EXIT_FAIL;
	}

	// only xfs support nowait getdents for now
	ret = check_xfs(dirname);
	if (ret == T_EXIT_FAIL)
		goto out_del;

	ret = chdir(dirname);
	if (ret) {
		fprintf(stderr, "chdir fails: %d\n", errno);
		goto out_del;
	}
	// Create N files with sequential names
	ret = create_files();
	if (ret == T_EXIT_FAIL)
		goto out_del;

	int dir_fd;
	ret = open_dir(&dir_fd);
	if (ret == T_EXIT_FAIL)
		goto out_del;
	// Read directory entries using io_uring getdents
	ret = uring_getdents(&ring, dir_fd);

	close(dir_fd);
out_del:
	// Delete the created files and directory
	ret2 = delete_files_and_directory(dirname);
	if (!ret && (ret2 == T_EXIT_FAIL)) {
		fprintf(stderr, "test succeeds but cleanning fails\n");
	}

	free(visited);
	if (filenames) {
		for (int i = 0; i < N; i++)
			free(filenames[i]);
		free(filenames);
	}

	return ret;
}

