#ifndef MONTECARLO_H
#define MONTECARLO_H

#include <stdint.h>

/*
 * task_msg_t  —  Parent → Worker
 *
 * Direction : parent (controller) writes to worker's task pipe.
 * Encoding  : fixed-width binary struct, written with a single write(2) call.
 * Semantics : instructs the worker to run `num_trials` Monte Carlo trials.
 *             A num_trials value of 0 is the shutdown sentinel: the worker
 *             must close its pipes and exit cleanly upon receiving it.
 * Fields    :
 *   task_id    – monotonically increasing identifier assigned by the parent
 *                so results can be matched back to the originating command.
 *   num_trials – number of random trials the worker should perform.
 *                0 means "shut down".
 */
typedef struct {
    uint32_t task_id;
    uint32_t num_trials;
} task_msg_t;

/*
 * result_msg_t  —  Worker → Parent
 *
 * Direction : worker writes to its result pipe; parent reads.
 * Encoding  : fixed-width binary struct, written with a single write(2) call.
 * Semantics : reports the outcome of a completed task.  The parent matches
 *             the result to the originating command via task_id, accumulates
 *             num_hits and num_trials across all workers, then computes the
 *             combined π estimate and 95 % confidence interval.
 * Fields    :
 *   task_id    – mirrors the task_id sent in the corresponding task_msg_t.
 *   num_trials – number of trials the worker actually performed (≥ 1).
 *   num_hits   – number of trials where x²+y² ≤ 1 (point inside unit circle).
 */
typedef struct {
    uint32_t task_id;
    uint32_t num_trials;
    uint32_t num_hits;
} result_msg_t;

/* Default number of worker processes spawned at startup. */
#define DEFAULT_NUM_WORKERS 4

/* Shutdown sentinel value for task_msg_t.num_trials. */
#define SHUTDOWN_SENTINEL 0U

#endif /* MONTECARLO_H */
