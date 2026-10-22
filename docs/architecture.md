# Architecture and thread-safety design

## Scope

This is a public-market-data collection and signal-labeling demo. It deliberately has
no authenticated trading, order management, position or PnL logic. The only exchange
writes are protocol-level WebSocket close frames.

## Components

| Component | Source | Responsibility |
|---|---|---|
| JSON DOM | `cpp/src/json.cpp` | Allocation-light parse of WebSocket text; numbers keep exact wire text |
| Core | `cpp/src/core.cpp` | Binance trade parser, dedup primitives, 30 s factors, deterministic strategy, replay |
| Runtime | `cpp/src/runtime.cpp` | Bounded queue, durable JSONL writer, SHA-256, JSON logger, CPU/RSS sampler, latency quantiles |
| Collector | `cpp/src/capture.cpp` | TLS WebSocket client, reconnects, queue wiring, persistence, summary |
| Report | `cpp/src/report.cpp` | Recompute hashes/counts, verify integrity, render acceptance report |
| Benchmark | `cpp/src/benchmark.cpp` | Synthetic parse / pipeline / persistence throughput |
| CLI | `cpp/src/main.cpp` | Argument parsing and exit codes |

## Data path

```
socket read (network thread)
  → RawFrame {seq, generation, connection_id, received_utc, received_steady_ns, payload}
  → raw_queue (bounded, non-blocking try_push)
  → writer thread:
        raw_frames.jsonl (batch flush; fsync every N records and at finalize)
        → process_queue (bounded, non-blocking try_push)
  → processor thread:
        parse → validate → dedup → out-of-order check
        → FactorEngine (30 s receive-time window, 10 s trade rate, 20-trade return)
        → evaluate() → BUY_SIGNAL / SELL_SIGNAL / HOLD
        → normalized_events.jsonl, factors.jsonl, signals.jsonl
```

The writer drains the raw queue before closing the processing queue, so a graceful stop
never loses a frame that was already accepted by the network loop. The processor drains
the processing queue before finalizing the derived files.

## Threads

| Thread | Owns | Notes |
|---|---|---|
| Main / network | Boost.Asio `io_context`, TLS stream, raw queue producer | C++20 coroutines; one active session at a time |
| Writer | `raw_frames.jsonl`, process-queue producer | Batch flush every 250 ms; fsync every N records |
| Processor | derived JSONL files, factors, strategy, dedup | Single-threaded by construction |
| Resource sampler | CPU/RSS vectors | One sample per second |
| Stop watcher | Posting socket cancellation | Wakes every 50 ms; posts cancellation onto the I/O thread |

Shared mutable state is limited to:

- `std::atomic<bool> fatal` (writer/processor → network)
- per-component `CaptureSummary` fields written by exactly one thread and read after join
- `std::atomic` flags in the stop path
- the logger's mutex (all threads)
- the bounded queues' mutex/condition-variable

The network thread does not touch derived writers; the processor does not touch the raw
writer; the writer does not compute factors. This keeps I/O and CPU work off the socket
read path except for the enqueue and timestamp.

## Cancellation and signal handling

`SIGINT`/`SIGTERM` handlers only set a `volatile sig_atomic_t` flag (async-signal-safe).
The stop watcher notices the flag within 50 ms and posts a socket-cancel closure to the
`io_context`. The socket is therefore only cancelled on its owning thread. The session
returns `Stopped`, the reconnect loop exits, queues close and drain, files finalize, and
`summary.json` is written before the process exits with code 130.

## Backpressure and loss detection

- Both queues are bounded and use `try_push`.
- On `raw_queue` overflow: increment `queue_overflow_events` / `dropped_events`, set
  `queue_overflow=true`, log `raw_queue_overflow`, mark the run invalid and stop.
- On `process_queue` overflow: same policy, counted separately.
- The collector never blocks the socket read indefinitely and never silently discards.
  The sample result becomes `FAIL`; `quality_checks.zero_queue_overflow` is false.
- The in-memory dedup table is also bounded (default 200,000 entries, LRU-style eviction).

## Reconnect and generation semantics

- Every connection attempt receives a monotonically increasing `connection_generation`.
- Each raw frame records its generation and a per-connection id.
- Deduplication is global across reconnects: a trade replayed after reconnect is counted
  as a duplicate.
- On reconnect the collector records a `GapWindow` with the approximate gap duration and
  last-frame/reconnect UTC timestamps. It explicitly logs that a reconnect cannot prove
  the exchange feed had no missing trades.
- Exponential backoff starts at `--reconnect-initial` (default 1 s), doubles per
  consecutive failure and is capped at `--reconnect-max` (default 15 s).
- Ten consecutive connection failures abort the run with a diagnostic (configurable).
- HTTP 451 is treated as a geographic access restriction: `BLOCKED`, no retry storm,
  no circumvention.

## TLS

- `wss://` uses Boost.Beast over `boost::asio::ssl::stream`.
- `verify_peer` plus `host_name_verification(host)` is mandatory; SNI is set explicitly.
- Trust sources, in order: `--ca-file`, `MARKETPIPE_CA_FILE`, `SSL_CERT_FILE`,
  `<cwd>/certs/cacert.pem`, paths next to the executable, Linux system bundles, plus
  OpenSSL default paths; the Windows Root store is also imported when accessible.
- Plain `ws://` is accepted only for loopback hosts, solely for the local integration
  test server. Non-loopback plaintext is rejected before any connection attempt.
- TLS handshake errors are logged with the OpenSSL error and terminate the run as a
  fatal configuration/trust failure rather than retrying indefinitely.

## Persistence and integrity

- The raw writer stores the exact frame text (JSON-escaped only), payload byte length and
  per-payload SHA-256.
- It flushes at least every 250 ms and fsyncs every N records (default 100) and at
  finalize. Errors are sticky and surfaced in `summary.json`.
- After finalization the collector re-reads `raw_frames.jsonl`, recomputes the file
  SHA-256 and verifies every record's sequence, generation ordering, payload hash and
  byte count. A mismatch marks the run FAIL.
- `report --run-dir` repeats these checks independently and writes `verification.json`.
- `summary.json` is written with a temp-file + fsync + rename.

## PASS criteria

A run is `PASS` only if all are true:

1. a real non-loopback WebSocket connection succeeded (TLS verified for `wss://`);
2. observed duration ≥ 99% of the requested duration;
3. at least one frame and one unique trade were processed;
4. raw record count equals frames received;
5. raw file SHA-256 matches both the incremental writer digest and the summary;
6. zero queue overflow, zero dropped events, zero persistence/processing failures;
7. zero disconnects, malformed frames, duplicates and out-of-order trades.

Reconnects, malformed frames, duplicates or out-of-order events yield `PARTIAL`.
HTTP 451 yields `BLOCKED`. Everything else yields `FAIL`.

## Known limits

This is a single-host engineering demo, not a colocated HFT engine. Latency includes the
OS network stack, TLS, scheduling and local disk. It cannot establish long-term
production reliability, exchange-side completeness or profitability. The 15-minute
sample describes one host, one feed and one observation window only.
