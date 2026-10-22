# 故障处理

每次采集失败也会写入 `system.log`、`summary.json` 和 `verification.json`。先看 `failure`、`quality_checks`，以及最后一条 `ws_*` / `*_failed` 日志，通常就能定位失败层。

## HTTP 451 / 地域限制

现象：

```json
{"result":"BLOCKED","http_status":451,"http_blocked":true,
 "failure":"Binance WebSocket HTTP 451: geographic access restriction"}
```

采集器记录 `capture_blocked_by_exchange` 后立即停止，不做重试风暴；会落盘空的 raw 文件并写 BLOCKED summary。它不会使用代理、VPN 或其他司法辖区规避。此时应运行本地 WebSocket 集成测试，并把阻塞作为验收结论记录。

CDN/WAF 返回的 `403` 作为 `FAIL` 上报，带 `http_status` 和响应体前缀，因为它不一定是法律访问限制。

## TLS 证书校验失败

现象：

```json
{"result":"FAIL","tls_failure":true,
 "failure":"TLS certificate verification failed: certificate verify failed ..."}
```

检查：

1. 信任来源：`--ca-file`、`MARKETPIPE_CA_FILE`、`SSL_CERT_FILE`、内置 `certs/cacert.pem`、OpenSSL 默认路径、Windows Root 存储。
2. `system.log` 中应有 `tls_ca_bundle_loaded` 记录所选路径，或 `tls_ca_bundle_load_failed` 记录 OpenSSL 错误。
3. 系统时钟必须正确；过期证书不能绕过校验。
4. 企业 TLS 拦截需要显式通过 `--ca-file` 安装企业根 CA。不存在 `--insecure` 模式。

绝不要通过关闭校验来“修复”该问题。

## DNS / 连接被拒绝

现象：重复 `ws_connect_failed`，带 `consecutive_failures`，随后 `ws_max_connect_failures` 和 `result=FAIL`。

- 在同一主机验证 DNS 和 `curl -I https://stream.binance.com:9443`。
- 检查防火墙/代理出站；采集器不读取代理环境变量。
- 退避有界（默认 1 秒 → 15 秒，连续 10 次失败）。连接被拒绝一定返回非零退出码。

## 空闲超时 / 断开

Beast keep-alive ping 由 `--idle-timeout-ms` 控制（默认 90 秒）。两个空闲周期内没有数据或 pong，连接视为断开。采集器计数 disconnect，按退避重连，递增 `connection_generations`，并记录 `gap_windows`。运行结果变为 `PARTIAL` 而不是 `PASS`；重连不能证明行情完整。

## 队列溢出 / 消费者过慢

现象：

```json
{"queue_overflow":true,"dropped_events":N,"result":"FAIL",
 "failure":"bounded queue overflow: sample invalid"}
```

采集器不会静默丢弃，而是记录 `raw_queue_overflow` 或 `process_queue_overflow`，标记样本无效并停止。处理方向：

- 检查磁盘是否是瓶颈（fsync 频率、杀毒软件、网络盘）。
- 短时突发可增大 `--queue-capacity`，但不能用它掩盖持续过载。
- 仅在持久化要求允许时降低 `--fsync-every`；默认 100。
- 持续过载是容量问题，不是丢弃数据的理由。

## 磁盘满 / 写入失败 / 最终 fsync 失败

现象：`persistence_error=true`、`raw_write_failed`、`writer_finalize_failed`、`raw_finalize_failed` 或 `ENOSPC`，退出码 2。

- 清理空间；运行目录是追加式写入，从不覆盖。
- 采集器检测短写、`fflush`/`fsync` 错误和最终确定错误。
- 失败样本为 `FAIL`；如果文件被截断，`report` 的记录/哈希校验也会失败。
- 不要把部分运行目录复制进验收报告。

## SIGINT / SIGTERM

处理器只设置标志。约 50 ms 内网络会话被取消，队列关闭并排空，raw 与派生文件 flush/fsync，summary 写入，`stopped_by_signal=true`，结果 `FAIL`，退出码 130。

如果进程被强制杀死（SIGKILL / `docker kill`），只能保证已经 fsync 的数据。请使用 `docker stop`（SIGTERM）并给予 ≥ 30 秒宽限。

## 采样时长不足 / 排空时间过长

`duration_actual_seconds` 只计算网络接收阶段，到 `run_network()` 返回为止；`drain_elapsed_seconds` 记录此后的排空、join、fsync 和报告耗时。PASS 要求实际采样时长严格达到目标值，排空时间不能补足。看到 `duration_meets_target=false` 或 `failure="capture duration below target"` 时，应检查是否提前收到 SIGINT/SIGTERM、连接失败或队列溢出导致采集提前结束，而不是调低门槛。

## 回放完整性失败 / 乱序成交

`replay` 返回 `2` 且 `replay_summary.json` 为 `FAIL` 时，先看 `integrity_errors` 和 `first_error`：

- `invalid raw JSON object`：原始 JSONL 损坏；
- `payload SHA-256 mismatch`：payload 被修改或日志被拼接；
- `payload_bytes mismatch`：帧长度元数据损坏；
- `frame_seq is not strictly increasing` / `connection_generation went backwards`：记录顺序或代次被修改。

不要编辑 raw 文件来“修复”回放；应重新采集或从可信备份恢复。合法输入但存在重复或乱序时，replay 返回 `3`/`PARTIAL`：重复成交跳过，未重复的乱序成交保留并标记 `out_of_order=true`，按到达（接收）顺序进入因子窗口；这与实时采集规则一致。

## 报告校验不匹配

以下情况 `report` 返回 2：

- `summary.json` 声称 PASS，但某个产物哈希不同；
- `raw_frames.jsonl` 有坏记录、非递增序号或 payload 哈希不匹配；
- 记录数与 `frames_received`/`raw_frames_records` 不一致；
- 必需产物缺失。

采集后不要编辑原始文件。校验归档运行时，原样复制目录并执行 `report --run-dir`。

## Windows DLL 找不到（0xC0000135）

构建会把 `libssl-3-x64.dll` 和 `libcrypto-3-x64.dll` 复制到可执行文件旁。若缺失，确认 `CMAKE_PREFIX_PATH` 指向真正提供 OpenSSL 的 vcpkg 安装后重新配置。Visual Studio 可能注入不同的 `VCPKG_ROOT`；CMake 也会根据定位到的 OpenSSL 配置推导 DLL 目录。

## Docker 数据消失

容器只写 `/app/data`。提供的 compose 使用命名卷 `marketpipe-data`，其初始所有权来自镜像目录，UID 10001 可写。如果改用宿主机 bind mount，请确保其对 UID 10001 可写（或覆盖 `user:`），否则 raw 写入会报持久化错误。`docker compose down -v` 会删除命名卷及其采集数据。

## 合成基准不是实时吞吐

任何 benchmark 输出都包含 `"benchmark_type":"SYNTHETIC_BENCHMARK"` 和 `"live_network":false`。不要把这类数字写入实时验收结论。模式 D 必须来自真实 `capture` 的 `summary.json`。
