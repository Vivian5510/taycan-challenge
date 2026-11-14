# F2 开启必要功能后的结论

被测主干为 `144a5ec`，group commit 已移除；本轮未修改引擎内核。显式启用
`data-log` / `posix-aio` / `fsync` / `512B` / `DURABLE_COMPACTION=ON`，段大小8MiB，
预算4GiB。热转冷、冷数据压缩、磁盘索引回收均有实际执行和旧段删除证据。

**结论：当前整体不通过。**当前找到的是恢复边界错误，而不是配置未开。
在全部已确认数据字节保持完整时，未确认大值写入的前部未同步页丢失、后部提交标记保留，
会让Open误报已提交前缀损坏而失败，无法服务此前已确认的状态。
这是确定性磁盘镜像故障模型；使用100KiB值，属于SDK公开Basic支持范围，超出正式性能
固定256B；不冒称原生SIGKILL脚本失败或已经验证物理断电。

开启回收后的10万Key×10轮Compaction已通过空间、最终内容和重开验证，普通公开SIGKILL
5/5通过。其余场景的最终状态、未完成原因及RSS以[本轮完整记录](../analysis/f2-enabled-verdict-20261114/README.md)为准。
磁盘保护/开发超时与真正的引擎断言错误分开保留，未完成不算通过。可选SDK Checkpoint未实现。

14个公开场景的本轮原状态为9 PASS、3 FAIL（均属于未完成）、1 NOT_RUN、1 SKIP：

- PASS：Basic、百万Key顺序、覆盖、删除、百万Key混合、重开恢复、并发、Compaction、普通崩溃恢复。
- random：600秒本地开发时限未结束；采样显示在写入及冷日志压缩推进，不宣称死锁或数据错误。
- demand_load：原1600万Key数据本身3.072GB，超过本地单库2GiB限额，部分运行后停止。
- memory_limit：2000万Key最终存活原始数据约2.560GB，超过本地限额，预检NOT_RUN。
- compaction_crash：准备阶段超过本地限额后中止，未注入SIGKILL，不当作恢复断言失败。
- Checkpoint：可选接口未实现，原测试两用例均SKIP。

这些限制使完整公开验收尚未通过；它们不证明索引/内存算法必然有错。
本轮实际确认的错误仍是上述大值未确认尾部部分页丢失时恢复失败。

## 性能参照

32线程、4GiB、100万唯一Key、32/256B、2秒预热/10秒测量；每格新库。
两者返回成功前同步，RocksDB8.9.1 `sync=true` / WAL开启 / 无压缩 / HAVE_FULLFSYNC=OFF。
第一轮四个受其他测试重叠影响的格子排除，下面只引用第二轮完整串行对照。

| 负载 | F2 ops/s | RocksDB ops/s | RocksDB/F2 |
|---|---:|---:|---:|
| 完整百万Key装载 | 9,273 | 126,887 | 13.68 |
| 随机读 | 692,512 | 780,650 | 1.13 |
| 随机覆盖写 | 9,087 | 207,421 | 22.83 |
| 60% Get/30% Put/10% Delete | 23,203 | 232,458 | 10.02 |

F2移除合批后每次调用独立同步、写调用经实例互斥串行；RocksDB保留原生提交调度。
这是当前两套实现的实际配置对照，没有证据把全部差距归因于某个单独开关。
F2各格RSS峰值约1363～1569MiB，RocksDB约294～756MiB，均低于本轮4GiB×110%。
F2 F_NOCACHE，RocksDB保留默认文件/块缓存；读结果不是冷盘I/O对照。
本轮1百万规模不能证明2亿Key预算/索引验收；macOS ARM64不是正式Linux x86_64/NVMe评分。
原版RocksDB仅为性能参照，不符合命题固定哈希桶结构约束。

[原始性能与哈希](../analysis/f2-enabled-verdict-20261114/performance/results.json) ·
[第二轮完整对照](../analysis/f2-enabled-verdict-20261114/clean-second-round.json) ·
[调度/监控问题与排除规则](../analysis/f2-enabled-verdict-20261114/README-diagnostic-notes.md) ·
[失败机制](../analysis/f2-enabled-verdict-20261114/root-cause.md)。

后续可用 `cmake --preset f2-durable-arm64-dev -S taycan_participant_sdk` 和
`cmake --build --preset f2-durable-arm64-dev` 复现完整功能开发配置，避免只构建默认native
或误关回收。该预设代表开启功能，不代表正式验收通过。
