# 同定位的磁盘 KV 对比基线

2026-10-28 修正：本命题的主对比对象应是嵌入式磁盘 KV 引擎，要求数据与索引能够在受限内存中工作，并在 Put/Delete 成功返回前完成持久化同步。Dragonfly/Garnet 的公开内存服务成绩不作为本命题的主基线；它们保留为数据结构、并发和测量方法参考。

## 主对比对象与配置条件

**RocksDB 是优先落地的工程基线。** 它的 LSM/SST、持久化索引、WAL、缓存和后台 Compaction 可以覆盖本题需要评估的主要成本。对比必须设置 `WriteOptions.sync=true`、`disableWAL=false`，每次调用写一个 Key。索引/过滤器可以纳入受限 block cache，但 memtable、table reader、后台任务及分配器也消耗内存，最终必须看同一口径的 VmRSS。缓存容量不等于整个进程的内存上限。

依据：[同步写定义](https://github.com/facebook/rocksdb/blob/4c38c45ac10a73ff933b20610925da828e52c8ae/include/rocksdb/options.h)、[索引/过滤器缓存选项](https://github.com/facebook/rocksdb/blob/4c38c45ac10a73ff933b20610925da828e52c8ae/include/rocksdb/table.h)、[WriteBufferManager](https://github.com/facebook/rocksdb/blob/4c38c45ac10a73ff933b20610925da828e52c8ae/include/rocksdb/write_buffer_manager.h)。

**Badger 是数据与 Value Log 配合的另一条工程基线。** 必须设置 `WithSyncWrites(true)`，一个逻辑 Put/Delete 使用一个 Key 的事务，并等待 `Commit()` 完成。需要给 `IndexCacheSize` 设置正的有限值；该版本默认 0 表示所有表索引留在内存，不能把默认配置当作已经满足索引内存约束。memtable、block cache、映射驻留页和 Go runtime 一并计入 VmRSS。

另一个容易忽略的条件是 **ValueThreshold**：核对版本默认值为 1 MiB，128 B Value 会进入 LSM。因此“用了 Badger”不能直接等同于“测了 Value 分离”。应分别记录默认布局，以及显式降低 threshold（例如 64 B）的候选布局；两者各自跑同一负载和内存约束，不预先宣称哪种更快。

依据：[实际默认值与配置说明](https://github.com/dgraph-io/badger/blob/78b99b8b0f014b539455186404697a0ff58c55f2/options.go)、[Commit 实现](https://github.com/dgraph-io/badger/blob/78b99b8b0f014b539455186404697a0ff58c55f2/txn.go)。

RocksDB 已增加适配器，位于 `benchmarks/baselines/rocksdb`，使用 Ubuntu 24.04 仓库提供的 **8.9.1** 作为首个本地工程基线；上述新版本源码用于核对机制，不能混称为实际被测版本。压测程序的 `--describe` 输出编译时引擎名、版本及同步/内存策略，报告另存动态库哈希。Badger 仍是待接入候选。

两个构建复用同一负载源文件，`run_comparison.py` 现在顺序驱动 F2 原生适配器与 RocksDB，检查参数匹配。单引擎报告仍为 `cross_engine_comparison=false`，对比控制器单独生成 `comparison.json`。F2 原生完成与同步 RocksDB 的持久化边界不同，参数一致仍不代表吞吐可以直接排名。此前自研引擎和冻结比较队列已退休，原始报告保留。

## 其他实现各自能说明什么

- **F2：磁盘索引的架构参考。** 它的 `ColdIndex` 明确是磁盘两级 Hash Index，和本题比普通 FASTER 内存 Hash Index 更接近。但现成 benchmark 的异步操作完成与 checkpoint 协议，不能直接视作逐条同步持久化 ACK。它可以帮助设计索引/冷热路径；没有补齐相同持久化语义前，吞吐不进入主对比表。
- **LMDB：可选的 B+tree 路线对照。** 使用正常同步 commit，不启用 `MDB_NOSYNC`、`MDB_NOMETASYNC` 或异步映射同步选项。它只有一个活动写事务，并且数据库通过 mmap 访问；驻留的文件映射页仍计入本题 VmRSS，map size 也不是 RSS 预算。因此需要单独验证它能否满足本题口径，再决定是否列入同预算成绩。
- **Bitcask：追加日志、Hint/Merge 的结构参考。** 它确实面向磁盘，但 keydir 在内存中的特点不符合本题冷索引按需换入的核心要求；它的默认同步策略也是 `none`。不能因为同属磁盘 KV 就把默认成绩当作完全可比基线。
- **Taycan 原实现：若后续获得可运行版本，应优先补入同条件对照。** 目前拿到的是设计文档及接口/测试包，不能从文档推出原实现的性能数字。
- **Dragonfly、Garnet、普通 FASTER：保留实现与方法参考。** 能借鉴分片、指纹、日志、提交前沿和延迟观测；此前引用的公开基准配置不作为本命题同定位的性能成绩依据。

依据：[F2 ColdIndex](https://github.com/microsoft/FASTER/blob/332ea7da532a688acf648ee60c4f63a1caeaf1f7/cc/src/index/cold_index.h)、[F2 benchmark](https://github.com/microsoft/FASTER/blob/332ea7da532a688acf648ee60c4f63a1caeaf1f7/cc/benchmark-dir/benchmark_f2.cc)、[LMDB 接口与约束](https://github.com/LMDB/lmdb/blob/74dd6c3ec648c31857fe0a100f11c2a34619e2d7/libraries/liblmdb/lmdb.h)、[Bitcask keydir 实现](https://github.com/basho/bitcask/blob/d84c8d913713da8f02403431217405f84ee1ba22/c_src/bitcask_nifs.c)、[Bitcask 同步策略](https://github.com/basho/bitcask/blob/d84c8d913713da8f02403431217405f84ee1ba22/priv/bitcask.schema)。

## 如何使用同类产品的公开基准数据

RocksDB 的 `db_bench` 源码直接提供了装载、随机读、覆盖、删除等磁盘引擎负载；默认 `num=1000000`、`key_size=16`、`value_size=100`、`batch_size=1`、`sync=false`。这些默认值与本题不完全相同，尤其不能漏掉同步写和 Key/Value 大小的统一。

**`fillsync` 会把请求数除以 1000。** 源码的分支先执行 `num_ /= 1000` 再开启同步。因此传 `--num=1000000 --benchmarks=fillsync` 实际不是完整百万条同步写。应使用完整数量的 `fillseq` 加 `--sync=true`，或用统一适配器驱动。核对操作数是验收的一部分，不能只看命令行里的 N。

以下是供后续落地的单线程语义核对命令，尚未在本地编译执行；它不是已经满足 VmRSS 预算的完整配置：

```sh
db_bench --benchmarks=fillseq --num=1000000 \
  --key_size=64 --value_size=128 --batch_size=1 --threads=1 \
  --sync=true --disable_wal=false --compression_type=none \
  --db=/var/lib/taycan-baselines/rocksdb-sync
```

RocksDB 的随机写生成也未必等于“每个唯一 Key 写一次”；跨产品正式比较应使用统一负载，以实际完整装载 Key 数和命中率为准。

依据：[db_bench 参数与 fillsync 分支](https://github.com/facebook/rocksdb/blob/4c38c45ac10a73ff933b20610925da828e52c8ae/tools/db_bench_tool.cc)。

Badger 官方的 [badger-bench](https://github.com/dgraph-io/badger-bench/tree/db150ce8b6292257786ba814adad01ef2c8e660f) 提供过与 RocksDB、LMDB、Bolt 的基准和原始日志，确实比内存缓存服务更适合用来核对场景。但 README 的环境仍基于 RocksDB 5.1.4，不能把历史成绩直接设为当前引擎的目标。要先核对版本、同步语义、线程、批大小、数据量和内存约束。

原始日志也显示为什么“同类产品”还不足以直接比较：`BENCH-rocks.txt` 使用过 2.5 亿 Key、128 B Value，但两个被测进程的峰值驻留内存并不相同；随机读输出还出现了大量未找到 Key 的情况。`BENCH-lmdb-bolt.md` 则记录过为避免逐次同步而使用 `NoSync`。因此这些数据用于发现需要覆盖的规模、内存与同步条件，不能直接拿其中的胜负比例来评价本题同步写引擎。

依据：[RocksDB/Badger 原始日志](https://github.com/dgraph-io/badger-bench/blob/db150ce8b6292257786ba814adad01ef2c8e660f/BENCH-rocks.txt)、[LMDB/Bolt 原始日志](https://github.com/dgraph-io/badger-bench/blob/db150ce8b6292257786ba814adad01ef2c8e660f/BENCH-lmdb-bolt.md)。

本地已保存这些主来源的固定 commit、完整源文件及 SHA-256，见 [来源清单](../research/benchmark-references/disk-kv-sources.json)。

## 本地压力参数的修正

- 百万 Key、64 B Key、128 B Value 来自 SDK；RocksDB 的默认百万条可作规模交叉核对，具体字节数以题目统一。
- reference 的 30 秒也可直接在 F2 C++ `benchmark.h` 的 `kRunSeconds` 中核对；它只是观察窗口，不代表借用了同持久化语义的成绩。
- pressure 的 1600 万 Key／256 MiB 来自 SDK demand_load；测量窗口改为 600 秒，与题面混合负载讨论一致，给持续覆盖与后台回收更长的观察期。
- 2 亿 Key、4/16 GiB、600 秒仍按题面讨论保留。
- 1/2/4/8 线程、预热和重复 3 轮属于本地方法，需要根据测评硬件调整并完整记录。

各引擎必须统一数据和分布、调用并发、写入批大小、ACK 持久化语义、OS cache 策略、磁盘、压缩策略和整个进程的 RSS 口径；所有数据/索引/WAL 都计入空间。若某引擎超预算，应保留该失败，不能给它额外内存后仍称同预算比较。
