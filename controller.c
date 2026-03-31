/*
 * controller.c — Monte Carlo controller (parent) process
 *
 * Usage:
 *   ./controller [num_workers [seed]]
 *
 *   num_workers  Number of concurrent worker processes to spawn (default 4,
 *                minimum 3 per assignment requirements).
 *   seed         Base random seed; worker i receives seed+i (default 42).
 *
 * The controller:
 *  1. Spawns `num_workers` worker processes, each connected via a dedicated
 *     task pipe (controller→worker) and result pipe (worker→controller).
 *  2. Presents an interactive prompt on stdout and reads commands from stdin:
 *       simulate <total_trials>   run a Monte Carlo π estimation
 *       workers                   print the number of active workers
 *       help                      print available commands
 *       quit                      shut down all workers and exit
 *  3. For each "simulate" command, distributes tasks to all workers, reads
 *     their results, computes π and a 95 % confidence interval, and prints
 *     a summary.
 *
 * Concurrency model:
 *   All workers are forked before the first command so they run concurrently
 *   from startup.  Workers block in read() waiting for a task_msg_t.  The
 *   controller writes one task per worker and then reads all results; the
 *   reads can arrive in any order.
 *
 * Error handling:
 *   Every syscall return value is checked.  SIGPIPE is ignored so that a
 *   write() to a dead worker's pipe returns EPIPE rather than killing the
 *   controller.  A SIGCHLD handler using WNOHANG reaps any worker that dies
 *   unexpectedly and marks it dead so the controller skips it.
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

/* -------------------------------------------------------------------------
 * Worker bookkeeping
 * ---------------------------------------------------------------------- */
typedef struct {
    pid_t  pid;
    int    task_write_fd;   /* controller writes task_msg_t here  */
    int    result_read_fd;  /* controller reads result_msg_t here */
    int    alive;           /* 1 = running, 0 = dead/reaped       */
} worker_t;

static worker_t workers[MAX_WORKERS];
static int      num_workers = 0;

/* -------------------------------------------------------------------------
 * SIGCHLD handler — reap unexpected worker deaths without blocking.
 * ---------------------------------------------------------------------- */
static void sigchld_handler(int signo)
{
    (void)signo;
    int saved_errno = errno;
    pid_t pid;
    int   status;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < num_workers; i++) {
            if (workers[i].pid == pid) {
                workers[i].alive = 0;
                break;
            }
        }
    }
    errno = saved_errno;
}

/* -------------------------------------------------------------------------
 * I/O helpers
 * ---------------------------------------------------------------------- */
static ssize_t read_full(int fd, void *buf, size_t len)
{
    size_t total = 0;
    char  *p = (char *)buf;
    while (total < len) {
        ssize_t n = read(fd, p + total, len - total);
        if (n == 0) return (ssize_t)total;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
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
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * spawn_workers – fork+exec `n` worker processes.
 *
 * For worker i:
 *   task_pipe[i]   : pipe whose read end is passed to the worker as argv[1]
 *   result_pipe[i] : pipe whose write end is passed to the worker as argv[2]
 *   seed_i         : base_seed + i, passed as argv[3]
 *
 * After exec, the controller closes the worker-side ends of every pipe.
 * ---------------------------------------------------------------------- */
static int spawn_workers(int n, unsigned int base_seed)
{
    /* Two pipe FDs per worker: [0]=read, [1]=write */
    int task_pipes[MAX_WORKERS][2];
    int result_pipes[MAX_WORKERS][2];

    for (int i = 0; i < n; i++) {
        if (pipe(task_pipes[i]) < 0) {
            perror("controller: pipe (task)");
            return -1;
        }
        if (pipe(result_pipes[i]) < 0) {
            perror("controller: pipe (result)");
            return -1;
        }
    }

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("controller: fork");
            return -1;
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
    }

    num_workers = n;
    return 0;
}

/* -------------------------------------------------------------------------
 * shutdown_workers – send shutdown sentinel to all alive workers, then
 *                    close pipes and wait for each child.
 * ---------------------------------------------------------------------- */
static void shutdown_workers(void)
{
    task_msg_t shutdown_msg;
    shutdown_msg.task_id    = 0;
    shutdown_msg.num_trials = SHUTDOWN_SENTINEL;

    for (int i = 0; i < num_workers; i++) {
        if (!workers[i].alive) continue;

        if (write_full(workers[i].task_write_fd, &shutdown_msg,
                       sizeof(shutdown_msg)) < 0) {
            /* Worker may have already died; SIGCHLD will have marked it. */
            if (errno != EPIPE) {
                perror("controller: write shutdown");
            }
        }
        close(workers[i].task_write_fd);
        close(workers[i].result_read_fd);
        workers[i].task_write_fd  = -1;
        workers[i].result_read_fd = -1;
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
        }
    }
}

/* -------------------------------------------------------------------------
 * run_simulation – distribute `total_trials` across all alive workers,
 *                  collect results, print π estimate and CI.
 *
 * task_id is incremented by the caller to keep IDs unique across commands.
 * ---------------------------------------------------------------------- */
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
     * every worker that is sent a task gets at least 1 trial.  Workers whose
     * computed chunk would be 0 are skipped entirely — sending 0 would
     * trigger the shutdown sentinel and kill those workers permanently.
     * Skipped workers remain blocked on read(), ready for the next command. */
    int worker_indices[MAX_WORKERS];
    int sent      = 0; /* index among alive workers, used to assign remainder */
    int alive_idx = 0;

    for (int i = 0; i < num_workers; i++) {
        if (!workers[i].alive) continue;

        /* Workers 0 .. remainder-1 each get one extra trial. */
        uint32_t chunk = base_chunk + (alive_idx < (int)remainder ? 1u : 0u);
        alive_idx++;

        if (chunk == 0) {
            /* This worker would receive the shutdown sentinel — skip it. */
            continue;
        }

        task_msg_t task;
        task.task_id    = task_id;
        task.num_trials = chunk;

        if (write_full(workers[i].task_write_fd, &task, sizeof(task)) < 0) {
            if (errno == EPIPE) {
                fprintf(stderr,
                        "controller: worker %d pipe broken, skipping\n", i);
                workers[i].alive = 0;
                continue;
            }
            perror("controller: write task");
            workers[i].alive = 0;
            continue;
        }
        worker_indices[sent] = i;
        sent++;
    }

    if (sent == 0) {
        fprintf(stderr, "controller: no workers accepted the task\n");
        return;
    }

    /* Collect results from workers that were sent a task. */
    uint64_t total_hits   = 0;
    uint64_t total_actual = 0;
    int      received     = 0;

    for (int s = 0; s < sent; s++) {
        int i = worker_indices[s];
        result_msg_t result;
        ssize_t n = read_full(workers[i].result_read_fd,
                               &result, sizeof(result));
        if (n <= 0 || (size_t)n < sizeof(result)) {
            fprintf(stderr,
                    "controller: failed to read result from worker %d: %s\n",
                    i, n < 0 ? strerror(errno) : "pipe closed");
            workers[i].alive = 0;
            continue;
        }
        total_hits   += result.num_hits;
        total_actual += result.num_trials;
        received++;
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

    printf("\n--- Simulation results (task %u) ---\n", task_id);
    printf("  Workers responded : %d / %d\n", received, sent);
    printf("  Trials run        : %lu\n", (unsigned long)total_actual);
    printf("  Hits (in circle)  : %lu\n", (unsigned long)total_hits);
    printf("  π estimate        : %.8f\n", pi_est);
    printf("  95%% CI            : [%.8f, %.8f]\n",
           pi_est - margin, pi_est + margin);
    printf("  Error vs. M_PI    : %+.8f\n", pi_est - M_PI);
    printf("------------------------------------\n\n");
}

/* -------------------------------------------------------------------------
 * print_help
 * ---------------------------------------------------------------------- */
static void print_help(void)
{
    printf("Commands:\n");
    printf("  simulate <N>   run N Monte Carlo trials and estimate π\n");
    printf("  workers        show number of active worker processes\n");
    printf("  help           show this help message\n");
    printf("  quit           shut down workers and exit\n");
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
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

    while (1) {
        printf("> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* EOF (e.g. redirected input) — treat as quit. */
            printf("\n");
            break;
        }

        /* Strip trailing newline. */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        if (strncmp(line, "simulate", 8) == 0) {
            char *rest = line + 8;
            /* Skip whitespace. */
            while (*rest == ' ' || *rest == '\t') rest++;

            if (*rest == '\0') {
                printf("Usage: simulate <total_trials>\n");
                continue;
            }

            long long total = atoll(rest);
            if (total <= 0) {
                printf("simulate: total_trials must be a positive integer\n");
                continue;
            }
            if ((unsigned long long)total > UINT32_MAX) {
                printf("simulate: total_trials too large (max %u)\n",
                       UINT32_MAX);
                continue;
            }

            run_simulation((uint32_t)total, task_id++);

        } else if (strcmp(line, "workers") == 0) {
            int alive = 0;
            for (int i = 0; i < num_workers; i++) {
                if (workers[i].alive) alive++;
            }
            printf("Active workers: %d / %d\n", alive, num_workers);

        } else if (strcmp(line, "help") == 0) {
            print_help();

        } else if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            break;

        } else if (line[0] == '\0') {
            /* Empty line — ignore. */
            continue;

        } else {
            printf("Unknown command: '%s'  (type 'help' for usage)\n", line);
        }
    }

    printf("Shutting down workers...\n");
    shutdown_workers();
    printf("All workers exited.  Goodbye.\n");
    return 0;
}
