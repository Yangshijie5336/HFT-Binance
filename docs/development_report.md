# MarketPipe HFT Demo — development report

Date: 2026-10-22

## Objective

Build a reproducible public-market-data demo covering live WebSocket ingestion, durable raw capture, bounded in-memory processing, factor calculation, deterministic signal generation, audit logs and performance measurement. Strategy PnL is explicitly out of scope.

## Design decisions and trade-offs

1. **Public feed**: Binance Spot WebSocket `btcusdt@trade`; it does not require API credentials. Each JSON trade message is journaled before parsing, with Binance maker-side semantics mapped to aggressor side.
2. **Raw durability first**: each raw frame is appended and fsynced before it is parsed or queued. This improves recoverability but adds storage latency; measured receive-to-process latency includes this cost.
3. **Bounded memory**: an `asyncio.Queue` and a fixed-capacity `deque` prevent unbounded growth. When the queue is full, ingestion applies backpressure instead of dropping events.
4. **Deterministic pipeline**: event validation and bounded deduplication precede rolling trade-flow factors. The demo strategy emits `BUY_SIGNAL`, `SELL_SIGNAL` or `HOLD`; no order execution code exists.
5. **Replayability**: the raw frame JSONL is the source of truth. Replay regenerates normalized events, factors and signals without a live connection.
6. **Honest performance reporting**: the run summary records observed duration, rates, latency quantiles, CPU/RSS, queue pressure, disconnects and SHA-256 hashes. A report is generated from the measured summary, not prefilled with expected numbers.
7. **Deployment**: Dockerfile, Compose configuration, CI image build and release-tag GHCR publishing workflow are included. A local Docker build and remote publication still require a Docker-enabled host and authenticated GitHub repository.

## AI-assisted development and independent verification

AI assistance was used to scaffold the event model, storage, processing pipeline, factor/strategy interfaces, CLI, tests, Docker configuration and documentation. Verification was performed independently by running the test suite and compile checks, then correcting a floating-point test assumption and improving O(1) rolling factor updates, capture result gates, structured JSON logs, raw-journal failure handling and deterministic replay coverage. AI-generated code is treated as a hypothesis until tests and runtime evidence pass.

## Results available in the authoring environment

- Unit/integration tests: **9 passed**.
- Python `compileall`: **PASS**.
- CLI help/import path: **PASS**.
- Live WebSocket connectivity probe: **BLOCKED** by Binance HTTP 451 geographic access restriction after the endpoint was reached.
- Real 900-second live capture: **NOT EXECUTED**; no live event dataset or live performance numbers are claimed.
- Docker image build: **NOT EXECUTED**, because Docker CLI is not installed in this environment.
- Public GitHub repository push: **NOT EXECUTED**, because no authenticated GitHub publishing session is available in this environment. `scripts/publish_to_github.ps1` is provided for a user-authenticated Windows host.

## What remains to complete the requested acceptance

1. Run `scripts/run_peak_window.ps1` on a host with public DNS/network access during its selected 13:00–17:00 UTC activity window; let the 900-second capture finish.
2. Verify `summary.json` and `performance_report.md`; preserve all raw frames and logs. Do not substitute synthetic fixtures for live data.
3. Run replay and compare normalized event IDs and signal actions against the capture output.
4. Run Docker build and the same capture with a host volume; record the actual image ID/digest and runtime exit code.
5. Authenticate GitHub CLI, run `scripts/publish_to_github.ps1`, verify public repository visibility and passing CI, then publish a version tag and verify the GHCR image digest/public visibility.

This report intentionally does not invent live performance results. The project is implemented and offline-tested, but the requested 15-minute live-data acceptance is pending a network-enabled runtime.
