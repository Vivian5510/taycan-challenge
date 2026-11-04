# Dragonfly 学习实验之前的 F2 基线

2026-11-04 按用户要求恢复 main。原 main 的首个 Git 提交 `cc28437` 已包含较早的
owner/Boost.Fiber/Helio 借鉴实验，不能直接用 Git 找到更早的父提交。本次从实验前保存的
源码快照恢复，以新提交记录恢复过程，不重写已有历史。

恢复边界是 `f2-open-source-wait-20261103` 完成、`f2-dfly-thread-audit-20261103` 开始
研究 Dragonfly 线程架构之前：SDK 调用线程直接进入共享 F2；写入沿用 caller-led
group commit；不经过固定 owner 路由、Boost.Fiber 或 Helio runtime。

## 源码依据

- `CMakeLists.txt`、`prepare_source.py`、`native_aio_file.inc` 从
  `analysis/f2-open-source-wait-20261103/source-final/` 原样恢复，并核对其历史 SHA256。
- `faster_backend.cc`、`f2_sync_runtime.inc`、`f2_commit_experiment.h`、
  `f2_commit_members.inc`、`f2_prefix_members.inc` 从
  `analysis/f2-compaction-validation-20261102/v5-source/` 恢复。后三个内核文件另与
  open-source-wait 的 caller 构建清单核对，全部匹配。
- 后续 GC 生命周期修复、提交前沿、恢复和 RocksDB 原生等待握手保持实验前已有状态。
  最终重新生成的 FASTER 源码须与 open-source-wait caller 的完整生成源码哈希逐项核对。
- 从 main 移除 owner/fiber runtime、完成事件、Helio 队列副本及相关探针和构建开关。
  研究 checkout 和旧二进制仍可保留作研究，但不参与当前构建。

恢复清单、哈希核对和本次验证见
[恢复记录](../analysis/main-pre-dfly-restore-20261104/README.md)。

## 保留的实验

- `archive/main-with-dfly-experiments-20261104` → `cc28437`：恢复前 main 的完整源码。
- `experiment/helio-integration-audit` → `90eedee`：最新完整 Helio 接入实验。
- 其余 `experiment/helio-*` 分支及历史结果保留。

旧报告中 `main-event` 指冻结的 Boost.Fiber/event 持久化实验配置，不能称为当前 main。
后续对照须显式记录分支、提交、构建配置、engine identity 和哈希。

## 当前构建

历史 CMake 默认值仍为 `native`、`inline`、`fullfsync`。本次恢复没有偷偷切换历史默认值；
`native` 操作完成不等于逐操作可恢复的持久化确认。开发同步持久化路径必须显式指定
`data-log`、`posix-aio` 和 `fsync`。恢复验收使用以下配置：

```sh
cmake -S taycan_participant_sdk -B build/f2-pre-dfly-main-20261104 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTAYCAN_BUILD_PERF_BENCHMARKS=ON \
  -DTAYCAN_F2_SYNC_STRATEGY=data-log -DTAYCAN_F2_PREFIX_BLOCK_BYTES=512 \
  -DTAYCAN_F2_NATIVE_IO=posix-aio -DTAYCAN_F2_MAC_SYNC=fsync \
  -DTAYCAN_F2_DURABLE_COMPACTION=OFF -DTAYCAN_F2_PREFIX_PIPELINE=OFF
cmake --build build/f2-pre-dfly-main-20261104 --parallel 4 --target \
  taycan_kv_bench basic_test concurrent_test recovery_test crash_worker f2_sync_api_probe
```

新目录可使用默认 Makefiles；本次本机实际构建使用已有 Ninja 程序的绝对路径，完整命令
保留在恢复记录。GC 实现仍在源码中，但该验证配置关闭 GC；不宣称恢复后已验收
GC、2 亿 Key/4GiB 或正式 Linux 环境。没有重新跑性能并复用旧吞吐作为恢复版成绩。
