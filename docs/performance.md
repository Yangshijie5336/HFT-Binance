# Performance method and results

## Honesty rules

- The synthetic benchmark is labelled `SYNTHETIC_BENCHMARK` and `live_network:false`.
- Mode D numbers come only from a real `capture` run against Binance and are read from
  `summary.json`.
- No benchmark result is copied into a live-capture claim.
- Latency is measured locally (`steady_clock`) from WebSocket read completion to parse
  completion and signal creation. It is not exchange-to-exchange latency.

## Modes

| Mode | What is measured | Command |
|---|---|---|
| A. Parse | JSON parse + Binance trade validation, no dedup/factors/IO | `marketpipe benchmark --events 100000 --mode parse` |
| B. Pipeline | Parse + global dedup + 30 s factors + strategy, no IO | `marketpipe benchmark --events 100000 --mode pipeline` |
| C. Pipeline + raw persistence | B plus raw JSONL write, batch flush and final fsync | `marketpipe benchmark --events 100000 --mode persist` |
| D. Live network | Real Binance WebSocket → raw persistence → parse/dedup/factors/signals | `marketpipe capture --duration 900` |

`benchmark --mode all` runs A, B and C in one process and writes
`data/benchmarks/synthetic_benchmark_summary.json`.

## Fields reported

Synthetic modes: `events`, `events_per_second`, `duration_seconds`, `p50_us`, `p95_us`,
`p99_us`, `latency_samples`, `signals_emitted`, `raw_frames_written`,
`raw_frames_sha256`, `output_file`, `cpu_percent`, `rss_bytes`, `dropped_events`,
`errors`.

Live mode (from `summary.json`): `duration_actual_seconds`,
`throughput_trades_per_second`, `ingress_trades_per_second`,
`receive_to_process_latency`, `receive_to_signal_latency`, `queue_wait_latency`,
`queue_high_water`, `queue_capacity`, `cpu_percent_avg`, `rss_bytes_peak`,
`raw_frames_records`, `raw_frames_bytes`, `trades_processed`, `signals_emitted`,
`duplicates`, `dropped_events`, `processing_failures`, `reconnects`, `disconnects`.

Disk write rate is derived as `raw_frames_bytes / duration_actual_seconds`.

## Environment

- Host OS: Windows 10/11 x64 (build host)
- Compiler: MSVC 19.51.36260 (Visual Studio 18), C++20, Release
- CMake 4.3.1, Ninja 1.13.2
- Boost 1.91 (header-only Beast/Asio), OpenSSL 3.6.2 (vcpkg x64-windows)
- Capture host is a normal developer workstation, not a colocated exchange host

## Measured results

Results are recorded only after execution; see the tables below.

### A/B/C — 100,000 synthetic events

Executed with the final Release binary (`SYNTHETIC_BENCHMARK`, `live_network:false`):

| Mode | events/s | p50 µs | p95 µs | p99 µs | CPU % | RSS B | errors/drops |
|---|---:|---:|---:|---:|---:|---:|---:|
| A. Parse only | 163,315 | 3.5 | 6.4 | 8.0 | 102.0 | 25,489,408 | 0 / 0 |
| B. Full pipeline | 135,678 | 6.4 | 11.5 | 17.6 | 98.7 | 26,112,000 | 0 / 0 |
| C. B + raw persistence | 53,132 | 10.6 | 407.5 | 636.5 | 86.5 | 27,082,752 | 0 / 0 |

Mode A signals are not computed. Modes B/C emitted 99,991 synthetic non-HOLD labels.
Mode C persisted 100,000 raw frames / 51,363,504 bytes, SHA-256
`84eb486b8050eb47a1e84cc829e8df53ee4d81f8b0815abe00b54e1fe43642a7`. Process CPU can
exceed 100% because it is summed CPU time divided by wall time.

The reproducible entry point is:

```bash
./build/marketpipe benchmark --events 100000 --mode all --output-dir data/benchmarks
# or
./scripts/benchmark_100k.sh
```

### D — real Binance 900-second capture

Executed on 2026-10-22 from 04:39:34.155 UTC to 04:54:34.609 UTC
(`live900-BTCUSDT-20261022`), result **PASS**:

| Metric | Value |
|---|---:|
| Observed duration | 900.1129 s |
| Frames / raw records | 24,903 / 24,903 |
| Trades processed / signals | 24,903 / 24,903 |
| Throughput | 27.67 trades/s |
| Raw bytes / disk write rate | 19,881,198 B / 22,087 B/s |
| Queue high-water / capacity | 292 / 65,536 |
| Duplicates / out-of-order / malformed | 0 / 0 / 0 |
| Reconnects / disconnects | 0 / 0 |
| Queue overflow / dropped / persistence errors | 0 / 0 / 0 |
| Avg CPU / peak RSS | 0.309 % / 36,896,768 B |
| Raw SHA-256 | `5836675014897976d47654e8d0fde5f765f7ff129d0ecba9c413d5c5a82919b8` |

| Stage | p50 µs | p95 µs | p99 µs |
|---|---:|---:|---:|
| Queue wait | 565.6 | 3,118.2 | 6,201.0 |
| Receive to process | 731.9 | 19,401.1 | 22,655.5 |
| Receive to signal | 731.8 | 19,401.1 | 22,655.4 |

Live signals in this window were 24,903 HOLD / 0 BUY / 0 SELL. The 21-trade return stayed
within -7.346e-05…+8.081e-05, below the 1e-4 momentum gate; the strategy was not
artificially loosened to manufacture a signal. See
[`acceptance_report.md`](acceptance_report.md) for full evidence and hashes.

## Interpretation limits

`PASS` means the software's duration/data/integrity/no-drop checks passed for one
15-minute window on one host. It does not establish long-term reliability, exchange-side
completeness, colocation-grade latency or profitability. The strategy is a deterministic
labeling demo; execution is disabled.
