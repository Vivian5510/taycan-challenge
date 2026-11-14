# FASTER/F2 调用线程执行基线

2026-11-14 按用户要求拆分：`main` 移除接入 F2 同步路径的 group commit；
完整合批实现保留在 `experiment/f2-group-commit-preserved-20261114`（`c529214`）。
[拆分范围、构建与验证](../../docs/main-without-group-commit-20261114.md)。

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
