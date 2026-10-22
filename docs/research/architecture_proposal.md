# 架构建议

## 基线架构

```text
Binance WSS/TLS
    │ 完整 WebSocket 文本帧
    ▼
Asio 网络协程
    │ RawFrame(seq,generation,connection_id,UTC,steady,payload)
    ▼
有界 raw_queue ──满则记录错误并终止本次采样
    ▼
持久化线程 ──JSONL、flush、周期 fsync、最终 fsync、SHA-256
    ▼
有界 process_queue ──满则记录错误并终止本次采样
    ▼
处理线程：JSON → 校验 → 去重/乱序标记 → 因子 → 策略
    ├── normalized_events.jsonl
    ├── factors.jsonl
    └── signals.jsonl
```

## 保留与适配

保留 `json.cpp` 的解析器、`core.cpp` 的 Trade/Factor/Strategy/Replay、`runtime.cpp` 的队列和 writer、`report.cpp` 的验证、`benchmark.cpp` 的合成基准。网络接入集中在 `capture.cpp`，不让业务层依赖 Boost socket 类型。CLI 继续通过 `main.cpp` 组装配置。

## 所有权

网络协程拥有连接对象和 RawFrame 的首次所有权；成功移动到 raw queue 后由 writer 拥有。writer 只有在原始帧写入成功后才将帧移动到 process queue。processor 拥有 Trade 去重表、ring、因子和派生 writer。关闭顺序为停止接收、关闭 raw queue、writer 排空并关闭 process queue、processor 排空、最终同步和报告。

## 错误传播

DNS、TCP、TLS、HTTP 451、WebSocket close、超时和服务器错误分别记录结构化事件。持久化、队列溢出、处理异常和完整性校验错误设置粘性失败状态。重连只对可恢复连接错误使用有限退避；HTTP 451 和 TLS 信任失败直接终止并保留 BLOCKED/FAIL 证据。

## 可选演进

只有基准证明锁竞争或分配成本显著时，才引入 SPSC ring 或对象池。若原始日志吞吐成为瓶颈，先增加文件轮转和批量提交实验，再评估二进制格式；JSONL 仍应保留可回放导出路径。
