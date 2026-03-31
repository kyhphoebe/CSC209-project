/*
 * worker.c — Monte Carlo worker process
 *
 * Usage (invoked by controller via execl):
 *   ./worker <task_read_fd> <result_write_fd> <seed>
 *
 * The worker reads task_msg_t structs from task_read_fd in a loop.
 * For each task it runs num_trials random Monte Carlo trials estimating π,
 * then writes a result_msg_t back on result_write_fd.
 * When a task with num_trials == SHUTDOWN_SENTINEL (0) is received, the
 * worker closes both pipe ends and exits with status 0.
 *
 * Error handling:
 *   - If read() on the task pipe returns 0 (pipe closed by parent) or -1,
 *     the worker treats it as an implicit shutdown and exits.
 *   - If write() on the result pipe fails, the worker exits immediately;
 *     the parent will detect this via SIGCHLD / waitpid.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "montecarlo.h"

/*
 * run_trials – perform `num_trials` Monte Carlo samples.
 *
 * Generates random points (x, y) uniformly in [0, 1) × [0, 1) and
 * counts how many satisfy x² + y² ≤ 1 (inside the unit-circle quadrant).
 * Uses rand_r() with a caller-supplied seed for re-entrancy; the seed is
 * updated after each call so successive invocations produce independent
 * streams.
 *
 * Returns the number of hits (points inside the circle).
 */
static uint32_t run_trials(uint32_t num_trials, unsigned int *seed)
{
    uint32_t hits = 0;
    for (uint32_t i = 0; i < num_trials; i++) {
        double x = (double)rand_r(seed) / ((double)RAND_MAX + 1.0);
        double y = (double)rand_r(seed) / ((double)RAND_MAX + 1.0);
        if (x * x + y * y <= 1.0) {
            hits++;
        }
    }
    return hits;
}

/*
 * read_full – read exactly `len` bytes from fd into buf.
 *
 * Retries on EINTR.  Returns the number of bytes read; a short read
 * (including 0 on EOF) is returned as-is so the caller can detect it.
 */
static ssize_t read_full(int fd, void *buf, size_t len)
{
    size_t total = 0;
    char *p = (char *)buf;
    while (total < len) {
        ssize_t n = read(fd, p + total, len - total);
        if (n == 0) {
            return (ssize_t)total;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/*
 * write_full – write exactly `len` bytes from buf to fd.
 *
 * Retries on EINTR.  Returns 0 on success, -1 on error.
 */
static int write_full(int fd, const void *buf, size_t len)
{
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < len) {
        ssize_t n = write(fd, p + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "worker: usage: worker <task_read_fd> <result_write_fd> <seed>\n");
        return 1;
    }

    int task_fd   = atoi(argv[1]);
    int result_fd = atoi(argv[2]);
    unsigned int seed = (unsigned int)atoi(argv[3]);

    if (task_fd < 0 || result_fd < 0) {
        fprintf(stderr, "worker: invalid file descriptors\n");
        return 1;
    }

    while (1) {
        task_msg_t task;
        ssize_t n = read_full(task_fd, &task, sizeof(task));

        if (n == 0) {
            /* Parent closed the write end – treat as implicit shutdown. */
            break;
        }
        if (n < 0) {
            fprintf(stderr, "worker: read task pipe: %s\n", strerror(errno));
            close(task_fd);
            close(result_fd);
            return 1;
        }
        if ((size_t)n < sizeof(task)) {
            /* Partial read — pipe closed mid-message; treat as shutdown. */
            break;
        }

        if (task.num_trials == SHUTDOWN_SENTINEL) {
            break;
        }

        uint32_t hits = run_trials(task.num_trials, &seed);

        result_msg_t result;
        result.task_id    = task.task_id;
        result.num_trials = task.num_trials;
        result.num_hits   = hits;

        if (write_full(result_fd, &result, sizeof(result)) < 0) {
            fprintf(stderr, "worker: write result pipe: %s\n", strerror(errno));
            close(task_fd);
            close(result_fd);
            return 1;
        }
    }

    close(task_fd);
    close(result_fd);
    return 0;
}
