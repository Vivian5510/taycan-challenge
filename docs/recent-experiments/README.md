# 最近十项实验

以下按最近的实验方向整理。原始日志、JSON、源码快照和哈希仍保存在 `analysis/` 中；本索引不复制大型数据，也不把失败、未执行或小规模诊断改写成通过。

| # | 时间 / 方向 | 结论 | 完整数据 |
|---:|---|---|---|
| 1 | 2026-11-24 原生 Linux：凑满批次的调用线程接手提交 | 满批交接缩短约 5.6 μs，但 Load 中位数由 47.3K 降至 42.7K ops/s，回退 9.7%；候选默认关闭 | [A/B 报告与逐轮日志](../../analysis/f2-batch-takeover-20261124/README.md) |
| 2 | 2026-11-24 原生 NVMe：批大小对 fdatasync 的影响 | 1–32 条/批同步约 0.46–0.48 ms；大批略增。微基准不代表引擎吞吐 | [扫描报告与原始 JSONL](../../analysis/f2-performance-rootcause-20261120/native-linux-fdatasync-batch-20261124/README.md) |
| 3 | 2026-11-23 持久前沿与并发提交建模 | 用连续 serial、数据范围与同步前沿检查批次 ACK/恢复约束；仍需完整并发落地验收 | [前沿模型目录](../../analysis/f2-performance-rootcause-20261120/frontier-20261123/) |
| 4 | 2026-11-20–24 F2 Load 性能分解 | 对比收集、F2 apply、写完成、数据同步、索引发布和 checkpoint；不同平台数字分开保留 | [性能根因总报告](../../analysis/f2-performance-rootcause-20261120/README.md) |
| 5 | 2026-11-20 WAL 组提交与逐条提交 | Linux SATA 上 4,096 Key、32 线程 Load：组提交约 7.9K vs 逐条约 271 ops/s；单线程没有收益 | [组提交 A/B](../../analysis/f2-wal-group-ab-20261120/README.md) |
| 6 | 2026-11-20 组提交窗口和流水线 | 25–100 μs 固定窗口结果接近；当前流水线没有稳定收益，部分窗口显著退化 | [窗口扫描](../../analysis/f2-wal-window-sweep-20261120/README.md) |
| 7 | 2026-11-21 TidesDB/RocksDB 持久写 | 最新源码逐条同步写约 5.6K/4.5K ops/s，批量结果另列；文章的高数字是每批 1,000 条记录 | [同机比较与同步语义](../../analysis/rocksdb-tidesdb-sync-20261121/README.md) |
| 8 | 2026-11-15 Linux 参数与机器扫描 | 固定 Linux 环境核对负载、线程、门槛解释；单轮筛选仅用于开发诊断 | [Linux 基准目录](../../analysis/linux-benchmark-20261115/) |
| 9 | 2026-11-14 F2 冷热迁移、Compaction 与完整公开场景 | 记录 PASS/FAIL/SKIP 和未运行原因；不能汇总成整体通过 | [公开场景状态](../../analysis/f2-enabled-verdict-20261114/summary.json) |
| 10 | 2026-11-14 同步边界、恢复与 I/O 变体 | 验证 ACK、尾页、同步错误、Compaction 及 AIO 候选；被拒候选和失败日志保留 | [恢复同步审计](../../analysis/f2-recovery-sync-audit-20261114/) · [单提交 I/O 实验](../../analysis/f2-single-commit-io-20261114/) |

本清单显示当前最可靠结论：同步设备耗时约 0.47 ms/批并非全部成本，批次调度改进也没有自动转化为更高吞吐。下一轮应在原生 Linux 上针对 F2 apply/日志写完成的阶段做独立变量实验，并继续用相同同步 ACK、正确性和重开恢复条件验收。
