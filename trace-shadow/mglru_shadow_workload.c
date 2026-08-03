// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif

#define HOT_ACCESSES 8

static volatile unsigned char sink;

static void die(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	exit(EXIT_FAILURE);
}

static void die_message(const char *message)
{
	fprintf(stderr, "%s\n", message);
	exit(EXIT_FAILURE);
}

static int open_target(const char *path, size_t page_size)
{
	struct stat st;
	int error;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		die("open");
	if (fstat(fd, &st))
		die("fstat");
	if (!S_ISREG(st.st_mode))
		die_message("target is not a regular file");
	if (st.st_size != (off_t)page_size)
		die_message("target size must be exactly one page");

	error = posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
	if (error) {
		errno = error;
		die("posix_fadvise(POSIX_FADV_RANDOM)");
	}

	return fd;
}

static void read_one_byte(int fd)
{
	unsigned char byte;
	ssize_t size = pread(fd, &byte, sizeof(byte), 0);

	if (size < 0)
		die("pread");
	if (size != sizeof(byte))
		die_message("short read from target");
	sink ^= byte;
}

/*
 * A new struct file has a fresh readahead state.  Reopening the file avoids
 * filemap_read() coalescing repeated accesses through ra->prev_pos, so every
 * iteration reaches folio_mark_accessed().
 */
static void make_workingset(const char *path, size_t page_size)
{
	int i;

	for (i = 0; i < HOT_ACCESSES; i++) {
		int fd = open_target(path, page_size);

		read_one_byte(fd);
		if (close(fd))
			die("close");
	}
}

static void evict_target(const char *path, size_t page_size, int workingset)
{
	unsigned char resident = 0;
	void *address;
	int fd = open_target(path, page_size);

	address = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd, 0);
	if (address == MAP_FAILED)
		die("mmap");
	if (madvise(address, page_size, MADV_RANDOM))
		die("madvise(MADV_RANDOM)");

	/* Materialize one clean, order-0 candidate and put it on this memcg's LRU. */
	sink ^= *(volatile unsigned char *)address;

	if (workingset)
		make_workingset(path, page_size);

	/*
	 * The kernel drains this CPU's LRU batch before MADV_PAGEOUT.  The harness
	 * pins this process to one CPU so the newly faulted folio cannot be left in
	 * another CPU's batch.
	 */
	if (madvise(address, page_size, MADV_PAGEOUT))
		die("madvise(MADV_PAGEOUT)");
	if (mincore(address, page_size, &resident))
		die("mincore");
	if (resident & 1)
		die_message("MADV_PAGEOUT did not evict the target page");

	if (munmap(address, page_size))
		die("munmap");
	if (close(fd))
		die("close");

	printf("shadow-created ws=%d path=%s\n", workingset, path);
}

static void refault_target(const char *path, size_t page_size)
{
	int fd = open_target(path, page_size);

	/* Buffered I/O deliberately keeps lru_gen_in_fault() false. */
	read_one_byte(fd);
	if (close(fd))
		die("close");

	printf("refaulted path=%s\n", path);
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s {evict-ws0|evict-ws1|refault|warm} [FILE]\n",
		program);
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	long system_page_size;
	size_t page_size;

	if (prctl(PR_SET_NAME, "mglru-shadow", 0, 0, 0))
		die("prctl(PR_SET_NAME)");

	if (argc == 2 && !strcmp(argv[1], "warm"))
		return EXIT_SUCCESS;
	if (argc != 3)
		usage(argv[0]);

	system_page_size = sysconf(_SC_PAGESIZE);
	if (system_page_size <= 0)
		die("sysconf(_SC_PAGESIZE)");
	page_size = (size_t)system_page_size;

	if (!strcmp(argv[1], "evict-ws0"))
		evict_target(argv[2], page_size, 0);
	else if (!strcmp(argv[1], "evict-ws1"))
		evict_target(argv[2], page_size, 1);
	else if (!strcmp(argv[1], "refault"))
		refault_target(argv[2], page_size);
	else
		usage(argv[0]);

	return EXIT_SUCCESS;
}
