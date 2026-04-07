# CSC209 Project — Parallel Monte Carlo π Estimation

A small C program that estimates **π** with a Monte Carlo method (random points in the unit square, fraction inside the quarter unit circle) using **multiple worker processes** coordinated by a parent **controller**.

## What this project does

- **Workers** run batches of trials with `rand_r`, count hits where \(x^2 + y^2 \le 1\), and stream **partial results** back so large jobs do not need one huge message.
- The **controller** splits a `simulate N` request across alive workers, reads `result_msg_t` messages, aggregates hits vs trials, and prints an estimate of π and run statistics.
- Communication is **binary and structured**: `task_msg_t` / `result_msg_t` in `montecarlo.h` (versioned protocol, task types for simulate / shutdown / status).

## Architecture

| Component    | Role |
|-------------|------|
| `controller.c` | `fork` + `execl("./worker", ...)`, two pipes per worker (tasks in, results out), `SIGCHLD` handler to reap dead children, interactive REPL |
| `worker.c`     | Loop: read tasks, run simulation or status, write results; exits on shutdown |
| `montecarlo.h` | Shared message layouts and constants (`PROTOCOL_VERSION`, batch size defaults, etc.) |

Requires **`worker` built next to `controller`** (same directory), because the controller execs `./worker`.

## Build and run

```bash
make              # builds controller and worker
./controller [num_workers [seed]]
```

Default worker count is defined in `montecarlo.h` (`DEFAULT_NUM_WORKERS`). The controller enforces a **minimum of 3 workers** at startup (see `controller.c`).

## Interactive commands

After starting the controller:

- `simulate <N>` — run `N` trials in parallel and print π estimate and stats  
- `setbatch <N>` — cap trials per partial result message  
- `partial on|off` — trace each partial result as it arrives  
- `stats` — query per-worker task counters  
- `workers` — show how many worker processes are active  
- `quit` — shut down workers and exit  

Type `help` in the program for the same list.

## Clean

```bash
make clean
```

## Requirements

- C compiler (`gcc` or compatible), POSIX-like environment (`unistd`, `fork`, `pipe`, `waitpid`).
