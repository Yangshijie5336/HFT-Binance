# HFT-Binance 工程研究报告

## 研究范围与结论

本报告基于本地仓库提交 `ff30af9` 审计，未把历史 README 描述直接当作事实。工作区无未提交改动。当前系统已经是 C++20、CMake、Boost.Beast/Asio、OpenSSL、CTest 的实时行情管线，包含本地 WebSocket 集成测试、重连、TLS 校验、双有界队列、JSONL 原始帧、SHA-256、回放和基准入口。

当前最重要的结论是：离线和本地网络路径已经具备可审计证据；真实 Binance 900 秒采样仍取决于外网资格和运行环境，不能由本地测试替代。当前公开证据支持的在线状态应继续标记为 `NOT_EXECUTED` 或 `BLOCKED`，直到产生新的真实运行目录。

## 官方协议事实

Binance 官方 Spot WebSocket 文档说明，原始成交流使用 `wss://stream.binance.com:9443/ws/<symbol>@trade`，成交事件包含 `e/E/s/t/p/q/T/m/M`；价格和数量字段以字符串提供，`m` 表示买方是否为做市方。来源：[Binance Spot WebSocket Streams](https://developers.binance.com/en/docs/catalog/core-trading-spot-trading/api/ws-streams/~)。当前解析器保留十进制文本，并把 `m=true` 映射为主动卖出，这是与协议字段语义一致的实现选择。

官方文档没有为客户端提供“断线自动补齐全部成交”的保证。重连后的缺口只能标记为未知；若要研究补查，应独立设计 REST 成交查询、ID 区间对账和重复合并实验，不能把 WebSocket 重连本身视为恢复证明。

## 当前调用链

`main.cpp` 解析 CLI → `run_capture()` 初始化目录、Logger、Raw/Derived writer、资源采样器和两个有界队列 → `run_network()` 使用 Boost.Asio 协程管理 DNS、TCP、TLS、WebSocket 会话 → `websocket_session()` 将完整文本帧封装为 `RawFrame` 并进入 raw 队列 → writer 线程原始 JSONL 写入、批量 flush/fsync，再把帧转入 process 队列 → processor 线程调用 `parse_binance_message()`、`TradeDeduplicator::classify()`、`FactorEngine::update()`、`evaluate()`，写出 normalized/factors/signals → 主线程关闭队列、等待排空、验证 raw 文件、生成 `summary.json` 和报告。

核心实现位于 `cpp/src/capture.cpp`、`cpp/src/core.cpp`、`cpp/src/runtime.cpp`、`cpp/src/report.cpp`。测试位于 `cpp/tests/test_unit.cpp` 和 `cpp/tests/test_integration.cpp`。

## 并发和内存判断

当前选择是单网络事件循环、一个 raw writer 线程、一个 processor 线程和资源采样线程。两个队列有容量上限且使用 `try_push`；满时计数、记录日志、停止采样并让结果失败。这个设计已满足可审计的所有权边界：网络线程拥有 socket 和 raw 队列生产权，writer 拥有原始文件，processor 拥有因子、去重和派生文件。

当前没有证据证明需要无锁队列。Boost.Asio 的异步模型适合网络层；队列容量和锁竞争应通过实验测量后再决定是否引入 SPSC ring buffer。队列容量的第一阶估算为 `峰值输入速率 × 最长消费者暂停时间 + 安全余量`，但安全余量必须在压力实验中验证。

来源：[Boost.Asio](https://www.boost.org/doc/libs/latest/doc/html/boost_asio.html) 将异步网络和低层 I/O 作为统一模型；[Boost.Beast](https://www.boost.org/doc/libs/latest/libs/beast/doc/html/index.html) 提供 WebSocket 连接、控制帧、超时和 TLS 分层接口。当前工程已通过本地 WebSocket 集成测试验证主要行为。

## 持久化和完整性

原始帧经历的状态应分开理解：网络读到内存、进入 raw 队列、writer 写入文件、flush 到 OS 文件缓存、fsync 完成、最终读取并校验 SHA-256。当前 raw writer 默认每 100 条 fsync，并在结束时最终同步；这是“批量耐久性”策略，不等同于每帧稳定存储。文件级 SHA-256 能证明最终文件内容与重算结果一致，不能证明断电前所有 OS 缓存都已落盘，也不能证明交易所到客户端没有网络丢失。

当前回放在第一遍验证 envelope、payload SHA-256、payload_bytes、frame_seq、连接代次和接收时钟，在第二遍使用保存的接收时间驱动因子。这是确定性回放的正确边界。仍需在真实运行中验证大文件轮转、磁盘写满和进程崩溃后的最后记录处理。

## 因子与策略

因子使用接收顺序推进 30 秒成交窗口、10 秒成交速率、最近 20 笔收益率、主动买卖量不平衡和平均成交规模。乱序但非重复的成交被标记并继续进入链路，重复成交被跳过。策略只生成 `BUY_SIGNAL`、`SELL_SIGNAL`、`HOLD`，没有订单接口。该实现强调可回放和可解释性，不代表交易收益能力。

## 性能解释

当前基准明确标记为 `SYNTHETIC_BENCHMARK`，不是网络吞吐量。已有 100,000 事件证据显示 parse、full pipeline 和 pipeline plus raw persistence 三种路径的吞吐不同；持久化路径明显受 fsync 和文件系统影响。基准代码使用合成 JSON、单次运行和采样分位数，适合容量回归，不能替代五次重复实验或真实 WebSocket 测量。

最可能的瓶颈按路径分别是 JSON DOM 解析、SHA-256/JSONL 序列化、fsync、跨线程队列等待和 TLS/网络抖动。需要用五次重复、同一机器、同一构建类型和独立输出目录确认，不能从单次最好成绩推断优化收益。

## 风险与未验证假设

- 真实 Binance 连接、HTTP 451/网络限制和 900 秒连续采样尚未在本次研究中重新执行，状态为 `NOT_EXECUTED`。
- 断线期间的交易所数据缺口无法仅凭本地序号证明不存在。
- JSONL 适合审计和回放，但不是最低延迟日志格式；二进制日志和文件轮转仍待实验。
- Windows 和 Linux 的 CA bundle、OpenSSL DLL、时钟与文件系统语义不同，需分别保留证据。
- 当前策略和因子是验证链路的模型，不是生产交易策略。

## 研究选择

保留 Boost.Beast/Asio + OpenSSL、双有界队列和 JSONL 原始日志作为下一阶段基线。只有当实验显示锁竞争或 JSONL/哈希开销成为目标瓶颈时，才考虑 SPSC ring、对象池、批量二进制日志或专门的 SIMD JSON 解析。此选择符合 C++ Core Guidelines 对资源管理、并发边界和错误传播的要求；指南是持续更新的参考，而非本项目已验证结果。[C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
