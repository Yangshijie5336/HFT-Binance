# 验收证据目录

本目录只保存小型、可提交的验收证据；大型 raw 与派生文件保留在 `data/runs/live900-BTCUSDT-20261022/` 或 CI 产物 中，不进入 Git。

| 文件 | 内容 |
|---|---|
| `summary.json` | 900 秒真实采样的完整汇总：结果、计数、延迟、CPU/RSS、质量门禁、产物哈希 |
| `verification.json` | 独立 `report` 重算的 raw SHA-256、记录数与逐帧 payload 校验 |
| `performance-report.md` | 由采集器生成的 900 秒性能与验收报告（中文） |
| `system-log.jsonl` | 900 秒运行的结构化 JSONL 日志副本 |
| `raw-frames.sha256.txt` | 原始文件字节数、记录数、SHA-256 与外部校验时间 |
| `replay-summary.json` | 对 900 秒 raw 文件执行 replay 的汇总 |
| `benchmark-100k.json` | 100,000 事件 A/B/C 合成基准原始输出 |
| `build-info.txt` | Windows 构建工具链信息 |
| `build-info-linux.txt` | WSL2 Ubuntu 构建工具链信息 |
| `ctest-windows.txt` | Windows CTest 原始日志（保持原样） |
| `ctest-linux.txt` | Linux WSL2 CTest 原始日志（保持原样） |

CTest 原始日志中可能包含 CTest 自身的英文/本地化输出，这些是未加工的原始证据，未做改写。其他说明性文档均为中文。

SHA-256：

```
5836675014897976d47654e8d0fde5f765f7ff129d0ecba9c413d5c5a82919b8  raw_frames.jsonl
```

完整验收结论见 [`../acceptance_report.md`](../acceptance_report.md)。
