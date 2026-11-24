# 当前性能结论

状态截至 2026-11-24。当前结果用于定位性能，不是正式评分。**性能门槛尚未通过，2 亿 Key 的最终档没有完成验收。**

## 原生 Linux Load 基线

用户提供的 Ubuntu x86_64 主机：i9-13900K、32 个逻辑 CPU、GIGABYTE AG510K2TB NVMe、ext4。相同 Release 二进制用开关切换提交者策略。每格为 30 万唯一 Key、32 个调用线程、32 B Key、256 B Value、4 GiB、逐条同步 API，三轮交错执行并在每轮后重开校验。

| 策略 | 三轮 ops/s | 中位数 | Put P99 中位数 | 峰值 RSS 中位数 |
|---|---|---:|---:|---:|
| 首调用者收集 | 46,475 / 47,344 / 47,300 | 47,300 | 1.606 ms | 284.5 MiB |
| 凑满线程接手 | 42,712 / 42,202 / 43,274 | 42,712 | 1.704 ms | 283.5 MiB |

直接接手将“凑满至取走批次”从 8.85 μs 降至 3.22 μs，但整批平均时间从 685.7 μs 增至 735.7 μs；F2 apply、日志写完成和 fdatasync 的阶段均变慢。整体吞吐回退 9.7%，所以 `F2_GC_BATCH_TAKEOVER` 默认关闭。两种策略的每轮 300,000 次 Put 均完成，六次重开校验通过。

## CPU affinity 对照

在同一台 13900K、同一 NVMe/ext4 和同一 `RWF_DSYNC` 二进制上，保持 32 个同步调用线程、30 万 Key、32 B Key、256 B Value、4 GiB，只限制进程可运行的 CPU 集。16 CPU 组使用 8 个 P 核各一个硬件线程和 8 个 E 核；32 CPU 组使用全部 32 个逻辑 CPU（本机共 24 个物理核）。不设置 F2 运行时变量，每档三轮。

| CPU affinity | 三轮 ops/s | 中位数 | 提交组数 |
|---|---:|---:|---:|
| 16 个物理核 / 16 个逻辑 CPU | 45,010 / 44,771 / 44,239 | 44,771 | 9,391 / 9,390 / 9,392 |
| 全部 24 个物理核 / 32 个逻辑 CPU | 44,586 / 44,418 / 43,889 | 44,418 | 9,394 / 9,395 / 9,400 |

32 个逻辑 CPU 组的中位数低约 0.8%，两组轮间范围重叠，提交组数也相同；本轮未观察到增加 CPU affinity 带来的吞吐提升。运行顺序为先 16 后 32，差异小于轮间波动，不能解释为 16 核更快。该测试只记录吞吐，没有采集 CPU 利用率、运行队列或 leader 的每线程 CPU 时间，因此不能单独判定 CPU 是否为次要限制；它也不等价于另一台 64 核机器的对照。逐轮报告见 [16 CPU](../analysis/f2-cpu-affinity-test-20261124/cpu16/README.md) 和[32 CPU](../analysis/f2-cpu-affinity-test-20261124/cpu32/README.md)。

随后对相同二进制做了一轮 `pidstat` 线程采样：30 万 Key、32 个同步 worker、4 GiB、同一 NVMe，Load 为 45,253 ops/s，测量阶段 6.629 秒。测量稳定区间进程合计约 174–181% CPU（按单逻辑 CPU=100%），即约 1.7–1.8 个逻辑 CPU；32 个 worker 都有少量运行时间，没有线程持续接近单核饱和。`perf stat` 因主机 `perf_event_paranoid=4` 被内核拒绝。本次单轮采样与 affinity A/B 一致，说明这组同步 Load 没有把 32 个可用逻辑 CPU 用满；它不能替代调用栈采样，也不能外推到 2 亿 Key。逐线程原始样本和完整 runner 报告见 [CPU 利用率诊断](../analysis/f2-cpu-utilization-20261124/)。

## Dragonfly shard 架构可行性

已克隆 Dragonfly `101047d3` 并初始化 Helio submodule `59a4e256`。Dragonfly 的 ProactorPool 默认按在线 CPU 创建 proactor 线程；每个 `EngineShard` 绑定到一个 proactor 线程并在线程局部保存。单 key 请求按 hash 选择 owner shard；同线程时可 inline 执行，否则投递到 shard fiber queue。常见单 shard 操作因此能在不同 shard 上并行运行，跨 shard 操作才需要 coordinator fiber 扇出和事务协调。架构细节见[Dragonfly shared-nothing 文档](../../dragonfly/docs/df-share-nothing.md)、[事务调度文档](../../dragonfly/docs/transaction.md)和[ProactorPool](../../dragonfly/helio/util/proactor_pool.h)。

这与当前命题的持久化要求存在关键差异：Dragonfly 是内存数据库；其 AOF 文档明确标注为未实现的设计提案，`everysec` 路径每秒异步 `fdatasync`，并不等于每个成功 Put 都在返回前持久化。因此不能直接把 Dragonfly 的 shard 架构或 AOF ACK 语义搬过来。

为测量“独立 F2 持久化域能否并行用盘”，加了默认关闭的 `F2_SHARD_COUNT` 实验：稳定 FNV-1a 路由到多个独立 F2 store，每个 store 有自己的 HybridLog、索引、提交状态和按份额缩小的内存预算；根目录 manifest 固定 shard 数。它复用了调用线程执行本地 batch leader，没有实现 Dragonfly 的 owner-thread、fiber queue 或跨 shard transaction。数据为同一 Release/RWF_DSYNC 二进制、30 万 Put、32 闭环线程、32 B Key、256 B Value、4 GiB，每档三轮交错；为保持总 batch target 约 32，请求阈值按 `ceil(32 / shard_count)` 设置。

当前单 store 的组提交仍由一个 leader 按 serial 顺序逐条调用 F2 `Upsert`/`Delete`，其余同步调用线程等待该组完成。CPU 样本显示了可用的处理器空间，但不能据此直接添加 shard 或并发改写 F2 索引：顺序分配、同 Key 冲突、F2 session 线程归属、HybridLog append 和索引发布都要保留现有持久化后 ACK 与恢复语义。下一步应先设计同一个 durable commit group 内的并行执行边界，再以正确性故障测试和同盘 A/B 验证；独立日志分片的负向结果不作为放弃 CPU 并行的依据。

| F2 shards | 三轮 ops/s | 中位数 ops/s | 提交组中位数 | Hot-log RWF_DSYNC 写中位数 | 平均 Put/提交组 |
|---:|---:|---:|---:|---:|---:|
| 1 | 43,544 / 44,400 / 44,221 | 44,221 | 9,396 | 9,488 | 31.9 |
| 2 | 29,209 / 30,192 / 29,870 | 29,870 | 23,767 | 23,852 | 12.6 |
| 4 | 13,888 / 13,705 / 13,586 | 13,705 | 52,429 | 52,520 | 5.7 |
| 8 | 6,949 / 6,678 / 6,662 | 6,678 | 106,111 | 106,180 | 2.8 |
| 16 | 3,943 / 4,109 / 4,113 | 4,109 | 181,346 | 181,397 | 1.7 |

结果随 shard 数增加而持续下降。32 个同步调用者的闭环模型让更多 shard 分走有限的同时请求，每 shard 的 batch 变小；持久写数从约 9.5K 增至 181K。该 NVMe 上并发提交 durable write 没有抵消屏障数量膨胀。底层 NVMe command 是否实际重叠仍未用 block trace 验证，但这版分片实现的吞吐已经明确负向，故 `F2_SHARD_COUNT` 保持实验选项，默认单 shard。1,000-key basic、overwrite、delete、concurrent、recovery、5 轮 SIGKILL crash recovery 均通过；最终 manifest guard 后又通过 basic、concurrent 和 2 轮 crash recovery。小规模正确性通过不改变负向性能结论。逐轮数据、命令和源码构建信息见[分片可行性实验](../analysis/f2-shard-feasibility-20261124/README.md)。

## `RWF_DSYNC` 写入持久化 A/B

2026-11-24 在同一台 NVMe/ext4 主机上，对比 `AIO + fdatasync` 与 Linux AIO `pwritev2(RWF_DSYNC)`。两边均为 30 万唯一 Key、32 线程、32 B Key、256 B Value、4 GiB、同步 API、相同组提交策略，并启用日志段预分配；每组各跑三次，交错测量。`RWF_DSYNC` 只作用于 hot HybridLog 写；首次创建文件的目录同步和 checkpoint 元数据同步不变。

| 持久化方式 | 三轮 ops/s | 中位数 | Put P99 中位数 | Hot-log AIO 写次数 | 独立 hot-log `fdatasync` | 全部 sync syscall / 累计时间 | F2 flush 阶段累计时间 |
|---|---|---:|---:|---:|---:|---:|---:|
| AIO + fdatasync | 45,231 / 44,793 / 44,652 | 44,793 | 1.638 ms | 9,453 | 9,379 | 9,477 / 4.324 s | 0.562 s |
| AIO + RWF_DSYNC | 48,263 / 47,790 / 47,551 | 47,790 | 1.622 ms | 9,453 | 0 | 98 / 0.080 s | 4.460 s |

独立 hot-log `fdatasync` 从每组一次降为零，三轮中位吞吐提升约 6.7%，P99 Put 中位数小幅改善。但 hot-log AIO 写数约 9,453 次，和基线的 9,379 个提交组接近；`flush_ns` 从 0.562 s 增至 4.460 s，说明持久化等待主要移入了带 `RWF_DSYNC` 的写完成阶段，并没有显著减少设备持久化屏障。该改动节省独立同步 syscall 的开销，不等同于把每组持久化合并成更少的设备操作。每个 AIO hot-log 写都带 `RWF_DSYNC`，完成回调后才允许对应提交组返回成功。构建选项 `TAYCAN_F2_AIO_RWF_DSYNC` 在 Linux `data-log` 默认开启；非 Linux、关闭该选项或启用 durable compaction 时不采用该路径。

另对已确认使用 `O_DIRECT` 的 hot-log 写叠加 Linux `RWF_HIPRI` 做三轮对照：普通 `RWF_DSYNC` 为 47,024 / 47,933 / 48,663 ops/s，叠加 `RWF_HIPRI` 为 48,398 / 48,067 / 47,764 ops/s；中位数相差约 0.3%，低于轮间波动，因此不保留该实现。

此构建已通过 basic、overwrite、delete、concurrent、recovery 和外部 SIGKILL crash_recovery 场景。以上仍是 30 万 Key、4 GiB 的诊断；它不能直接与 64 GiB 的 Load 门槛比较。2 亿 Key 正式 Load、4/16 GiB 的 600 秒混合负载尚未验收，不能据此认定命题门槛已通过。

同一构建在 4 GiB、30 万 Key、32 线程下另跑一轮 2 秒预热 + 10 秒 60/30/10 混合负载，得到 56,185 ops/s。它高于 50,000 ops/s 的混合吞吐线，但仅为单轮短诊断，不代表两亿 Key、600 秒正式评测。Load 的 60,000 ops/s 门槛指定 64 GiB 预算，不能拿上表 4 GiB Load 数值直接判定该门槛；64 GiB / 2 亿 Key 的 Load 仍未完成验收。

## Hot-log 空间预分配

同一 NVMe/ext4、AIO + `fdatasync` 构建下，对 `F2_PREFIX_PREWRITE` 做两轮同条件对照。两边均为 30 万 Key、32 线程、4 GiB、`sample_every=16`、`F2_GC_MIN_BATCH=32`、`F2_GC_BATCH_NOTIFY=1`、`F2_PREFIX_READ_OVERLAP=2`；只改变预分配设置。

| 设置 | 两轮 ops/s | F2 数据同步累计时间 | 数据文件同步数 | 提交组数 |
|---|---:|---:|---:|---:|
| 默认开启（未设置变量） | 45,114 / 44,854 | 4.34 / 4.36 s | 9,396 / 9,393 | 9,396 / 9,393 |
| `F2_PREFIX_PREWRITE=0` | 25,600 / 26,048 | 9.22 / 9.06 s | 9,430 / 9,413 | 9,430 / 9,413 |

两边提交组数量相近，关闭预分配后数据同步阶段累计时间约翻倍，表明 ext4 动态分配的工作落入每组 `fdatasync`。段扩展时按 4 MiB 块写零并同步可把这部分分配开销提前；Linux `data-log` 现在默认启用，`F2_PREFIX_PREWRITE=0` 保留为显式关闭项。该差异不能作为相对最初 47.3K 基线的新增提升，因为最初 A/B 已启用预分配。

试过用 `posix_fallocate` 替代预写零，以减少额外写入；在同一 RWF_DSYNC 默认配置下三轮仅得 25,826 / 25,930 / 26,026 ops/s，`flush_ns` 中位数为 9.66 s，较零写预分配下的约 4.50 s 更慢。该路径会留下 ext4 unwritten extents，后续 `O_DIRECT + RWF_DSYNC` 写入要承担 extent 转换成本，因此没有保留。

## 默认批次收集

最初 47K 级 A/B 通过环境变量启用了 adaptive batch collector；未设置这些变量时，collector 维持固定等待，单轮测得 41,776 ops/s、9,488 个提交组。现在 Linux `data-log` 默认启用 adaptive collection、32 请求阈值、50 μs 上限和 batch notification。启用 `RWF_DSYNC`、预分配且不设置任何 F2 环境变量的单轮 30 万 Key Load 为 47,356 ops/s、9,393 个提交组；缩小 correctness 矩阵也在这套默认配置下通过。该单轮用于确认默认路径与已测批次策略一致，不是正式评分或新一轮统计提升。

## 混合负载读写重叠

不设置运行时变量时，4 GiB、30 万 Key、32 线程、2 秒预热 + 10 秒 60/30/10 混合短测，旧的固定独占 gate 单轮为 30,980 ops/s；动态 `UntilPublish` 默认 gate 三轮为 51,376 / 49,738 / 49,572 ops/s，中位数 49,738，GET P99 中位数约 0.688 ms。写者无读者时立即采用独占 gate；有活跃读者时，读者在数据同步期间使用旧索引，写者持久化后再独占发布新索引。`F2_PREFIX_READ_OVERLAP=0` 可强制关闭该重叠。短测仍不能替代 600 秒混合验收，且中位吞吐略低于 50,000 ops/s 门槛。

## 跨组同步合并试验

为减少真实持久化屏障次数，检查了异步 durable writer 的跨组聚合实现。原等待代码的无谓词 `wait_for` 会被每次队列通知提前唤醒；改为等待第二个 job 或 1 ms 超时后，在 NVMe/ext4、30 万 Key、32 线程、`F2_PREFIX_PREWRITE=1`、`F2_GC_ADAPTIVE=0` 下，实际将 15,725 个 job 合并为 10,137 次数据文件 `fdatasync`，平均每次覆盖 1.55 个 job、最多 3 个。独立数据文件同步次数仍高于基线的 9,379 次；提交组平均仅约 19 个操作，AIO 写入和 footer/填充数据增多。吞吐为 21,337 ops/s、Put P99 为 2.75 ms，因此该 pipeline 维持关闭，不作为优化结果。缩小到 1,000 Key 的 basic、overwrite、delete、concurrent、recovery 和 5 轮 SIGKILL crash_recovery 均通过；100,000 Key、10 轮 overwrite 长测因 1 ms 单 job 等待仍在运行时被中断，完整 pipeline correctness 矩阵未完成。

随后用 `RWF_DSYNC=OFF` 的同一 Release 构建复测，以免每个 AIO 写先自行持久化而掩盖 `fdatasync` 合并；控制组保留默认 `RWF_DSYNC=OFF`、关闭 pipeline，pipeline 组开启 durable writer 和 pipeline，二者使用 32 线程、30 万 Key、32 B Key、256 B Value、4 GiB、`F2_GC_MIN_BATCH=32`、预分配和相同同步 API。控制组三轮为 45,160 / 44,983 / 44,721 ops/s，中位数 44,983；每轮数据文件同步数为 9,403 / 9,395 / 9,391，中位数 9,395。pipeline 在 100 μs durable-writer 窗口下三轮为 23,750 / 23,778 / 23,982 ops/s，中位数 23,778；每轮约 18.8K 个提交组、18.2K 次 `fdatasync`，每次同步平均只覆盖 1.03 个 job。通用 adaptive collector 关闭了专用 batch collector，50 μs 收集上限下提交组平均只有约 16 个操作，吞吐约为控制组一半。

把通用收集等待提高到 400 μs 的单轮为 27,413 ops/s、12,468 个提交组、12,467 次 `fdatasync`，最大只合并 2 个 job；提高到 1 ms 后为 16,396 ops/s，9,411 个提交组合并为 9,379 次 `fdatasync`，相对控制组中位数只减少 16 次（约 0.17%），吞吐却下降约 64%。控制组每个提交组已经覆盖约 31.9 个操作，接近 32 个同步调用者能同时提供的上限；在逐条同步 API 和 32 个闭环调用线程下，当前组提交已接近可达到的最少屏障数。

该 pipeline 还有独立的可见性差异：`async_prepare` 在数据同步前调用 `PrefixPublishIndex()`，因此读者可在对应 Put 返回前看到新值。若后续持久化失败，错误处理中的 `PrefixAbortIndex()` 只清除暂存更新，不能撤回已发布的索引项；实例随后 poison 并拒绝读写，但这条失败语义与默认的“持久化后发布索引”路径不同。启用前需要明确并验证失败请求的可见性与恢复语义。跨组 writer 仍关闭：这些测量没有形成有意义的屏障削减，吞吐显著回退，且它改变了发布时序。逐轮日志和 runner 报告见 [跨组同步复测](../analysis/f2-sync-coalesce-retest-20261124/)。

## 同一主机上的 fdatasync 微基准

对连续预写数据执行 `pwrite` + 一次 `fdatasync`，每组数据为 1、2、4、8、16、32、64、128 条，每档三轮。单次同步中位数约 0.46–0.48 ms；至 32 条，单组总时间中位数约 0.486 ms，64/128 条分别约 0.498/0.505 ms。它说明该盘上同步延迟对这些组大小变化不大，但这是裸文件微基准，不包含 F2 apply、索引发布和恢复协议，不能当作 KV 引擎吞吐。

## 与旧结果的关系

- 之前 WSL2/VHDX 上 30 万 Key 的 F2 组提交中位数约 38.4K ops/s；100 万 Key 为约 37.6K ops/s。其存储路径与原生 NVMe 不同。
- 本次 47.3K 是原生 Linux NVMe、30 万 Key 的基线，不能与旧 WSL2 数值直接解释成实现改进；硬件/虚拟化路径和数据规模都变了。
- 本轮另一个 10 秒混合诊断约 50.2K/51.3K ops/s，各一轮且无 Compaction；不足以证明直接接手有收益，更不能代表正式 600 秒混合负载。
- 30 万 Key 工作集约 240 MiB，不能代表 2 亿 Key 的索引页落盘与内存淘汰压力。

## 正式门槛与结论

命题要求 2 亿 Key：32 线程、64 GiB 预算下 Load 平均至少 60,000 ops/s；4 GiB 和 16 GiB 两档各以 32 线程运行 600 秒 60/30/10 混合负载，平均至少 50,000 ops/s。当前无运行时环境变量的默认构建，在 4 GiB、30 万 Key Load 单轮为 47,356 ops/s，在同档 10 秒混合短测三轮中位数为 49,738 ops/s；两者都不是同规模、同运行时长的正式评测单元。64 GiB / 2 亿 Key Load 和 4/16 GiB / 600 秒混合负载均未完成，因此现在的明确结论是**尚未证明通过**。短测不能替代正式门槛。评测要求原文见[命题方完整文档](命题方完整文档.md)，Docs 原始快照为 revision 1471。

功能方面，近期 F2 `data-log` 开发验证的公开检查矩阵为 71 项通过，另有 32 写者三处 SIGKILL 边界 3/3 通过；这只覆盖开发矩阵，不代表完整 14 场景及 2 亿 Key 验收。Compaction 默认关闭或未完成全规模验收、Checkpoint 未接入，均是剩余工作。

原始报告与逐轮数据：

- [原生 Linux 批次接手 A/B](../analysis/f2-batch-takeover-20261124/README.md)
- [fdatasync 批大小扫描](../analysis/f2-performance-rootcause-20261120/native-linux-fdatasync-batch-20261124/README.md)
- [F2 性能根因与历史实验](../analysis/f2-performance-rootcause-20261120/README.md)
- [公开正确性与功能状态审计](../analysis/f2-enabled-verdict-20261114/summary.json)
