/*
 * worker.c - Worker-side message loop and simulation execution.
 *
 * Invoked by controller via exec:
 *   ./worker <task_read_fd> <result_write_fd> <seed>
 *
 * Protocol behavior:
 * - Receives task_msg_t from task_read_fd.
 * - Handles TASK_SIMULATE / TASK_STATUS_REQ / TASK_SHUTDOWN.
 * - Sends result_msg_t responses to result_write_fd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "montecarlo.h"

/*
 * Run num_trials Monte Carlo samples using rand_r(seed).
 * Returns number of points that satisfy x*x + y*y <= 1.0.
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
 * Read exactly len bytes unless EOF/error occurs first.
 * Retries on EINTR and returns bytes read (or -1 on error).
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
            if (errno != EINTR) {
                return -1;
            }
        } else {
            total += (size_t)n;
        }
    }
    return (ssize_t)total;
}

/*
 * Write exactly len bytes.
 * Retries on EINTR and returns 0 on success, -1 on error.
 */
static int write_full(int fd, const void *buf, size_t len)
{
    size_t total = 0;
    const char *p = (const char *)buf;
    while (total < len) {
        ssize_t n = write(fd, p + total, len - total);
        if (n < 0) {
            if (errno != EINTR) {
                return -1;
            }
        } else {
            total += (size_t)n;
        }
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

    uint32_t tasks_completed = 0;
    int running = 1;
    while (running) {
        task_msg_t task;
        ssize_t n = read_full(task_fd, &task, sizeof(task));

        if (n == 0) {
            /* Parent closed the write end – treat as implicit shutdown. */
            running = 0;
        } else if (n < 0) {
            fprintf(stderr, "worker: read task pipe: %s\n", strerror(errno));
            close(task_fd);
            close(result_fd);
            return 1;
        } else if ((size_t)n < sizeof(task)) {
            /* Partial read — pipe closed mid-message; treat as shutdown. */
            running = 0;
        } else if (task.version != PROTOCOL_VERSION) {
            /* Version mismatch is recoverable: return RESULT_ERROR and wait for next request. */
            result_msg_t mismatch;
            mismatch.version         = PROTOCOL_VERSION;
            mismatch.task_id         = task.task_id;
            mismatch.msg_type        = RESULT_ERROR;
            mismatch.num_trials      = 0;
            mismatch.num_hits        = 0;
            mismatch.batch_index     = 0;
            mismatch.batch_total     = 0;
            mismatch.tasks_completed = tasks_completed;
            mismatch.error_code      = EPROTO;
            (void)write_full(result_fd, &mismatch, sizeof(mismatch));
        } else if (task.msg_type == TASK_SHUTDOWN) {
            running = 0;
        } else {
            result_msg_t result;
            int response_written = 0;
            result.version         = PROTOCOL_VERSION;
            result.task_id         = task.task_id;
            result.msg_type        = RESULT_ERROR;
            result.num_trials      = 0;
            result.num_hits        = 0;
            result.batch_index     = 0;
            result.batch_total     = 0;
            result.tasks_completed = tasks_completed;
            result.error_code      = 0;

            if (task.msg_type == TASK_SIMULATE) {
                if (task.num_trials == 0) {
                    result.msg_type = RESULT_ERROR;
                    result.error_code = EINVAL;
                    if (write_full(result_fd, &result, sizeof(result)) < 0) {
                        fprintf(stderr, "worker: write result pipe: %s\n", strerror(errno));
                        close(task_fd);
                        close(result_fd);
                        return 1;
                    }
                    response_written = 1;
                } else {
                    uint32_t batch_trials = task.batch_trials == 0
                                                ? SIM_BATCH_TRIALS
                                                : task.batch_trials;
                    uint32_t batch_total =
                        (task.num_trials + batch_trials - 1U) / batch_trials;
                    uint32_t trials_left = task.num_trials;
                    tasks_completed++;

                    /* Stream one RESULT_SIMULATE per batch so controller can aggregate incrementally. */
                    for (uint32_t batch = 1; batch <= batch_total; batch++) {
                        uint32_t this_batch = trials_left > batch_trials
                                                  ? batch_trials
                                                  : trials_left;
                        uint32_t hits = run_trials(this_batch, &seed);

                        result.msg_type = RESULT_SIMULATE;
                        result.num_trials = this_batch;
                        result.num_hits = hits;
                        result.batch_index = batch;
                        result.batch_total = batch_total;
                        result.tasks_completed = tasks_completed;
                        result.error_code = 0;

                        if (write_full(result_fd, &result, sizeof(result)) < 0) {
                            fprintf(stderr, "worker: write result pipe: %s\n", strerror(errno));
                            close(task_fd);
                            close(result_fd);
                            return 1;
                        }
                        trials_left -= this_batch;
                    }
                    response_written = 1;
                }
            } else if (task.msg_type == TASK_STATUS_REQ) {
                result.msg_type = RESULT_STATUS;
                result.tasks_completed = tasks_completed;
            } else {
                result.msg_type = RESULT_ERROR;
                result.error_code = EPROTO;
            }

            if (!response_written) {
                if (write_full(result_fd, &result, sizeof(result)) < 0) {
                    fprintf(stderr, "worker: write result pipe: %s\n", strerror(errno));
                    close(task_fd);
                    close(result_fd);
                    return 1;
                }
            }
        }
    }

    close(task_fd);
    close(result_fd);
    return 0;
}
