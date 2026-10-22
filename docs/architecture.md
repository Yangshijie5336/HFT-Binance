# Architecture

## Scope

MarketPipe is a public market-data engineering demo. It consumes Binance Spot WebSocket `btcusdt@trade` events for `BTCUSDT`. Public trade feeds require no API key. The program has no order adapter and never submits orders.

## Event path

1. **WebSocket receiver**: receives a complete text frame and stamps local UTC plus monotonic receive time.
2. **Raw journal**: appends the exact WebSocket text in `raw_frames.jsonl`, then flushes and `fsync`s before parsing or enqueueing. If persistence fails, the capture must fail rather than silently continue.
3. **Parser and bounded queue**: validates symbol, side, positive price/quantity, exchange timestamp and trade ID. The bounded `asyncio.Queue` applies backpressure instead of dropping events.
4. **In-memory ring**: a bounded `deque` holds the most recent normalized trades for inspection.
5. **Deduplication**: an LRU-like bounded ordered map deduplicates `(symbol, trade_id)`.
6. **Factors**: rolling aggressor-side quantity imbalance, N-trade return, trade rate and mean trade size.
7. **Strategy**: emits `BUY_SIGNAL`, `SELL_SIGNAL` or `HOLD`; it has no execution path.
8. **Derived journals**: normalized events, factor snapshots and signals are append-only JSONL, flushed per record and fsynced every 100 records plus shutdown.
9. **Metrics**: throughput, sampled receive-to-process/signal latency, queue high-water mark, backpressure, reconnects, CPU and peak RSS.

## Persistence semantics

`raw_frames.jsonl` is the source of truth. Every WebSocket frame is written as the exact received text and `fsync`ed before its contents enter the pipeline. Snapshot, update, heartbeat/status and subscription error frames are all captured. Derived files can be regenerated with `marketpipe replay --input <raw_frames.jsonl>`.

The raw frame's durable write occurs before processing. This favors auditability over minimum latency: synchronous filesystem flushes can reduce measured throughput and affect receive latency. This trade-off is reported, not hidden. A local filesystem and storage device still determine actual durability guarantees.

## Reconnect and loss semantics

The collector reconnects with bounded exponential backoff. Every received frame is journaled before parsing. TCP/WebSocket disconnections are recorded in the logs and summary. The system cannot claim zero exchange-side gaps during a disconnection unless the venue provides a replayable sequence protocol; gaps are therefore reported as an observability limitation, not silently inferred away.

## Concurrency

A single asyncio receiver feeds one bounded processing queue and one consumer task. File writes run in a worker thread so fsync does not block the event loop's protocol machinery, while the receiver awaits each raw-frame durability acknowledgement. The queue and ring have fixed capacities. No unbounded per-event in-memory history is retained.

## Security

Only public market data is used. No API credentials, account endpoints, order APIs, or production trading are present. The strategy's signal records include `execution=DISABLED_DEMO_ONLY`.

