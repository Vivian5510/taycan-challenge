# F2 底座评估

当前以 F2 作为引擎底座，RocksDB 作为同机参照。入口见 [构建与评测说明](taycan_participant_sdk/README.md)。

2026-11-04：`main` 已恢复到 Dragonfly 学习实验之前的调用线程执行路径。
owner 分片、Boost.Fiber、Helio 队列/完成事件及原生 runtime 实验保存在各实验分支。
[当前基线、构建参数与恢复依据](docs/main-pre-dfly-baseline.md)。旧报告里的 `main-event`
是冻结的 Boost.Fiber 实验配置，不能再称为当前 main 的性能数据。

- `experiments/faster-native-adapter/`：SDK → F2 的操作、session、异步完成与恢复适配。
- `research/FASTER/`：固定提交的上游源码。
- `taycan_participant_sdk/include/`、`tests/`：接口与正确性/故障协议。
- `taycan_participant_sdk/benchmarks/`：共享工作负载、RocksDB 基线、终端进度与结果报告。
- `analysis/`：历史实验、日志与结果；已退休实现只有归档，没有活动构建入口。

当前结论见 [F2 底座评估报告](analysis/f2-base-native-perf-10k-20261030/README.md)。
最新同步写实现及同条件 RocksDB 对比见 [F2 内部复用 RocksDB 提交组件](analysis/f2-rocksdb-wal-20261030/README.md)。
后续提交调度优化、负结果与 F2 特有方向研究见 [WAL 优化报告](analysis/f2-wal-optimization-20261030/README.md)。
已完成小规模同步 ACK/恢复和 WAL 回收验证；在线数据 Compaction 及 2 亿 Key / 4 GiB 尚未达标验证。
