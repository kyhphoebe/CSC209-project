/*
 * controller.c - Parent process for orchestration and aggregation.
 *
 * Usage:
 *   ./controller [num_workers [seed]]
 *
 * Main responsibilities:
 * - Spawn a worker pool and keep per-worker pipe/pid state.
 * - Accept interactive commands (simulate, setbatch, partial, stats, workers, quit).
 * - Send typed task messages and validate typed responses.
 * - Aggregate batched simulation results and print statistics.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <limits.h>

#include "montecarlo.h"

/* Maximum workers supported. */
#define MAX_WORKERS 64

/* Worker process and pipe endpoints owned by controller. */
typedef struct {
    pid_t  pid;
    int    task_write_fd;   /* controller writes task_msg_t here  */
    int    result_read_fd;  /* controller reads result_msg_t here */
    int    alive;           /* 1 = running, 0 = dead/reaped       */
} worker_t;

static worker_t workers[MAX_WORKERS];
static int      num_workers = 0;
static uint32_t simulate_batch_trials = SIM_BATCH_TRIALS;
/* When non-zero, print each RESULT_SIMULATE as it is read. */
static int      trace_partial_results = 0;

static void close_fd_if_open(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void close_worker_fds(int i)
{
    close_fd_if_open(&workers[i].task_write_fd);
    close_fd_if_open(&workers[i].result_read_fd);
}

static void mark_worker_dead(int i)
{
    workers[i].alive = 0;
    close_worker_fds(i);
}

/*
 * Reap exited children without blocking, close their pipe fds, and clear pid.
 * Matches shutdown/mark_worker_dead cleanup so descriptors do not linger after
 * unexpected worker death.
 */
static void sigchld_handler(int signo)
{
    (void)signo;
    int saved_errno = errno;
    pid_t pid;
    int   status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int found = 0;
        for (int i = 0; i < num_workers && !found; i++) {
            if (workers[i].pid == pid) {
                workers[i].alive = 0;
                close_worker_fds(i);
                workers[i].pid = 0;
                found = 1;
            }
        }
    }
    errno = saved_errno;
}

/* I/O helpers for fixed-size message transport. */
static ssize_t read_full(int fd, void *buf, size_t len)
{
    size_t total = 0;
    char  *p = (char *)buf;
    while (total < len) {
        ssize_t n = read(fd, p + total, len - total);
        if (n == 0) return (ssize_t)total;
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

static int write_full(int fd, const void *buf, size_t len)
{
    size_t      total = 0;
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

/*
 * Spawn n workers and initialize controller-side worker state.
 *
 * On failure, rolls back partial startup:
 * - closes created pipe FDs
 * - terminates/reaps spawned children
 * - resets worker bookkeeping
 */
static int spawn_workers(int n, unsigned int base_seed)
{
    /* Two pipe FDs per worker: [0]=read, [1]=write */
    int task_pipes[MAX_WORKERS][2];
    int result_pipes[MAX_WORKERS][2];
    int pipes_ready = 0;
    int spawned = 0;

    for (int i = 0; i < n; i++) {
        task_pipes[i][0] = -1;
        task_pipes[i][1] = -1;
        result_pipes[i][0] = -1;
        result_pipes[i][1] = -1;

        workers[i].pid = 0;
        workers[i].task_write_fd = -1;
        workers[i].result_read_fd = -1;
        workers[i].alive = 0;
    }

    for (int i = 0; i < n; i++) {
        if (pipe(task_pipes[i]) < 0) {
            perror("controller: pipe (task)");
            goto fail;
        }
        if (pipe(result_pipes[i]) < 0) {
            perror("controller: pipe (result)");
            goto fail;
        }
        pipes_ready++;
    }

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("controller: fork");
            goto fail;
        }

        if (pid == 0) {
            /* ---- child ---- */

            /* Close all pipe ends that belong to other workers. */
            for (int j = 0; j < n; j++) {
                if (j != i) {
                    close(task_pipes[j][0]);
                    close(task_pipes[j][1]);
                    close(result_pipes[j][0]);
                    close(result_pipes[j][1]);
                }
            }
            /* Close the controller-side ends of our own pipes. */
            close(task_pipes[i][1]);    /* parent writes here, child doesn't */
            close(result_pipes[i][0]);  /* parent reads here, child doesn't  */

            /* Build argv strings for the worker. */
            char task_fd_str[16], result_fd_str[16], seed_str[16];
            snprintf(task_fd_str,   sizeof(task_fd_str),   "%d", task_pipes[i][0]);
            snprintf(result_fd_str, sizeof(result_fd_str), "%d", result_pipes[i][1]);
            snprintf(seed_str,      sizeof(seed_str),      "%u", base_seed + (unsigned int)i);

            execl("./worker", "./worker", task_fd_str, result_fd_str, seed_str,
                  (char *)NULL);
            /* execl only returns on error. */
            perror("controller: execl worker");
            _exit(1);
        }

        /* ---- parent ---- */
        /* Close the worker-side ends; we don't need them. */
        close(task_pipes[i][0]);
        close(result_pipes[i][1]);

        workers[i].pid           = pid;
        workers[i].task_write_fd = task_pipes[i][1];
        workers[i].result_read_fd = result_pipes[i][0];
        workers[i].alive         = 1;
        spawned++;
    }

    num_workers = n;
    return 0;

fail:
    for (int i = 0; i < pipes_ready; i++) {
        if (task_pipes[i][0] >= 0) close(task_pipes[i][0]);
        if (task_pipes[i][1] >= 0) close(task_pipes[i][1]);
        if (result_pipes[i][0] >= 0) close(result_pipes[i][0]);
        if (result_pipes[i][1] >= 0) close(result_pipes[i][1]);
    }

    for (int i = 0; i < spawned; i++) {
        close_worker_fds(i);
        if (workers[i].pid > 0) {
            kill(workers[i].pid, SIGTERM);
            if (waitpid(workers[i].pid, NULL, 0) < 0 && errno != ECHILD) {
                perror("controller: waitpid rollback");
            }
        }
        workers[i].pid = 0;
        workers[i].alive = 0;
    }
    num_workers = 0;
    return -1;
}

/*
 * Graceful shutdown path:
 * - send TASK_SHUTDOWN to alive workers
 * - close controller-side FDs for all workers
 * - waitpid() all known child PIDs
 */
static void shutdown_workers(void)
{
    task_msg_t shutdown_msg;
    shutdown_msg.version    = PROTOCOL_VERSION;
    shutdown_msg.task_id    = 0;
    shutdown_msg.msg_type   = TASK_SHUTDOWN;
    shutdown_msg.num_trials = 0;
    shutdown_msg.batch_trials = 0;

    for (int i = 0; i < num_workers; i++) {
        if (workers[i].alive &&
            write_full(workers[i].task_write_fd, &shutdown_msg,
                       sizeof(shutdown_msg)) < 0) {
            /* Worker may have already died; SIGCHLD will have marked it. */
            if (errno != EPIPE) {
                perror("controller: write shutdown");
            }
        }
        close_worker_fds(i);
    }

    /* Blocking wait for every child. */
    for (int i = 0; i < num_workers; i++) {
        if (workers[i].pid > 0) {
            int status;
            if (waitpid(workers[i].pid, &status, 0) < 0) {
                if (errno != ECHILD) {
                    perror("controller: waitpid");
                }
            }
            workers[i].alive = 0;
            workers[i].pid = 0;
        }
    }
}

/*
 * Run one simulation command across alive workers.
 *
 * Flow:
 * - partition total_trials across alive workers
 * - send TASK_SIMULATE request to each selected worker
 * - read batched RESULT_SIMULATE messages until each worker's chunk is complete
 * - validate version/task_id/message type/batch metadata
 * - aggregate totals and print estimate and CI
 */
static void run_simulation(uint32_t total_trials, uint32_t task_id)
{
    /* Count alive workers. */
    int alive_count = 0;
    for (int i = 0; i < num_workers; i++) {
        if (workers[i].alive) alive_count++;
    }

    if (alive_count == 0) {
        fprintf(stderr, "controller: no alive workers\n");
        return;
    }

    uint32_t base_chunk = total_trials / (uint32_t)alive_count;
    uint32_t remainder  = total_trials % (uint32_t)alive_count;

    /* Send tasks.
     * Distribute remainder one-by-one to the first `remainder` workers so
     * every worker that is sent a task gets at least 1 trial.
     * Workers with a computed chunk of 0 are skipped and remain idle. */
    int worker_indices[MAX_WORKERS];
    uint32_t expected_trials_for_worker[MAX_WORKERS];
    uint32_t batch_trials_for_worker[MAX_WORKERS];
    int sent      = 0; /* index among alive workers, used to assign remainder */
    int alive_idx = 0;

    for (int i = 0; i < num_workers; i++) {
        if (workers[i].alive) {
            /* Workers 0 .. remainder-1 each get one extra trial. */
            uint32_t chunk = base_chunk + (alive_idx < (int)remainder ? 1u : 0u);
            alive_idx++;

            if (chunk > 0) {
                task_msg_t task;
                int write_ok = 1;
                task.version    = PROTOCOL_VERSION;
                task.task_id    = task_id;
                task.msg_type   = TASK_SIMULATE;
                task.num_trials = chunk;
                task.batch_trials = simulate_batch_trials;

                if (write_full(workers[i].task_write_fd, &task, sizeof(task)) < 0) {
                    write_ok = 0;
                    if (errno == EPIPE) {
                        fprintf(stderr,
                                "controller: worker %d pipe broken, skipping\n", i);
                    } else {
                        perror("controller: write task");
                    }
                    mark_worker_dead(i);
                }

                if (write_ok) {
                    /* Persist per-worker expectations so receive-side validation is strict. */
                    worker_indices[sent] = i;
                    expected_trials_for_worker[sent] = chunk;
                    batch_trials_for_worker[sent] = simulate_batch_trials;
                    sent++;
                }
            }
        }
    }

    if (sent == 0) {
        fprintf(stderr, "controller: no workers accepted the task\n");
        return;
    }

    /* Collect partial results from workers that were sent a task. */
    uint64_t total_hits   = 0;
    uint64_t total_actual = 0;
    int      workers_completed = 0;
    int      partial_messages  = 0;

    for (int s = 0; s < sent; s++) {
        int i = worker_indices[s];
        uint32_t expected_trials_i = expected_trials_for_worker[s];
        uint32_t received_trials_i = 0;
        uint32_t batch_trials_i = batch_trials_for_worker[s];
        uint32_t expected_batches_i =
            (expected_trials_i + batch_trials_i - 1U) / batch_trials_i;
        uint32_t last_batch_seen = 0;
        int worker_ok = 1;

        /* Keep reading partial messages until this worker's assigned chunk is done. */
        /*
         * Lines 381-443: stderr for result read / protocol errors; stdout partial trace.
         * Drafted with Claude; I reviewed, modified, and tested it.
         */
        while (received_trials_i < expected_trials_i && worker_ok) {
            result_msg_t result;
            ssize_t n = read_full(workers[i].result_read_fd, &result, sizeof(result));
            if (n <= 0 || (size_t)n < sizeof(result)) {
                fprintf(stderr,
                        "controller: failed to read result from worker %d: %s\n",
                        i, n < 0 ? strerror(errno) : "pipe closed");
                mark_worker_dead(i);
                worker_ok = 0;
            } else if (result.task_id != task_id || result.msg_type != RESULT_SIMULATE) {
                fprintf(stderr,
                        "controller: protocol error from worker %d (task %u type %u)\n",
                        i, result.task_id, result.msg_type);
                mark_worker_dead(i);
                worker_ok = 0;
            } else if (result.version != PROTOCOL_VERSION) {
                fprintf(stderr,
                        "controller: protocol version mismatch from worker %d (got %u)\n",
                        i, result.version);
                mark_worker_dead(i);
                worker_ok = 0;
            } else if (result.batch_total != expected_batches_i ||
                       result.batch_index == 0 ||
                       result.batch_index > result.batch_total ||
                       result.batch_index <= last_batch_seen) {
                fprintf(stderr,
                        "controller: invalid batch metadata from worker %d\n", i);
                mark_worker_dead(i);
                worker_ok = 0;
            } else if (result.num_trials == 0 ||
                       result.num_hits > result.num_trials) {
                fprintf(stderr,
                        "controller: invalid trial/hit counts from worker %d "
                        "(trials=%u hits=%u)\n",
                        i, result.num_trials, result.num_hits);
                mark_worker_dead(i);
                worker_ok = 0;
            } else {
                last_batch_seen = result.batch_index;
                received_trials_i += result.num_trials;
                /* Defensive check against over-reporting by a worker. */
                if (received_trials_i > expected_trials_i) {
                    fprintf(stderr,
                            "controller: worker %d exceeded assigned trials\n", i);
                    mark_worker_dead(i);
                    worker_ok = 0;
                } else {
                    total_hits += result.num_hits;
                    total_actual += result.num_trials;
                    partial_messages++;
                    if (trace_partial_results) {
                        printf(
                            "[partial] task %u worker[%d] batch %u/%u  "
                            "trials=%u hits=%u  cumulative trials=%lu hits=%lu\n",
                            task_id, i, result.batch_index, result.batch_total,
                            result.num_trials, result.num_hits,
                            (unsigned long)total_actual,
                            (unsigned long)total_hits);
                        fflush(stdout);
                    }
                }
            }
        }

        if (received_trials_i == expected_trials_i) {
            workers_completed++;
        } else {
            /* Incomplete response stream: retire worker to protect future tasks. */
            mark_worker_dead(i);
        }
    }

    if (total_actual == 0) {
        fprintf(stderr, "controller: received no results\n");
        return;
    }

    /* π estimate. */
    double p      = (double)total_hits / (double)total_actual;
    double pi_est = 4.0 * p;

    /* 95 % confidence interval via normal approximation of the binomial. */
    double margin = 1.96 * sqrt(p * (1.0 - p) / (double)total_actual) * 4.0;

    /*
     * Lines 469-478: stdout simulation summary, π estimate, and confidence interval.
     * Drafted with Claude; I reviewed, modified, and tested it.
     */
    printf("\n--- Simulation results (task %u) ---\n", task_id);
    printf("  Workers responded : %d / %d\n", workers_completed, sent);
    printf("  Partial messages  : %d\n", partial_messages);
    printf("  Trials run        : %lu\n", (unsigned long)total_actual);
    printf("  Hits (in circle)  : %lu\n", (unsigned long)total_hits);
    printf("  π estimate        : %.8f\n", pi_est);
    printf("  95%% CI            : [%.8f, %.8f]\n",
           pi_est - margin, pi_est + margin);
    printf("  Error vs. M_PI    : %+.8f\n", pi_est - M_PI);
    printf("------------------------------------\n\n");
}

static void query_worker_stats(uint32_t task_id)
{
    int worker_indices[MAX_WORKERS];
    int sent = 0;

    for (int i = 0; i < num_workers; i++) {
        if (workers[i].alive) {
            task_msg_t task;
            int write_ok = 1;
            task.version = PROTOCOL_VERSION;
            task.task_id = task_id;
            task.msg_type = TASK_STATUS_REQ;
            task.num_trials = 0;
            task.batch_trials = 0;
            if (write_full(workers[i].task_write_fd, &task, sizeof(task)) < 0) {
                perror("controller: write stats request");
                mark_worker_dead(i);
                write_ok = 0;
            }
            if (write_ok) {
                worker_indices[sent++] = i;
            }
        }
    }

    if (sent == 0) {
        printf("No alive workers to report stats.\n");
        return;
    }

    /*
     * Lines 515-537: stdout/stderr for per-worker stats responses.
     * Drafted with Claude; I reviewed, modified, and tested it.
     */
    printf("\n--- Worker stats (task %u) ---\n", task_id);
    for (int s = 0; s < sent; s++) {
        int i = worker_indices[s];
        result_msg_t result;
        ssize_t n = read_full(workers[i].result_read_fd, &result, sizeof(result));
        if (n <= 0 || (size_t)n < sizeof(result) || result.task_id != task_id) {
            fprintf(stderr, "controller: failed to read stats from worker %d\n", i);
            mark_worker_dead(i);
        } else if (result.version != PROTOCOL_VERSION) {
            fprintf(stderr, "controller: protocol version mismatch from worker %d\n", i);
            mark_worker_dead(i);
        } else if (result.msg_type == RESULT_STATUS) {
            printf("  worker[%d] pid=%d tasks_completed=%u\n",
                   i, workers[i].pid, result.tasks_completed);
        } else if (result.msg_type == RESULT_ERROR) {
            printf("  worker[%d] pid=%d reported error_code=%u\n",
                   i, workers[i].pid, result.error_code);
        } else {
            fprintf(stderr, "controller: unexpected stats msg type from worker %d\n", i);
            mark_worker_dead(i);
        }
    }
    printf("------------------------------\n\n");
}

/* Print command usage for the interactive prompt. */
static void print_help(void)
{
    /*
     * Lines 547-554: stdout interactive help text.
     * Drafted with Claude; I reviewed, modified, and tested it.
     */
    printf("Commands:\n");
    printf("  simulate <N>   run N Monte Carlo trials and estimate π\n");
    printf("  setbatch <N>   set max trials per partial result message\n");
    printf("  partial on|off print each partial result as it arrives\n");
    printf("  stats          query per-worker processed-task counters\n");
    printf("  workers        show number of active worker processes\n");
    printf("  help           show this help message\n");
    printf("  quit           shut down workers and exit\n");
}

/*
 * Program entry point.
 * Initializes signal handlers, starts workers, and runs the REPL loop.
 */
int main(int argc, char *argv[])
{
    int          n          = DEFAULT_NUM_WORKERS;
    unsigned int base_seed  = 42u;

    if (argc >= 2) {
        n = atoi(argv[1]);
        if (n < 3) {
            fprintf(stderr,
                    "controller: num_workers must be at least 3 (got %d)\n", n);
            return 1;
        }
        if (n > MAX_WORKERS) {
            fprintf(stderr,
                    "controller: num_workers exceeds MAX_WORKERS (%d)\n",
                    MAX_WORKERS);
            return 1;
        }
    }

    if (argc >= 3) {
        base_seed = (unsigned int)atoi(argv[2]);
    }

    /* Ignore SIGPIPE so write() to a dead worker returns EPIPE. */
    struct sigaction sa_pipe;
    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa_pipe, NULL) < 0) {
        perror("controller: sigaction SIGPIPE");
        return 1;
    }

    /* Install SIGCHLD handler to reap unexpected worker deaths. */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa_chld, NULL) < 0) {
        perror("controller: sigaction SIGCHLD");
        return 1;
    }

    printf("Monte Carlo π Simulator\n");
    printf("Spawning %d worker processes (base seed %u)...\n", n, base_seed);

    if (spawn_workers(n, base_seed) < 0) {
        fprintf(stderr, "controller: failed to spawn workers\n");
        return 1;
    }

    printf("Workers ready.  Type 'help' for available commands.\n\n");

    uint32_t task_id   = 1;
    char     line[256];

    int running = 1;
    /*
     * Lines 623-715: REPL prompt, command feedback printf calls, and EOF handling.
     * Drafted with Claude; I reviewed, modified, and tested it.
     */
    while (running) {
        printf("> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* EOF (e.g. redirected input) — treat as quit. */
            printf("\n");
            running = 0;
        } else {
            /* Strip trailing newline. */
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

            if (strncmp(line, "simulate", 8) == 0) {
                char *rest = line + 8;
                int valid = 1;
                /* Skip whitespace. */
                while (*rest == ' ' || *rest == '\t') rest++;

                if (*rest == '\0') {
                    printf("Usage: simulate <total_trials>\n");
                    valid = 0;
                }

                if (valid) {
                    long long total = atoll(rest);
                    if (total <= 0) {
                        printf("simulate: total_trials must be a positive integer\n");
                        valid = 0;
                    } else if ((unsigned long long)total > UINT32_MAX) {
                        printf("simulate: total_trials too large (max %u)\n",
                               UINT32_MAX);
                        valid = 0;
                    }

                    if (valid) {
                        run_simulation((uint32_t)total, task_id++);
                    }
                }

            } else if (strncmp(line, "partial", 7) == 0) {
                char *rest = line + 7;
                while (*rest == ' ' || *rest == '\t') rest++;
                if (*rest == '\0') {
                    printf("Partial result tracing: %s\n",
                           trace_partial_results ? "on" : "off");
                } else if (strcmp(rest, "on") == 0) {
                    trace_partial_results = 1;
                    printf("Partial result tracing enabled.\n");
                } else if (strcmp(rest, "off") == 0) {
                    trace_partial_results = 0;
                    printf("Partial result tracing disabled.\n");
                } else {
                    printf("Usage: partial [on|off]  (no args: show current)\n");
                }

            } else if (strcmp(line, "workers") == 0) {
                int alive = 0;
                for (int i = 0; i < num_workers; i++) {
                    if (workers[i].alive) alive++;
                }
                printf("Active workers: %d / %d\n", alive, num_workers);

            } else if (strncmp(line, "setbatch", 8) == 0) {
                char *rest = line + 8;
                while (*rest == ' ' || *rest == '\t') rest++;
                if (*rest == '\0') {
                    printf("Current batch size: %u\n", simulate_batch_trials);
                } else {
                    long long v = atoll(rest);
                    if (v <= 0 || (unsigned long long)v > UINT32_MAX) {
                        printf("setbatch: batch size must be in [1, %u]\n", UINT32_MAX);
                    } else {
                        simulate_batch_trials = (uint32_t)v;
                        printf("Batch size updated to %u trials per partial result\n",
                               simulate_batch_trials);
                    }
                }

            } else if (strcmp(line, "stats") == 0) {
                query_worker_stats(task_id++);

            } else if (strcmp(line, "help") == 0) {
                print_help();

            } else if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
                running = 0;

            } else if (line[0] != '\0') {
                printf("Unknown command: '%s'  (type 'help' for usage)\n", line);
            }
        }
    }

    printf("Shutting down workers...\n");
    shutdown_workers();
    printf("All workers exited.  Goodbye.\n");
    return 0;
}
