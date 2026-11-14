# 恢复修复合入与 I/O 调优记录

按用户要求，恢复修复 `26e4466` 已快进合入 main；没有把此前 group commit 带回主干。
本轮实验在 `experiment/f2-single-commit-io-20261114`，默认仍为原POSIX AIO、F_NOCACHE、
一次完成回调。逐调用成功前完成fsync、再发布索引，未放宽。

四配置筛选使用同一百万唯一Key、32线程、4GiB、32/256B、GC开启、8MiB段。
原AIO装载11997 ops/s；仅热日志buffered16854；小热日志直接写13352；组合14749。
这些是单轮筛选，不能据此采用。

后续验证发现：buffered候选在GC复制后长时间等待维护门，人工停止、保留失败数据库；
尚未证明缓存选项是根因。hot-inline首次测试人工中止，不应定性成相同死锁。
本轮补跑原规模Compaction在600秒开发时限内未完成：百万次写入完成后，最终Get处于Pending，GC等待维护门；原FAIL:timeout保留，不冒称官方超时。见[完整记录](../analysis/f2-single-commit-io-20261114/README.md)。

AIO一次取出4个完成回调的候选，在Compaction写入返回IOError，随后普通SIGKILL恢复
Key491为NotFound，前序已确认171592次操作，因此拒绝。它不是空间断言失败。
上限2虽然通过71项诊断、Compaction、公开SIGKILL5轮与真实同步边界，仍须验证收益和并发语义。

固定两轮确认（各格新库装载百万Key；update和mix预热2秒、测10秒，第二轮反转次序）：

| 负载 | 原上限1中位 ops/s | 上限2中位 ops/s | 变化 |
|---|---:|---:|---:|
| 完整装载 | 10479 | 10457 | -0.2% |
| 随机覆盖写 | 10355 | 8721 | -15.8% |
| 60% Get / 30% Put / 10% Delete | 23481 | 22961 | -2.2% |

没有稳定收益，不采用上限2。小组件实验还复现了可重入性缺陷：先把多个完成job移到
TryComplete局部数组，会让第一个回调重入poll时看不到后续已完成事件。相同实验中上限1
正常推进，上限2在第一个回调等待兄弟回调时超时。这是组件缺陷证据，但未证明它就是上述
所有Compaction/SIGKILL故障的唯一根因。详见[重入实验](../analysis/f2-single-commit-io-20261114/reentry-findings.md)。

所有失败、中止和结果保留。该轮是macOS ARM64本地诊断，不是正式Linux/NVMe或2亿Key验收；
没有把未运行的GC强杀矩阵记成通过。新的性能候选没有自动合入main。

多个写调用共用一次持久同步可以满足成功不丢：全部WAL先写完、fsync成功之后，每个调用
才分别返回。平均16不是固定凑批门槛。同步前崩溃，这组还未成功；同步后部分线程未收到
返回，结果可能未知但数据已持久化。原始真实同步阻塞、EIO、强杀与双方相同页丢失证据见
[提交边界说明](../analysis/f2-single-commit-io-20261114/group-commit-boundary.md)。
