# MarketPipe HFT Demo (C++20)

The primary implementation is now a portable C++20/CMake project under `cpp/`. The original Python implementation remains as a migration reference. The C++ core provides Binance trade parsing, durable JSONL replay, rolling factors, deterministic signals, and a synthetic benchmark without third-party runtime dependencies.

## Build C++

```powershell
cmake -S . -B build-cpp -G Ninja
cmake --build build-cpp
ctest --test-dir build-cpp --output-on-failure
.\build-cpp\marketpipe.exe synthetic --events 100000
```

`replay --input <raw_frames.jsonl> <output-dir>` regenerates normalized events, signals, and a replay summary. The C++ `capture` command currently exits `BLOCKED` until a platform WebSocket transport is selected; it never pretends to have captured live data.

A reproducible real-time public trade-feed capture and processing pipeline. It captures Binance Spot WebSocket `btcusdt@trade` trades, durably journals every raw frame, processes validated events through a bounded in-memory queue and ring buffer, calculates rolling factors, emits deterministic demo signals, and reports measured performance.

> **Safety:** public market data only. No API keys, private feeds, order endpoints, or live trading. `BUY_SIGNAL` / `SELL_SIGNAL` are labels only; execution is disabled in code.

## Architecture

`Binance Spot WebSocket @trade → raw frame journal (flush + fsync) → parse/validate/deduplicate → bounded queue → in-memory ring → factors → signal → JSONL outputs + metrics`

See [`docs/architecture.md`](docs/architecture.md), [`docs/interfaces.md`](docs/interfaces.md), and [`docs/validation.md`](docs/validation.md).

## Requirements

- Python 3.12+
- Internet access to `wss://ws.kraken.com/v2`
- A writable local disk

## Run locally

```bash
python -m venv .venv
# Linux/macOS
. .venv/bin/activate
# Windows PowerShell
# .\.venv\Scripts\Activate.ps1
python -m pip install -e '.[test]'
python -m pytest -q
python -m marketpipe.cli capture --duration 900 --symbol BTCUSDT --output-dir data/runs
```

The program creates a unique run folder under `data/runs/`. The actual live 15-minute run is an explicit runtime step; the repository does not ship fabricated live data or fabricated performance results.

## Replay captured data

```bash
python -m marketpipe.cli replay --input data/runs/<run-id>/raw_frames.jsonl --output-dir data/replays/replay-001
```

Compare `normalized_events.jsonl` event keys and `signals.jsonl` actions from the original run and replay. Receive-to-process latency will differ during replay, but event ordering and strategy decisions should be reproducible.

## Docker

```bash
docker compose build
docker compose run --rm marketpipe
```

The compose file mounts `./data` so raw capture and logs survive container removal. The image runs as a non-root user. To run the same capture for 15 minutes, use the default compose command or override it explicitly:

```bash
docker compose run --rm marketpipe capture --duration 900 --symbol BTCUSDT --output-dir /app/data/runs
```

A successful Dockerfile/Compose definition is not itself proof that an image was built. Record the actual build and runtime result in the validation report.

## Output files

- `raw_frames.jsonl`: exact WebSocket frame text, receive time and sequence; fsync per frame.
- `normalized_events.jsonl`: validated and deduplicated trade events.
- `factors.jsonl`: factor snapshot per processed event.
- `signals.jsonl`: `BUY_SIGNAL`, `SELL_SIGNAL` or `HOLD` per processed event; no execution path.
- `system.log`: structured logs, flushed and fsynced per log record.
- `summary.json`: measured run metrics.

## Data source

Binance documents the public Spot WebSocket trade stream at <https://github.com/binance/binance-spot-api-docs/blob/master/web-socket-streams.md>. Public market-data subscriptions do not require API credentials. Each raw frame is stored before parsing.

## Performance caveat

This is a single-process Python engineering demo, not a colocated, exchange-grade HFT execution engine. The raw per-frame fsync policy prioritizes recoverability and auditability over minimum latency. Use the measured 15-minute result, not an assumed number, to discuss performance.

## Sample during a higher-activity window (Windows PowerShell)

After installing the project in `.venv`, run:

```powershell
.\scripts\run_peak_window.ps1
```

The script waits for the next 13:00–17:00 UTC window (approximately 21:00–01:00 China time / 22:00–02:00 Japan time) and captures 900 seconds. This is a practical observation window around Europe/US overlap, not a promise of maximum activity; the report must use the actual observed trade rate. To start immediately, use `-Immediate`.

## GitHub and image publication

The `CI` workflow runs tests and builds the Docker image on GitHub Actions. Tagging a release such as `v0.1.0` triggers the GHCR publishing workflow. After the first package is published, verify its package visibility and explicitly set it to public if public pulls are required. A local Dockerfile is not evidence of a successfully built/published image; use the CI run and image digest as evidence.
