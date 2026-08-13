// SPDX-License-Identifier: GPL-2.0
/*
 * Process creation microbenchmark.
 *
 * perf bench has no fork/exec test, and process creation is exactly where
 * a per-mm allocation or a per-task struct growth shows up.  Three modes,
 * because they stress different things and a change can move one without
 * moving the others:
 *
 *   fork  - fork() + immediate _exit() in the child.  Measures mm
 *           duplication and task teardown.  This is the mode that would
 *           show a per-mm scheduler allocation.
 *   exec  - fork() + execve() of /bin/true.  Adds mm teardown and a fresh
 *           address space, closer to what a shell or build system does.
 *   thread - pthread_create() + join.  Shares the mm, so a difference
 *           between this and "fork" localises the cost to mm handling
 *           rather than to task_struct.
 *
 * Prints elapsed nanoseconds per iteration to stdout, nothing else, so the
 * driver can consume it directly.
 *
 * Build: cc -O2 -o forkbench forkbench.c -lpthread
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void *thread_noop(void *arg)
{
	(void)arg;
	return NULL;
}

static int do_fork(void)
{
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0)
		_exit(0);

	if (waitpid(pid, NULL, 0) < 0)
		return -1;
	return 0;
}

static int do_exec(void)
{
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		char *const argv[] = { (char *)"/bin/true", NULL };
		char *const envp[] = { NULL };

		execve("/bin/true", argv, envp);
		_exit(127);
	}

	if (waitpid(pid, NULL, 0) < 0)
		return -1;
	return 0;
}

static int do_thread(void)
{
	pthread_t t;

	if (pthread_create(&t, NULL, thread_noop, NULL) != 0)
		return -1;
	if (pthread_join(t, NULL) != 0)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "fork";
	long iters = argc > 2 ? atol(argv[2]) : 10000;
	int (*op)(void);
	struct timespec start, end;
	double ns;
	long i;

	if (!strcmp(mode, "fork")) {
		op = do_fork;
	} else if (!strcmp(mode, "exec")) {
		op = do_exec;
	} else if (!strcmp(mode, "thread")) {
		op = do_thread;
	} else {
		fprintf(stderr, "usage: %s [fork|exec|thread] [iterations]\n",
			argv[0]);
		return 2;
	}

	if (iters < 1) {
		fprintf(stderr, "iterations must be positive\n");
		return 2;
	}

	/*
	 * Warm the path once so the first timed iteration is not paying for
	 * lazy binding, page faults on the child stack, or a cold /bin/true
	 * page cache entry.
	 */
	for (i = 0; i < 100; i++) {
		if (op() < 0) {
			fprintf(stderr, "warmup failed: %s\n", strerror(errno));
			return 1;
		}
	}

	if (clock_gettime(CLOCK_MONOTONIC, &start) < 0)
		return 1;

	for (i = 0; i < iters; i++) {
		if (op() < 0) {
			fprintf(stderr, "iteration %ld failed: %s\n", i,
				strerror(errno));
			return 1;
		}
	}

	if (clock_gettime(CLOCK_MONOTONIC, &end) < 0)
		return 1;

	ns = (double)(end.tv_sec - start.tv_sec) * 1e9 +
	     (double)(end.tv_nsec - start.tv_nsec);

	printf("%.1f\n", ns / (double)iters);
	return 0;
}
