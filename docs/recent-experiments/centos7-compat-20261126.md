# CentOS 7 提交包兼容修复与短混合负载

分支 `experiment/centos7-package-fix-20261126` 以原包提交 `45f5130` 为基点，修复源码位于 `code/submission-engine/`。通过 `python3 script/package_centos7_engine.py` 使用原始官方 SDK 的 `package.sh` 打包。

修复包括 libuuid 的 strndup 特性检测、GCC 8.3 原子对象构造、WAL 的 `O_DSYNC` 兼容路径，以及 FASTER 原生 AIO 队列满时消费完成事件并重试。没有改变持久化后 ACK、WAL 格式、Group Commit 或后台 Compaction 的默认开关。

在真实 CentOS 7.9 / Linux 3.10 / GCC 8.3.1 / glibc 2.17 的 QEMU x86_64 客体中，完整 SDK 构建和官方 basic 默认用例通过；128 Key 的 recovery、concurrent 和一次 SIGKILL 恢复诊断通过。完整默认规模验收、Compaction 和正式性能未验证。

32 调用线程、2,000 Key、4 GiB、32/256 B、60% 读 / 30% 写 / 10% 删、预热 1 秒和测量 10 秒的单轮结果：

| 路径 | 总吞吐（含读） | Put/Delete 合计 | 总体 P99 | Put P99 | RSS 峰值 |
| --- | ---: | ---: | ---: | ---: | ---: |
| AIO + Direct I/O + O_DSYNC | 42,889 ops/s | 17,134 ops/s | 3.408 ms | 5.833 ms | 75.68 MiB |
| 缓冲写 + fdatasync | 41,261 ops/s | 16,476 ops/s | 3.441 ms | 5.177 ms | 79.70 MiB |

同一个二进制，只切换同步路径。两轮的 value 内容检查和两次重开状态一致性检查通过，成功数据库已清理。差值约 4%，没有显示一致的显著收益，也不能归因到 AIO 单一因素。数据集留在内存，测量阶段磁盘读取为 0，不能用来判断正式规模的索引换入换出性能。

这些是 4 vCPU QEMU 环境中的诊断值，不是原版与修复版回归对照或正式门槛结果。原包编译失败和修复初期 AIO EAGAIN 造成 Open 卡住的失败均保留，最终修复版另行记录 PASS。

专项补验：同一包与二进制在 CentOS 7 中运行官方 checkpoint（2,048 Key，4 KiB Value）和 compaction（4,096 Key，4 KiB Value、10 轮）均 PASS，仍采用官方 256 MiB 预算和空间断言。Checkpoint 的独立副本与并发一致性两个用例通过；Compaction 空间、最新值/删除状态及重开全量检查通过，事件证据确认 34 个完整 GC 周期、hot/cold/cold-index 共 82 次原生 compaction 成功、删除 92 个旧段。最终目录 46,146,535 字节，小于本次允许的 50,454,496 字节。此为小规模诊断，默认规模及压缩过程中故障注入未验收；成功数据库已清理。
