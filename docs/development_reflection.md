# Development reflection (template to finalize after the live run)

## Decisions and trade-offs

- **Public source**: Kraken Spot WebSocket v2 trade channel avoids credentials and private account data. The feed sends trade events when orders match; a message may batch several trades.
- **Durability vs latency**: the raw journal is fsynced per frame before processing. This is intentionally conservative and likely slower than a high-throughput asynchronous batch writer. The measured latency and throughput should determine whether a later WAL/batched durability mode is warranted.
- **Bounded memory vs completeness**: the ring buffer and queue are bounded. Backpressure blocks ingestion instead of dropping events. The raw journal is independent of the processing queue, so the received frame is durable before the pipeline can stall. A long storage stall can still cause a socket disconnect; that must be reported.
- **Factor/strategy scope**: the factors and strategy are deterministic examples intended to exercise the full pipeline, not claim alpha or trading profitability.
- **Replay**: raw frames are the reproducibility source; derived outputs can be rebuilt without a live connection.

## AI collaboration and verification

Record the prompts/changes used, review each diff, ask the AI to explain the failure modes, and independently run unit tests, compile checks, a live 900-second capture, and replay comparison. AI-generated code is not considered correct until tests and runtime evidence support it.

## To complete after the live run

Replace this template with actual observations: event/frame counts, throughput, p50/p95/p99 latency, CPU/RSS, disconnects, backpressure, raw journal size, replay parity, Docker build/run results, and any failures. Do not invent metrics before running the live sample.
