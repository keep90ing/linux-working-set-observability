/*
 * mglru_workload.c — workload for the MGLRU / workingset refault experiment.
 *
 * One file-backed mmap() split into two regions:
 *   HOT    — a background thread loops over it continuously once started
 *   STREAM — sequentially scanned on command, twice
 *
 * The process is expected to be placed in a memory-limited cgroup by the
 * orchestrator *before* it starts faulting pages.
 *
 * Experiment phases:
 *   Setup     — create a cold file-backed mapping and publish metadata
 *   T0        — start and warm up the continuously accessed HOT region
 *   T0 -> T1  — scan STREAM for the first time to trigger reclaim/eviction
 *   T1 -> T2  — scan STREAM again to revisit evicted pages and cause refaults
 *   Cleanup   — stop the HOT thread, release resources, and terminate
 *
 * FIFO commands drive those phases:
 *   start_hot        — start the HOT loop thread, report HOT_STARTED
 *   scan1            — scan STREAM once, report SCAN1_DONE
 *   scan2            — scan STREAM again, report SCAN2_DONE
 *   snapshot <PHASE> — mincore() residency of both regions, append JSON
 *                      lines to the residency file, report
 *                      SNAPSHOT_DONE:<PHASE>
 *   exit             — stop the HOT thread and terminate
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static volatile unsigned long g_checksum;
static volatile int g_stop;

static unsigned char *g_hot;
static size_t g_hot_size;
static size_t g_page_size;
static int g_hot_cpu = -1;

static void die(const char *msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

static size_t parse_size(const char *s)
{
	char *end = NULL;
	errno = 0;
	unsigned long long v = strtoull(s, &end, 10);
	if (errno != 0 || end == s) {
		fprintf(stderr, "invalid size: %s\n", s);
		exit(EXIT_FAILURE);
	}
	switch (toupper((unsigned char)*end)) {
	case 'K': v <<= 10; end++; break;
	case 'M': v <<= 20; end++; break;
	case 'G': v <<= 30; end++; break;
	case '\0': break;
	default:
		fprintf(stderr, "invalid size suffix in: %s\n", s);
		exit(EXIT_FAILURE);
	}
	if (*end != '\0' || v == 0) {
		fprintf(stderr, "invalid size: %s\n", s);
		exit(EXIT_FAILURE);
	}
	return (size_t)v;
}

static size_t round_up(size_t v, size_t align)
{
	return (v + align - 1) / align * align;
}

static void set_self_affinity(int cpu, const char *who)
{
	cpu_set_t set;

	if (cpu < 0)
		return;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
		fprintf(stderr, "warning: failed to pin %s to CPU %d\n",
			who, cpu);
	else
		fprintf(stderr, "%s pinned to CPU %d\n", who, cpu);
}

/* Read one byte per page across [base, base+length). */
static void touch_region(const unsigned char *base, size_t length)
{
	const volatile unsigned char *p = base;
	unsigned long sum = 0;

	for (size_t off = 0; off < length; off += g_page_size)
		sum += p[off];
	g_checksum += sum;
}

static void *hot_loop(void *arg)
{
	(void)arg;
	set_self_affinity(g_hot_cpu, "HOT thread");
	unsigned long rounds = 0;

	while (!g_stop) {
		touch_region(g_hot, g_hot_size);
		rounds++;
	}
	fprintf(stderr, "HOT thread exiting after %lu rounds\n", rounds);
	return NULL;
}

static void snapshot(FILE *out, const char *phase, const char *region,
		     unsigned char *base, size_t length)
{
	size_t npages = length / g_page_size;
	unsigned char *vec = malloc(npages);
	size_t resident = 0;

	if (!vec)
		die("malloc mincore vec");
	if (mincore(base, length, vec) != 0)
		die("mincore");
	for (size_t i = 0; i < npages; i++)
		resident += vec[i] & 1;
	free(vec);

	fprintf(out,
		"{\"phase\":\"%s\",\"region\":\"%s\","
		"\"resident_pages\":%zu,\"total_pages\":%zu}\n",
		phase, region, resident, npages);
	fprintf(stderr, "snapshot %s: %s %zu/%zu resident\n",
		phase, region, resident, npages);
}

static void write_metadata(const char *path, const unsigned char *base,
			   size_t total, size_t hot_size, size_t stream_size)
{
	FILE *f = fopen(path, "w");
	if (!f)
		die("fopen metadata");

	uintptr_t start = (uintptr_t)base;
	fprintf(f,
		"{\n"
		"  \"pid\": %ld,\n"
		"  \"page_size\": %zu,\n"
		"  \"mapping\": {\n"
		"    \"start\": \"0x%" PRIxPTR "\",\n"
		"    \"length\": %zu\n"
		"  },\n"
		"  \"regions\": {\n"
		"    \"hot\": { \"start\": \"0x%" PRIxPTR "\", \"length\": %zu },\n"
		"    \"stream\": { \"start\": \"0x%" PRIxPTR "\", \"length\": %zu }\n"
		"  }\n"
		"}\n",
		(long)getpid(), g_page_size, start, total,
		start, hot_size,
		start + hot_size, stream_size);
	if (fclose(f) != 0)
		die("fclose metadata");
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s --file PATH --control-fifo PATH --metadata PATH\n"
		"          --residency PATH [--hot-size SIZE (64M)]\n"
		"          [--stream-size SIZE (1G)] [--hot-cpu CPU]\n"
		"          [--stream-cpu CPU]\n",
		prog);
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	const char *file_path = NULL, *fifo_path = NULL;
	const char *meta_path = NULL, *residency_path = NULL;
	size_t hot_size = 64UL << 20;
	size_t stream_size = 1UL << 30;
	int stream_cpu = -1;

	static const struct option opts[] = {
		{ "file",         required_argument, NULL, 'f' },
		{ "hot-size",     required_argument, NULL, 'h' },
		{ "stream-size",  required_argument, NULL, 's' },
		{ "control-fifo", required_argument, NULL, 'c' },
		{ "metadata",     required_argument, NULL, 'm' },
		{ "residency",    required_argument, NULL, 'j' },
		{ "hot-cpu",      required_argument, NULL, 'H' },
		{ "stream-cpu",   required_argument, NULL, 'S' },
		{ NULL, 0, NULL, 0 }
	};

	int c;
	while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		switch (c) {
		case 'f': file_path = optarg; break;
		case 'h': hot_size = parse_size(optarg); break;
		case 's': stream_size = parse_size(optarg); break;
		case 'c': fifo_path = optarg; break;
		case 'm': meta_path = optarg; break;
		case 'j': residency_path = optarg; break;
		case 'H': g_hot_cpu = atoi(optarg); break;
		case 'S': stream_cpu = atoi(optarg); break;
		default: usage(argv[0]);
		}
	}
	if (!file_path || !fifo_path || !meta_path || !residency_path)
		usage(argv[0]);

	/*
	 * Setup phase: create the controlled file-backed address space.
	 *
	 * The orchestrator has already placed this process in the experiment
	 * memory cgroup. Build one page-aligned mapping for HOT and STREAM, but
	 * do not prefault it: the later commands intentionally create the page
	 * faults and memory pressure measured at T0, T1, and T2.
	 */
	long ps = sysconf(_SC_PAGESIZE);
	if (ps <= 0)
		die("sysconf(_SC_PAGESIZE)");
	g_page_size = (size_t)ps;

	hot_size = round_up(hot_size, g_page_size);
	stream_size = round_up(stream_size, g_page_size);
	size_t total = hot_size + stream_size;

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
	 * Ask the kernel to drop any page cache this file already has, so
	 * the experiment starts from a cold cache without touching the
	 * global drop_caches knob.
	 */
	err = posix_fadvise(fd, 0, (off_t)total, POSIX_FADV_DONTNEED);
	if (err != 0)
		fprintf(stderr, "warning: posix_fadvise(DONTNEED): %s\n",
			strerror(err));

	g_hot = base;
	g_hot_size = hot_size;
	unsigned char *stream = base + hot_size;

	set_self_affinity(stream_cpu, "main (STREAM) thread");

	fprintf(stderr,
		"mapping %zu bytes at %p (page size %zu)\n"
		"  hot    %p +%zu\n  stream %p +%zu\n",
		total, (void *)base, g_page_size,
		(void *)g_hot, hot_size, (void *)stream, stream_size);

	write_metadata(meta_path, base, total, hot_size, stream_size);

	FILE *residency = fopen(residency_path, "a");
	if (!residency)
		die("fopen residency file");
	setvbuf(residency, NULL, _IOLBF, 0);

	/*
	 * Setup complete. READY lets the orchestrator begin the phased run and
	 * collect all later snapshots while this mapping remains alive.
	 */
	printf("READY\n");
	fflush(stdout);

	pthread_t hot_thread;
	int hot_running = 0;
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

			if (strcmp(line, "start_hot") == 0) {
				/*
				 * T0 preparation: start continuous HOT access. The
				 * orchestrator allows a short warm-up before requesting the
				 * T0 residency and counter snapshots.
				 */
				if (!hot_running) {
					if (pthread_create(&hot_thread, NULL,
							   hot_loop, NULL))
						die("pthread_create");
					hot_running = 1;
				}
				printf("HOT_STARTED\n");
				fflush(stdout);
			} else if (strcmp(line, "scan1") == 0) {
				/*
				 * T0 -> T1: first STREAM pass. Because STREAM is larger
				 * than the cgroup limit, this pass should create reclaim and
				 * evict some of the earlier file-backed pages.
				 */
				fprintf(stderr, "STREAM scan 1...\n");
				touch_region(stream, stream_size);
				printf("SCAN1_DONE\n");
				fflush(stdout);
			} else if (strcmp(line, "scan2") == 0) {
				/*
				 * T1 -> T2: second STREAM pass. Revisiting pages evicted
				 * during the first pass is expected to produce workingset
				 * refault feedback while HOT continues running.
				 */
				fprintf(stderr, "STREAM scan 2...\n");
				touch_region(stream, stream_size);
				printf("SCAN2_DONE\n");
				fflush(stdout);
			} else if (strncmp(line, "snapshot", 8) == 0) {
				/*
				 * T0/T1/T2 observation point: record only point-in-time
				 * residency with mincore(). Counter and MGLRU snapshots are
				 * collected separately by the orchestrator.
				 */
				const char *phase = line[8] == ' ' ?
					line + 9 : "unknown";
				snapshot(residency, phase, "hot",
					 g_hot, hot_size);
				snapshot(residency, phase, "stream",
					 stream, stream_size);
				fflush(residency);
				printf("SNAPSHOT_DONE:%s\n", phase);
				fflush(stdout);
			} else if (strcmp(line, "exit") == 0) {
				/* Cleanup phase: all T2 observations are complete. */
				running = 0;
				break;
			} else {
				fprintf(stderr, "unknown command: %s\n", line);
			}
		}
		fclose(fifo);
	}

	/* Cleanup phase: stop concurrent access before unmapping the regions. */
	if (hot_running) {
		g_stop = 1;
		pthread_join(hot_thread, NULL);
	}
	fclose(residency);
	munmap(base, total);
	close(fd);
	fprintf(stderr, "workload exiting (checksum %lu)\n", g_checksum);
	return EXIT_SUCCESS;
}
