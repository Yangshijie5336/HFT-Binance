# 900 秒实时采样验收报告

## 结论

**PASS** — 对 Binance Spot `btcusdt@trade` 的真实连续 WebSocket 采集运行了 **900.0714655 秒**（严格达到 900 秒目标，排空时间另计 0.0426052 秒），32,499 个接收帧全部落盘，32,499 笔成交全部处理，raw 文件 SHA-256 通过校验；队列溢出、丢弃、持久化错误、异常帧、重复/乱序和重连均为 0。

本报告只依据实际执行产物生成。合成基准数字单独列示，绝不作为实时网络结果。

## 最新公网采样（2026-10-23）

为本次交付再次执行了 Binance Spot `BTCUSDT` 公网 WebSocket 900 秒采样，运行目录为
`data/runs/public900-BTCUSDT-20261023/`。目标时长 900 秒，实际采样
**900.0712497 秒**；接收、落盘和处理均为 **29,140** 帧/成交，重连、重复、乱序、
队列溢出和持久化错误均为 0。raw 文件大小为 23,382,704 字节，SHA-256 为
`37753e2b03cac5f8520518585e97166842c95921c2625afecc0ff44ee5cb7cb7`，独立 `report`
校验退出码为 0。精简证据已归档在
[`docs/evidence/public900-20261023-summary.json`](evidence/public900-20261023-summary.json)；
完整 raw、派生 JSONL 和日志保留在本机采样目录，不提交到 Git。

## 审查问题修复验证

本次验收针对审查提交 `d4cdaf1` 的 5 个问题：

| 问题 | 修复 | 证据 |
|---|---|---|
| 回放使用当前时间 | replay 读取原始 `received_monotonic_ns`、帧序号、代次、连接 id | 两笔间隔 31 秒的成交 replay 后第二笔 `window_trades=1`、`trade_rate_10s=0.1`；实时/回放业务输出逐项对比 0 差异 |
| 坏日志仍 PASS | 回放先校验 envelope、逐帧 SHA-256、字节数、序号；零有效成交/损坏输入返回 `2`/`FAIL` | `{broken` 与错误哈希均返回 2；回归单测覆盖 |
| 跨线程数据竞争 | `finished`、溢出标志/计数、丢弃计数、持久化错误改为原子对象 | ThreadSanitizer 下 21/21 单元、基础采集 1/1、队列溢出 1/1，无竞态报告 |
| 时长包含排空且允许 99% | 停止接收时固定采样结束时间；排空单独记录；PASS 严格 `duration_actual >= target` | 本次 900.0715 ≥ 900；提前停止测试中 `duration_actual < 1s`、`drain > duration`，结果 FAIL |
| 乱序成交被丢弃 | 未重复的乱序成交保留并标记 `out_of_order=true`，按接收顺序进入因子/信号 | 集成测试 5 帧中 1 重复、1 乱序，处理 4 笔，输出 4 行；replay 同样处理并标记 |

## 运行标识与环境

| 项目 | 值 |
|---|---|
| Git 审查基线 | `d4cdaf1`（本修复在其上增量开发） |
| Run id | `live900-BTCUSDT-20261022-v2` |
| 运行目录 | `data/runs/live900-BTCUSDT-20261022-v2`（不提交） |
| 端点 | `wss://stream.binance.com:9443/ws/btcusdt@trade` |
| 交易对 | `BTCUSDT` |
| TLS | TLSv1.3，`TLS_AES_128_GCM_SHA256`，对端与主机名校验 |
| 主机 | Windows 10/11 x64 工作站（非托管） |
| 编译器 | MSVC 19.51.36260，C++20，Release |
| 构建环境 | CMake 4.3.1、Ninja 1.13.2、Boost 1.91、OpenSSL 3.6.2 |

| 字段 | UTC |
|---|---|
| 开始 | `2026-10-22T12:01:29.455417400Z` |
| 结束 | `2026-10-22T12:16:30.005366800Z` |
| 目标采样时长 | 900 秒 |
| 实际采样时长 | **900.0714655 秒** |
| 排空/最终确定时长 | **0.0426052 秒**（不计入采样） |
| 总处理墙钟时长 | 900.1140707 秒 |

## 采集结果

| 指标 | 数值 |
|---|---:|
| 接收 WebSocket 帧 | 32,499 |
| 落盘 raw 帧 | 32,499 |
| raw 字节 | 26,113,033 |
| 看到成交事件 | 32,499 |
| 已处理唯一成交 | 32,499 |
| 发出信号 | 32,499 |
| 信号：BUY / SELL / HOLD | 0 / 0 / 32,499 |
| 重复成交 | 0 |
| 乱序成交 | 0 |
| 异常帧 | 0 |
| 服务端错误帧 | 0 |
| 断开 | 0 |
| 重连 | 0 |
| 连接代次 | 1 |
| 队列峰值 / 容量 | 485 / 65,536 |
| 队列溢出事件 | 0 |
| 丢弃事件 | 0 |
| 持久化错误 | 0 |
| 处理失败 | 0 |
| 吞吐 | 36.11 笔/秒 |
| raw 磁盘写入速率 | 29,012 字节/秒 |
| 每分钟平均/峰值唯一成交 | 2,166 / 4,218 |
| 每秒峰值唯一成交 | 1,166 |
| 进程 CPU 均值 | 0.418 % |
| RSS 峰值 | 41,607,168 字节（39.7 MiB） |

`summary.json` 中全部 11 项质量门禁为 true，包括 `duration_meets_target`、零队列溢出、零持久化错误、raw SHA-256 校验和 TLS 校验。

### 实时窗口为何只有 HOLD

策略要求同时满足 `|imbalance| ≥ 0.15` 与 `|return_n| ≥ 1e-4`。本窗口 21 笔滚动收益范围为 **-5.136e-05 至 +7.533e-05**，低于 1e-4 动量门限；成交流不平衡度为 -0.972 至 1.0。因此确定性规则对全部 32,499 笔成交输出 `HOLD`。没有为制造 BUY/SELL 而修改阈值；三种动作均由单元测试覆盖。窗口价格从 81,812.01 变为 81,804.01。

## 延迟（本地接收→处理，非交易所间延迟）

样本 6,499 条（每 5 笔成交采样一次），单位微秒。

| 阶段 | P50 | P95 | P99 | 最大 |
|---|---:|---:|---:|---:|
| 队列等待 | 875.3 | 5,448.1 | 13,054.0 | 40,775.0 |
| 接收→处理 | 1,086.0 | 20,050.0 | 31,220.2 | 66,092.9 |
| 接收→信号 | 1,085.9 | 20,049.9 | 31,220.1 | 66,092.7 |

P95/P99 尾部包含本地磁盘 fsync、调度和本机负载影响。这些不是交易所到交易所延迟。

## 完整性证据

外部 `Get-FileHash`、采集器增量哈希和重读哈希一致：

```
raw_frames.jsonl
  记录数: 32499
  字节数: 26113033
  sha256: 7495d8d0fdc92840ee0ccacb1646e9a4a0467ede175781400cee820be66a5301
```

`verification.json` 独立通过：`summary_result_pass`、`raw_frames_exist`、`raw_sha256_matches_summary`、`raw_records_match_summary`、`raw_payload_hashes_valid`、`collector_raw_integrity_verified`、`artifact_hashes_match`。

Artifact 哈希（也写入 `summary.json`）：

| 文件 | SHA-256 |
|---|---|
| `raw_frames.jsonl` | `7495d8d0fdc92840ee0ccacb1646e9a4a0467ede175781400cee820be66a5301` |
| `normalized_events.jsonl` | `83c115f3e1480d1af2870dbaf6b97fd7153dc6438d9880e2c4fac6c044b495a3` |
| `factors.jsonl` | `6d2651b84f1e13ed822b4ad3d5791382b826ef9843e3b44e9b4c172f66c63b1f` |
| `signals.jsonl` | `5d5b99e664a39384bdcbb810acb85f0b0dd2e85218e48762c5b14a92392c21f8` |
| `system.log` | `ab7273a1446bf80948420deb98b639f45e71d8bbab616c92073b24d2ff2e7765` |

派生文件大小：`normalized_events.jsonl` 19,343,047 B；`factors.jsonl` 12,830,766 B；`signals.jsonl` 7,504,843 B；`system.log` 3,352 B。

完整 raw 与派生文件不提交，保留在 `data/runs/live900-BTCUSDT-20261022-v2/`，手工 live-capture CI 工作流会作为 Artifact 上传。小型证据副本位于 `docs/evidence/`。

## Replay 与业务输出一致性

对 900 秒 raw 执行 replay：

```json
{"result":"PASS","replay":true,"records_read":32499,"frames_validated":32499,
 "integrity_errors":0,"trades_processed":32499,"duplicates":0,
 "out_of_order":0,"malformed_frames":0,"exit_code":0,"execution_enabled":false}
```

逐项比较实时与 replay 的 32,499 条因子（`trade_id`、`trade_imbalance`、`return_n`、`trade_rate_10s`、`avg_trade_size`、`window_trades`、`received_ns`）以及 32,499 条信号（`trade_id`、`action`、`score`、`price`）：**差异均为 0**。证据见 [`docs/evidence/replay-parity.txt`](evidence/replay-parity.txt)。

31 秒间隔、错误哈希和非法 JSON 的回放回归输出见 [`docs/evidence/replay-regression.txt`](evidence/replay-regression.txt)。

## A/B/C 基准（合成，不是实时吞吐）

最终二进制，每模式 100,000 事件，`SYNTHETIC_BENCHMARK`，`live_network:false`：

| 模式 | events/s | P50 µs | P95 µs | P99 µs | CPU % | RSS B | 错误 |
|---|---:|---:|---:|---:|---:|---:|---:|
| A. 纯解析 | 165,667 | 3.6 | 5.4 | 7.5 | 100.9 | 25,485,312 | 0 |
| B. 完整链路（去重+因子+策略） | 136,562 | 6.5 | 9.6 | 14.9 | 99.7 | 26,124,288 | 0 |
| C. B + raw JSONL 持久化 | 42,148.8 | 14.1 | 392.4 | 693.3 | 84.1 | 27,037,696 | 0 |

模式 C 写入 100,000 个 raw 帧（51,363,504 字节），SHA-256 为 `1c4193c1a80dbb7dbea003e450979dc83e225251c26d8445596d6f1393143ad8`。A/B 产生 99,991 个合成非 HOLD 标签；A 按定义只做解析。CPU 可超过 100%，因为它是多核累计 CPU 时间除以墙钟时间。

## 测试

`ctest --test-dir build-ninja --output-on-failure`：

```
1/4 marketpipe_help ............... Passed
2/4 marketpipe_unit ............... Passed   （21 个用例）
3/4 marketpipe_integration ........ Passed   （15 个本地回环 WebSocket 场景）
4/4 marketpipe_benchmark_100k ..... Passed
100% tests passed, 0 tests failed out of 4
Total Test time (real) = 24.56 sec
```

集成套件覆盖：正常流、异常帧、突然断开与重连代次隔离、重复/乱序成交、有界队列溢出、SIGINT/SIGTERM 排空与最终确定、提前停止不能靠排空补足时长、HTTP 451、模拟磁盘满、最终 fsync 失败、被篡改 raw 文件检测、replay 完整性与一致性、连接拒绝。所有测试都不访问 Binance。原始 CTest 输出见 [`docs/evidence/ctest-windows.txt`](evidence/ctest-windows.txt)。

### Linux 交叉验证（WSL2 Ubuntu 22.04）

同一提交使用 GCC 11.4 / CMake 3.22.1 / Ninja 1.10.1 / Boost 1.74 / OpenSSL 3.0.2 构建，CTest 4/4 通过（21 单元 + 15 集成 + 100k 基准），耗时 21.80 秒。证据见 [`docs/evidence/ctest-linux.txt`](evidence/ctest-linux.txt)。

### ThreadSanitizer

在 `-fsanitize=thread` 构建下，单元测试 21/21、基础采集 1/1、队列溢出 1/1 通过，无竞态报告。证据见 [`docs/evidence/tsan.txt`](evidence/tsan.txt)。

## Docker 与 CI 状态

- 已实现多阶段 Dockerfile、非 root UID 10001、命名卷 `marketpipe-data`、构建阶段 CTest 和 `STOPSIGNAL SIGTERM`。
- Windows Docker Desktop 已实际恢复并验证：`docker build -t marketpipe-hft-demo:0.3.0 .` 成功，镜像摘要为 `sha256:07b215d6e2824b543afaa3bf2cae9374ef6dd9b6f88780101d4015f11b7ca947`；默认帮助命令通过，UID 10001 执行 1,000 事件 pipeline benchmark 通过，输出明确标记 `SYNTHETIC_BENCHMARK`/`live_network:false`。
- Compose 默认命令是 900 秒 `capture`；运行短暂容器冒烟时应显式覆盖命令，例如 `docker compose run --rm marketpipe benchmark --events 1000 --mode pipeline`。本次 compose 冒烟采样已在确认后优雅停止，不计入公网 900 秒验收结果。
- CI 使用的 Linux 构建/CTest 路径已在 WSL2 实际执行并通过 4/4。
- 提交已推送到 `origin/main`，因此 GitHub Actions 工作流由推送触发。仓库的 Actions 页面/API 需要认证，本环境没有已登录的 `gh` 或可用令牌，因此**不声称任何 Actions Run URL、状态或产物**。CI 证据必须从推送后的实际 Actions 运行中读取。

## 复现

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

./build/marketpipe capture --symbol BTCUSDT --duration 900 \
  --output-dir data/runs --run-id live900-BTCUSDT-20261022-v2
./build/marketpipe report --run-dir data/runs/live900-BTCUSDT-20261022-v2
sha256sum data/runs/live900-BTCUSDT-20261022-v2/raw_frames.jsonl
./build/marketpipe replay \
  --input data/runs/live900-BTCUSDT-20261022-v2/raw_frames.jsonl \
  --output-dir data/replays/live900-v2
./build/marketpipe benchmark --events 100000 --mode all --output-dir data/benchmarks
```

## 限制

这是单台非托管 Windows 工作站上的一个 15 分钟观察窗口。它不能证明长期可靠性、交易所侧完整性、托管级低延迟或盈利能力。策略是确定性的标签示例，执行始终禁用。回放可复现业务输出，但不能恢复交易所侧未收到的数据。
