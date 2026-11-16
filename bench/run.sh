#!/usr/bin/env bash
# epoll vs select(): how throughput and latency change as idle connections
# pile up. Uses redis-benchmark (or valkey-benchmark).
#
#   bench/run.sh [build-dir]        (default: build)
#
# Prints two markdown tables. Raw CSV goes to bench/raw/.
#
# Sweep 1: one active client, N idle connections. Each request wakes the
#   loop once, so select()'s per-wakeup cost of rebuilding and scanning the
#   whole fd set shows up directly.
# Sweep 2: 50 active clients. Each wakeup now serves many ready sockets,
#   so the same scan cost is spread across many requests.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=${1:-build}
BENCH=$(command -v redis-benchmark || command -v valkey-benchmark)
REQUESTS=${REQUESTS:-50000}
REPEATS=${REPEATS:-3}                          # median of this many runs
IDLE_SWEEP=${IDLE_SWEEP:-"0 250 500 1000"}   # select() caps out at FD_SETSIZE=1024
PORT=7300
mkdir -p bench/raw

pids=()
cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done; wait 2>/dev/null || true; pids=(); }
trap cleanup EXIT

wait_port() {
  for _ in $(seq 50); do
    (exec 3<>/dev/tcp/127.0.0.1/"$1") 2>/dev/null && return 0
    sleep 0.1
  done
  echo "server on port $1 did not start" >&2; exit 1
}

# once <server> <idle> <clients> <tag> -> prints "rps p50_ms" for one run
once() {
  local server=$1 idle=$2 clients=$3 tag=$4
  PORT=$((PORT + 1))
  "$BUILD/$server" "$PORT" > /dev/null & pids+=($!)
  wait_port "$PORT"
  if [ "$idle" -gt 0 ]; then
    python3 bench/idle_clients.py "$PORT" "$idle" > /dev/null & pids+=($!)
    sleep 1.5
  fi
  local csv="bench/raw/${server}_idle${idle}_c${clients}_${tag}.csv"
  # A run where select() rejects connections (over FD_SETSIZE) can hang;
  # cap it and report 0.
  timeout 60 "$BENCH" -p "$PORT" -t ping_mbulk -n "$REQUESTS" -c "$clients" --csv -q 2>/dev/null > "$csv" || true
  cleanup
  sleep 0.3
  # CSV: test, rps, avg, min, p50, p95, p99, max
  tail -n 1 "$csv" | tr -d '"' | awk -F, '$2 ~ /^[0-9.]+$/ { printf "%.0f %.3f\n", $2, $5; ok=1 } END { if (!ok) print "0 0" }'
}

# run <server> <idle> <clients> -> median "rps p50_ms" over REPEATS runs
run() {
  for r in $(seq "$REPEATS"); do once "$@" "r$r"; done | sort -n | awk '{ a[NR]=$0 } END { print a[int((NR+1)/2)] }'
}

fmt() { if [ "$1" = 0 ]; then echo "rejected"; else echo "$1"; fi; }

for clients in 1 50; do
  echo
  echo "Active clients: $clients"
  echo
  echo "| idle connections | epoll req/s | select req/s | epoll p50 (ms) | select p50 (ms) |"
  echo "|---:|---:|---:|---:|---:|"
  for idle in $IDLE_SWEEP; do
    read -r e_rps e_p50 <<< "$(run redis "$idle" "$clients")"
    read -r s_rps s_p50 <<< "$(run redis-select "$idle" "$clients")"
    if [ "$s_rps" = 0 ]; then s_p50="-"; fi
    echo "| $idle | $(fmt "$e_rps") | $(fmt "$s_rps") | $e_p50 | $s_p50 |"
  done
done
