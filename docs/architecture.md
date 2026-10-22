# 架构与线程安全设计

## 范围

这是一个公开行情采集与信号标注的工程示例。它刻意不包含认证交易、订单管理、持仓或 PnL 逻辑。唯一的交易所写操作是协议层的 WebSocket 关闭帧。

## 组件

| 组件 | 源文件 | 职责 |
|---|---|---|
| JSON DOM | `cpp/src/json.cpp` | 轻量解析 WebSocket 文本；数字保留精确原文 |
| 核心 | `cpp/src/core.cpp` | Binance 成交解析、去重原语、30 秒因子、确定性策略、replay |
| 运行时 | `cpp/src/runtime.cpp` | 有界队列、持久化 JSONL 写入器、SHA-256、JSON 日志、CPU/RSS 采样、延迟分位 |
| 采集器 | `cpp/src/capture.cpp` | TLS WebSocket 客户端、重连、队列接线、持久化、summary |
| 报告 | `cpp/src/report.cpp` | 重算哈希/计数、校验完整性、生成验收报告 |
| 基准 | `cpp/src/benchmark.cpp` | 合成解析 / 处理链路 / 持久化吞吐 |
| CLI | `cpp/src/main.cpp` | 参数解析与退出码 |

## 数据通路

```
socket 读取（网络线程）
  → RawFrame {seq, generation, connection_id, received_utc, received_steady_ns, payload}
  → raw_queue（有界，非阻塞 try_push）
  → writer 线程：
        raw_frames.jsonl（批量 flush；每 N 条 fsync，最终再 fsync）
        → process_queue（有界，非阻塞 try_push）
  → processor 线程：
        解析 → 校验 → 去重 → 乱序检查
        → FactorEngine（30 秒接收时间窗、10 秒成交速率、20 笔收益）
        → evaluate() → BUY_SIGNAL / SELL_SIGNAL / HOLD
        → normalized_events.jsonl、factors.jsonl、signals.jsonl
```

writer 在关闭处理队列前先排空 raw 队列，因此优雅停止不会丢失已被网络循环接收的帧。processor 在最终确定派生文件前排空处理队列。

## 线程

| 线程 | 拥有 | 说明 |
|---|---|---|
| 主线程 / 网络线程 | Boost.Asio `io_context`、TLS 流、raw 队列生产者 | C++20 协程；同一时间只有一个活动会话 |
| writer | `raw_frames.jsonl`、process 队列生产者 | 每 250 ms 批量 flush；每 N 条 fsync |
| processor | 派生 JSONL 文件、因子、策略、去重 | 结构上单线程 |
| 资源采样 | CPU/RSS 向量 | 每秒一次 |
| 停止监视 | 投递 socket 取消操作 | 每 50 ms 唤醒；把取消投递到 I/O 线程 |

共享可变状态仅限于：

- `std::atomic<bool> fatal`（writer/processor → 网络）
- 原子停止标志 `finished`（主线程 → 停止监视线程）
- 原子队列溢出标志/计数、丢弃计数和持久化错误标志（网络、writer、processor → 主线程）
- `CaptureSummary` 中按组件划分的字段：每个字段只由一个线程写，join 后读取
- logger 的互斥锁（所有线程）
- 有界队列的互斥锁/条件变量

网络线程不触碰派生 writer；processor 不触碰 raw writer；writer 不计算因子。除入队和时间戳外，I/O 与 CPU 工作都不在 socket 读取路径上。

## 取消与信号处理

`SIGINT`/`SIGTERM` 处理器只设置 `volatile sig_atomic_t` 标志（异步信号安全）。停止监视器在 50 ms 内发现该标志，并向 `io_context` 投递 socket 取消闭包。因此 socket 只会在其所属线程上被取消。会话返回 `Stopped`，重连循环退出，队列关闭并排空，文件最终确定，进程在退出码 130 前写好 `summary.json`。

## 背压与丢失检测

- 两个队列都有容量上限，并使用 `try_push`。
- raw 队列溢出：递增 `queue_overflow_events` / `dropped_events`，置 `queue_overflow=true`，记录 `raw_queue_overflow`，将运行标记为无效并停止。
- process 队列溢出：相同策略，分别计数。
- 采集器不会无限阻塞 socket 读取，也绝不静默丢弃。样本结果变为 `FAIL`；`quality_checks.zero_queue_overflow` 为 false。
- 内存去重表同样有界（默认 200,000 条，LRU 式淘汰）。

## 重连与代次语义

- 每次连接尝试获得单调递增的 `connection_generation`。
- 每个原始帧记录其连接代次和连接 id。
- 去重是跨重连全局的：重连后重复出现的成交会被计为 duplicate 并跳过。
- 未重复但成交 ID 小于当前最大 ID 的成交标记为 `out_of_order=true`，**仍进入**因子、信号与派生输出；因子按到达（接收）顺序处理，而不是丢弃或按交易所时间重排。该规则保证实时与 replay 一致。
- 重连时采集器记录 `GapWindow`，包含近似缺口时长、最后帧时间与重连 UTC 时间，并明确记录重连不能证明交易所行情无缺失。
- 指数退避从 `--reconnect-initial`（默认 1 秒）开始，每次连续失败翻倍，上限 `--reconnect-max`（默认 15 秒）。
- 连续 10 次连接失败会带诊断中止运行（可配置）。
- HTTP 451 被视为地域访问限制：标记 `BLOCKED`，不进行重试风暴，不规避限制。

## TLS

- `wss://` 使用 Boost.Beast over `boost::asio::ssl::stream`。
- 强制 `verify_peer` 与 `host_name_verification(host)`；显式设置 SNI。
- 信任来源依次为：`--ca-file`、`MARKETPIPE_CA_FILE`、`SSL_CERT_FILE`、`<cwd>/certs/cacert.pem`、可执行文件旁路径、Linux 系统 bundle、OpenSSL 默认路径；Windows Root 存储可访问时也会导入。
- 明文 `ws://` 仅允许回环地址，且只用于本地集成测试服务。非回环明文在连接前即拒绝。
- TLS 握手错误会带 OpenSSL 错误记录，并作为致命配置/信任失败终止运行，而不是无限重试。

## 时长与排空

- 采样起点是采集初始化完成、网络循环开始前的单调时钟时刻。
- 采样终点是网络循环返回（停止接收/到达 deadline）时的单调时钟时刻，随后才关闭队列、join 工作线程、fsync 和生成汇总。
- `duration_actual_seconds` 只包含采样区间；`drain_elapsed_seconds` 单独记录排空与最终确定耗时；`processing_elapsed_seconds` 是总墙钟耗时。
- PASS 要求 `duration_actual_seconds >= duration_target_seconds`，不再使用 99% 容差；排空耗时不能补偿提前停止。

## 持久化与完整性

- raw writer 保存精确帧文本（仅 JSON 转义）、payload 字节数和每帧 payload SHA-256。
- 至少每 250 ms flush 一次，每 N 条（默认 100）fsync，最终再 fsync。错误是粘性的，并上报到 `summary.json`。
- 最终确定后，采集器重新读取 `raw_frames.jsonl`，重算文件 SHA-256，并校验每条记录的序号、代次顺序、payload 哈希和字节数。不匹配即标记 FAIL。
- `report --run-dir` 独立重复这些检查，并写入 `verification.json`。
- `summary.json` 通过临时文件 + fsync + rename 写入。

## PASS 判定

只有以下条件全部满足才是 `PASS`：

1. 成功建立真实非回环 WebSocket 连接（`wss://` 通过 TLS 校验）；
2. 实测采样时长 ≥ 目标时长（严格比较，不含排空时间）；
3. 至少处理一个原始帧和一笔唯一成交；
4. 原始记录数等于接收帧数；
5. raw 文件 SHA-256 与增量 writer 摘要及 summary 一致；
6. 队列溢出、丢弃、持久化/处理失败均为零；
7. 断开、异常帧、重复和乱序均为零。

出现重连、异常帧、重复或乱序则为 `PARTIAL`。HTTP 451 为 `BLOCKED`。其余为 `FAIL`。

## Replay 完整性

`replay` 必须先验证完整 raw envelope：

- 每行必须是合法 JSON 对象；
- 必须包含 `payload`、`payload_sha256`、`frame_seq`、`connection_generation`、接收单调时间和 `received_utc`；
- 逐帧重算 payload SHA-256，并校验可选 `payload_bytes`；
- `frame_seq` 严格递增、连接代次不回退。

任何完整性错误、零有效成交或 malformed payload 都返回非零退出码并写 `result=FAIL`，不会跳过坏记录后错误地输出 PASS。通过校验后，replay 使用原始 `received_monotonic_ns`、帧序号、代次和连接 id 驱动因子与策略，因此窗口和信号可复现。重复成交跳过，乱序成交保留并标记。

## 已知限制

这是一个单主机工程示例，不是托管在交易所机房的 HFT 引擎。延迟包含操作系统网络栈、TLS、调度和本地磁盘。它不能证明长期生产可靠性、交易所侧完整性或盈利能力。15 分钟样本只描述一台主机、一个行情源和一个观察窗口。
