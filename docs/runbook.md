# 运行手册

## 1. 前置条件

- C++20 编译器、CMake ≥ 3.20、Ninja
- Boost Beast/Asio 头文件（≥ 1.74）与 OpenSSL 3.x
- 可写磁盘，能容纳 `raw_frames.jsonl`（BTCUSDT 通常 15 分钟几 MB，活跃窗口可能更大）
- 实时采集需要到 `stream.binance.com:9443` 的出站 TLS
- 不需要也不接受 API Key

Debian/Ubuntu：

```bash
sudo apt-get install -y build-essential cmake ninja-build libboost-all-dev libssl-dev ca-certificates
```

## 2. 构建与测试

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

预期 CTest 套件：

- `marketpipe_help`
- `marketpipe_unit`（18 个用例）
- `marketpipe_integration`（14 个本地回环 WebSocket 场景）
- `marketpipe_benchmark_100k`（解析 + 处理链路 + 持久化）

CTest 不访问 Binance。

Windows：在 Developer PowerShell 中运行，设置英文 MSVC include 诊断以便 Ninja 跟踪头文件依赖，并显式传入 vcpkg 前缀：

```powershell
$env:VSLANG = "1033"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=D:/Dev/vcpkg/installed/x64-windows
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

如果修改了公共头文件而增量构建似乎没有生效，执行 `cmake --build build --clean-first`。这是本地化 MSVC `/showIncludes` 的限制，不是源码问题。

## 3. 实时行情冒烟测试

```bash
./build/marketpipe capture --symbol BTCUSDT --duration 5 --output-dir data/smoke
```

成功样例：

```
capture result=PASS run_dir=data/smoke/20261022T...Z_BTCUSDT frames=40 processed=40 signals=40
```

然后独立校验：

```bash
./build/marketpipe report --run-dir data/smoke/<run-id>
```

如果结果为 `BLOCKED` 且 HTTP 451，应停止并记录阻塞；不要使用代理或其他司法辖区规避。本地 WebSocket 测试仍可正常运行。

## 4. 900 秒验收采样

```bash
./build/marketpipe capture \
  --symbol BTCUSDT \
  --duration 900 \
  --output-dir data/runs \
  --run-id live900-BTCUSDT-$(date -u +%Y%m%d)
```

有效验收运行要求：

- 实际时长 ≥ 891 秒；
- 至少一个原始帧和一笔已处理成交；
- `raw_frames_records == frames_received`；
- `raw_integrity_verified == true`；
- 无队列溢出、丢弃、持久化错误或处理失败；
- 无断开、异常帧、重复或乱序成交。

运行时避免竞争性的 CPU/磁盘负载。采集器写入结构化日志；连接开始/结束、退避和潜在缺口都在 `system.log`。

运行期间：

```bash
tail -f data/runs/<run-id>/system.log
```

**不要**在运行时编辑或截断目录内文件。Ctrl+C 会优雅停止并写入 FAIL summary、退出码 130；它不会被伪装成满足时长门禁。

## 5. 校验与归档

```bash
./build/marketpipe report --run-dir data/runs/<run-id>
sha256sum data/runs/<run-id>/raw_frames.jsonl
```

`report` 会重算每个 产物哈希和所有原始记录哈希。归档：

- `summary.json`
- `verification.json`
- `performance_report.md`
- `system.log`
- `raw_frames.jsonl` 及其 SHA-256
- 运行开始/结束 UTC 时间

大型原始采样不得提交到 Git。保留在本地或附加到 GitHub Release / Actions 产物。`Manual live capture` 工作流会把运行目录作为 产物上传（保留 14 天）。

## 6. 性能基准

```bash
./build/marketpipe benchmark --events 100000 --mode all \
  --output-dir data/benchmarks
```

运行：

- A. 纯解析
- B. 完整内存处理链路
- C. 完整链路 + raw JSONL 持久化

每种模式报告 `events_per_second`、P50/P95/P99、CPU、RSS、原始帧数量/字节、丢弃/错误数，以及持久化基准文件的 SHA-256。输出明确为合成数据，不是真实网络吞吐。模式 D 只能来自真实 `capture` 的 `summary.json`。

## 7. 回放（Replay）

```bash
./build/marketpipe replay \
  --input data/runs/<run-id>/raw_frames.jsonl \
  --output-dir data/replays/<name>
```

Replay 从持久化原始帧重放标准化事件、因子和信号。Replay 期间 receive-to-process 延迟不可与实时采集比较。

## 8. Docker

```bash
docker compose build
docker compose run --rm marketpipe
```

Compose 服务执行 900 秒采集，写入命名卷 `marketpipe-data` 的 `/app/data/runs`。镜像以 UID 10001 运行，SIGTERM 宽限 60 秒。构建阶段执行 CTest；在引用容器结果前必须先完成 `docker build`。

查看或归档卷：

```bash
docker compose run --rm --entrypoint ls marketpipe -l /app/data/runs
docker compose cp marketpipe:/app/data/runs ./data-from-container
```

如改用宿主机 bind mount，请确保目录对 UID 10001 可写（例如 `sudo chown -R 10001:10001 ./data`），或覆盖 compose 的 user；否则非 root 进程无法持久化帧。

## 9. CI

- `.github/workflows/ci.yml`：构建、CTest、100k 基准、Docker 构建、产物上传。
- `.github/workflows/live-capture.yml`：手工 900 秒实时运行；保留 BLOCKED 证据。
- `.github/workflows/publish-image.yml`：打 tag 后经测试发布到 GHCR。

报告 CI 证据时必须引用实际 CI Run URL/编号和下载的产物，不能凭本地假设。
