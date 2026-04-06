#ifndef MONTECARLO_H
#define MONTECARLO_H

#include <stdint.h>

/* Shared wire-protocol version for controller/worker messages. */
#define PROTOCOL_VERSION 1U

/* Controller -> worker request types. */
#define TASK_SIMULATE   1U
#define TASK_SHUTDOWN   2U
#define TASK_STATUS_REQ 3U

/* Worker -> controller response types. */
#define RESULT_SIMULATE 1U
#define RESULT_STATUS   2U
#define RESULT_ERROR    3U

/* Default max trials carried in one partial simulation response. */
#define SIM_BATCH_TRIALS 50000U

/*
 * Controller request message.
 *
 * Notes:
 * - task_id is assigned by controller and should be monotonic.
 * - num_trials and batch_trials are used only for TASK_SIMULATE.
 * - version must match PROTOCOL_VERSION on both endpoints.
 */
typedef struct {
    uint32_t version;
    uint32_t task_id;
    uint32_t msg_type;
    uint32_t num_trials;
    uint32_t batch_trials;
} task_msg_t;

/*
 * Worker response message.
 *
 * Notes:
 * - task_id must match the request being answered.
 * - For RESULT_SIMULATE, num_hits <= num_trials and batch_* describe the
 *   partial-result position.
 * - For RESULT_STATUS, tasks_completed reports cumulative simulate tasks.
 * - For RESULT_ERROR, error_code carries worker-side failure details.
 */
typedef struct {
    uint32_t version;
    uint32_t task_id;
    uint32_t msg_type;
    uint32_t num_trials;
    uint32_t num_hits;
    uint32_t batch_index;
    uint32_t batch_total;
    uint32_t tasks_completed;
    uint32_t error_code;
} result_msg_t;

/* Default number of worker processes spawned at startup. */
#define DEFAULT_NUM_WORKERS 4

#endif /* MONTECARLO_H */
