# F2 底座第一轮评估

本轮按“先测 F2 底座性能和命题功能覆盖，再决定后续改造”的顺序执行。当前活动性能对象是**无旁路 WAL 的 F2 原生完成语义适配器**，代码快照位于 `experiments/faster-native-adapter/`。之前的适配层 WAL 已从活动源码中清除，历史结果只用于审计。

## 性能诊断

环境为 Apple Silicon / macOS ARM64 原生后端、FASTER 提交 `332ea7da532a688acf648ee60c4f63a1caeaf1f7`。测试数据固定为 10,000 Key、32 B Key、256 B Value、4 GiB `mem_limit_bytes`、1/4 个闭环调用线程；每个单元完整装载后预热 1 秒、测量 3 秒、单轮。

| 工作负载 | 1 线程 ops/s | 4 线程 ops/s | RSS 峰值范围 |
|---|---:|---:|---:|
| load | 412,002 | 467,134 | 157.25–157.62 MiB |
| read | 994,003 | 959,525 | 157.48–158.16 MiB |
| contest_mix 60/30/10 | 868,240 | 1,011,211 | 486.72–515.45 MiB |

这是 F2 API 调用完成速度，不是命题有效吞吐：该基线不提供每次 Put/Delete 持久化 ACK，而且当前禁用了自动 Compaction。测量期写入期间的数据目录增长很少，部分输出和索引 Checkpoint 工作在关闭阶段发生。Open 约 2.15–2.42 秒，close 约 2.45–3.29 秒，close 后 10k Key 测试目录约 260–616 MiB。不能把 0.4–1.0 M ops/s 外推到同步 ACK、冷盘或 2 亿 Key。

## 功能覆盖

| 能力 | F2 内核源码 | 当前无 WAL 直连适配 | 结果/缺口 |
|---|---|---|---|
| Get/Put/Delete 与完整 Key 比较 | 有 | 直接映射 Read/Upsert/Delete | 32/256 B 小规模 Basic PASS |
| 前台并发 session | 有；上游有 ConcurrentOps 测试 | 每个 API 调用在调用线程建立/结束 session，没有适配器全局操作锁 | 1,000 Key 并发测试 PASS；尚未做高并发大规模验证 |
| Checkpoint / Recover | F2 两层 checkpoint/recover 已实现 | 新库和正常关闭 checkpoint；Open 从发布 token 恢复 | 1,000 Key Recovery PASS；SDK `CreateCheckpoint` 可选副本接口 SKIP |
| 成功返回前持久化 | HybridLog 异步刷盘；普通 Upsert/Delete 完成与稳定落盘不同 | 未实现 durable frontier / checkpoint 后尾部重放 | 64 Key、3 轮 SDK crash test FAIL：确认 178 条后第 179 条本应可选，重开读到 NotFound，丢失已确认状态 |
| 索引冷化与内存限制 | ColdIndex 将 hash chunk 放在磁盘，可按需读取；F2 有 Hot-Cold 迁移 | 当前启动规划按预算分配主表和日志缓冲；自动回收被关闭 | 10k Key 的 157–515 MiB RSS 不能证明 2 亿 Key/4 GiB 达标 |
| Compaction / 空间回收 | Hot-Cold 与 Cold-Cold compaction 机制及上游测试存在 | 当前适配没有启用 | 256 Key 小型 SDK compaction 测试失败：目录 134,750,713 B，限值 16,924,096 B；另一个受控原生组件探针手动调用 F2 热转冷后可以读回，但不等于自动预算回收已集成 |
| 2 亿 Key 两档性能 | 尚未在本轮目标规模验证 | 未运行 | 4/16 GiB、相同 2 亿 Key 均为 NOT RUN |

“F2 内核有”是源码能力，“适配器当前启用了”以及“通过命题验收”是另外两个判定，不能混为一谈。完整 F2 原生诊断数值、命令、二进制哈希、RSS 记录和原始 FAIL/SKIP 日志在本目录 `results.json`、`functional/` 与 `verification-summary.json`。

## 已退休的旁路 WAL 对照

前一轮适配层 `F2-WAL` 版在 64、2,048 Key 下通过小规模 crash test；4 GiB 下 100k Key Recovery 和 10k Key Concurrent 也通过。它使用每次写单独同步的 sidecar WAL 和 writer mutex，不能代表 F2 原生性能，也不是当前实现选项。结果证据保留于 [持久化原型报告](../analysis/f2-durability-20261029/README.md)，源码归档清单位于 `analysis/retired-implementations-20261030/`。

## 决策

F2 值得作为一个候选底座继续评估：其 ColdIndex、Checkpoint/Recover 和 Hot-Cold 迁移确实覆盖命题需要的核心机制，10k 热数据原生调用路径在本机吞吐诊断中也没有暴露接口级性能问题。决定性风险是把 F2 原生完成语义变成题目要求的 ACK 持久化，同时保留恢复索引能力，并在 4 GiB 下接通 Compaction 与空间回收。

下一阶段应比较两种 **F2 内核内方案**：扩展 HybridLog 的 durable frontier/恢复重放，或让 F2 内部以批提交推进 durable checkpoint。先以相同 10k/100k 数据、1/4 线程、同步完成的规则做微基准及崩溃边界测试，记录 fsync 次数、吞吐、恢复进度、RSS 和目录空间；未证明 ACK 正确前不以异步原生 F2 吞吐做决策。尚未运行 2 亿 Key，也不在本报告给出最终“继续/停止 F2”结论。
