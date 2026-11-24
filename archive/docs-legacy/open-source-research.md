# 开源实现调研：Dragonfly、FASTER/F2、Garnet/Tsavorite

核对日期：2026-10-27。三个只读源码副本位于 research/：

- Dragonfly：2cd7a2ebaf8c6ee3724c6aa1883abd5ff514b30f
- FASTER/F2：332ea7da532a688acf648ee60c4f63a1caeaf1f7
- Garnet/Tsavorite：a371e041f842e47e2839d82e696f63e56762f66

本文只记录源码事实和可组合的机制，不预定 Taycan 最终路线。

## Taycan 设计文档中的图示补充

设计文档页面包含 5 张关键图。图示比正文多给出了当前 Taycan 方案的具体边界：

- 用户接口包括 `Put`、`BatchWrite`、`Delete`、`Get`、`Scan`、`PrefixScan`、`GetUpdateSince`、`GetCurrentSequenceNumber`、`GetOldestSequenceNumber`、`ManualCompact` 和 `CheckPoint`。
- 内存层是 `BucketGroup -> Bucket`。每个 Bucket 有 `SpinLock` 和 `SkipList/RB-tree`，因此它不是只有 fingerprint/address 的无序 Hash slot；Bucket 内的有序结构是 PrefixScan 的基础。
- 写路径是“请求 -> active file 追加记录 -> 将 Key/Value position 插入 Bucket 索引 -> 返回结果”。读路径是“请求 -> 查询索引 -> 取出 Value position -> 查询文件 -> 返回结果”。图中没有定义索引发布与数据持久化之间的崩溃原子性。
- 文件管理器至少包含 `index file (.idx)`、`block file (.bf)`、`append file (.af)`、`active file`、`Current`、`manifest` 和 `LOCK`。`.idx` 被描述为只包含索引信息的文件；Recovery 图强调用 Compaction 生成的 index file 加速恢复，而不是把它描述成运行时按需加载的磁盘索引。
- Compaction 的图示流程是：按无效数据比例选择文件，生成新的 block file，再更新内存索引；旧文件何时删除、读者如何排空、崩溃时如何选择新旧索引版本，正文没有展开。
- SSD 层列出了 SPDK、io_uring 和 `pread/pwrite` 三种 I/O 方向，说明 I/O 后端是可替换的，但没有给出 Direct I/O、对齐、批量提交和 fsync 边界。

这意味着设计文档中的“哈希引擎”准确地说是“哈希路由 + Bucket 内局部有序索引 + 追加/压缩文件”。它与 F2 的两级磁盘 Hash Index 只有部分相似：F2 可以参考冷索引按 chunk 取数，不能直接替换 Taycan 的 Bucket 内有序结构。尤其需要确认 PrefixScan 如何定位涉及的 Bucket；如果普通 Hash 会把相同前缀分散到多个 Bucket，图示结构本身还不足以实现全量前缀扫描。

文档还把 BatchWrite 原子恢复、快照和事务列为需求或 TODO，没有给出协议。因此，后续设计必须单独定义 `Current/manifest/.idx/.af/.bf` 的版本关系、提交点、恢复顺序，以及 Compaction 与前台读写交错时的地址发布规则。

## Dragonfly

Dragonfly 的核心是 shared-nothing 分片和 DashTable。DashTable 使用 directory → segment → bucket 的结构，segment 内采用开放寻址、fingerprint、邻居 bucket 和 stash。插入失败时只 split 相关 segment，不整体扩容。查找先用 fingerprint 筛槽，再用完整 Key predicate 精确匹配。

主要源码：

- research/dragonfly/src/core/dash.h
- research/dragonfly/src/core/dash_internal.h:105-230
- research/dragonfly/src/core/dash_internal.h:1146-1205

可借鉴的是紧凑 metadata、segment 作为扩展边界、fingerprint 只做初筛。题目要求完整 Key 比较，这和 DashTable 的 predicate 查找原则一致。

Dragonfly 的 tiering 只负责 Value：

- 主索引仍然在内存 DashTable；
- Value 序列化到外部文件；
- external/cool 状态由内存对象记录；
- Read 通过异步 Future 读取外部段；
- ReclaimMemory 从 cool queue 释放对象；
- Warmup 把 Value 重新加载；
- MaterializeForIndexing 让需要内部结构的 Value 回到可读状态。

主要源码：

- research/dragonfly/src/server/tiered_storage.h:84-140
- research/dragonfly/src/server/tiered_storage.cc:516-563
- research/dragonfly/src/server/tiered_storage.cc:807-950

它不能直接解决 Taycan 的索引持久化，因为 Dragonfly 的全部 Key 索引仍是内存权威结构。可以借鉴异步读取、回收目标、backpressure、pending read/cancel/warmup 和小对象 page/bin 合并。

## FASTER 原始 KV

FASTER KV 的基本路径是：

查询 Key → 内存 Hash Index → logical record address → HybridLog 的内存页或磁盘页 → 完整 Key 比较 → Value。

Hash Index 桶项保存 Hash tag 和逻辑地址；记录头包含 previous address，可以沿 Hash chain 查找碰撞记录和旧版本。逻辑地址可拆成 page 和 offset，记录页可以在内存和外部设备之间移动。

主要源码：

- research/FASTER/cc/src/index/mem_index.h
- research/FASTER/cc/src/core/address.h
- research/FASTER/cc/src/core/faster.h

可借鉴：

- 统一逻辑地址；
- append-only 日志和 mutable tail；
- 旧页变只读后刷盘；
- 索引保存小路由信息，完整 Key 在记录中比较；
- checkpoint 保存索引状态和日志地址范围。

限制是原始 FASTER 的 Hash Index 仍主要驻留内存，因此不能直接满足 2 亿 Key 下的磁盘索引约束。

## F2 ColdIndex

F2 在 FASTER 上增加 hot/cold 两层：

- hot-log index：内存 Hash Index；
- hot log：活跃写入和热记录；
- cold-log index：两层 Hash Index；
- cold log：主要在磁盘；
- read cache：保存读热、写冷的磁盘记录。

ColdIndex 的关键路径是：

Key Hash → chunk_id + tag → 内存 chunk directory → 磁盘 hash chunk → chunk 内 bucket entry → cold log record address。

主要源码：

- research/FASTER/cc/src/index/cold_index.h:25-110
- research/FASTER/cc/src/index/cold_index.h:340-455
- research/FASTER/cc/src/core/f2.h:39-132
- research/FASTER/cc/src/core/f2.h:680-780

cold_index.h 中的 ReadIndexEntry 会根据 hash 计算 chunk_id、index_in_chunk 和 tag_in_chunk，再异步读取磁盘 chunk；未完成时返回 Pending。TryUpdateEntry/RmwIndexEntry 用来更新索引项。F2 的 Compaction 使用 ConditionalInsert、地址检查和 Hot/Cold 分开回收，避免后台搬迁覆盖前台新版本。

这是目前最接近 Taycan 题目“磁盘索引按需加载”的公开实现。但不能原样搬用，因为 F2 还有 Hot/Cold store、read cache、epoch 和自己的 checkpoint 体系，而 Taycan 只有 Put/Get/Delete、严格 RSS 和成功返回前同步持久化。

## Garnet/Tsavorite

Garnet 是 RESP cache-store，底层 Tsavorite 是 FASTER 的演进分支。Tsavorite store 包含：

- latch-free Hash Index；
- 跨内存和磁盘的 record log；
- 内存 circular buffer 页；
- 旧页刷到外部 device；
- 逻辑地址指向内存页或磁盘页；
- checkpoint/recovery；
- revivification 复用已删除或过期的内存槽位。

本地论文摘录位于 /tmp/garnet-paper.txt:276-330、410-430、482-535。仓库实现位于 research/garnet/libs/storage/Tsavorite。

Garnet 的提交策略也很有参考价值：

- CommitFrequencyMs=0：每个操作立即发起 commit；
- CommitFrequencyMs=-1：手动提交；
- WaitForCommit=false：默认不等待持久化就返回；
- WaitForCommit=true：发送响应前等待 AOF commit；
- Tsavorite LogCommitPolicy 还提供 Default、MaxParallel 和 RateLimit，并在被抑制的 commit 后安排 retry。

源码：

- research/garnet/libs/server/Servers/GarnetServerOptions.cs:154-209
- research/garnet/libs/server/Resp/RespServerSession.cs:1534-1547
- research/garnet/libs/server/AOF/GarnetLog.cs:472-555
- research/garnet/libs/storage/Tsavorite/cs/src/core/TsavoriteLog/LogCommitPolicy.cs:47-197

可借鉴：

- mutable region + immutable/read-only old pages；
- 逻辑地址；
- epoch/version 保护；
- revivification 和 free list；
- 可配置提交前沿；
- checkpoint 的 Drain/Flush 状态机。

限制是 Tsavorite 的运行时 Hash Index 仍然是内存结构，tiering 主要针对日志和值，不等于题目要求的持久化磁盘索引。

## 按 Taycan 场景组合的候选组件

这不是最终架构，只是候选组件池。

### 哈希层

Dragonfly 的 segment/open addressing/stash 可用于降低内存 metadata；FASTER 的 tag + address + previous address 可用于记录链和逻辑地址；完整 Key 比较必须保留。

### 数据层

Taycan AF/BF 可以采用 FASTER/Tsavorite 的逻辑地址日志思想：

- AF 作为追加和提交目标；
- BF 作为稳定的只读压缩段；
- 地址使用 file/segment/page/offset，而不是裸文件指针；
- BF 搬迁时用 expected version + expected old address 做条件发布。

### 索引层

F2 ColdIndex 是最直接的候选：

- 内存保留 chunk directory 和近期增量；
- 磁盘保留 hash chunk；
- 查询按 chunk 读取；
- chunk 缓存有明确 RSS 上限；
- chunk 更新要有版本或地址条件。

### 提交层

Garnet 的提交前沿可转成 Taycan 的两阶段状态：

- durable log end：数据记录和操作日志已经同步；
- published index version：索引版本已经发布。

ACK 前必须到达 durable log end；恢复从最后一个完整索引版本回放之后的增量。Group Commit 不能只在积累足够请求后提交，因为公开 crash worker 一次只允许一条未确认操作。

### 回收层

Dragonfly 的 cool queue/backpressure、F2 的 conditional insert、Tsavorite 的 epoch/revivification、Taycan 的 CURRENT/Manifest 可以组合成：

- 前台读取或写入先 pin 当前索引/数据版本；
- 后台搬迁时检查 Key、版本和旧地址仍匹配；
- 新索引和新数据完整持久化后发布；
- 等读者离开后再回收旧文件和旧索引页。

## 当前结论

现在不应该直接宣布采用某一套架构。源码调研把候选范围缩小到三个值得独立验证的组件：

1. F2 风格的磁盘 cold index chunk；
2. FASTER/Tsavorite 风格的逻辑地址追加日志；
3. Garnet/Dragonfly 风格的提交前沿、异步 I/O、回收目标和 backpressure。

真正需要 benchmark 和故障验证的问题是：

- chunk 大小对冷读 P99 和 RSS 的影响；
- 内存增量大小对读放大和恢复时间的影响；
- 单请求同步、短 Group Commit、按字节/时间触发提交的写 P99；
- 条件地址更新能否覆盖 Compaction 与前台 Put/Delete 交错；
- 回收期间是否会超出 RSS；
- checkpoint/Manifest 是否能把索引、日志和数据绑定为同一个一致版本。

三份源码副本是只读调研副本，参赛包和引擎代码没有修改。
