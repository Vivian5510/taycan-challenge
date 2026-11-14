# F2 恢复修复与同步写性能审计

被测分支 `fix/f2-recovery-and-sync-audit-20261114`；主干不混入合批实验。本轮先修复尾部恢复，再确认实际同步边界与性能差异。

## 已完成的正确性验证

- 原失败在原二进制上重现，原失败数据库未覆盖。修复后四种尾部页丢失情形4/4 PASS；逐个丢失未确认100KiB写入涉及的26个OS页，26/26 PASS。
- 保留原有CRC/序号/地址链校验。只恢复连续的完整校验前缀；发现不完整数据后不再播放后续记录，若之后还有有效提交标记则按中段损坏拒绝恢复。中段值损坏、footer损坏、整块丢失三例均拒绝，未改成跳过错误继续恢复。
- 71项专项诊断PASS。旧验证脚本硬编码log.log0，开启8MiB分段后先报诊断工具错误；已按真实segment与绝对位置寻址，原失败保留validation目录，新结果独立目录。未放宽断言。
- F2完整功能配置（data-log/posix-aio/fsync/512B/GC ON/8MiB段）原规模Basic、100K恢复、10K并发、100K×10 Compaction、公开100K SIGKILL5轮均PASS。
- 真实GC8项通过：copy期间继续512次前台ACK后强杀，以及复制完成、逻辑GC、checkpoint未发布root、root已发布未删段、hot/cold/index删段后的7个内部阶段强杀；每项重开两次确认ACK前缀。
- 两引擎使用相同74次ACK写入/删除、不关闭/不先checkpoint的故障镜像：真实pending fsync之前暂停，只改变ACK文件尾之后的未同步页；四种镜像各重开两次，双方全部通过。
- RocksDB8.9.1 sync=true/WAL ON/无压缩，通过公开Basic、100K恢复、10K并发、100K SIGKILL5轮。
- 相同实际fsync边界检查：两者均在内核fsync被暂停时不ACK、返回EIO时不ACK；同步后ACK再kill可重开两次保持正确。内核fsync前kill，未知操作可存在或丢失，已确认状态不丢。

## 修复范围

原实现见到完整footer便假定其前部记录完整；同步前后页可独立落盘，因此完整footer不是已完成同步的证明。新代码对数据不完整的尾部保留最后一个完整校验的恢复前缀，随后正常恢复并裁掉未提交尾部。写路径未增加WAL、合批、后台提交线程或持久屏障；这是恢复路径修改。
该规则处理崩溃导致的未同步尾部，不能纠正成功同步后介质又损坏已确认字节；已确认字节被直接截掉的诊断仍校验失败。旧footer格式不变。新修复未完成2亿Key、完整按需/内存大场景、真实物理断电验收；不沿用它们的PASS。

## RocksDB是否真的同步

本地RocksDB官方源码db_impl_write.cc中，MergeBatch/WriteToWAL之后调用WritableFileWriter::Sync，再完成写组。同步请求不能加入非同步组。基线WriteOptions.sync=true且disableWAL=false。HAVE_FULLFSYNC=OFF时macOS官方port把fdatasync映射为fsync；F2同样显式调用fsync。
开发interposer记录的不是API标志，而是实际内核SYS_fsync调用。暂停和EIO注入证实ACK顺序；计数开启时两个引擎使用相同的真实内核fsync，未用成功空返回替代。计数器关闭的独立单元用于观察插桩扰动。初次dlsym interpose递归导致的诊断SIGSEGV保留syscall-smoke，不当成引擎故障；改为调用真实内核后自检成功。

详细归因见[性能解释](findings.md)：大部分差距来自同步合批，剩余主要在逐条HybridLog flush/同步路径；尚未优化或恢复main的group commit。
## 固定性能实验

百万唯一Key、32/256B、4GiB、同seed/同共享驱动；每格新库，逐格运行，无正确性worker重叠。装载测完整百万条且只统计成功返回的调用。线程1/32；RocksDB默认和max_write_batch_group_size_bytes=1控制组（单次batch本身不拆分，后继调用不能并组）；F2不合批。计数器开/关分别保留。

| 单元 | ops/s | 数据/WAL sync次数 | ops/sync | 同步累计耗时 s |
|---|---:|---:|---:|---:|
| f2-t1-default-audit | 11,978 | 1,000,001 | 1.00 | 25.91 |
| rocksdb-t1-default-audit | 39,494 | 1,000,000 | 1.00 | 19.35 |
| rocksdb-t1-group1-audit | 39,832 | 1,000,000 | 1.00 | 18.29 |
| f2-t32-default-audit | 11,827 | 1,000,001 | 1.00 | 25.50 |
| rocksdb-t32-default-audit | 210,396 | 62,425 | 16.02 | 1.78 |
| rocksdb-t32-group1-audit | 34,163 | 1,000,000 | 1.00 | 19.56 |
| f2-t32-default-plain | 12,734 | 未插桩 | — | — |
| rocksdb-t32-default-plain | 141,338 | 未插桩 | — | — |

性能矩阵状态：PASS。这些是本机同口径实验，不代表Linux/NVMe正式分数或真实断电验证。

[原始性能](sync-performance/results.json) · [真实同步边界](syscall-boundaries/results.json) · [专项恢复](validation-segment-aware/results.json) · [公开F2](public-enabled/results.json) · [公开RocksDB](rocks-correctness/results.json) · [每页丢失矩阵](page-hole-matrix/results.json)。
