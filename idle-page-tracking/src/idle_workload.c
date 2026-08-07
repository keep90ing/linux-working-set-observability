/*
 * idle_workload.c — workload for the idle page tracking experiment.
 *
 * Creates a single file-backed mmap() split into three tracked regions and
 * one optional mapped-only tail:
 *   HOT    — scanned HOT_ROUNDS times during the observation window
 *   STREAM — scanned exactly once during the observation window
 *   UNUSED — prefaulted, then never touched again
 *   UNFAULTED — mapped but never faulted in (excluded from idle tracking)
 *
 * Experiment phases:
 *   Phase 0 — create the mapping and prefault the three tracked regions
 *   Phase 1 — publish metadata, then wait while the observer marks PFNs idle
 *   Phase 2 — on "run", execute the controlled access patterns
 *   Phase 3 — stay alive while the observer reads the resulting idle bitmap
 *   Phase 4 — on "exit", release resources and terminate
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static volatile unsigned long g_checksum;

static void die(const char *msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

/* Parse "64M", "1G", "4096" etc. into bytes. */
static size_t parse_size(const char *s, int allow_zero)
{
	char *end = NULL;
	unsigned long long multiplier = 1;
	if (!isdigit((unsigned char)s[0])) {
		fprintf(stderr, "invalid size: %s\n", s);
		exit(EXIT_FAILURE);
	}
	errno = 0;
	unsigned long long v = strtoull(s, &end, 10);
	if (errno != 0 || end == s) {
		fprintf(stderr, "invalid size: %s\n", s);
		exit(EXIT_FAILURE);
	}
	switch (toupper((unsigned char)*end)) {
	case 'K': multiplier = 1ULL << 10; end++; break;
	case 'M': multiplier = 1ULL << 20; end++; break;
	case 'G': multiplier = 1ULL << 30; end++; break;
	case '\0': break;
	default:
		fprintf(stderr, "invalid size suffix in: %s\n", s);
		exit(EXIT_FAILURE);
	}
	if (*end != '\0' || (!allow_zero && v == 0) ||
	    v > SIZE_MAX / multiplier) {
		fprintf(stderr, "invalid size: %s\n", s);
		exit(EXIT_FAILURE);
	}
	return (size_t)(v * multiplier);
}

static size_t round_up(size_t v, size_t align)
{
	return (v + align - 1) / align * align;
}

/* Read one byte per page so every page becomes (or stays) resident. */
static void touch_region(const unsigned char *base, size_t length,
			 size_t page_size)
{
	const volatile unsigned char *p = base;
	unsigned long sum = 0;

	for (size_t off = 0; off < length; off += page_size)
		sum += p[off];
	g_checksum += sum;
}

static void write_metadata(const char *path, size_t page_size,
			   const unsigned char *base, size_t total,
			   size_t hot_size, size_t stream_size,
			   size_t unused_size, size_t unfaulted_size,
			   long hot_rounds)
{
	FILE *f = fopen(path, "w");
	if (!f)
		die("fopen metadata");

	uintptr_t start = (uintptr_t)base;
	fprintf(f,
		"{\n"
		"  \"pid\": %ld,\n"
		"  \"page_size\": %zu,\n"
		"  \"hot_rounds\": %ld,\n"
		"  \"mapping\": {\n"
		"    \"start\": \"0x%" PRIxPTR "\",\n"
		"    \"length\": %zu\n"
		"  },\n"
		"  \"regions\": {\n"
		"    \"hot\": { \"start\": \"0x%" PRIxPTR "\", \"length\": %zu },\n"
		"    \"stream\": { \"start\": \"0x%" PRIxPTR "\", \"length\": %zu },\n"
		"    \"unused\": { \"start\": \"0x%" PRIxPTR "\", \"length\": %zu },\n"
		"    \"unfaulted\": { \"start\": \"0x%" PRIxPTR "\", \"length\": %zu }\n"
		"  }\n"
		"}\n",
		(long)getpid(), page_size, hot_rounds, start, total,
		start, hot_size,
		start + hot_size, stream_size,
		start + hot_size + stream_size, unused_size,
		start + hot_size + stream_size + unused_size, unfaulted_size);
	if (fclose(f) != 0)
		die("fclose metadata");
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s --file PATH --control-fifo PATH --metadata PATH\n"
		"          [--hot-size SIZE] [--stream-size SIZE]\n"
		"          [--unused-size SIZE] [--unfaulted-size SIZE]\n"
		"          [--hot-rounds N]\n"
		"SIZE accepts K/M/G suffixes (default: hot 64M, stream 256M,\n"
		"unused 64M, unfaulted 0, hot-rounds 100)\n",
		prog);
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	const char *file_path = NULL, *fifo_path = NULL, *meta_path = NULL;
	size_t hot_size = 64UL << 20;
	size_t stream_size = 256UL << 20;
	size_t unused_size = 64UL << 20;
	size_t unfaulted_size = 0;
	long hot_rounds = 100;

	static const struct option opts[] = {
		{ "file",         required_argument, NULL, 'f' },
		{ "hot-size",     required_argument, NULL, 'h' },
		{ "stream-size",  required_argument, NULL, 's' },
		{ "unused-size",  required_argument, NULL, 'u' },
		{ "unfaulted-size", required_argument, NULL, 'n' },
		{ "hot-rounds",   required_argument, NULL, 'r' },
		{ "control-fifo", required_argument, NULL, 'c' },
		{ "metadata",     required_argument, NULL, 'm' },
		{ NULL, 0, NULL, 0 }
	};

	int c;
	while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (c) {
		case 'f': file_path = optarg; break;
		case 'h': hot_size = parse_size(optarg, 0); break;
		case 's': stream_size = parse_size(optarg, 0); break;
		case 'u': unused_size = parse_size(optarg, 0); break;
		case 'n': unfaulted_size = parse_size(optarg, 1); break;
		case 'r':
			hot_rounds = strtol(optarg, NULL, 10);
			if (hot_rounds <= 0) {
				fprintf(stderr, "invalid --hot-rounds\n");
				return EXIT_FAILURE;
			}
			break;
		case 'c': fifo_path = optarg; break;
		case 'm': meta_path = optarg; break;
		default: usage(argv[0]);
		}
	}
	if (!file_path || !fifo_path || !meta_path)
		usage(argv[0]);

	/*
	 * Phase 0: setup and prefault.
	 *
	 * Build one page-aligned file-backed mapping. Touch every page in the
	 * three tracked regions once so they are resident before the observation
	 * window. The optional UNFAULTED tail remains mapped but non-resident; it
	 * makes mapped capacity, RSS, and window-touched pages independently
	 * visible without changing the original idle-tracking semantics.
	 */
	long ps = sysconf(_SC_PAGESIZE);
	if (ps <= 0)
		die("sysconf(_SC_PAGESIZE)");
	size_t page_size = (size_t)ps;

	size_t orig_hot = hot_size, orig_stream = stream_size,
	       orig_unused = unused_size, orig_unfaulted = unfaulted_size;
	hot_size = round_up(hot_size, page_size);
	stream_size = round_up(stream_size, page_size);
	unused_size = round_up(unused_size, page_size);
	unfaulted_size = round_up(unfaulted_size, page_size);
	if (hot_size != orig_hot || stream_size != orig_stream ||
	    unused_size != orig_unused || unfaulted_size != orig_unfaulted)
		fprintf(stderr,
			"note: region sizes rounded up to page size %zu\n",
			page_size);

	if (hot_size > SIZE_MAX - stream_size ||
	    hot_size + stream_size > SIZE_MAX - unused_size ||
	    hot_size + stream_size + unused_size > SIZE_MAX - unfaulted_size) {
		fprintf(stderr, "mapping size overflow\n");
		return EXIT_FAILURE;
	}
	size_t total = hot_size + stream_size + unused_size + unfaulted_size;

	int fd = open(file_path, O_RDWR | O_CREAT, 0600);
	if (fd < 0)
		die("open backing file");
	int err = posix_fallocate(fd, 0, (off_t)total);
	if (err != 0) {
		errno = err;
		die("posix_fallocate");
	}

	unsigned char *base = mmap(NULL, total, PROT_READ | PROT_WRITE,
				   MAP_SHARED, fd, 0);
	if (base == MAP_FAILED)
		die("mmap");

	if (madvise(base, total, MADV_NOHUGEPAGE) != 0)
		fprintf(stderr, "warning: madvise(MADV_NOHUGEPAGE): %s\n",
			strerror(errno));

	/*
	 * Readahead allocates multi-page folios in the page cache, and the
	 * page_idle bitmap silently ignores tail pages — only the head PFN
	 * of a folio is trackable. MADV_RANDOM disables readahead on this
	 * mapping so every fault allocates an order-0 folio and each PFN's
	 * idle bit works individually. (MADV_NOHUGEPAGE alone does not
	 * control page cache folio order.)
	 */
	if (madvise(base, total, MADV_RANDOM) != 0)
		fprintf(stderr, "warning: madvise(MADV_RANDOM): %s\n",
			strerror(errno));

	unsigned char *hot = base;
	unsigned char *stream = base + hot_size;
	unsigned char *unused = base + hot_size + stream_size;
	unsigned char *unfaulted = unused + unused_size;

	fprintf(stderr,
		"mapping %zu bytes at %p (page size %zu)\n"
		"  hot      %p +%zu\n"
		"  stream   %p +%zu\n"
		"  unused   %p +%zu\n"
		"  unfaulted %p +%zu (mapped only)\n",
		total, (void *)base, page_size,
		(void *)hot, hot_size, (void *)stream, stream_size,
		(void *)unused, unused_size, (void *)unfaulted, unfaulted_size);

	fprintf(stderr, "prefaulting the three tracked regions...\n");
	touch_region(hot, hot_size, page_size);
	touch_region(stream, stream_size, page_size);
	touch_region(unused, unused_size, page_size);
	fprintf(stderr, "prefault done (checksum %lu)\n", g_checksum);

	write_metadata(meta_path, page_size, base, total,
		       hot_size, stream_size, unused_size, unfaulted_size,
		       hot_rounds);

	/*
	 * Phase 1: establish the idle observation window.
	 *
	 * READY tells the orchestrator that prefault and metadata publication are
	 * complete. This process then waits on the FIFO while the observer maps
	 * virtual pages to PFNs, marks those PFNs idle, and verifies the bitmap.
	 */
	printf("READY\n");
	fflush(stdout);

	/*
	 * Command loop. Each writer (echo cmd > fifo) produces EOF when it
	 * closes the FIFO, so reopen after every EOF until "exit" arrives.
	 */
	int running = 1;
	while (running) {
		FILE *fifo = fopen(fifo_path, "r");
		if (!fifo)
			die("fopen control fifo");

		char line[128];
		while (fgets(line, sizeof(line), fifo)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (line[0] == '\0')
				continue;

			if (strcmp(line, "run") == 0) {
				/*
				 * Phase 2: controlled access window.
				 *
				 * HOT is touched repeatedly, STREAM exactly once, and
				 * UNUSED not at all. Idle tracking should therefore see
				 * HOT and STREAM as non-idle, but keep UNUSED idle.
				 */
				fprintf(stderr,
					"access phase: hot x%ld, stream x1, "
					"unused untouched\n", hot_rounds);
				for (long i = 0; i < hot_rounds; i++)
					touch_region(hot, hot_size, page_size);
				touch_region(stream, stream_size, page_size);
				fprintf(stderr,
					"access phase done (checksum %lu)\n",
					g_checksum);

				/*
				 * Phase 3: post-access observation.
				 *
				 * Announce completion but keep the mapping and process alive
				 * so the orchestrator can re-read pagemap and the idle bitmap.
				 */
				printf("ACCESS_DONE\n");
				fflush(stdout);
			} else if (strcmp(line, "exit") == 0) {
				/* Phase 4: the observer is done; leave the command loop. */
				running = 0;
				break;
			} else {
				fprintf(stderr, "unknown command: %s\n", line);
			}
		}
		fclose(fifo);
	}

	/* Phase 4: release the workload-owned mapping and file descriptor. */
	munmap(base, total);
	close(fd);
	fprintf(stderr, "workload exiting\n");
	return EXIT_SUCCESS;
}
