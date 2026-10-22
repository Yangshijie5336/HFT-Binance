# Interfaces

## Command line

```
marketpipe --help
marketpipe capture [options]
marketpipe replay --input FILE [--output-dir DIR]
marketpipe benchmark [--events N] [--mode all|parse|pipeline|persist] [--output-dir DIR]
marketpipe report --run-dir DIR
marketpipe synthetic [--events N]
```

### `capture`

| Option | Default | Validation |
|---|---|---|
| `--symbol` | `BTCUSDT` | non-empty; URL stream name is lowercased |
| `--ws-url` | `wss://stream.binance.com:9443/ws/<symbol>@trade` | `ws://` only for loopback |
| `--duration` | `900` | positive seconds (fractional allowed) |
| `--output-dir` | `data/runs` | writable directory |
| `--run-id` | `<UTC timestamp>Z_<symbol>` | explicit run subdirectory name |
| `--queue-capacity` | `65536` | positive |
| `--ring-capacity` | `20000` | positive |
| `--fsync-every` | `100` | positive; raw/derived records between fsyncs |
| `--connect-timeout-ms` | `10000` | positive |
| `--idle-timeout-ms` | `90000` | positive; Beast keep-alive pings enabled |
| `--reconnect-initial` | `1.0` | non-negative seconds |
| `--reconnect-max` | `15.0` | positive seconds |
| `--max-connect-failures` | `10` | positive consecutive failures |
| `--ca-file` | discovered | PEM bundle; explicit load failure aborts |
| `--quiet` | off | suppress stderr mirror |

Exit status:

| Code | Meaning |
|---|---|
| `0` | `PASS` or `PARTIAL` summary written and verified |
| `2` | `FAIL` / `BLOCKED` / argument error / persistence or TLS failure |
| `130` | stopped by SIGINT or SIGTERM; summary still finalized |

### `replay`

Reads `raw_frames.jsonl`, extracts each `payload`, and runs the same parser/factors/
strategy in arrival order. Outputs `normalized_events.jsonl`, `factors.jsonl`,
`signals.jsonl`, `replay_summary.json` in `--output-dir` (default
`data/replays/<UTC timestamp>_replay`). Exit `2` on open/write/finalize failure.

### `benchmark`

Modes:

- `parse`: JSON → validated trade only.
- `pipeline`: parse + dedup + factors + strategy, no disk.
- `persist`: pipeline plus raw JSONL persistence and final fsync.
- `all`: all three sequentially (default).

The output is explicitly labelled `"benchmark_type":"SYNTHETIC_BENCHMARK"` and
`"live_network":false`. It must never be presented as live network throughput.

### `report`

Recomputes artifact SHA-256 values and validates `raw_frames.jsonl` record-by-record.
Writes `verification.json` and regenerates `performance_report.md`. Exit `0` only when
the summary is `PASS` and every check passes; `3` for a valid `PARTIAL`; `2` otherwise.

## Run directory

```
<output-dir>/<run-id>/
  raw_frames.jsonl
  normalized_events.jsonl
  factors.jsonl
  signals.jsonl
  system.log
  summary.json
  verification.json
  performance_report.md
```

`artifact_sha256` covers `raw_frames.jsonl`, `normalized_events.jsonl`, `factors.jsonl`,
`signals.jsonl` and `system.log`. It is computed after those files are finalized and
before `summary.json` is written.

## Raw frame record

One JSON object per WebSocket text frame, in receive order:

```json
{
  "record_type": "raw_websocket_frame",
  "run_id": "live900-BTCUSDT-20261022",
  "connection_id": "live900-BTCUSDT-20261022-conn-1-123456",
  "connection_generation": 1,
  "frame_seq": 42,
  "received_at_utc": "2026-10-22T04:33:01.123456789Z",
  "received_monotonic_ns": 1234567890123,
  "received_utc": "2026-10-22T04:33:01.123456789Z",
  "received_ns": 1234567890123,
  "payload": "{\"e\":\"trade\",...}",
  "raw_payload": "{\"e\":\"trade\",...}",
  "payload_sha256": "…64 hex…",
  "payload_bytes": 217
}
```

`payload` is the exact wire text after JSON string escaping; `payload_sha256` is computed
over the unescaped text. `frame_seq` is global and strictly increasing.
`connection_generation` is non-decreasing.

## Normalized event

```json
{
  "stage": "normalized_event",
  "event": {
    "symbol": "BTCUSDT",
    "side": "buy",
    "price_text": "67000.12000000",
    "qty_text": "0.00120000",
    "price": 67000.12,
    "qty": 0.0012,
    "trade_id": "123456789",
    "exchange_timestamp": "2026-10-22T04:33:01.120Z",
    "exchange_ts_ns": 1761117181120000000,
    "received_utc": "2026-10-22T04:33:01.123456789Z",
    "received_ns": 1234567890123,
    "frame_seq": 42,
    "connection_generation": 1,
    "connection_id": "…",
    "event_key": "BTCUSDT:123456789",
    "source_type": "trade"
  },
  "processed_ns": 1234567890456,
  "ring_size": 42
}
```

`side` is the aggressor side derived from Binance `m`: `m=true` → `sell`, otherwise
`buy`. Duplicates and out-of-order trades are counted but not written to this stream.

## Factor record

```json
{
  "stage": "factor_snapshot",
  "symbol": "BTCUSDT",
  "trade_id": "123456789",
  "price": 67000.12,
  "qty": 0.0012,
  "trade_imbalance": 0.34,
  "return_n": 0.00012,
  "trade_rate_10s": 8.4,
  "avg_trade_size": 0.0011,
  "window_trades": 30,
  "exchange_ts_ns": 1761117181120000000,
  "received_ns": 1234567890123,
  "computed_ns": 1234567890500
}
```

Factor definitions:

- `trade_imbalance = (buy_qty - sell_qty) / (buy_qty + sell_qty)` over a 30 s window
  keyed by local receive time.
- `return_n = last_price / first_price - 1` over up to 21 recent prices.
- `trade_rate_10s = trades in the last 10 s / 10`.
- `avg_trade_size = window_qty / window_trades`.

## Signal record

```json
{
  "symbol": "BTCUSDT",
  "trade_id": "123456789",
  "action": "BUY_SIGNAL",
  "reason": "positive_trade_imbalance_and_momentum",
  "score": 0.42,
  "price": 67000.12,
  "created_ns": 1234567890550,
  "execution": "DISABLED_DEMO_ONLY"
}
```

Rules:

- `BUY_SIGNAL` if `imbalance >= 0.15` and `return_n >= 0.0001`.
- `SELL_SIGNAL` if `imbalance <= -0.15` and `return_n <= -0.0001`.
- otherwise `HOLD`.

`score = clamp(imbalance * 0.7 + return_n * 0.3 * 1000, -1, 1)`. No order is sent.

## `summary.json`

Key fields (see the file for the complete set):

| Field | Meaning |
|---|---|
| `result` | `PASS`, `PARTIAL`, `FAIL`, `BLOCKED` |
| `failure` | null or concrete diagnostic |
| `started_utc` / `finished_utc` / `started_wall_ns` / `finished_wall_ns` | wall clock |
| `duration_target_seconds` / `duration_actual_seconds` | requested vs observed |
| `frames_received` / `raw_frames_records` / `raw_frames_bytes` | raw ingest/persistence |
| `trade_events_seen` / `trades_processed` / `signals_emitted` | pipeline counters |
| `duplicates` / `out_of_order` / `malformed_frames` | validation counters |
| `subscription_acks` / `server_errors` | protocol frames |
| `reconnects` / `disconnects` / `connection_generations` | connection history |
| `gap_windows[]` | generation, approximate gap seconds, timestamps |
| `queue_capacity` / `queue_high_water` / `queue_overflow` / `dropped_events` | backpressure |
| `persistence_error` / `processing_failures` | durability/failure flags |
| `raw_frames_sha256` / `raw_integrity_verified` | raw file integrity |
| `queue_wait_latency` / `receive_to_process_latency` / `receive_to_signal_latency` | sampled percentiles |
| `cpu_percent_avg` / `rss_bytes_peak` | sampled process resources |
| `activity_by_minute_utc` | unique processed trades by exchange minute |
| `quality_checks` | PASS/FALSE gate map |
| `artifact_sha256` | file hashes |
| `execution_enabled` | always `false` |

Latency samples are taken every fifth processed event and capped at 200,000. They are
local timings, not exchange-to-exchange latency.
