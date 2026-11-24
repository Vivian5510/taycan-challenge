# RocksDB 提交组件复用探针

同一 `rocksdb_commit.h` 已被 F2 的 `rocksdb-wal` 构建直接复用，并扩展了恢复序列号和 WAL
代次切换接口；F2 的调用及恢复测试见 [接入报告](../../analysis/f2-rocksdb-wal-20261030/README.md)。

直接链接固定 RocksDB 8.9.1 的 WriteThread、WriteBatch、log::Writer/Reader 和文件同步代码。此目录验证组件能否用于 F2 内核的提交层，**不包含 F2 引擎集成或 checkpoint/日志回收**。

源码分析及集成边界见 [RocksDB 同步提交研究](../../docs/rocksdb-durable-write-study.md)，原始结果见 [final/results.json](../../analysis/rocksdb-commit-source-study-20261030/final/results.json)。

```sh
cmake -S experiments/rocksdb-commit-reference -B build/rocksdb-commit-reference-20261030 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/rocksdb-commit-reference-20261030 --parallel 4
python3 experiments/rocksdb-commit-reference/run_probe.py \
  --binary build/rocksdb-commit-reference-20261030/rocksdb_commit_probe \
  --results analysis/rocksdb-commit-reference-new-run
```

默认源码和库路径指向本工作区的既有 arm64 构建；其他环境需显式指定匹配的 ROCKSDB_SOURCE 和 ROCKSDB_LIBRARY。每次结果目录必须不存在，已有日志拒绝覆盖。原生组件没有改动，观测包装层只记录调用次数、字节并注入失败。操作的实际存储格式由 RocksDB WriteBatch/Writer 负责。
