# F2 group commit 分支拆分

2026-11-14 按用户要求，保留现有合批实现，并从 main 移除该 feature。

- 保留分支：`experiment/f2-group-commit-preserved-20261114`，提交
  `c529214abeb75990a66c7fefc016aa2f013279a5`。该快照保留 F2 同步路径接入的
  RocksDB `WriteThread`/`WriteBatch`、caller-led group commit、可选 prefix pipeline，
  以及此前已提交的正确性审计。它的数据与索引底座仍是 F2。
- main 的移除基于 `dd90535e67d3b72afddf0687208da83fb761b107`。没有把保留分支新增的
  Linux 故障审计、尾部恢复补丁或尚未完成的 GC 调试改动一并合入 main。
- 未完成的 GC 工作单独保存：根仓库 stash 对象
  `496feac04dfd3553c248f8575a1736e3a71af9fe`；研究 FASTER checkout 的 stash 对象
  `3e40db2ce10fa417f43f127b9c7344daa4e386e2`。研究源码已恢复干净，避免污染生成源码。

## main 的调用与持久化路径

`KVEngine::Put/Delete → SyncMutate → F2 Upsert/Delete → HybridLog flush 完成 →
同步数据文件 → 发布哈希索引 → 返回成功`。

`data-log` 每次调用分配一个 serial、一个原有格式的提交标记、单独等待一次数据同步。
写调用通过实例互斥锁串行进入；没有 leader/follower、收集窗口、批量请求队列或提交线程。
Get 仍通过共享维护门进入 F2 Read；同步前不发布新索引，checkpoint 使用独占维护门。
错误处理保持维护门直到索引/session 清理结束；应用、异步 I/O 或同步出错后实例进入失败状态，
随后调用返回错误。达到原有字节阈值仍执行完整 checkpoint，不改成每次调用 checkpoint。

移除 `rocksdb-wal` 策略和 F2 prefix pipeline、批量提交/等待生成器与构建接线；
其源码可从保留分支取回。独立 RocksDB 对照实验仍在仓库中，不参与 F2 提交路径。
CRC32C 继续链接原有 RocksDB 校验实现以保持数据格式和校验性能；实际可执行文件中没有
`rocksdb::WriteThread`、`rocksdb::WriteBatch` 或 `reference::Committer` 符号。

默认 `native` 策略仍是原生操作完成语义，不能据本次修改称它支持成功即持久化。
本轮同步路径采用以下显式配置：

```sh
cmake -S taycan_participant_sdk -B build/main-f2-single-request-20261114 \
  -DCMAKE_BUILD_TYPE=Release -DTAYCAN_BUILD_PERF_BENCHMARKS=ON \
  -DTAYCAN_F2_SYNC_STRATEGY=data-log -DTAYCAN_F2_PREFIX_BLOCK_BYTES=512 \
  -DTAYCAN_F2_NATIVE_IO=posix-aio -DTAYCAN_F2_MAC_SYNC=fsync \
  -DTAYCAN_F2_DURABLE_COMPACTION=OFF
cmake --build build/main-f2-single-request-20261114 --parallel 4 --target \
  basic_test concurrent_test recovery_test crash_worker taycan_kv_bench f2_sync_api_probe
python3 experiments/faster-native-adapter/run_data_log_validation.py \
  --build-dir build/main-f2-single-request-20261114 \
  --results analysis/main-f2-single-request-20261114/validation
```

## 本次验证与适用范围

[原始结果、源码与二进制哈希](../analysis/main-f2-single-request-20261114/README.md)。

- 71 项本机诊断检查全部通过，包括公开 Basic 两用例、64 Key 的公开 recovery/concurrent
  诊断、原公开 SIGKILL 驱动 5 轮、单调用与 16 调用者的七种注入边界及两次重开、
  同 Key 更新/删除顺序、实际哈希碰撞、跨日志缓冲写恢复、内容损坏拒绝与三类失败处理。
- 主诊断预算 4 GiB、32/256 B。跨缓冲恢复子实验显式使用 32 MiB 和 64 KiB value，
  用于触发缓冲跨越；Basic 保留公开固定二进制/大 value 用例。不是修改正式参数后宣称验收。
- 额外 32 个调用线程各执行 256 次 Put/Delete，共 8,192 次；成功后两次重开验证全部值。
  计数精确为 8,192 次单操作提交、8,192 次数据同步，证明并发请求没有再被合批。
  探针历史命令名 `group-serve/group-verify` 只是测试入口名，不代表 main 存在 group commit。
- 默认 native 独立构建并通过 Basic 两用例；其持久化承诺没有升级。
- 真实碰撞探针继续验证两个同 bucket/tag Key 的值与恢复；原先专属于多 Key 同批的
  `cross_key_links>0` 断言不再适用，已改为确认碰撞探针完成，未降低值正确性断言。

这是 macOS ARM64 原生开发回归；没有重跑完整公开默认规模、2 亿 Key/4 GiB、GC、
page-cache 丢失或正式 Linux 评测。Compaction 默认仍关闭，既有未完成问题仍保留。
本次没有进行性能对照，不宣称移除合批后吞吐提升；单请求同步在高并发下可能增加同步次数。
