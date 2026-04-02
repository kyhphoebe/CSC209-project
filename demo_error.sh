#!/usr/bin/env bash
set -euo pipefail

tmpdir="$(mktemp -d)"
cmd_fifo="${tmpdir}/controller.in"
out_file="${tmpdir}/controller.out"

cleanup() {
  exec 3>&- || true
  rm -rf "${tmpdir}"
}
trap cleanup EXIT

echo "[1/5] Building project"
make clean >/dev/null
make >/dev/null

echo "[2/5] Starting controller in background"
mkfifo "${cmd_fifo}"
./controller 4 42 <"${cmd_fifo}" >"${out_file}" 2>&1 &
controller_pid=$!
exec 3>"${cmd_fifo}"
sleep 1

echo "[3/5] Sending initial commands"
printf "workers\nsimulate 200000\n" >&3
sleep 1

echo "[4/5] Killing one worker process"
worker_pid="$(pgrep -f "./worker" | head -n 1 || true)"
if [[ -z "${worker_pid}" ]]; then
  echo "No worker PID found; ensure controller is running."
else
  kill "${worker_pid}"
  echo "Killed worker PID ${worker_pid}"
fi
sleep 1

echo "[5/5] Demonstrating degraded-but-running behavior"
printf "workers\nsimulate 200000\nworkers\nquit\n" >&3
exec 3>&-
wait "${controller_pid}"
cat "${out_file}"
