# 从 RocksDB 复用同步提交实现

后续 F2 内核接入已经完成，最新实现与真实 KVEngine 结果见
[F2 接入报告](../analysis/f2-rocksdb-wal-20261030/README.md)。本篇下文记录接入之前的源码研究和组件验证范围。

研究版本为本地 RocksDB 8.9.1，提交 `49ce8a1064dd1ad89117899839bf136365e49e79`。源码保留在 `research/rocksdb-v8.9.1/`，本轮没有修改 RocksDB 或 FASTER 的上游研究副本。

## 已确认的实现

| 环节 | RocksDB 源码 | 对 F2 的直接意义 |
|---|---|---|
| 注册请求、leader 交接、唤醒 follower | `db/write_thread.cc:401`、`:437`、`:662` | 可以直接复用 WriteThread；它是调用线程间的写协调器，不是为每条写请求派生后台线程 |
| 选择一组兼容请求 | `db/write_thread.cc:474` | sync 请求不能加入不执行 sync 的 leader；按 WAL、限流和组大小等条件分组 |
| 合并真实 WriteBatch | `db/db_impl/db_impl_write.cc:1273` | 单请求直接复用原 batch，多请求才合并；完整 Put/Delete、序列号保存在成熟 WriteBatch 格式中 |
| 记录分块与 CRC32C | `db/log_writer.cc:65`、`db/log_format.h:45` | 32 KiB 是记录分片边界，不是每次请求固定写 32 KiB；追加实际数据，不像当前 HybridLog 原型每次重写 1 MiB 尾页 |
| flush 和同步分工 | `file/writable_file_writer.cc:432` | flush 将用户态缓冲写入文件，sync 再请求稳定存储；WriteOptions.sync=true 必须等待后者 |
| WAL 组写及同步 | `db/db_impl/db_impl_write.cc:1365` | 先 AddRecord，再同步相关 WAL；正常单 WAL 情况下一组一次日志同步 |
| 目录同步条件 | `db/db_impl/db_impl_write.cc:1266`、`:1434` | 新 WAL 的文件名需要持久化时同步目录，不对每个 Put 遍历、同步全部目录 |
| 写入内存、完成请求 | `db/db_impl/db_impl_write.cc:559`、`:645` | WAL 成功后再更新内存状态，发布序列号、传播结果并唤醒请求；这些完成条件缺一不可 |
| 校验并恢复 WAL | `db/db_impl/db_impl_open.cc:1191`、`db/log_reader.cc` | 复用 Reader 对分片、CRC 和残缺尾记录的处理；由 F2 实现具体重放目标 |
| 保留和删除旧日志 | `db/db_impl/db_impl_files.cc:297` | 删除依赖已发布的恢复状态和最小需保留日志号，不能只因本次日志同步成功就截断旧日志 |

RocksDB 标准写队列不是完全并行的 WAL 追加：它在一个写组中由一个 leader 维持 WAL 顺序，其他请求继续排队。`WriteThread` 的注释明确不依赖 DB 大锁来维持这项正确性。写日志有序和“让所有 Get 都拿一个跨磁盘 I/O 的写锁”是两回事。

`EnterAsBatchGroupLeader` 收集已经排队的请求，本路径没有为凑批强制等待 100 微秒。一个调用线程、每次一个同步 Put 时仍然只有一条请求，不能把尚未持久化的写提前返回来制造批量。

## 已完成的直接复用验证

代码在 `experiments/rocksdb-commit-reference/`。本轮直接链接现有 RocksDB 静态库，原样使用以下组件：

- `WriteThread`：排队、组装、leader 交接和唤醒；
- `WriteBatch` / `WriteBatchInternal`：操作、完整字节数据、序列号及组内合并；
- `log::Writer`、`WritableFileWriter`、POSIX 文件实现：追加、分片、CRC、缓冲和同步；
- `log::Reader`：校验、重组和读取持久日志。

集成代码只负责将这些组件接在一起、保留 F2 apply 的回调边界、同步新日志目录，以及提供故障观测。本轮既没有复制 RocksDB 的 LSM 数据引擎，也没有重新写一份 WAL 编解码和写请求队列。

最终探针 15 项通过，包括 1/4 线程各两次同步写和全量恢复、SIGKILL 后删除/大值恢复、两次恢复一致、残缺尾记录、CRC 损坏拒绝、同步/追加错误的全组失败和后续拒绝写。记录在 [结果](../analysis/rocksdb-commit-source-study-20261030/final/results.json)。失败注入中所有写都失败，成功操作数和吞吐是 0，不将失败请求数当作吞吐。

每单元 1,024 次 Put，Key 32 B / Value 256 B，真实同步调用，一线程一次最多一个请求在途：

| caller | 同步文件次数 | 同步目录次数 | 追加字节 | 说明 |
|---|---:|---:|---:|---|
| 1 | 1,024 | 1 | 318,527 | 一条写一次同步，没有额外 checkpoint 元数据 |
| 4，第 1 轮 | 475 | 1 | 308,096 | 由原生 WriteThread 自动分组，没有人为等待凑批 |
| 4，第 2 轮 | 477 | 1 | 308,134 | 平均约 2.15 条操作共用一次同步，不是人为构造 256 条异步批次 |

本机两轮单线程约 257/263 次/s，4 caller 约 558/559 次/s。这是日志组件测试，**不包含 F2 数据更新、索引、checkpoint 或日志回收，不能充当 F2 性能**。macOS 的库编译启用了 `HAVE_FULLFSYNC`，`env/io_posix.cc:1402` 实际调用 `F_FULLFSYNC`；并未用仅 OS page cache 的写入来冒充稳定持久化。上述结果也不能说明目标 Linux/NVMe 的性能上限。

## F2 集成的具体边界

普通前台写必须是：复用 WriteThread 注册当前请求；leader 为合并 WriteBatch 分配有序日志序列号；WAL 追加并同步；在同样的序列顺序下调用 F2 Upsert/Delete 并等待 pending 操作完成；最后释放本组请求。日志模块和完成协调由 F2 实例持有，KVEngine 只做方法映射。

`Committer::Apply` 已留出 WAL 稳定之后、请求返回之前的调用位置。单纯 WAL 落稳还不够：必须将数据发布给 F2，使已经成功的 Put 能被随后 Get 读到。若 F2 apply 失败，即使日志已经落稳也不能返回成功；该未知操作允许在重开时被重放。相同 Key 的顺序必须在运行时与恢复时一致，不能直接照搬 RocksDB 的并发 MemTable 更新入口到 F2。

Checkpoint 和日志回收属于 F2 特有集成，不能靠搬运 RocksDB DBImpl 解决：

1. 恢复根必须保存 F2 checkpoint token、日志代次及其已覆盖的日志序列号。首次正确实现应在明确的写入边界取得完整前缀；F2 的多 session CPR commit point 不能未经证明就取最大序号当作全局已覆盖前缀。
2. 新的 F2 checkpoint、依赖和恢复根均稳定以后，才能删除被覆盖的旧 WAL。普通 Put 不发布新 checkpoint、不扫描目录、不重写恢复根。
3. 重开先恢复 F2 checkpoint，再使用 RocksDB Reader 校验日志，按序只重放未覆盖操作；重放时不能再次生成 WAL。恢复继续写入应开启新日志代次，不能覆盖或直接追加到残缺记录后。
4. 日志轮换/恢复根发布/旧日志删除三个边界都要进行崩溃注入。尚未完成这些集成前，不将日志组件 PASS 标记为 F2 crash_recovery PASS。
5. 是否让低频 checkpoint 完全后台化，需要证明 checkpoint 对应前缀和在线迁移的关系；不能再次用“后台”二字掩盖未解决的正确性边界。

## 本轮对旧实现的修正

此前每组 HybridLog checkpoint、递归目录同步和根发布的原型不适合作为前台同步提交机制。缩小日志页只能改变尾页写放大，无法消除这个协议成本；上一轮临时放宽页大小范围的修改已经撤回，没有继续这个调参实验。

现有 `commit-log` 实验虽然也做了日志先持久化、后更新 F2，但其队列、格式、CRC 和 Reader 是自行实现。后续 F2 集成应复用本次验证过的原生组件，替换这些重复实现，而不是继续维护数套自定义 WAL。

RocksDB `DBImpl::WriteImpl` 本身还依赖 VersionSet、ColumnFamily、MemTable、错误处理和日志回收状态。可以直接复用本轮组件；不能只搬一个函数就声称获得整套 DB 的正确性保证。当前调用的是固定版本的内部 C++ 接口，构建必须固定对应源码、编译参数和库哈希。上游的版权声明和 `LICENSE.Apache`、`LICENSE.leveldb` 保留在研究副本中，若后续提取源码随包发布应一并保留来源及适用声明。
