# 900 秒实时采样验收报告

## 结论

**PASS** — 对 Binance Spot `btcusdt@trade` 的真实连续 WebSocket 采集运行了 900.1129 秒，24,903 个接收帧全部落盘，24,903 笔成交全部处理，raw 文件 SHA-256 通过校验；队列溢出、丢弃、持久化错误、异常帧、重复/乱序和重连均为 0。

本报告只依据实际执行产物生成。合成基准数字单独列示，绝不作为实时网络结果。

## 运行标识与环境

| 项目 | 值 |
|---|---|
| Git 基线提交 | `76474cf`（本次变更在其上增量开发） |
| 运行 ID | `live900-BTCUSDT-20261022` |
| 运行目录 | `data/runs/live900-BTCUSDT-20261022`（不提交） |
| 端点 | `wss://stream.binance.com:9443/ws/btcusdt@trade` |
| 交易对 | `BTCUSDT` |
| TLS | TLSv1.3，`TLS_AES_128_GCM_SHA256`，对端与主机名校验 |
| 主机 | Windows 10/11 x64 工作站（非托管） |
| 编译器 | MSVC 19.51.36260，C++20，Release |
| 构建环境 | CMake 4.3.1、Ninja 1.13.2、Boost 1.91、OpenSSL 3.6.2 |

起止时间为采集器实测墙钟时间：

| 字段 | UTC |
|---|---|
| 开始 | `2026-10-22T04:39:34.155114400Z` |
| 结束 | `2026-10-22T04:54:34.608986900Z` |
| 目标时长 | 900 秒 |
| 实际采集时长 | **900.1129 秒** |
| 处理/排空时长 | 900.4539 秒 |

## 采集结果

| 指标 | 数值 |
|---|---:|
| 接收 WebSocket 帧 | 24,903 |
| 落盘 raw 帧 | 24,903 |
| raw 字节 | 19,881,198 |
| 看到成交事件 | 24,903 |
| 已处理唯一成交 | 24,903 |
| 发出信号 | 24,903 |
| 信号：BUY / SELL / HOLD | 0 / 0 / 24,903 |
| 重复成交 | 0 |
| 乱序成交 | 0 |
| 异常帧 | 0 |
| 服务端错误帧 | 0 |
| 断开 | 0 |
| 重连 | 0 |
| 连接代次 | 1 |
| 队列峰值 / 容量 | 292 / 65,536 |
| 队列溢出事件 | 0 |
| 丢弃事件 | 0 |
| 持久化错误 | 0 |
| 处理失败 | 0 |
| 吞吐 | 27.67 笔/秒 |
| raw 磁盘写入速率 | 22,087 字节/秒 |
| 每分钟平均/峰值唯一成交 | 1,660 / 3,072 |
| 每秒峰值唯一成交 | 462 |
| 进程 CPU 均值 | 0.309 % |
| RSS 峰值 | 36,896,768 B（35.2 MiB） |

`summary.json` 中全部 11 项质量门禁为 true：时长 ≥ 99%、至少一笔成交、raw 记录数匹配、零溢出、零持久化错误、raw SHA-256 校验通过、零处理失败、无断开、无异常帧、无重复/乱序、TLS 校验通过。

### 实时窗口为何只有 HOLD

策略要求同时满足 `|imbalance| ≥ 0.15` 与 `|return_n| ≥ 1e-4`。本 15 分钟窗口的 21 笔滚动收益范围是 **-7.346e-05 到 +8.081e-05**，低于 1e-4 动量门限；成交流不平衡度范围为 -1.0 到 +0.615。因此确定性规则对全部 24,903 笔成交输出 `HOLD`。没有伪造标签来强行产生 BUY/SELL；三种动作均由单元测试覆盖。

## 延迟（本地接收→处理，非交易所间延迟）

样本 4,980 条（每 5 笔成交采样一次），单位微秒。

| 阶段 | P50 | P95 | P99 | 最大 |
|---|---:|---:|---:|---:|
| 队列等待 | 565.6 | 3,118.2 | 6,201.0 | 11,073.4 |
| 接收→处理 | 731.9 | 19,401.1 | 22,655.5 | 52,955.0 |
| 接收→信号 | 731.8 | 19,401.1 | 22,655.4 | 52,954.9 |

P95/P99 尾部包含本地磁盘 fsync 和调度影响。这些不是交易所到交易所延迟。

## 完整性证据

外部 `Get-FileHash`、采集器增量哈希和重读哈希一致：

```
raw_frames.jsonl
  记录数: 24903
  字节数: 19881198
  sha256: 5836675014897976d47654e8d0fde5f765f7ff129d0ecba9c413d5c5a82919b8
```

`verification.json` 独立通过：

- `summary_result_pass`、`raw_frames_exist`
- `raw_sha256_matches_summary`
- `raw_records_match_summary`
- `raw_payload_hashes_valid`
- `collector_raw_integrity_verified`
- `artifact_hashes_match`

产物哈希（也写入 `summary.json`）：

| 文件 | SHA-256 |
|---|---|
| `raw_frames.jsonl` | `5836675014897976d47654e8d0fde5f765f7ff129d0ecba9c413d5c5a82919b8` |
| `normalized_events.jsonl` | `b5a2e245d3f589d0da7c3f0360d6c20901a78c75835c111672bbde80d8e16bc0` |
| `factors.jsonl` | `3bf57c187d1d43b183e6fc5665154c367ac98bf557b58a32991bd39f383500a1` |
| `signals.jsonl` | `fa77b1f2296b2e35f4629ec343c49277cec928f656237717247702f266cf9ae9` |
| `system.log` | `4e8932968d9a0224b61f82793dee8264a5fae4092b991cdf60bf14b40ee8ae97` |

派生文件大小：`normalized_events.jsonl` 14,330,103 B；`factors.jsonl` 9,350,992 B；`signals.jsonl` 5,264,157 B；`system.log` 3,300 B。

完整 raw 与派生文件刻意不提交，保留在 `data/runs/live900-BTCUSDT-20261022/`，运行手工 live-capture CI 工作流时会作为 产物上传。小型证据副本位于 `docs/evidence/`。

## 回放（Replay）校验

对采集到的 raw 文件执行 `replay`：

```json
{"result":"PASS","replay":true,"records_read":24903,"trades_processed":24903,
 "duplicates":0,"out_of_order":0,"malformed_frames":0,"execution_enabled":false}
```

标准化/因子/信号各 24,903 行；实时与 replay 都输出 24,903 个 HOLD 标签。Replay 与实时处理使用同一套解析器、因子和策略。

## A/B/C 基准（合成，不是实时吞吐）

最终二进制，每模式 100,000 事件，`SYNTHETIC_BENCHMARK`，`live_network:false`：

| 模式 | events/s | P50 µs | P95 µs | P99 µs | CPU % | RSS B | 错误 |
|---|---:|---:|---:|---:|---:|---:|---:|
| A. 纯解析 | 163,315 | 3.5 | 6.4 | 8.0 | 102.0 | 25,489,408 | 0 |
| B. 完整链路（去重+因子+策略） | 135,678 | 6.4 | 11.5 | 17.6 | 98.7 | 26,112,000 | 0 |
| C. B + raw JSONL 持久化 | 53,132 | 10.6 | 407.5 | 636.5 | 86.5 | 27,082,752 | 0 |

模式 C 写入 100,000 个 raw 帧（51,363,504 字节），SHA-256 为 `84eb486b8050eb47a1e84cc829e8df53ee4d81f8b0815abe00b54e1fe43642a7`。A/B 共产生 99,991 个合成非 HOLD 标签；A 按定义只做解析。进程 CPU 可超过 100%，因为它是多核累计 CPU 时间除以墙钟时间。

## 测试

`ctest --test-dir build-ninja --output-on-failure`：

```
1/4 marketpipe_help ............... Passed
2/4 marketpipe_unit ............... Passed   （18 个用例）
3/4 marketpipe_integration ........ Passed   （14 个本地回环 WebSocket 场景）
4/4 marketpipe_benchmark_100k ..... Passed
100% tests passed, 0 tests failed out of 4
Total Test time (real) = 21.48 sec
```

集成套件覆盖：正常流、异常帧、突然断开与重连代次隔离、重复/乱序成交、有界队列溢出、SIGINT/SIGTERM 排空与最终确定、HTTP 451、模拟磁盘满、最终 fsync 失败、被篡改 raw 文件检测、replay 一致性和连接拒绝。所有测试都不访问 Binance。原始 CTest 输出见 [`docs/evidence/ctest-windows.txt`](evidence/ctest-windows.txt)。

### Linux 交叉验证（WSL2 Ubuntu 22.04）

同一提交使用 GCC 11.4 / CMake 3.22.1 / Ninja 1.10.1 / Boost 1.74 / OpenSSL 3.0.2 配置和构建，CTest 4/4 通过（18 单元 + 14 集成 + 100k 基准），耗时 21.93 秒。该过程发现并修复了一个 Linux 专属的测试服务端退出问题；证据见 [`docs/evidence/ctest-linux.txt`](evidence/ctest-linux.txt)。

## Docker 与 CI 状态

- 已实现多阶段 Dockerfile、非 root UID 10001、命名卷 `marketpipe-data`、构建阶段 CTest 和 `STOPSIGNAL SIGTERM`。
- **本机未实际执行 Docker**：当前 Windows 环境没有 Docker 引擎或 `docker` CLI。不能声称本地镜像已验证。CI 工作流会构建镜像并验证其以非 root 启动。
- CI 使用的 Linux 构建/CTest 路径已在 WSL2 实际执行并通过 4/4。
- 提交已推送到 `origin/main`，因此 GitHub Actions 工作流已配置并由推送触发。仓库的 Actions 页面/API 需要认证，本环境没有已登录的 `gh` 或可用令牌，因此**不声称任何 Actions 运行 URL、状态或产物**。CI 证据必须从推送后的实际 Actions 运行中读取。

## 复现

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure

./build/marketpipe capture --symbol BTCUSDT --duration 900 \
  --output-dir data/runs --run-id live900-BTCUSDT-20261022
./build/marketpipe report --run-dir data/runs/live900-BTCUSDT-20261022
sha256sum data/runs/live900-BTCUSDT-20261022/raw_frames.jsonl
./build/marketpipe benchmark --events 100000 --mode all --output-dir data/benchmarks
```

## 限制

这是单台非托管 Windows 工作站上的一个 15 分钟观察窗口。它不能证明长期可靠性、交易所侧完整性、托管级低延迟或盈利能力。策略是确定性的标签示例，执行始终禁用。
