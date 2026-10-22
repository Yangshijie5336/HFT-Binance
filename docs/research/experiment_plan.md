# 实验计划

## 0. 环境记录

记录 Git SHA、编译器、CMake/Ninja、Boost、OpenSSL、CPU、内存、磁盘、Windows/Linux、Docker 版本。所有输出放入 `docs/evidence/research/<timestamp>/`，合成与真实目录分开。

## 1. 离线确定性

目的：验证解析、因子、信号、回放和完整性。命令：

```powershell
cmake --build build-ninja
ctest --test-dir build-ninja --output-on-failure
marketpipe replay --input <raw_frames.jsonl> --output-dir <replay-dir>
```

输入：固定 Binance JSON fixtures、重复 ID、乱序 ID、错误 SHA、截断 JSON。指标：有效帧、成交、重复、乱序、因子、信号、输出 SHA-256。失败判据：结果与黄金输出不同，或坏 raw 返回 PASS。

## 2. 本地 WebSocket

目的：不依赖外网验证连接、ping/pong、断线、重连、异常帧、连接代次和优雅停止。现有集成测试使用 `cpp/tests/local_ws_server.hpp`。命令：

```powershell
ctest --test-dir build-ninja -R marketpipe_integration --output-on-failure
```

失败判据：线程无法退出、队列未排空、代次混淆、summary 未落盘或错误码为 0。

## 3. 队列和持久化故障

通过已有 fault injection 测试模拟 raw/derived 写入失败、最终 fsync 失败和队列溢出。记录 `queue_overflow_events`、`dropped_events`、`persistence_error`、退出码和完整性报告。任何静默丢失均失败。

## 4. 性能 A/B/C

使用 `benchmark --events 100000 --mode all`，至少重复五次，分别保存 parse、pipeline、persist 的 JSON。报告中位数、最小/最大和标准差，不混入网络数据。记录 events/s、P50/P95/P99、CPU、RSS、原始字节、fsync 次数和错误数。

## 5. 在线 D

前提：可合法访问 Binance Spot WebSocket、TLS CA 正常、Docker/本机时钟和磁盘可用。命令：

```powershell
marketpipe capture --symbol BTCUSDT --duration 900 --output-dir data/runs
marketpipe report --run-dir data/runs/<run-id>
```

必须保存 raw、system.log、summary、verification、performance_report。HTTP 451、DNS、TLS、连接拒绝标记 `BLOCKED`；不使用历史或合成数据补足。PASS 需要采样时长、raw 完整性、计数关系、处理链路和报告全部通过。

## 6. 长期与故障

增加 1 小时本地服务运行、周期断线、持续空闲、SIGINT/SIGTERM、磁盘写满和进程崩溃实验。每项记录故障注入时间、预期状态、实际状态、退出码和恢复边界。
