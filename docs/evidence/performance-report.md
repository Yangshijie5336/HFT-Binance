# MarketPipe live-feed performance and acceptance report

## Result

- **Run status:** `PASS`
- **Symbol:** `BTCUSDT`
- **Source:** `wss://stream.binance.com:9443/ws/btcusdt@trade`
- **Run id:** `live900-BTCUSDT-20261022`
- **Start UTC:** `2026-10-22T04:39:34.155114400Z`
- **Finish UTC:** `2026-10-22T04:54:34.608986900Z`
- **Target duration:** 900 s
- **Observed capture duration:** 900.11286959999995 s
- **Processing/drain duration:** 900.45390999999995 s
- **Failure:** `none`
- **Stopped by signal:** no

## Event, connection and storage counters

| Metric | Value |
|---|---:|
| WebSocket frames received | 24903 |
| Raw frames journaled | 24903 |
| Raw frame bytes | 19881198 |
| Trade events seen | 24903 |
| Unique trades processed | 24903 |
| Signals emitted | 24903 |
| Duplicate trades | 0 |
| Out-of-order trades | 0 |
| Malformed frames | 0 |
| Subscription acknowledgements | 0 |
| Server error frames | 0 |
| Disconnects | 0 |
| Reconnect attempts | 0 |
| Connection generations | 1 |
| Queue high-water / capacity | 292 / 65536 |
| Ring capacity | 20000 |
| Queue overflow | no |
| Dropped events | 0 |
| Persistence error | no |
| Processing failures | 0 |
| Throughput | 27.666530321987967 trades/s |
| Observed ingress | 27.666530321987967 trade events/s |
| Raw disk write rate | 22087.449998170763 bytes/s |
| Average unique trades/minute | 1659.991819319278 |
| Peak unique trades/minute | 3072 |
| Peak unique trades/second | 462 |
| Average sampled CPU | 0.30887462171620472 % |
| Peak sampled RSS | 36896768 bytes |
| HTTP status | 0 |

## Latency (sampled; microseconds)

Samples are taken every fifth processed event, capped at 200,000. They measure local receive-to-process and receive-to-signal latency, not exchange-to-exchange latency.

| Stage | p50 | p95 | p99 | Samples |
|---|---:|---:|---:|---:|
| Queue wait | 565.60000000000002 | 3118.1999999999998 | 6201 | 4980 |
| Receive to process | 731.89999999999998 | 19401.099999999999 | 22655.5 | 4980 |
| Receive to signal | 731.79999999999995 | 19401.099999999999 | 22655.400000000001 | 4980 |

## Reconnect gap record

No reconnect gap was observed.

## Quality gates

- `artifact_hashes_match`: **PASS**
- `collector_raw_integrity_verified`: **PASS**
- `raw_frames_exist`: **PASS**
- `raw_payload_hashes_valid`: **PASS**
- `raw_records_match_summary`: **PASS**
- `raw_sha256_matches_summary`: **PASS**
- `summary_result_pass`: **PASS**

## Artifact integrity (SHA-256)

- `factors.jsonl`: `3bf57c187d1d43b183e6fc5665154c367ac98bf557b58a32991bd39f383500a1`
- `normalized_events.jsonl`: `b5a2e245d3f589d0da7c3f0360d6c20901a78c75835c111672bbde80d8e16bc0`
- `raw_frames.jsonl`: `5836675014897976d47654e8d0fde5f765f7ff129d0ecba9c413d5c5a82919b8`
- `signals.jsonl`: `fa77b1f2296b2e35f4629ec343c49277cec928f656237717247702f266cf9ae9`
- `system.log`: `4e8932968d9a0224b61f82793dee8264a5fae4092b991cdf60bf14b40ee8ae97`

## Persistence and honest interpretation

- Raw WebSocket frames are stored as exact text (base JSON escaping only) with run_id, connection generation, receive UTC/monotonic timestamps, sequence, payload bytes and payload SHA-256.
- Raw and derived files are flushed in batches and fsynced every buffered_batch_flush_fsync_every_100_records_plus_final and on finalization; finalization errors are reported, never ignored.
- A `PASS` requires a real WebSocket connection, at least 99% of the requested duration, persisted raw frames, matching SHA-256, zero queue overflow, zero persistence/processing failures, and no disconnects, malformed or duplicate trades.
- A synthetic benchmark is never evidence of live network throughput. The strategy emits labels only; execution is disabled.
