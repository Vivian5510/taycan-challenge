# FASTER/F2 实验适配器

2026-11-03 新增默认关闭的 `TAYCAN_F2_FIBER_EXPERIMENT`：Boost.Fiber/Context 1.85.0、
单 owner/长期 F2 session、已提交索引读取、`aio_fsync` 完成后 ACK。当前只在 macOS
后端做 C++17 开发实验，拒绝与分片 owner、旧 pipeline 或 durable GC 同时启用。
51条小规模验证、故障/中断记录、合批窗口和性能边界见
[fiber 实验报告](../../analysis/f2-fiber-experiment-20261103/README.md)。默认路径未切换。

同日进一步对照Helio实现了可选定向通知、16项调度与热读内联，并用保持原批量的
`inline32` 拆分复验。`F2_FIBER_DISPATCH_MODE=targeted` 在混合/热读中有收益，纯写
并非统一受益；其余模式未稳定胜出，默认仍为 `legacy`。68条开发验证（含20次SIGKILL）、
76个性能单元和适用边界见 [调度优化报告](../../analysis/f2-fiber-dispatch-20261103/README.md)。

随后恢复Dragonfly固定owner主线，原样引入Helio BSD有界队列（含许可证），并适配consumer
短任务执行。`F2_FIBER_INBOX=helio`、`F2_FIBER_CONSUMER=tasklet` 均为默认关闭的实验选项；
保留owner数据所有权和同步后ACK。热读有收益但混合/纯写回退，原因、采样和不默认采用的
决定见 [Helio适配报告](../../analysis/f2-helio-queue-20261103/README.md)。

完成端进一步参考Helio `Done/EventCount` 的ready发布、等待登记和对象生命周期，新增
`F2_FIBER_COMPLETION=event`：为同步SDK调用线程复用OS完成事件，owner仅通知已登记
等待者。它保持数据访问在owner，macOS使用Mach semaphore，Linux分支尚未验证。
默认仍为CV。生命周期探针、42条验证、66格对照和ASAN运行库限制见
[完成通知报告](../../analysis/f2-helio-completion-20261103/README.md)。

新增显式 `TAYCAN_F2_PREFIX_PIPELINE=ON` 的 F2 内核提交实验，移植 Tsavorite 的有界在途提交、
持久前沿覆盖与顺序完成机制。第一轮与补齐合并规则后的性能、受控可见性/重叠强杀和源码
复用范围见 [源码深度移植实验](../../analysis/f2-source-reuse-20261031/README.md)。当前保留为实验候选。

macOS I/O 新增显式 `TAYCAN_F2_NATIVE_IO=posix-aio` 实验开关，默认仍为 inline。
原生 RocksDB 并发扫描、F2 AIO 完成语义/强杀恢复验证及 Get 尾延迟归因见
[I/O 并发实验](../../analysis/f2-io-concurrency-20261030/README.md)。

新增显式 `data-log` 实验：借鉴 Tsavorite/WiscKey，让 F2 HybridLog 自身承载可恢复提交，
不额外写 WAL。经过 I/O 小实验改进句柄同步后，跨缓冲装载累计写入减少约 38%，当前吞吐
和尾延迟仍未胜出，保留为实验候选。设计、71 项检查与两轮同条件对照见
[F2 原生日志提交报告](../../analysis/f2-native-data-log-20261030/README.md)。默认策略未变。

最新接入实现为 `-DTAYCAN_F2_SYNC_STRATEGY=rocksdb-wal`：F2 实例直接复用 RocksDB 原生
WriteThread、WriteBatch、WAL Writer/Reader，接通真实 KVEngine 同步写、恢复和 WAL 代次回收。
实现、正确性和同条件 RocksDB 对比见 [接入报告](../../analysis/f2-rocksdb-wal-20261030/README.md)。
默认 native 仍是原生完成语义的基线；以下 HybridLog/自写日志实验保留为历史对照。

该策略现在按实测结果采用自适应分组收集，缺省预算 50 微秒，单调用者不等待；
`F2_WAL_GROUP_WAIT_US=0` 可关闭。详细计时默认编译关闭。最新装载/混合对照、延迟取舍和
F2 HybridLog 进一步优化方向见 [WAL 优化报告](../../analysis/f2-wal-optimization-20261030/README.md)。

同步接口的实验结果分别见 [完整 checkpoint/WAL 对比](../../analysis/f2-sync-final-20261030/README.md)
和 [F2 HybridLog 提交前沿实验](../../analysis/f2-hybridlog-durable-20261030/README.md)。显式
使用 TAYCAN_F2_SYNC_STRATEGY=hybridlog、checkpoint 或 commit-log 构建；默认 native 仍保留
原生完成语义。HybridLog 策略在 F2 内部等待持久化 serial、同步日志依赖并发布恢复根后，才
让 Put/Delete 成功返回；它不使用旁路 WAL。

## 当前 F2 底座评估

此快照作为无旁路 WAL 的 F2 原生完成语义基线，直接将 SDK 调用映射到 F2 Read/Upsert/Delete。2026-10-30 在 macOS ARM64 测得的 10k Key、32/256 B 诊断吞吐，以及当前 SDK 功能覆盖见 [F2 底座评估报告](../../analysis/f2-base-native-perf-10k-20261030/README.md)。数据不是逐操作持久化成绩，也不是 2 亿 Key 命题评测。

自动 Compaction 当前关闭；SDK 的独立副本 `CreateCheckpoint` 尚未接入。新库初始化和正常关闭时，适配器仍会调用 F2 内核 checkpoint；重开时使用 F2 自己的 checkpoint token。默认 native 策略中，Put/Delete 成功返回并不意味着这次修改已包含在可恢复的持久化边界中。HybridLog 实验构建已在小规模 SIGKILL 测试中验证该边界，但尚未验证正式 Linux/NVMe、断电、Compaction 和官方数据规模。

下文更早的 16-key 可用性记录仍作为历史证据保留。

此目录是当前唯一保留的 F2 底座实验适配器，不是最终参赛包；上游研究副本保持不变。

旧的异步提交调度探针必须显式开启 `-DTAYCAN_F2_COMMIT_EXPERIMENT=ON`，再构建
`f2_durability_strategy_probe`；它不改变默认 Get/Put/Delete 的完成语义。同步 KV API 策略
使用独立的 `TAYCAN_F2_SYNC_STRATEGY` 选项和 engine identity，不能混作原生基线。详见
[性能审计](../../analysis/f2-internal-commit-audit-20261030/README.md)。

Windows/Linux 是上游 C++ 文件后端；此原型增加 macOS ARM64 诊断后端。
构建树中的 Address::kOffsetBits 暂从 25 改为 20，日志页由 32 MiB 改为 1 MiB。
这是格式分支，不能把其结果归为未经修改的 F2。

Basic 两个用例已通过；恢复、并发和崩溃探针结果见
analysis/faster-native-feasibility-20261029/README.md，未完成完整默认验收。
FASTER 原生 Upsert 完成点不是题目要求的“成功返回前持久化”。
默认 native 路径仍只有打开、关闭边界的 F2 checkpoint；只有 opt-in 的 HybridLog 实验策略提供了新的 durable frontier 协议。

请勿将此原型打包提交，也不要拿诊断后端的吞吐与 RocksDB 作性能对比。
