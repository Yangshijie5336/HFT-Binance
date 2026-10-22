# AI 协作记录

本阶段按用户约束只做资料检索、代码审计和实验设计，没有修改生产源码、构建配置或既有 README。

初始假设是项目仍停留在 capture BLOCKED；仓库审计后发现 HEAD `ff30af9` 已包含 Boost.Beast/Asio TLS 采集、本地 WebSocket 测试、队列溢出和 raw 完整性修复，因此研究范围调整为验证现状和记录下一阶段实验，而不是重复实现采集器。

网络研究使用 Binance、Boost.Beast、Boost.Asio、C++ Core Guidelines 和 CMake 官方文档。官方资料支持协议字段、WebSocket endpoint、异步 I/O 和 WebSocket 控制帧/超时接口的事实；本项目是否能在目标网络完成 900 秒采样仍需真实运行验证。

已执行的验证是当前 CTest、本地 WebSocket 集成测试、Docker 构建和 Compose 烟测。未执行的公网采样、五次性能重复和 perf/WPA 分析在报告中标记为 `NOT_EXECUTED`，没有把合成数字写成实时结论。

后续人工决策：先用现有双队列和 JSONL 作为基线，只有实验显示锁竞争、fsync 或解析成为瓶颈时再引入更复杂的数据结构或日志格式。
