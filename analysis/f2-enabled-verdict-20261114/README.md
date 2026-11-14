# 启用必要配置后的 F2 验收与性能结论

当前总体结论：**NOT_PASS**。被测主干 `144a5ec`，引擎源码未改动；开启 data-log 同步、durable compaction、热转冷、冷日志与冷索引回收。主诊断预算 4 GiB。

## 正确性

| 场景 | 状态 | Key 数 | RSS峰值 MiB | 耗时 s | 原因/边界 |
|---|---|---:|---:|---:|---|
| basic | PASS | 100000 | 20.84 | 0.85 | 原场景断言通过 |
| sequential | PASS | 1000000 | 1446.31 | 117.61 | 原场景断言通过 |
| random | FAIL | 5000000 | 1559.22 | 600.05 | timeout |
| overwrite | PASS | 100000 | 578.20 | 145.03 | 原场景断言通过 |
| delete | PASS | 100000 | 275.73 | 60.95 | 原场景断言通过 |
| mixed | PASS | 1000000 | 1566.91 | 439.32 | 原场景断言通过 |
| recovery | PASS | 100000 | 233.33 | 28.40 | 原场景断言通过 |
| checkpoint | SKIP | 131072 | 1096.53 | 78.06 | 未执行或可选接口未实现 |
| concurrent | PASS | 10000 | 109.61 | 15.95 | 原场景断言通过 |
| demand_load | FAIL | 16000000 | 1561.05 | 353.62 | resource_preflight_original_16M_live_dataset_exceeds_2GiB_local_database_cap; stopping partial run, not an engine assertion |
| memory_limit | NOT_RUN | 20000000 | 0.00 | 0.00 | Preflight: final live uncompressed KV bytes alone exceed local 2GiB database cap; no smaller dataset substituted |
| compaction | PASS | 100000 | 1375.38 | 119.19 | 原场景断言通过 |
| crash_recovery | PASS | 100000 | 153.95 | 86.60 | 原场景断言通过 |
| compaction_crash | FAIL | 100000 | 1095.02 | 179.04 | 准备阶段超过本地磁盘保护后中止；没有注入崩溃，不算通过，也不能当数据错误 |

使用原生公开 C++ 断言与各场景默认 Key 数/Key Value 大小；仅按用户选择把内存预算设为 4 GiB，严格监控预算×110%。这是本地验收，不是正式 Linux VmRSS。单库 2 GiB、空闲20GiB保护；开发超时与保护中止不冒充引擎断言失败。

## 明确的能力错误

**未确认尾部部分页丢失时恢复失败。**准备67次已确认操作，在下一次100KiB写入的同步前强杀，再只改变未确认尾部：保留后页/提交标记、丢失前部未同步页。67次已确认操作的全部字节保持不变；重开却报 `malformed data record inside committed prefix`。原确认状态本应仍可恢复，未确认操作允许丢失。该100KiB Value属于SDK公开Basic覆盖的接口范围，超出正式性能负载的256B；本模型没有证明固定256B负载也会以同样方式失败。

四种镜像情形中：全部未确认尾部丢失、全部保留、尾页丢失通过；前页丢失但后页/标记保留失败。此为确定性磁盘镜像模型，不是真实断电或原生SIGKILL脚本；不能说原生SIGKILL也失败。[原始记录](pending-page-loss/results.json)。

开启配置后的实际 `compaction` 通过100,000 Key×10轮并发更新、空间比例、最终值及重开校验；最终目录372,975,729B，存活逻辑274,130,592B。GC trace 记录19个完整周期及约14.28GB累计旧段删除，磁盘索引也实际参与；不能继续称本轮没有开启回收。

运行期预算尚需大场景验收，不能把小规模RSS未超限等同2亿Key达标。可选SDK Checkpoint 未实现与内部恢复checkpoint是两件事。

## 同条件性能

固定32线程、1,000,000唯一Key、32/256B、4GiB、seed20261021，F2 data-log fsync-after-flush，RocksDB8.9.1 sync=true/WAL开启/无压缩（HAVE_FULLFSYNC=OFF）；相同共享驱动源码，每格新库，2秒预热、10秒测量、2轮反转引擎顺序。装载测完整百万条，失败不重试替换。

RocksDB为结构不合命题的性能参照，不能当合规候选；F2开启回收、移除group commit。F2 F_NOCACHE，RocksDB保留默认buffered SST与block cache，因此不是冷盘算法对照。

| 负载 | F2 ops/s（完成轮） | RocksDB ops/s（完成轮） |
|---|---:|---:|
| load | 10,574（2/2） | 未形成两轮有效成绩；有效观察126,887（1/2） |
| read | 未形成两轮有效成绩；有效观察692,512（1/2） | 未形成两轮有效成绩；有效观察780,650（1/2） |
| update | 未形成两轮有效成绩；有效观察9,087（1/2） | 161,980（2/2） |
| contest_mix | 20,713（2/2） | 235,746（2/2） |

第一轮受调度重叠影响的四个格子已排除，见[监控与调度记录](README-diagnostic-notes.md)。为便于直接比较，报告另保留[第二轮完整对照](clean-second-round.json)，不把单轮观察伪装成两轮中位数。

性能完成状态：PASS。这些数据只描述该配置下的本机吞吐，不说明F2已通过命题全部验收。[原始性能](performance/results.json) · [固定计划](comparison-plan.json)。

完整公开原始输出与监控见 [第一组](public-enabled/results.json)、[覆盖/删除/混合/Checkpoint](public-small/results.json)、[随机](public-random/results.json)、[按需加载](public-demand/results.json)、[内存场景预检](public-memory/results.json)。失败数据库原地保留；成功库清理但保留日志/哈希/文件清单。不自动修复再覆盖本轮失败。
