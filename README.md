# MarketPipe HFT Demo — C++20 Binance 公共逐笔成交采集器

- 公开项目地址：<https://github.com/Yangshijie5336/HFT-Binance>
- 交付报告（架构取舍、AI 协作、验证证据）：[docs/delivery_report.md](docs/delivery_report.md)

一个单进程 C++20 行情数据工程示例：采集 **Binance Spot `btcusdt@trade` WebSocket** 公开逐笔成交，持久化每一个原始帧，解析与校验成交，计算滚动成交流因子，输出确定性的 `BUY_SIGNAL` / `SELL_SIGNAL` / `HOLD` 标签，并生成可核验的性能报告。

> **安全边界：** 仅使用公开行情。没有 API Key、私有接口、下单或执行路径。`BUY_SIGNAL` / `SELL_SIGNAL` 只是标签。

Python 参考实现已经删除，仓库现在只保留 C++20 / CMake / Ninja 工程。

## 最新验收结果

最新公网 900 秒采样（`public900-BTCUSDT-20261023`）结果为 **PASS**：

- 实际采样 **900.0712 秒**（严格达到目标；排空 0.0352 秒单独记录）。
- 29,140 个原始帧全部落盘，29,140 笔成交全部处理；0 重连、0 断开、0 队列溢出、0 丢弃、0 持久化/处理错误。
- 原始文件 SHA-256：
  `37753e2b03cac5f8520518585e97166842c95921c2625afecc0ff44ee5cb7cb7`，已由 `verification.json` 独立复核。
- 全部 11 项质量门禁通过；详见 [验收报告](docs/acceptance_report.md)、[交付报告](docs/delivery_report.md) 与 [证据目录](docs/evidence/)。

大型原始/派生文件保留在被 Git 忽略的 `data/runs/`、Release asset 或 CI 产物中，不提交到源码仓库。

## 架构

```
Binance Spot WebSocket（TLS，Beast/Asio/OpenSSL）
        │  文本帧 + 接收时间 + 连接代次
        ▼
有界 raw 队列  ──►  writer 线程  ──►  raw_frames.jsonl
        │                              │  批量 flush / 周期 fsync / 最终 fsync
        │                              ▼
        └────────────────────►  有界处理队列
                                       │
                                       ▼
                         解析 → 校验 → 去重 / 乱序标记（保留处理）
                                       │
                                       ▼
                          滚动因子 → 确定性策略
                                       │
                                       ▼
             normalized_events.jsonl · factors.jsonl · signals.jsonl
                                       │
                                       ▼
              summary.json · verification.json · performance_report.md
```

两个队列都有容量上限。发生溢出时，采集器会记录错误、将样本标记为无效、停止网络会话并排空已入队数据，绝不静默丢弃。每次重连都会递增连接代次并记录潜在缺口窗口；采集器**不会**声称重连可以保证无遗漏。

线程与故障模型见 [架构设计](docs/architecture.md)。

## 环境要求

- CMake ≥ 3.20、Ninja、支持 C++20 的编译器
- Boost ≥ 1.74（Beast / Asio / System 头文件）
- OpenSSL 3.x
- Windows：MSVC 2022+ 与 vcpkg；Linux：`g++`、`libboost-all-dev`、`libssl-dev`
- 可写磁盘
- 实时采集需要到 `stream.binance.com:9443` 的出站 TLS 访问

仓库内置 Mozilla CA bundle：`certs/cacert.pem`，在系统信任不可用时使用。TLS 对端与主机名校验始终开启，不提供 `--insecure` 开关。

## 构建

### Linux / CI

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

### Windows（MSVC + vcpkg）

如果 vcpkg 位于 `D:/Dev/vcpkg`：

```powershell
$env:VSLANG = "1033"   # 让 MSVC /showIncludes 输出英文，Ninja 才能跟踪头文件依赖
cmd /c "call ""D:\Dev\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=D:/Dev/vcpkg/installed/x64-windows"
cmd /c "call ""D:\Dev\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" && cmake --build build -j 8"
ctest --test-dir build --output-on-failure
```

Windows 构建会把 OpenSSL 运行库 DLL 复制到可执行文件旁。上面的 vcpkg 路径必须指向真正包含 Boost 和 OpenSSL 的安装；Visual Studio 注入的 `VCPKG_ROOT` 可能指向它自带的 vcpkg。修改 `marketpipe.hpp` 后若增量构建未重新编译依赖，请执行 `cmake --build build --clean-first`（本地化 MSVC 输出会影响 Ninja 的头文件依赖解析）。

## 命令

```text
marketpipe capture   --symbol BTCUSDT --duration 900 --output-dir data/runs
marketpipe replay    --input <raw_frames.jsonl> [--output-dir <dir>]
marketpipe benchmark --events 100000 [--mode all|parse|pipeline|persist]
marketpipe report    --run-dir data/runs/<run>
marketpipe synthetic --events 100000        # 旧版仅因子基准
```

### 可复现离线运行入口

在 Windows PowerShell 中执行：

```powershell
.\scripts\run_reproducible.ps1
```

该入口自动构建并运行 CTest，使用仓库固定的
`tests/fixtures/reproducible_raw_frames.jsonl` 执行两次 replay，比较
`normalized_events.jsonl`、`factors.jsonl` 和 `signals.jsonl` 的 SHA-256，
并在 `data/reproducible/summary.json` 写入提交 SHA、fixture 哈希、命令和比较结果。
默认还执行 100,000 事件的合成基准；如只验证确定性回放可使用
`-SkipBenchmark`。该模式不访问 Binance，结果不会被公网、当前时间或网络状态改变。

`capture` 默认 900 秒、`BTCUSDT`、`wss://stream.binance.com:9443/ws/btcusdt@trade`。常用参数：

| 参数 | 含义 |
|---|---|
| `--ws-url URL` | 覆盖端点（除本地回环测试外必须使用 `wss://`） |
| `--ca-file PATH` | 指定 PEM CA bundle，覆盖自动发现 |
| `--queue-capacity N` | 每个有界队列容量（默认 65536） |
| `--ring-capacity N` | 内存中已处理成交环容量（默认 20000） |
| `--fsync-every N` | 原始/派生文件 fsync 间隔（默认每 100 条 + 最终） |
| `--connect-timeout-ms N` | DNS/TCP/TLS/WebSocket 握手超时 |
| `--idle-timeout-ms N` | Beast 空闲超时；启用 keep-alive ping |
| `--reconnect-initial` / `--reconnect-max` | 指数退避上下界 |
| `--run-id ID` | 指定运行目录名 |
| `--quiet` | 不把结构化日志镜像到 stderr |

退出码：`capture` 的 `0` 为 PASS/PARTIAL，`2` 为 FAIL/BLOCKED，`130` 为 SIGINT/SIGTERM 中断；`replay` 的 `0` 为 PASS，`2` 为完整性/业务失败，`3` 为 PARTIAL。失败运行仍会写入 `system.log`、`summary.json` 和校验报告。

PASS 要求实际采样时长严格达到目标值，且排空/最终确定耗时单独记录、不能用于补足时长。

## 运行目录

每次运行创建 `<output-dir>/<run-id>/`：

| 文件 | 内容 |
|---|---|
| `raw_frames.jsonl` | 原始 WebSocket 文本、run_id、连接代次、接收 UTC/单调时间、序号、payload 字节数与 SHA-256 |
| `normalized_events.jsonl` | 校验、去重后的成交，保留精确十进制文本 |
| `factors.jsonl` | 每笔已处理成交的 30 秒成交流因子快照 |
| `signals.jsonl` | `BUY_SIGNAL` / `SELL_SIGNAL` / `HOLD`；执行禁用 |
| `system.log` | 连接、队列、持久化、停止流程的结构化诊断日志 |
| `summary.json` | 计数、延迟分位、CPU/RSS、缺口窗口、结果与原因 |
| `verification.json` | 外部重算的 raw SHA-256、记录数和逐帧 payload 校验 |
| `performance_report.md` | 由 `summary.json` 生成的人类可读报告 |

原始数据在落盘前不会转换为 `double`。价格和数量文本在原始记录与标准化事件中都保持精确；`double` 只用于因子计算。

## 测试

CTest 覆盖：

- JSON/解析边界与异常 Binance 帧
- 重复/乱序成交与有界去重（乱序成交保留并标记，不丢弃）
- replay 使用原始接收时间、逐帧 SHA-256 校验与损坏输入失败
- 严格采样时长与排空时间分离
- 有界队列溢出与停止时排空
- 磁盘满、最终 fsync 失败、打开失败
- 原始帧持久化与 SHA-256 校验
- 完整因子 + 策略链路
- 本地 WebSocket 集成：正常流、异常帧、突然断开与重连代次隔离、队列溢出、HTTP 451、SIGINT
- 可复现的 100,000 事件合成基准

测试不访问 Binance 网络。真实采样是手工验收步骤。

## Docker

公共镜像：`ghcr.io/yangshijie5336/marketpipe-hft-demo:v0.3.0`（非 root UID 10001）。

```bash
docker pull ghcr.io/yangshijie5336/marketpipe-hft-demo:v0.3.0
docker run --rm ghcr.io/yangshijie5336/marketpipe-hft-demo:v0.3.0 --help
docker compose build
docker compose run --rm marketpipe
```

镜像为多阶段构建，只写 `/app/data`；构建阶段执行 CTest。Compose 使用命名卷 `marketpipe-data`（挂载到 `/app/data`），采集数据在容器删除后仍保留且对非 root UID 可写。优雅 `SIGTERM` 会排空队列并 fsync（`stop_grace_period: 60s`）。如需改用宿主目录，请确保 UID 10001 可写，或覆盖 compose 的 `user:`。

## 证据与 CI

CI 会构建、运行 CTest（含本地 WebSocket 集成测试）、执行 100,000 事件基准、构建 Docker 镜像并上传测试/基准证据。真实采样为手工工作流，会以产物形式上传运行目录。大型原始采样、日志和凭据不会提交到 Git。

- 最新 CI：<https://github.com/Yangshijie5336/HFT-Binance/actions/runs/40057231538>
- Release 与原始数据：<https://github.com/Yangshijie5336/HFT-Binance/releases/tag/v0.3.0>

- [架构与线程安全](docs/architecture.md)
- [CLI、文件与 JSON 接口](docs/interfaces.md)
- [运行手册](docs/runbook.md)
- [故障处理](docs/troubleshooting.md)
- [性能方法与实测结果](docs/performance.md)
- [验收报告](docs/acceptance_report.md)
- [交付报告（精简）](docs/delivery_report.md)
