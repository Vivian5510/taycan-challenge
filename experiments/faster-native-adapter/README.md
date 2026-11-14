# FASTER/F2 调用线程执行基线

`26e4466` 的尾部恢复修复已合入 main。本轮单请求 I/O 调优结果见
[实验记录](../../docs/f2-single-commit-io-20261114.md)：候选未采用，main 默认保持不变。
hot-inline、热日志 buffered 等构建项仅为该实验分支的显式控制，不代表已通过验收。

恢复尾部部分页丢失的修复与实际fsync性能审计见[修复记录](../../docs/f2-recovery-sync-audit-20261114.md)。

2026-11-14 按用户要求拆分：`main` 移除接入 F2 同步路径的 group commit；
完整合批实现保留在 `experiment/f2-group-commit-preserved-20261114`（`c529214`）。
[拆分范围、构建与验证](../../docs/main-without-group-commit-20261114.md)。

后续启用冷热迁移与回收的复测见 [验收结论](../../docs/f2-enabled-verdict-20261114.md)。
本机完整功能配置用 `cmake --preset f2-durable-arm64-dev -S taycan_participant_sdk`；
该预设开启 data-log/posix-aio/fsync、512 B 提交对齐和 durable compaction，仍是开发配置。

数据与索引仍由 F2 执行。显式 `data-log` 路径现在由 SDK 调用线程一次执行一条
F2 Upsert/Delete，等待 HybridLog 写完成及文件同步，再发布索引并返回。
写请求通过实例互斥锁串行进入，不设收集窗口、批量队列或后台提交线程。
保留原有提交标记、恢复格式、周期 checkpoint、同步失败拒绝后续请求的行为。
CRC32C 仍复用 RocksDB 的校验实现；F2 写路径不再调用 WriteThread/WriteBatch。

旧 `rocksdb-wal` 策略、prefix pipeline、批量等待探针和相关构建开关已移至保留分支。
[源码复用历史](SOURCE_REUSE.md) 中的分组与流水线描述对应保留分支，不能当成当前 main。

默认构建仍是 `native`、`inline`、`fullfsync`；`native` 只保证 F2 原生操作完成，
没有逐调用可恢复的持久化确认。需要同步持久化的本机开发验证必须显式选择
`data-log`、`posix-aio`、`fsync`、512 字节提交对齐，命令见上述拆分记录。

自动 Compaction 仍由显式开关控制，默认关闭；SDK `CreateCheckpoint` 未接入。
本轮验证仅覆盖此次提交调度移除，不等于 GC、2 亿 Key/4 GiB、Linux 正式评测已通过。
上游研究 checkout 保持独立，构建时只修改生成副本。
