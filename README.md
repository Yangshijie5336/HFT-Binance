# MarketPipe HFT Demo — C++20 Binance public-trade collector

A single-process, C++20 market-data engineering demo that captures the **Binance Spot
`btcusdt@trade` WebSocket** feed, persists every raw frame, parses and validates trades,
computes rolling trade-flow factors, emits deterministic `BUY_SIGNAL` / `SELL_SIGNAL` /
`HOLD` labels, and produces a verifiable performance report.

> **Safety:** public market data only. There are no API keys, private endpoints, order
> placement or execution paths. `BUY_SIGNAL` / `SELL_SIGNAL` are labels only.

The Python reference implementation has been removed. The repository is now a C++20 /
CMake / Ninja project.

## Latest acceptance result

The final 900-second live run (`live900-BTCUSDT-20261022`) is **PASS**:

- 24,903 / 24,903 frames and trades persisted and processed, 0 reconnects/disconnects,
  0 queue overflow, 0 dropped events, 0 persistence or processing errors.
- Raw file SHA-256
  `5836675014897976d47654e8d0fde5f765f7ff129d0ecba9c413d5c5a82919b8`, independently
  re-verified from `verification.json`.
- All eleven quality gates true; see [`acceptance_report.md`](docs/acceptance_report.md)
  and the small evidence copies in [`docs/evidence/`](docs/evidence/).

Large raw/derived files stay under the git-ignored `data/runs/` directory or CI artifacts.

## Architecture

```
Binance Spot WebSocket (TLS, Beast/Asio/OpenSSL)
        │  text frame + receive timestamps + connection generation
        ▼
bounded raw-frame queue  ──►  writer thread  ──►  raw_frames.jsonl
        │                         │  batch flush / periodic fsync / final fsync
        │                         ▼
        └──────────────► bounded processing queue
                                  │
                                  ▼
                    parse → validate → dedup / out-of-order check
                                  │
                                  ▼
                    rolling factors → deterministic strategy
                                  │
                                  ▼
             normalized_events.jsonl · factors.jsonl · signals.jsonl
                                  │
                                  ▼
                    summary.json · verification.json · performance_report.md
```

Both queues are capped. On overflow the collector records the event, marks the sample
invalid, stops the network session and drains what is already queued; it never silently
drops data. Reconnects increment a connection generation and record a potential-gap
window; the collector does **not** claim that a reconnect is lossless.

See [architecture](docs/architecture.md) for the thread and failure model.

## Requirements

- CMake ≥ 3.20, Ninja, a C++20 compiler
- Boost ≥ 1.74 (Beast / Asio / System headers)
- OpenSSL 3.x
- Windows: MSVC 2022+ and vcpkg; Linux: `g++`, `libboost-all-dev`, `libssl-dev`
- A writable disk for the run directory
- For live capture: outbound TLS access to `stream.binance.com:9443`

A Mozilla CA bundle is vendored at `certs/cacert.pem` and is used when system trust is
not available. TLS peer and hostname verification are always enabled; there is no
`--insecure` switch.

## Build

### Linux / CI

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

### Windows (MSVC + vcpkg)

If vcpkg is installed at `D:/Dev/vcpkg`:

```powershell
$env:VSLANG = "1033"   # make MSVC /showIncludes English so Ninja tracks header deps
cmd /c "call ""D:\Dev\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=D:/Dev/vcpkg/installed/x64-windows"
cmd /c "call ""D:\Dev\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" && cmake --build build -j 8"
ctest --test-dir build --output-on-failure
```

The build copies the OpenSSL runtime DLLs next to the executables on Windows. The
`vcpkg` path above must point at the installation that actually contains Boost and
OpenSSL; a `VCPKG_ROOT` injected by Visual Studio may point at its bundled vcpkg.
If header dependencies look stale after editing `marketpipe.hpp`, run
`cmake --build build --clean-first` (localized MSVC output otherwise defeats Ninja's
dependency parsing).

## Commands

```text
marketpipe capture   --symbol BTCUSDT --duration 900 --output-dir data/runs
marketpipe replay    --input <raw_frames.jsonl> [--output-dir <dir>]
marketpipe benchmark --events 100000 [--mode all|parse|pipeline|persist]
marketpipe report    --run-dir data/runs/<run>
marketpipe synthetic --events 100000        # legacy factor-only benchmark
```

`capture` defaults to 900 seconds, symbol `BTCUSDT`, and
`wss://stream.binance.com:9443/ws/btcusdt@trade`. Useful options:

| Option | Meaning |
|---|---|
| `--ws-url URL` | Override endpoint (`wss://` required except loopback tests) |
| `--ca-file PATH` | Explicit PEM CA bundle; overrides discovery |
| `--queue-capacity N` | Capacity of each bounded queue (default 65536) |
| `--ring-capacity N` | In-memory processed-trade ring (default 20000) |
| `--fsync-every N` | Raw/derived fsync cadence (default 100 records + final) |
| `--connect-timeout-ms N` | DNS/TCP/TLS/WebSocket handshake timeout |
| `--idle-timeout-ms N` | Beast idle timeout; keep-alive pings are enabled |
| `--reconnect-initial` / `--reconnect-max` | Exponential backoff bounds |
| `--run-id ID` | Explicit run directory name |
| `--quiet` | Do not mirror structured logs to stderr |

Exit codes: `0` PASS/PARTIAL, `2` FAIL/BLOCKED, `130` interrupted by SIGINT/SIGTERM.
A failed run still writes `system.log`, `summary.json` and a verification report.

## Run directories

Each run creates `<output-dir>/<run-id>/` containing:

| File | Content |
|---|---|
| `raw_frames.jsonl` | Exact WebSocket text, run_id, generation, receive UTC/monotonic time, sequence, payload bytes and SHA-256 |
| `normalized_events.jsonl` | Validated, deduplicated trades with exact decimal text |
| `factors.jsonl` | 30 s trade-flow snapshot per processed trade |
| `signals.jsonl` | `BUY_SIGNAL` / `SELL_SIGNAL` / `HOLD`; execution disabled |
| `system.log` | Structured connection, queue, persistence and shutdown diagnostics |
| `summary.json` | Counters, latency percentiles, CPU/RSS, gap windows, result and reasons |
| `verification.json` | Recomputed raw SHA-256, record count and payload-hash checks |
| `performance_report.md` | Human-readable report generated from `summary.json` |

Raw data is never converted to `double` before persistence. Decimal price and quantity
text remain exact in both the raw record and the normalized event; `double` is used only
for factor computation.

## Tests

CTest runs:

- JSON/parser edge cases and malformed Binance frames
- duplicate / out-of-order trade handling and bounded dedup
- bounded queue overflow and drain-on-stop
- disk-full, final-fsync and open failures
- raw frame persistence and SHA-256 verification
- complete factor + strategy chain
- loopback WebSocket integration: normal stream, malformed frames, abrupt disconnect
  and reconnect generation isolation, queue overflow, HTTP 451, SIGINT drain
- a reproducible 100,000-event synthetic benchmark

No test requires Binance connectivity. The live test is a manual acceptance step.

## Docker

The image is a multi-stage build, runs as UID 10001, and writes only under `/app/data`.
It runs CTest during the build stage.

```bash
docker compose build
docker compose run --rm marketpipe
```

The compose service writes to the named volume `marketpipe-data` (mounted at
`/app/data`), so raw captures survive container removal and remain writable by the
non-root UID 10001. Graceful `SIGTERM` handling drains queues and fsyncs artifacts
(compose `stop_grace_period: 60s`). To use a host directory instead, bind-mount it and
ensure it is writable by UID 10001, or override the compose `user:`.

## Evidence and CI

CI builds, runs CTest (including the local WebSocket integration test), runs the
100,000-event benchmark, builds the Docker image and uploads test/benchmark evidence.
Live captures are a manual workflow and upload the run directory as an artifact. Large
raw captures, logs and credentials are never committed to Git.

- [Architecture and thread safety](docs/architecture.md)
- [CLI, file and JSON interfaces](docs/interfaces.md)
- [Runbook](docs/runbook.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Performance method and measured results](docs/performance.md)
- [Acceptance report](docs/acceptance_report.md)
