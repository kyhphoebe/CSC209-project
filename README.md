# CSC209 — Parallel Monte Carlo π Simulator

## Language

Written in **C** for POSIX systems, built with **gcc** using `-Wall -Wextra -g` (see `Makefile`). Uses standard process and IPC APIs (`fork`, `pipe`, `exec`, `waitpid`, etc.).

---

## Project overview

This project is a multiprocess Monte Carlo simulator. A parent **controller** forks several **worker** children and talks to each over two **pipes** using a small binary protocol (`task_msg_t` / `result_msg_t` in `montecarlo.h`). Workers draw random points in the unit square and count how many fall inside the quarter unit circle (points where **x² + y² ≤ 1**); the controller **partitions** each `simulate` job across workers, **aggregates** partial results, and prints an estimate of **π**, a confidence interval, and diagnostics. It demonstrates process management, non-blocking child reaping (`SIGCHLD`), and structured IPC.

---

## Demo

Build, run with four workers and a fixed base seed, then try a simulation and exit:

```bash
make clean && make
./controller 4 42
```

Example session (your π estimate and counts will differ slightly each run):

```text
Monte Carlo π Simulator
Spawning 4 worker processes (base seed 42)...
Workers ready.  Type 'help' for available commands.

> help
Commands:
  simulate <N>   run N Monte Carlo trials and estimate π
  setbatch <N>   set max trials per partial result message
  partial on|off print each partial result as it arrives
  stats          query per-worker processed-task counters
  workers        show number of active worker processes
  help           show this help message
  quit           shut down workers and exit

> simulate 200000

--- Simulation results (task 1) ---
  Workers responded : 4 / 4
  Partial messages  : ...
  Trials run        : 200000
  Hits (in circle)  : ...
  π estimate        : 3.14.......
  95% CI            : [..., ...]
  Error vs. M_PI    : ...
------------------------------------

> stats
> workers
> quit
Shutting down workers...
All workers exited.  Goodbye.
```

Optional: turn on streaming partial messages with `partial on` before `simulate` to see each batch as it arrives.

---

## Architecture (brief)

| File | Role |
|------|------|
| `controller.c` | Spawns `./worker`, REPL, task split/merge, `SIGCHLD` handling |
| `worker.c` | Reads tasks, runs trials with `rand_r`, writes results |
| `montecarlo.h` | Protocol version, message structs, defaults |

The controller runs `execl("./worker", ...)`, so **`worker` must be built in the same directory** as `controller`.

---

## Build, run, clean

```bash
make                    # builds controller + worker
./controller [num_workers [seed]]
make clean
```

If you pass `num_workers` on the command line, it must be **at least 3** and at most `MAX_WORKERS` (see `controller.c`). With no arguments, defaults from `montecarlo.h` apply.
