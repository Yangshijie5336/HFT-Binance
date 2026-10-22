# Interfaces and data contracts

## Source

- WebSocket: `wss://stream.binance.com:9443/ws/btcusdt@trade` (fallback port 443)
- Stream: `btcusdt@trade`
- Default symbol: `BTCUSDT`
- Subscribe message: `{"method":"SUBSCRIBE","params":["btcusdt@trade"],"id":1}`

## Local command line

### Live capture

```bash
marketpipe capture --duration 900 --symbol BTCUSDT --output-dir data/runs
```

Creates a unique run directory containing:

- `raw_frames.jsonl`: exact received frame text and receive metadata.
- `normalized_events.jsonl`: validated, deduplicated events.
- `factors.jsonl`: factor snapshot per processed event.
- `signals.jsonl`: strategy decision per processed event.
- `system.log`: structured system logs with per-record fsync.
- `summary.json`: measured counters, latency, throughput, CPU and RSS.

### Deterministic replay

```bash
marketpipe replay --input data/runs/<run-id>/raw_frames.jsonl --output-dir data/replays/replay-001
```

Replay uses original exchange timestamps and event payloads. Performance timestamps differ because the replay is local; strategy outputs should be semantically reproducible.

## Event schema

Each raw frame record has `run_id`, `connection_id`, `connection_generation`, `frame_seq`, `received_at_utc`, `received_monotonic_ns`, `payload`, `payload_sha256` and `payload_bytes`. `payload` preserves the original text frame. Binance trade streams normally send one trade per frame; the parser also accepts batched trade arrays for compatibility.

Normalized events include `symbol`, `side`, `price`, `qty`, original `price_text`/`qty_text`, `trade_id`, exchange timestamp, receive timestamp, source frame sequence and `event_key`. Binance `m=true` is mapped to `side=sell` because the buyer was the maker and the seller was the aggressor.

Factors: `trade_imbalance`, `return_n`, `trade_rate_10s`, `avg_trade_size`, `window_trades`.

Signals: `BUY_SIGNAL`, `SELL_SIGNAL`, `HOLD`, reason, score, price and `execution=DISABLED_DEMO_ONLY`.
