# redisCpp

A Redis-compatible server in C++23, built from scratch on a single-threaded, non-blocking event loop. It speaks RESP2, so `redis-cli` and `redis-benchmark` work against it unchanged.

It includes a `select()` build of the same server, sharing every line of command logic, to measure what epoll actually buys you.

## Features

| Area | Commands |
|---|---|
| Connection | `PING`, `ECHO` |
| Strings | `SET key value [PX ms]`, `GET`, `TYPE` |
| Lists | `LPUSH`, `RPUSH`, `LPOP key [count]`, `LRANGE` (negative indices), `LLEN` |
| Blocking | `BLPOP key timeout` (`0` waits forever, fractional seconds allowed) |
| Streams | `XADD` (explicit, `ts-*`, and `*` IDs, with ordering checks), `XRANGE` (`-` and `+`) |

## Quick start

```sh
cmake -S . -B build
cmake --build build -j
./build/redis            # listens on 6379; pass a port to change it
redis-cli PING           # PONG
```

## Architecture

```
src/
  main_epoll.cpp     the server: epoll wait loop
  main_select.cpp    same server on select(), for benchmarking
  server.{hpp,cpp}   accept / read / dispatch / BLPOP wake-ups / timeouts
  resp.{hpp,cpp}     RESP2 parser and reply writers
  context.{hpp,cpp}  keyspace and per-client state
  commands/          strings, lists, streams, and the command registry
```

Everything except the wait loop lives in one `redis_core` library. The two binaries differ only in a small `Poller` interface (`add(fd)`, `remove(fd)`) and their `main()`, so the benchmark compares the multiplexers and nothing else.

**One thread, no locks.** All state is owned by the event loop. A request is parsed from the client's input buffer, dispatched, and answered before the loop moves on, so commands are atomic without a mutex.

**Incremental parsing.** Each client has an input buffer. The parser reports "incomplete" until a whole command has arrived, so commands split across TCP packets and pipelined commands both work.

**BLPOP without blocking a thread.** An empty-list `BLPOP` marks the client as blocked and appends it to a per-key FIFO queue. `LPUSH`/`RPUSH` serve waiters in arrival order. Commands a blocked client pipelines are held in its buffer and run as soon as it is woken.

**Timeouts drive the loop's sleep.** Before each wait, the loop computes the nearest BLPOP deadline and sleeps exactly that long (or indefinitely if there is none), then replies `*-1` to every expired waiter. `PX` keys expire lazily on access and are also swept each iteration.

**Bad input never takes the server down.** Malformed numbers reply with an error, and broken RESP framing replies `-ERR Protocol error`. Commands parse all arguments before changing any state, so a rejected command has no side effects.

## Tests

```sh
cd build && ctest --output-on-failure
```

- **Unit tests** (GoogleTest): the RESP parser, including every possible split point of a command.
- **End-to-end tests** (`tests/e2e_test.py`): start a real server and compare exact reply bytes for every command, plus pipelining, PX expiry, FIFO BLPOP waiters, timeouts, disconnects while blocked, and malformed input. They run against both the epoll and select binaries.

## Benchmark: epoll vs select

`bench/run.sh` starts each server, holds N idle connections open, and runs `redis-benchmark` (`PING`, 50,000 requests) against it. Each cell is the median of 3 runs.

**One active client.** Every request wakes the loop once, so the per-wakeup cost is fully exposed:

| idle connections | epoll req/s | select req/s | epoll p50 (ms) | select p50 (ms) |
|---:|---:|---:|---:|---:|
| 0 | 10811 | 11701 | 0.071 | 0.063 |
| 250 | 11221 | 8173 | 0.071 | 0.095 |
| 500 | 11682 | 4548 | 0.063 | 0.175 |
| 1000 | 11628 | 1787 | 0.063 | 0.447 |

**50 active clients.** Each wakeup now finds many ready sockets:

| idle connections | epoll req/s | select req/s | epoll p50 (ms) | select p50 (ms) |
|---:|---:|---:|---:|---:|
| 0 | 20375 | 27747 | 1.071 | 0.863 |
| 250 | 27488 | 28185 | 0.879 | 0.951 |
| 500 | 25694 | 27189 | 0.855 | 1.079 |
| 1000 | 25800 | rejected | 0.895 | - |

What this shows:

- **epoll is flat.** It only returns the sockets that are ready, so idle connections cost nothing per wakeup.
- **select pays for every connection on every wakeup.** It rebuilds the fd set and the kernel scans all of it, so with one active client and 1000 idle ones it serves 6.5x fewer requests and its median latency is 7x higher.
- **Under load the same cost hides.** With 50 busy clients, one scan is shared across many ready sockets, and the two are within run-to-run noise. The multiplexer only matters when connections are many and mostly idle, which is the normal shape of real server traffic.
- **select has a hard ceiling.** 1000 idle plus 50 active connections exceeds `FD_SETSIZE` (1024), so the select build has to refuse connections.

Setup: client and server on the same machine, an AMD Ryzen 3 3250U laptop (2 cores, 4 threads). The absolute numbers are bounded by the benchmark client sharing those cores; the trend between columns is the result.

## Known limitations

- **Replies assume the socket can take them.** Replies go straight to `send()` on a non-blocking socket. A client that stops reading until its kernel buffer fills would lose reply bytes. The fix is a per-client output buffer flushed on `EPOLLOUT`.
- Each reply is several small `send()` calls instead of one buffered write per loop iteration, which costs throughput under pipelining.
- No persistence, replication, or `XREAD`. The keyspace is in memory only.
