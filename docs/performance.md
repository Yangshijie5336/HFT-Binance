# 性能方法与实测结果

## 真实性规则

- 合成基准明确标记 `SYNTHETIC_BENCHMARK` 与 `live_network:false`。
- 模式 D 的数字只来自真实 Binance `capture`，读取 `summary.json`。
- 不把基准成绩复制进实时采样结论。
- 延迟是本地 `steady_clock` 从 WebSocket 读取完成到解析完成/信号生成的时间，不是交易所到交易所延迟。

## 模式

| 模式 | 测量内容 | 命令 |
|---|---|---|
| A. 解析 | JSON 解析 + Binance 成交校验，无去重/因子/IO | `marketpipe benchmark --events 100000 --mode parse` |
| B. 处理链路 | 解析 + 全局去重 + 30 秒因子 + 策略，无 IO | `marketpipe benchmark --events 100000 --mode pipeline` |
| C. 处理链路 + raw 持久化 | B 加上 raw JSONL 写入、批量 flush、最终 fsync | `marketpipe benchmark --events 100000 --mode persist` |
| D. 真实网络 | 真实 Binance WebSocket → raw 持久化 → 解析/去重/因子/信号 | `marketpipe capture --duration 900` |

`benchmark --mode all` 在一个进程中依次运行 A/B/C，并写入 `data/benchmarks/synthetic_benchmark_summary.json`。

## 上报字段

合成模式：`events`、`events_per_second`、`duration_seconds`、`p50_us`、`p95_us`、`p99_us`、`latency_samples`、`signals_emitted`、`raw_frames_written`、`raw_frames_sha256`、`output_file`、`cpu_percent`、`rss_bytes`、`dropped_events`、`errors`。

真实模式（来自 `summary.json`）：`duration_actual_seconds`、`drain_elapsed_seconds`、`processing_elapsed_seconds`、`throughput_trades_per_second`、`ingress_trades_per_second`、`receive_to_process_latency`、`receive_to_signal_latency`、`queue_wait_latency`、`queue_high_water`、`queue_capacity`、`cpu_percent_avg`、`rss_bytes_peak`、`raw_frames_records`、`raw_frames_bytes`、`trades_processed`、`signals_emitted`、`duplicates`、`out_of_order`、`dropped_events`、`processing_failures`、`reconnects`、`disconnects`。

磁盘写入速率 = `raw_frames_bytes / duration_actual_seconds`。`duration_actual_seconds` 在停止接收时固定，不包含排空；PASS 要求实际采样严格达到目标时长。`drain_elapsed_seconds` 单独记录排空/最终确定耗时，不能用于补足采样时长。

## 环境

- 宿主系统：Windows 10/11 x64（构建与采样主机）
- 编译器：MSVC 19.51.36260（Visual Studio 18），C++20，Release
- CMake 4.3.1，Ninja 1.13.2
- Boost 1.91（header-only Beast/Asio），OpenSSL 3.6.2（vcpkg x64-windows）
- 采集主机为普通开发工作站，不是交易所托管主机

## 实测结果

以下结果均已实际执行。

### A/B/C — 100,000 条合成事件

使用最终 Release 二进制（`SYNTHETIC_BENCHMARK`，`live_network:false`）：

| 模式 | events/s | P50 µs | P95 µs | P99 µs | CPU % | RSS B | 错误/丢弃 |
|---|---:|---:|---:|---:|---:|---:|---:|
| A. 纯解析 | 165,667 | 3.6 | 5.4 | 7.5 | 100.9 | 25,485,312 | 0 / 0 |
| B. 完整处理链路 | 136,562 | 6.5 | 9.6 | 14.9 | 99.7 | 26,124,288 | 0 / 0 |
| C. B + raw 持久化 | 42,148.8 | 14.1 | 392.4 | 693.3 | 84.1 | 27,037,696 | 0 / 0 |

模式 A 不计算信号。B/C 共产生 99,991 个合成非 HOLD 标签。模式 C 写入 100,000 个原始帧、51,363,504 字节，SHA-256 为 `1c4193c1a80dbb7dbea003e450979dc83e225251c26d8445596d6f1393143ad8`。进程 CPU 可以超过 100%，因为它是累计 CPU 时间除以墙钟时间。

可复现入口：

```bash
./build/marketpipe benchmark --events 100000 --mode all --output-dir data/benchmarks
# 或
./scripts/benchmark_100k.sh
```

### D — 真实 Binance 900 秒采样

2026-10-22 12:01:29.455 UTC 至 12:16:30.005 UTC 执行（`live900-BTCUSDT-20261022-v2`），结果 **PASS**：

| 指标 | 数值 |
|---|---:|
| 实际采样时长 | 900.0715 秒 |
| 排空/最终确定时长 | 0.0426 秒（不计入采样） |
| 帧数 / raw 记录数 | 32,499 / 32,499 |
| 已处理成交 / 信号 | 32,499 / 32,499 |
| 吞吐 | 36.11 笔/秒 |
| raw 字节 / 磁盘写入速率 | 26,113,033 B / 29,012 字节/秒 |
| 队列峰值 / 容量 | 485 / 65,536 |
| 重复 / 乱序 / 异常帧 | 0 / 0 / 0 |
| 重连 / 断开 | 0 / 0 |
| 队列溢出 / 丢弃 / 持久化错误 | 0 / 0 / 0 |
| CPU 均值 / RSS 峰值 | 0.418 % / 41,607,168 B |
| raw SHA-256 | `7495d8d0fdc92840ee0ccacb1646e9a4a0467ede175781400cee820be66a5301` |

| 阶段 | P50 µs | P95 µs | P99 µs |
|---|---:|---:|---:|
| 队列等待 | 875.3 | 5,448.1 | 13,054.0 |
| 接收→处理 | 1,086.0 | 20,050.0 | 31,220.2 |
| 接收→信号 | 1,085.9 | 20,049.9 | 31,220.1 |

该窗口信号为 32,499 个 HOLD / 0 个 BUY / 0 个 SELL。21 笔滚动收益在 -5.136e-05…+7.533e-05 之间，低于 1e-4 动量门限；没有为了制造信号而人为放松策略。完整证据与哈希见[验收报告](acceptance_report.md)。

## 解释边界

`PASS` 只表示该软件在单主机、单 15 分钟窗口内通过了时长/数据/完整性/无丢弃检查。它不能证明长期可靠性、交易所侧完整性、托管级低延迟或盈利能力。策略只是确定性的标签示例，执行始终禁用。
