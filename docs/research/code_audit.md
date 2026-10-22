# 代码审计

| 模块 | 实际职责 | 证据 | 当前判断 |
|---|---|---|---|
| `cpp/src/main.cpp` | 解析 capture/replay/benchmark/report 参数和退出码 | `main()` | 已接通 |
| `cpp/src/capture.cpp` | Asio 协程、TLS、WebSocket、重连、队列、采集汇总 | `run_capture()`, `network_loop()` | 已接通，本地集成可测 |
| `cpp/src/core.cpp` | JSON 成交解析、去重、因子、策略、replay | `parse_binance_message()`, `FactorEngine::update()`, `replay()` | 已接通 |
| `cpp/src/runtime.cpp` | 有界队列、JSONL writer、fsync、SHA-256、日志、资源指标 | `BoundedQueue`, `JsonlFileWriter` | 已接通 |
| `cpp/src/report.cpp` | 重算产物、raw integrity、performance report | `run_report()` | 已接通 |
| `cpp/src/benchmark.cpp` | 合成 parse/pipeline/persist 三路径 | `run_benchmark()` | 已明确标记 synthetic |
| `cpp/tests/test_unit.cpp` | JSON、解析、因子、队列、writer、回放完整性 | 28 个测试宏附近 | 已通过 |
| `cpp/tests/test_integration.cpp` | 本地 WebSocket、重连、故障注入、停止、回放 | 16 个测试宏附近 | 已通过 |

## 处理链路

网络接收的帧先构造成 `RawFrame`，写入 raw queue。writer 写 raw JSONL 后才交给 process queue。processor 使用 `parse_binance_message` 生成 `Trade`，去重器把相同 symbol+trade ID 判为重复，把较小数值 ID 标记为乱序；乱序仍进入因子但带标志。`FactorEngine` 使用接收时间窗口，`evaluate` 产生模拟信号。

## 已验证事实

- 当前 HEAD 为 `ff30af9`，工作区与 origin/main 一致。
- CMake 构建成功，CTest 4/4 通过。
- 本地 WebSocket 集成测试覆盖断线、重连、HTTP 451、队列溢出、磁盘故障和信号停止。
- 回放先做 SHA-256、字节数、帧序号、连接代次和时间检查；坏输入非零失败。
- benchmark 输出显式包含 `SYNTHETIC_BENCHMARK` 和 `live_network=false`。
- Docker Desktop 已运行，镜像构建和 Compose help/benchmark 烟测已执行。

## 未验证或受限事实

- 本次研究没有重新执行 900 秒 Binance 在线采样，因此不报告实时 PASS。
- 合成 benchmark 不代表网络吞吐量。
- 本地 WebSocket 不证明公网 DNS、TLS、HTTP 451 或交易所侧无缺口。
- 未执行 perf/WPA 采样，热点判断仍是待验证假设。

## 风险

1. 网络断线期间缺口只能观测，不能由本地序号补齐。
2. JSON DOM、SHA-256、fsync 和 JSONL 序列化可能主导持久化路径延迟。
3. Windows/Linux CA、OpenSSL 和文件同步路径需要分别留证。
4. 当前因子和策略只用于链路验证。
