# CSC209 — Monte Carlo worker pool

Parent `controller` process coordinates worker children over pipes to run Monte Carlo simulations in parallel.

## Build

```bash
make
```

Produces `controller` and `worker` in the current directory.

## Run

```bash
./controller [num_workers [seed]]
```

Example:

```bash
./controller 4 42
```

The program is interactive: use commands such as `simulate`, `setbatch`, `stats`, and `quit` (see `controller.c` usage comment).

## Clean

```bash
make clean
```
