# CSC209 A3 Demo Guide

This document matches the assignment handout: **Part 1 ~1.5 min feature demo**, **Part 2 ~1.5 min code walkthrough**, total ~3 minutes. Focus on pipes, concurrency, protocol complexity, and robustness.

---

## Before you record

1. **Build** (on the machine you will demo, e.g. teach.cs or your laptop):

   ```bash
   cd /path/to/CSC209-project
   make clean && make
   ```

2. **Opening shot** (required by handout): show **MarkUs group name** + **all team member names** (text file or slide is fine).

3. **Two terminals** (recommended for Part 1):
   - **Terminal A**: run `./controller`
   - **Terminal B**: `pgrep`, `kill` worker PIDs

4. **Optional**: if live `kill` is awkward, run `./demo_error.sh` once as a backup clip (same story: worker dies, controller keeps working).

---

## Part 1 — Feature demo (~1.5 minutes)

**Goal:** Show that data flows over pipes, workers run concurrently, the protocol has multiple message types and batched results, and the system survives one dead worker.

### Scene 0 — Start (10–15 s)

**Terminal A:**

```bash
cd /path/to/CSC209-project
./controller 4 42
```

**Say (short):**

> This is our Category 1 project: one controller and a pool of worker processes. Communication is over pipes with structured messages, not just signaling.

---

### Scene 1 — Commands and help (15–20 s)

**Terminal A** (after the `>` prompt):

```text
help
```

**Say:**

> The controller is interactive. Besides simulate, we added `stats` to query workers and `setbatch` to control how many trials go in each partial result message — that increases message frequency and shows a richer protocol.

---

### Scene 2 — Status query (control-plane traffic) (20–25 s)

```text
stats
```

**Say:**

> Here the controller sends a status request to each worker and reads structured replies. This is separate from the Monte Carlo workload — so we have more than one kind of exchange over the pipes.

---

### Scene 3 — Tune batch size, then simulate (data-plane + batches) (35–45 s)

```text
setbatch 20000
simulate 200000
```

**Point at the output** (do not read every line):

- `Partial messages` — more than one result message per worker when batches are smaller than the chunk.
- `Workers responded` — all workers that got work replied.
- `Trials run` / `Hits` / `π estimate` / `95% CI` — correct aggregation.

**Say:**

> For simulate, each worker streams multiple partial results. The controller validates batch metadata and sums hits and trials. `setbatch` lets us change how chatty the protocol is — smaller batch means more messages for the same total trials.

---

### Scene 4 — Kill one worker, prove degradation (25–35 s)

**Terminal B** (while controller is still running):

```bash
pgrep -fl "./worker"
```

Pick one PID from a line that looks like `./worker ...` (not `grep` itself). Then:

```bash
kill <PID>
```

**Back to Terminal A:**

```text
workers
simulate 200000
workers
```

**Say:**

> I killed one worker. The controller still runs; active count drops. The next simulate still completes using the remaining workers. That shows error handling and that we are not a brittle single-path design.

---

### Scene 5 — Clean exit (10–15 s)

```text
quit
```

**Say:**

> Shutdown sends an explicit shutdown message type and we wait for children — no zombies left behind in normal use.

---

### Part 1 fallback — Scripted demo

If live typing is risky:

```bash
./demo_error.sh
```

Narrate the same story: first run with 4 workers, then one worker killed, then second simulate with 3 responding.

---

## Part 2 — Code walkthrough (~1.5 minutes)

**Goal:** Land exactly where Part 1 happened: protocol structs, simulate path with batches, status path, spawn/shutdown, worker loop.

**Do not read line-by-line.** Use **file + function name** and **one sentence per idea**.

---

### Stop 1 — Wire protocol (`montecarlo.h`) (~25 s)

Open `montecarlo.h`.

**Show:**

- `PROTOCOL_VERSION`
- `TASK_SIMULATE`, `TASK_STATUS_REQ`, `TASK_SHUTDOWN`
- `RESULT_SIMULATE`, `RESULT_STATUS`, `RESULT_ERROR`
- `task_msg_t`: `version`, `task_id`, `msg_type`, `num_trials`, `batch_trials`
- `result_msg_t`: `version`, `task_id`, `msg_type`, `num_trials`, `num_hits`, `batch_index`, `batch_total`, `tasks_completed`, `error_code`

**Say:**

> All messages are fixed-size structs — that is our encoding. We have multiple request and response types, version checks, and batch fields so the controller can verify partial streams. This is the communication complexity the rubric cares about: encoding, types, who sends what, and what happens on mismatch.

---

### Stop 2 — Worker: simulate + batches (`worker.c`, `main`) (~25 s)

Open `worker.c`, scroll to the main loop that reads `task_msg_t`.

**Point to:**

- Branch on `task.msg_type` for `TASK_SIMULATE`, `TASK_STATUS_REQ`, and errors.
- The loop over batches that sends multiple `RESULT_SIMULATE` messages with `batch_index` / `batch_total`.

**Say:**

> The worker is message-driven. For simulate it does not send one giant reply; it streams partial results so the parent sees incremental traffic. Status and errors are separate response types.

---

### Stop 3 — Controller: dispatch + receive + validate (`controller.c`, `run_simulation`) (~35 s)

Open `controller.c`, function `run_simulation`.

**Point to:**

- Partitioning `total_trials` across alive workers.
- Building `task_msg_t` with `PROTOCOL_VERSION`, `TASK_SIMULATE`, and `batch_trials`.
- The loop that reads until each worker’s assigned trials are fully received.
- Checks on `task_id`, `msg_type`, `version`, and batch metadata.

**Say:**

> The controller splits work, sends typed tasks, then reads until each worker’s chunk is accounted for. It rejects bad protocol data instead of corrupting the aggregate. That is the pairing between worker batching and parent-side verification.

---

### Stop 4 — Controller: status (`controller.c`, `query_worker_stats`) (~15 s)

Open `query_worker_stats`.

**Say:**

> Stats is a second request path — same pipes, different message types — so we are not a one-trick simulate-only protocol.

---

### Stop 5 — Lifecycle (`controller.c`, `spawn_workers`, `shutdown_workers`, `sigchld_handler`) (~20 s)

**Point briefly:**

- `spawn_workers`: `pipe`, `fork`, `execl`, rollback on failure.
- `sigchld_handler`: reap unexpected exits with `waitpid(..., WNOHANG)`.
- `shutdown_workers`: `TASK_SHUTDOWN`, close fds, `waitpid`.

**Say:**

> Concurrency is a real worker pool: processes exist together, not fork-wait-fork. We reap zombies on unexpected death and shut down cleanly with explicit shutdown messages and waits.

---

## Checklist vs handout

| Requirement | How demo shows it |
|-------------|-------------------|
| Meaningful pipe data | Struct messages, batches, stats |
| ≥3 concurrent workers | `./controller 4 42` + simulate |
| Error handling | Kill worker; next commands still work |
| Protocol documentation story | Part 2 walks `montecarlo.h` + `run_simulation` |
| Video length | Aim ~3 min; Part 1 / Part 2 split clear |

---

## Quick command cheat sheet (copy-paste)

**Terminal A:**

```text
help
stats
setbatch 20000
simulate 200000
workers
simulate 200000
quit
```

**Terminal B (after first simulate, optional):**

```bash
pgrep -fl "./worker"
kill <one_worker_pid>
```

---

## Notes

- If `pgrep` shows no workers, controller is not running or already quit — restart `./controller`.
- Prefer **reasonable** trial counts (e.g. 200k) so output is readable in one screen without scrolling forever.
- Keep Part 2 independent of the report: **show code behavior**, do not summarize the whole project background.
