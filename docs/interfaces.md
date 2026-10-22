# 接口说明

## 命令行

```
marketpipe --help
marketpipe capture [options]
marketpipe replay --input FILE [--output-dir DIR]
marketpipe benchmark [--events N] [--mode all|parse|pipeline|persist] [--output-dir DIR]
marketpipe report --run-dir DIR
marketpipe synthetic [--events N]
```

### `capture`

| 参数 | 默认值 | 校验 |
|---|---|---|
| `--symbol` | `BTCUSDT` | 非空；URL 流名转为小写 |
| `--ws-url` | `wss://stream.binance.com:9443/ws/<symbol>@trade` | `ws://` 仅允许回环 |
| `--duration` | `900` | 正数秒（允许小数） |
| `--output-dir` | `data/runs` | 可写目录 |
| `--run-id` | `<UTC 时间戳>Z_<symbol>` | 指定运行子目录名 |
| `--queue-capacity` | `65536` | 正数 |
| `--ring-capacity` | `20000` | 正数 |
| `--fsync-every` | `100` | 正数；raw/派生文件 fsync 间隔记录数 |
| `--connect-timeout-ms` | `10000` | 正数 |
| `--idle-timeout-ms` | `90000` | 正数；启用 Beast keep-alive ping |
| `--reconnect-initial` | `1.0` | 非负秒数 |
| `--reconnect-max` | `15.0` | 正秒数 |
| `--max-connect-failures` | `10` | 连续失败上限 |
| `--ca-file` | 自动发现 | PEM bundle；显式加载失败会中止 |
| `--quiet` | 关闭 | 不镜像日志到 stderr |

退出状态：

| 退出码 | 含义 |
|---|---|
| `0` | `PASS` 或 `PARTIAL`，summary 已写入并通过校验 |
| `2` | `FAIL` / `BLOCKED` / 参数错误 / 持久化或 TLS 失败 |
| `130` | 被 SIGINT 或 SIGTERM 停止；summary 仍会最终确定 |

### `replay`

读取采集器生成的 `raw_frames.jsonl`，先校验 envelope、逐帧 payload SHA-256、`payload_bytes`、`frame_seq` 递增和连接代次，再用原始 `received_monotonic_ns` 恢复因子时钟。在 `--output-dir`（默认 `data/replays/<UTC 时间戳>_replay`）输出 `normalized_events.jsonl`、`factors.jsonl`、`signals.jsonl`、`replay_summary.json`。

- 完整性错误、非法 envelope、零有效成交或 malformed payload：`result=FAIL`，退出码 `2`，不输出通过结论。
- 存在重复或乱序成交但输入完整：`result=PARTIAL`，退出码 `3`；乱序成交仍被处理并标记。
- 干净输入：`result=PASS`，退出码 `0`。

`replay_summary.json` 包含 `frames_validated`、`integrity_errors`、`trades_processed`、`duplicates`、`out_of_order`、`malformed_frames`、`exit_code`。

### `benchmark`

模式：

- `parse`：仅 JSON → 校验后的成交。
- `pipeline`：解析 + 去重 + 因子 + 策略，不落盘。
- `persist`：处理链路 + raw JSONL 持久化与最终 fsync。
- `all`：依次运行三种（默认）。

输出明确标记 `"benchmark_type":"SYNTHETIC_BENCHMARK"` 和 `"live_network":false`，绝不能作为实时网络吞吐量展示。

### `report`

重算产物 SHA-256，并逐条校验 `raw_frames.jsonl`。写入 `verification.json` 并重新生成 `performance_report.md`。只有 summary 为 `PASS` 且所有检查通过才返回 `0`；有效 `PARTIAL` 返回 `3`；其余返回 `2`。

## 运行目录

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

`artifact_sha256` 覆盖 `raw_frames.jsonl`、`normalized_events.jsonl`、`factors.jsonl`、`signals.jsonl` 和 `system.log`。它在这些文件最终确定后、写入 `summary.json` 前计算。

## 原始帧记录

每个 WebSocket 文本帧一行 JSON，按接收顺序排列：

```json
{
  "record_type": "raw_websocket_frame",
  "run_id": "live900-BTCUSDT-20261022-v2",
  "connection_id": "live900-BTCUSDT-20261022-v2-conn-1-123456",
  "connection_generation": 1,
  "frame_seq": 42,
  "received_at_utc": "2026-10-22T04:33:01.123456789Z",
  "received_monotonic_ns": 1234567890123,
  "received_utc": "2026-10-22T04:33:01.123456789Z",
  "received_ns": 1234567890123,
  "payload": "{\"e\":\"trade\",...}",
  "raw_payload": "{\"e\":\"trade\",...}",
  "payload_sha256": "…64 位十六进制…",
  "payload_bytes": 217
}
```

`payload` 是 JSON 字符串转义后的精确线上文本；`payload_sha256` 对未转义文本计算。`frame_seq` 全局严格递增，`connection_generation` 非递减。

## 标准化事件

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
    "out_of_order": false,
    "source_type": "trade"
  },
  "processed_ns": 1234567890456,
  "ring_size": 42
}
```

`side` 是 Binance `m` 推导的主动方：`m=true` → `sell`，否则 `buy`。重复成交跳过；未重复的乱序成交保留，标记 `out_of_order=true`，并进入因子与信号链路。因子窗口按到达（接收）顺序推进，这是实时与 replay 的共同规则。

## 因子记录

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
  "out_of_order": false,
  "computed_ns": 1234567890500
}
```

因子定义：

- `trade_imbalance = (buy_qty - sell_qty) / (buy_qty + sell_qty)`，窗口 30 秒，以本地接收时间计时。
- `return_n = last_price / first_price - 1`，最多 21 个近期价格。
- `trade_rate_10s = 最近 10 秒成交数 / 10`。
- `avg_trade_size = 窗口成交量 / 窗口成交数`。

## 信号记录

```json
{
  "symbol": "BTCUSDT",
  "trade_id": "123456789",
  "action": "BUY_SIGNAL",
  "reason": "positive_trade_imbalance_and_momentum",
  "score": 0.42,
  "price": 67000.12,
  "out_of_order": false,
  "created_ns": 1234567890550,
  "execution": "DISABLED_DEMO_ONLY"
}
```

规则：

- `imbalance >= 0.15` 且 `return_n >= 0.0001` → `BUY_SIGNAL`。
- `imbalance <= -0.15` 且 `return_n <= -0.0001` → `SELL_SIGNAL`。
- 其余 → `HOLD`。

`score = clamp(imbalance * 0.7 + return_n * 0.3 * 1000, -1, 1)`。不会发送任何订单。

## `summary.json`

关键字段（完整字段见文件）：

| 字段 | 含义 |
|---|---|
| `result` | `PASS`、`PARTIAL`、`FAIL`、`BLOCKED` |
| `failure` | null 或具体诊断 |
| `started_utc` / `finished_utc` / `started_wall_ns` / `finished_wall_ns` | 墙钟时间 |
| `duration_target_seconds` / `duration_actual_seconds` | 目标时长与实际采样时长（不含排空） |
| `processing_elapsed_seconds` / `drain_elapsed_seconds` | 总处理墙钟时长 / 排空与最终确定时长 |
| `frames_received` / `raw_frames_records` / `raw_frames_bytes` | 接收/落盘 |
| `trade_events_seen` / `trades_processed` / `signals_emitted` | 处理链路计数 |
| `duplicates` / `out_of_order` / `malformed_frames` | 校验计数 |
| `subscription_acks` / `server_errors` | 协议帧 |
| `reconnects` / `disconnects` / `connection_generations` | 连接历史 |
| `gap_windows[]` | 代次、近似缺口秒数、时间戳 |
| `queue_capacity` / `queue_high_water` / `queue_overflow` / `dropped_events` | 背压 |
| `persistence_error` / `processing_failures` | 持久化/失败标志 |
| `raw_frames_sha256` / `raw_integrity_verified` | 原始文件完整性 |
| `queue_wait_latency` / `receive_to_process_latency` / `receive_to_signal_latency` | 采样延迟分位 |
| `cpu_percent_avg` / `rss_bytes_peak` | 进程资源采样 |
| `activity_by_minute_utc` | 按交易所分钟统计的唯一成交 |
| `quality_checks` | PASS/FALSE 门禁映射；`duration_meets_target` 要求实际采样严格达到目标 |
| `artifact_sha256` | 文件哈希 |
| `execution_enabled` | 始终为 `false` |

延迟每 5 笔已处理成交采样一次，上限 200,000 条。它们是本地时间，不是交易所到交易所延迟。
