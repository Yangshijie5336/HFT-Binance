# MarketPipe HFT Demo — C++20 Binance 公共逐笔成交采集器

一个单进程 C++20 行情数据工程示例：采集 **Binance Spot `btcusdt@trade` WebSocket** 公开逐笔成交，持久化每一个原始帧，解析与校验成交，计算滚动成交流因子，输出确定性的 `BUY_SIGNAL` / `SELL_SIGNAL` / `HOLD` 标签，并生成可核验的性能报告。

> **安全边界：** 仅使用公开行情。没有 API Key、私有接口、下单或执行路径。`BUY_SIGNAL` / `SELL_SIGNAL` 只是标签。

Python 参考实现已经删除，仓库现在只保留 C++20 / CMake / Ninja 工程。

## 最新验收结果

最终 900 秒真实采样（`live900-BTCUSDT-20261022`）结果为 **PASS**：

- 24,903 个原始帧全部落盘，24,903 笔成交全部处理；0 重连、0 断开、0 队列溢出、0 丢弃、0 持久化/处理错误。
- 原始文件 SHA-256：
  `5836675014897976d47654e8d0fde5f765f7ff129d0ecba9c413d5c5a82919b8`，已由 `verification.json` 独立复核。
- 全部 11 项质量门禁通过；详见 [验收报告](docs/acceptance_report.md) 与 [证据目录](docs/evidence/)。

大型原始/派生文件保留在被 Git 忽略的 `data/runs/` 或 CI 产物 中，不提交到源码仓库。

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
                         解析 → 校验 → 去重 / 乱序检测
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

退出码：`0` 为 PASS/PARTIAL，`2` 为 FAIL/BLOCKED，`130` 为 SIGINT/SIGTERM 中断。失败运行仍会写入 `system.log`、`summary.json` 和校验报告。

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
- 重复/乱序成交与有界去重
- 有界队列溢出与停止时排空
- 磁盘满、最终 fsync 失败、打开失败
- 原始帧持久化与 SHA-256 校验
- 完整因子 + 策略链路
- 本地 WebSocket 集成：正常流、异常帧、突然断开与重连代次隔离、队列溢出、HTTP 451、SIGINT
- 可复现的 100,000 事件合成基准

测试不访问 Binance 网络。真实采样是手工验收步骤。

## Docker

镜像为多阶段构建，以 UID 10001 非 root 运行，只写 `/app/data`；构建阶段执行 CTest。

```bash
docker compose build
docker compose run --rm marketpipe
```

Compose 使用命名卷 `marketpipe-data`（挂载到 `/app/data`），采集数据在容器删除后仍保留且对非 root UID 可写。优雅 `SIGTERM` 会排空队列并 fsync（`stop_grace_period: 60s`）。如需改用宿主目录，请确保 UID 10001 可写，或覆盖 compose 的 `user:`。

## 证据与 CI

CI 会构建、运行 CTest（含本地 WebSocket 集成测试）、执行 100,000 事件基准、构建 Docker 镜像并上传测试/基准证据。真实采样为手工工作流，会以 产物形式上传运行目录。大型原始采样、日志和凭据不会提交到 Git。

- [架构与线程安全](docs/architecture.md)
- [CLI、文件与 JSON 接口](docs/interfaces.md)
- [运行手册](docs/runbook.md)
- [故障处理](docs/troubleshooting.md)
- [性能方法与实测结果](docs/performance.md)
- [验收报告](docs/acceptance_report.md)
