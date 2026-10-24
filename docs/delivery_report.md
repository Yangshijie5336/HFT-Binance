# 交付报告（精简版）

## 0. 要求对照

| 要求 | 状态 | 证据 |
|---|---|---|
| GitHub 公开仓库 | 已满足 | <https://github.com/Yangshijie5336/HFT-Binance> |
| 架构/接口文档同步归档 | 已满足 | [architecture.md](architecture.md)、[interfaces.md](interfaces.md) 已在 `main` |
| Docker 镜像可运行 | 已满足 | 公共镜像 `ghcr.io/yangshijie5336/marketpipe-hft-demo:v0.3.0`，UID 10001 |
| 采样原始数据 | 已满足 | Release asset `public900-BTCUSDT-20261023.zip` |
| 性能验证文档 | 已满足 | [performance.md](performance.md)、[acceptance_report.md](acceptance_report.md) |
| 结果可复现入口 | 已满足 | `scripts/run_reproducible.ps1`、CLI replay/benchmark/report、Docker |
| 精简完整报告 | 已满足 | 本文件（架构取舍、AI 协作、验证与限制） |

## 1. 项目地址与交付物

- GitHub 公开仓库：<https://github.com/Yangshijie5336/HFT-Binance>
- 默认分支：`main`
- Release：<https://github.com/Yangshijie5336/HFT-Binance/releases/tag/v0.3.0>
- 原始数据 asset：<https://github.com/Yangshijie5336/HFT-Binance/releases/download/v0.3.0/public900-BTCUSDT-20261023.zip>（5,069,750 字节，zip SHA-256 `717d7afa8cac1c673da5f36701cf99f98f99a14b7f90cc1b5922fa27dc198145`）
- 公共 Docker 镜像：`ghcr.io/yangshijie5336/marketpipe-hft-demo:v0.3.0`，公共 digest `sha256:343e091956f492d09ad75b2f707438ab6899a92dcf76d70692e5618085caab64`
- 架构设计：[architecture.md](architecture.md)
- 接口文档：[interfaces.md](interfaces.md)
- 运行手册：[runbook.md](runbook.md)
- 故障处理：[troubleshooting.md](troubleshooting.md)
- 性能方法与结果：[performance.md](performance.md)
- 验收报告：[acceptance_report.md](acceptance_report.md)
- 实验/研究材料：[research/](research/)

Release `v0.3.0` 的 zip 包含 900 秒 PASS 采样的完整 raw、派生 JSONL、日志、summary、verification 和性能报告，不把大文件直接放进 Git。该样本的 raw SHA-256 为
`37753e2b03cac5f8520518585e97166842c95921c2625afecc0ff44ee5cb7cb7`。

## 2. 系统边界与架构决策

系统是一个 C++20 单进程公开行情管线：

```
Binance WSS/TLS → Asio 协程 → 有界 raw 队列 → writer/JSONL
→ 有界 process 队列 → 解析/去重/乱序标记 → 因子 → 信号
→ summary/verification/performance report
```

选择 Boost.Beast/Asio/OpenSSL 而不是自行实现 TLS/WebSocket；选择 C++20 协程而不是每连接多线程；选择 JSONL 而不是二进制日志，是为了可审计、可回放、可直接用外部工具检查。代价是 JSON 解析、序列化和 fsync 的成本更高，因此性能文档明确区分合成 A/B/C 与真实 D，不把 JSONL 单次基准当作网络吞吐。

两个队列都有容量上限。溢出时记录错误、标记样本无效并停止，而不是静默丢弃；这会让一次采样失败，但保留“失败可解释”的边界。raw 文件使用批量 flush/fsync、最终同步和完整 SHA-256 复核，因此“耐久性”是可度量的策略，而不是口头承诺。

## 3. 辩证思考与关键取舍

- **完整性 vs 可用性**：队列满时选择 fail-fast，不丢弃后继续宣称成功。代价是短时过载可能终止采样；收益是任何成功结果都有可核查的输入计数。
- **低延迟 vs 可审计性**：保留原始 JSON 文本和十进制价格/数量，解析与因子使用接收顺序。没有为了跑分把原始数据转成 double 或二进制后丢失证据。
- **重连是否等于恢复**：不成立。每次重连递增连接代次并记录潜在缺口；`PARTIAL` 如实保留，不用“自动重连”掩盖缺口。
- **回放是否等价**：只有使用原始 `received_monotonic_ns`、帧序号、代次和连接 id 才等价；使用当前时间会让 30 秒窗口不可复现。该问题在代码审查中被发现并修复。
- **时长口径**：采样结束于停止接收，排空和 fsync 单独计时；PASS 要求实际采样严格达到目标，而不是用排空时间补足 99%。
- **乱序成交**：未重复的乱序成交保留并标记 `out_of_order`，按接收顺序进入因子；重复成交才跳过。这样既不丢合法成交，也不伪装成有序。
- **报告诚实性**：合成基准永远标记 `SYNTHETIC_BENCHMARK`；新采样遇到上游断线时记录 `PARTIAL`，没有拿历史 PASS 冒充新结果。

## 4. 与 AI 的协作方式

AI 负责：搭建 C++20/CMake 工程、实现 Beast TLS WebSocket、双有界队列、JSONL writer、解析/因子/策略、测试与文档初稿，并执行构建、CTest、Docker 与采样命令。

人类审查负责设定边界和否定错误结论：例如要求回放使用保存时间、坏日志必须失败、共享状态必须无竞态、严格时长、乱序不能直接丢弃。AI 对每个审查项补充回归测试并重新执行验证，而不是只改说明文字。

协作中实际发生并记录的问题包括：

- 首次 Windows 增量构建出现陈旧目标文件导致崩溃，改为 `--clean-first` 并记录 `VSLANG` 原因；
- 900 秒早期版本曾因 `tcp_stream` 握手超时每 10 秒断开，修复后重新采样；
- Docker Desktop 引擎一度停止，先恢复引擎再验证镜像，而不是把“Dockerfile 存在”当作镜像可用；
- 一次新公网采样遇到 11 次重连，保留为 PARTIAL，没有宣称 PASS。

## 5. 正确性与可靠性验证

| 验证 | 结果 | 证据 |
|---|---|---|
| Windows CTest | 4/4 通过（21 单元 + 15 本地 WebSocket 集成 + 100k 基准），24.56 s | [ctest-windows.txt](evidence/ctest-windows.txt) |
| Linux WSL2 CTest | 4/4 通过，21.80 s | [ctest-linux.txt](evidence/ctest-linux.txt) |
| ThreadSanitizer | 21/21 单元 + 基础采集/队列溢出场景，无竞态报告 | [tsan.txt](evidence/tsan.txt) |
| 回放完整性 | 坏哈希/非法 JSON/时间倒退返回 2；有效 raw 使用保存时间，实时/回放因子与信号差异 0 | [replay-regression.txt](evidence/replay-regression.txt)、[replay-parity.txt](evidence/replay-parity.txt) |
| Docker | 公共镜像 `ghcr.io/yangshijie5336/marketpipe-hft-demo:v0.3.0`，digest `sha256:343e091956f492d09ad75b2f707438ab6899a92dcf76d70692e5618085caab64`，UID 10001，`--help`/benchmark 通过 | 本报告第 6 节 |
| 公网 900 秒 PASS | 29,140 帧/成交，900.0712 s，0 重连/丢弃/持久化错误，raw SHA 见上 | [public900 证据](evidence/public900-20261023-summary.json) |
| 另一轮公网采样 | 55,463 帧，但因上游 TLS 中断 11 次重连、最大潜在缺口 95.96 s，如实为 PARTIAL | 本地 `data/runs/live900-BTCUSDT-20261022-v3/` |
| GitHub Actions CI | `main` 构建、CTest、100k 基准、Docker 构建成功 | <https://github.com/Yangshijie5336/HFT-Binance/actions/runs/40057231538> |
| GHCR 发布 | tag `v0.3.0` 触发的镜像发布成功，公共可拉取 | <https://github.com/Yangshijie5336/HFT-Binance/actions/runs/40057249754> |

900 秒 PASS 样本的关键指标：实际采样 **900.0712497 秒**，排空 0.0352 秒，29,140
帧/成交/信号，队列峰值 569/65,536，CPU 均值 0.311%，RSS 峰值 39,718,912 B，
吞吐 32.38 笔/秒，`verification.json` 为 `verified:true`。

## 6. Docker 复现

```bash
docker pull ghcr.io/yangshijie5336/marketpipe-hft-demo:v0.3.0
docker run --rm ghcr.io/yangshijie5336/marketpipe-hft-demo:v0.3.0 --help
docker run --rm --entrypoint id ghcr.io/yangshijie5336/marketpipe-hft-demo:v0.3.0
docker compose run --rm marketpipe benchmark --events 1000 --mode pipeline
```

已验证公共镜像输出 `uid=10001(marketpipe)`，benchmark 明确标记
`SYNTHETIC_BENCHMARK` 和 `live_network:false`。Compose 默认命令是 900 秒 capture；
短烟测必须显式覆盖命令，避免误启动长采样。

## 7. 可复现入口

离线确定性入口（不访问 Binance）：

```powershell
.\scripts\run_reproducible.ps1
```

该入口构建、运行 CTest、用 `tests/fixtures/reproducible_raw_frames.jsonl` 执行两次
replay，比较三份输出 SHA-256，并可选运行 100,000 事件基准；结果写入
`data/reproducible/summary.json`。

真实 900 秒采样与校验：

```bash
./build/marketpipe capture --symbol BTCUSDT --duration 900 --output-dir data/runs
./build/marketpipe report --run-dir data/runs/<run-id>
sha256sum data/runs/<run-id>/raw_frames.jsonl
./build/marketpipe replay --input data/runs/<run-id>/raw_frames.jsonl --output-dir data/replays/<run-id>
./build/marketpipe benchmark --events 100000 --mode all --output-dir data/benchmarks
```

大体积 raw 不提交到 Git；由 Release asset 或 CI Artifact 保存，仓库只保留小型
summary、verification、SHA 和报告。

## 8. 仍然存在的限制

- 策略是确定性标签示例，窗口内可能全部为 HOLD；不代表收益能力。
- 单主机、单时间窗口的结果不能证明长期可靠性或交易所侧完整性；重连缺口只能标记。
- GitHub Actions 运行成功不等于公网行情永远可达；公网采样仍需按运行时资格执行。
- 公共 GHCR 镜像已发布；私有网络或受限环境仍需按组织策略配置 registry 访问。
