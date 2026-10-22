# Runbook

## 1. Prerequisites

- C++20 compiler, CMake ≥ 3.20, Ninja
- Boost Beast/Asio headers (≥ 1.74) and OpenSSL 3.x
- Writable disk with room for `raw_frames.jsonl` (BTCUSDT is typically a few MB per
  15 minutes; a bursty window may be larger)
- Outbound TLS to `stream.binance.com:9443` for live capture
- No API key is required or accepted

On Debian/Ubuntu:

```bash
sudo apt-get install -y build-essential cmake ninja-build libboost-all-dev libssl-dev ca-certificates
```

## 2. Build and test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Expected CTest suite:

- `marketpipe_help`
- `marketpipe_unit` (17 cases)
- `marketpipe_integration` (13 loopback WebSocket scenarios)
- `marketpipe_benchmark_100k` (parse + pipeline + persistence)

No CTest test contacts Binance.

Windows: run from a Developer PowerShell, set English MSVC include diagnostics so Ninja
tracks header dependencies, and pass the vcpkg prefix explicitly:

```powershell
$env:VSLANG = "1033"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=D:/Dev/vcpkg/installed/x64-windows
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

If a public header changed and an incremental build seems to ignore it, run
`cmake --build build --clean-first`. This is a localized-MSVC `/showIncludes` limitation,
not a source issue.

## 3. Smoke test the live feed

```bash
./build/marketpipe capture --symbol BTCUSDT --duration 5 --output-dir data/smoke
```

Success looks like:

```
capture result=PASS run_dir=data/smoke/20261022T...Z_BTCUSDT frames=40 processed=40 signals=40
```

Then verify independently:

```bash
./build/marketpipe report --run-dir data/smoke/<run-id>
```

If `result` is `BLOCKED` with HTTP 451, stop here and record the block; do not use a
proxy or another jurisdiction to bypass it. Local WebSocket tests still run normally.

## 4. 900-second acceptance capture

```bash
./build/marketpipe capture \
  --symbol BTCUSDT \
  --duration 900 \
  --output-dir data/runs \
  --run-id live900-BTCUSDT-$(date -u +%Y%m%d)
```

Requirements for a valid acceptance run:

- actual duration ≥ 891 s;
- at least one raw frame and one processed trade;
- `raw_frames_records == frames_received`;
- `raw_integrity_verified == true`;
- no queue overflow, dropped events, persistence errors or processing failures;
- no disconnects, malformed frames, duplicates or out-of-order trades.

Run it without competing CPU/disk load. The capture writes a heartbeat-free structured
log; connection start/end, backoff and potential gaps are recorded in `system.log`.

During the run:

```bash
tail -f data/runs/<run-id>/system.log
```

Do **not** edit or truncate files in the run directory while it is live. A graceful stop
with Ctrl+C writes a FAIL summary and exits 130; it does not pretend to satisfy the
duration gate.

## 5. Verify and archive

```bash
./build/marketpipe report --run-dir data/runs/<run-id>
sha256sum data/runs/<run-id>/raw_frames.jsonl
```

`report` recomputes every artifact hash and all raw record hashes. Archive:

- `summary.json`
- `verification.json`
- `performance_report.md`
- `system.log`
- `raw_frames.jsonl` and its SHA-256
- the run start/end UTC timestamps

Large raw captures must not be committed to Git. Keep them locally or attach them to a
GitHub Release / Actions artifact. The `Manual live capture` workflow uploads the run
directory as an artifact with 14-day retention.

## 6. Performance benchmarks

```bash
./build/marketpipe benchmark --events 100000 --mode all \
  --output-dir data/benchmarks
```

This runs:

- A. pure parse
- B. full in-memory pipeline
- C. full pipeline + raw JSONL persistence

Each mode reports `events_per_second`, P50/P95/P99, CPU, RSS, raw frame count/bytes,
dropped/error counts and the SHA-256 of the persisted benchmark file. The output is
explicitly synthetic and is not real-network throughput. Mode D is measured only from a
real `capture` run and comes from `summary.json`.

## 7. Replay

```bash
./build/marketpipe replay \
  --input data/runs/<run-id>/raw_frames.jsonl \
  --output-dir data/replays/<name>
```

Replay reproduces normalized events, factors and signals from the durable raw frames.
Receive-to-process latency is not comparable during replay.

## 8. Docker

```bash
docker compose build
docker compose run --rm marketpipe
```

The compose service runs a 900 s capture into `/app/data/runs` on the named volume
`marketpipe-data`. The image runs as UID 10001 and handles SIGTERM with a 60 s grace
period. The build stage runs CTest; a successful `docker build` is required before
quoting container results.

To inspect or archive the volume:

```bash
docker compose run --rm --entrypoint ls marketpipe -l /app/data/runs
docker compose cp marketpipe:/app/data/runs ./data-from-container
```

For a host bind mount, make the directory writable by UID 10001 (for example
`sudo chown -R 10001:10001 ./data`) or override the compose user; otherwise the non-root
process cannot persist frames.

## 9. CI

- `.github/workflows/ci.yml`: build, CTest, 100k benchmark, Docker build, artifact upload.
- `.github/workflows/live-capture.yml`: manual 900 s live run; preserves BLOCKED evidence.
- `.github/workflows/publish-image.yml`: tag-triggered GHCR publish after tests.

Quote the CI run URL/id and the downloaded artifact, not a local assumption, when
reporting CI evidence.
