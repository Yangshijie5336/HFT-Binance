# 验收证据目录

本目录只保存小型、可提交的验收证据。大型 raw/派生数据保留在 `data/runs/`，并通过 GitHub Release `v0.3.0` 的 `public900-BTCUSDT-20261023.zip` 交付，不直接提交到 Git。

## 主验收样本：public900-BTCUSDT-20261023

| 指标 | 数值 |
|---|---:|
| 结果 | **PASS** |
| 实际采样 | 900.0712497 秒（排空 0.035172 秒另计） |
| 帧 / 成交 / 信号 | 29,140 / 29,140 / 29,140 |
| 重连 / 断开 | 0 / 0 |
| raw 字节 | 23,382,704 |
| raw SHA-256 | `37753e2b03cac5f8520518585e97166842c95921c2625afecc0ff44ee5cb7cb7` |

完整 `summary.json`、`verification.json`、`performance_report.md` 和 raw 在 Release 压缩包及本机 `data/runs/public900-BTCUSDT-20261023/` 中。精简 summary 见 `public900-20261023-summary.json`。

## 文件清单

| 文件 | 内容 |
|---|---|
| `public900-20261023-summary.json` | 主验收样本的精简 summary |
| `summary.json` | 早期 `live900-BTCUSDT-20261022-v2` 的完整汇总 |
| `verification.json` | 早期样本独立 `report` 校验 |
| `performance-report.md` | 早期样本中文性能报告 |
| `system-log.jsonl` | 早期样本结构化日志 |
| `raw-frames.sha256.txt` | 早期样本 raw 字节数、记录数、SHA-256 |
| `replay-summary.json` | 早期样本 replay 汇总 |
| `replay-parity.txt` | 实时与 replay 因子/信号逐项对比，差异 0 |
| `replay-regression.txt` | 31 秒间隔、错误哈希、非法 JSON、时间倒退回归 |
| `tsan.txt` | ThreadSanitizer 结果 |
| `benchmark-100k.json` | 100,000 事件 A/B/C 合成基准 |
| `build-info*.txt` | Windows/Linux 工具链 |
| `ctest-*.txt` | 原始 CTest 日志（保持原样） |

CTest 原始日志包含 CTest 自身的英文/本地化输出，属于未加工证据，未改写。其他说明性文档均为中文。

完整交付说明见 [`../delivery_report.md`](../delivery_report.md)，验收结论见 [`../acceptance_report.md`](../acceptance_report.md)。
