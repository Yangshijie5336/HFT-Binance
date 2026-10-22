# Validation plan and performance interpretation

## Correctness gates

1. Unit tests validate timestamp parsing, trade validation, factor formulas, deterministic strategy behavior, JSONL persistence and deduplication.
2. Integration test runs the pipeline against synthetic events, then checks the normalized, factor and signal journals.
3. Live 15-minute test captures actual public Binance Spot `btcusdt@trade` events. A valid live report must include the UTC start/end, actual duration, frame/trade counts, raw bytes, output sizes, reconnect count, backpressure count, dropped events, latency percentiles, CPU and peak RSS.
4. Replay the captured `raw_frames.jsonl` into a separate output directory and compare normalized event IDs and signal actions. Raw capture and replay are separate outputs.
5. Build the Docker image and run the same 15-minute capture with `/app/data` mounted to the host.

## Performance metrics

- `throughput_trades_per_second = trades_processed / actual_duration_seconds`.
- Receive-to-process and receive-to-signal latency are sampled every fifth event; p50/p95/p99 are approximate sample quantiles, not exchange-to-exchange latency.
- Queue high-water and backpressure indicate whether the consumer keeps up.
- `dropped_events` must remain zero. The queue blocks the receiver instead of dropping events.
- CPU is sampled once per second with `psutil`; peak RSS is sampled at the same cadence.
- Raw frames are fsynced individually. Derived journals are fsynced every 100 records and at shutdown. This intentionally adds I/O overhead.

## Honest acceptance

Do not write a PASS report until a real 900-second capture has completed. If public network access, DNS, TLS or the WebSocket endpoint is unavailable, record BLOCKED with the actual error. Do not substitute generated/synthetic events for the live sample. Unit tests and replay validation are not evidence of live market-data availability.

## Limitations

This is a single-process Python event-processing demo, not a colocated exchange HFT engine. Latency includes Python scheduling, thread-pool filesystem writes, local storage and network path. There is no authenticated trading, order placement or PnL objective. A 15-minute run measures the observed feed and host only; it cannot establish long-term production reliability.
