# 本地源码与深度移植记录

2026-11-02 补充核对见 [提交与完成通知源码走查](../../analysis/f2-commit-source-study-20261102/README.md)。此前 Tsavorite 移植复用了提交覆盖思路，但 C++ 两线程、共享队列锁和等待机制属于本地实现，并没有等价复用其 Task 调度；本地流水线已暴露并修正全体广播问题。同步等待应优先继续直接复用 RocksDB WriteThread。

2026-11-03 的独立候选见 [开源等待与 I/O 实验](../../analysis/f2-open-source-wait-20261103/README.md)。`prepare_commit_wait.py` 从固定 RocksDB `write_thread.cc` 提取完整的 BlockingAwaitState/AwaitState/SetState 方法（只更名、移除 DB 计时与测试钩子），继续用上游 Writer 与 AdaptationContext；生成的 `f2_commit_wait.h`、Apache 许可及来源哈希一起保留。`F2_PIPELINE_LOCAL_WAIT` 为显式开关，默认关闭。批量 AIO 完成候选借鉴 KVell 的批量组织，但仍使用本地 macOS POSIX AIO，并不复用其 O_DIRECT 提交语义。

本目录保持 F2 为存储底座。SDK Adapter 仍只转发 Get/Put/Delete；新的提交协调在生成的
F2Kv/FasterKv 内部，数据写入 F2 HybridLog，未增加旁路日志或另一套数据引擎。

| 来源 | 版本/许可证 | 使用方式 |
| --- | --- | --- |
| Microsoft FASTER/F2 | `332ea7da532a688acf648ee60c4f63a1caeaf1f7`，MIT | 实际存储、索引、地址链、checkpoint 与恢复底座；只修改构建副本 |
| Microsoft Garnet/Tsavorite | `a371e041f842e47e2839d82e696f63e56762f66f`，MIT | C# 提交调度和覆盖完成机制按 F2 C++ 的线程、索引与 I/O 边界移植；不是直接链接 C# TsavoriteLog |
| RocksDB | `49ce8a1064dd1ad89117899839bf136365e49e79`，双许可证 | 现有 caller-led 对照直接链接原生 WriteThread、WriteBatch、WAL Writer/Reader、CRC32C；流水线候选继续使用 CRC32C，未使用其写队列 |
| KVell | `a6cbc0cb186c7934dc6be785225d809f62730fea`，MIT | 阅读分片、工作队列、批量 I/O 和完成回调；本轮没有移植 slab 原地写或按 Key 分片 |
| WiscKey | FAST'16 论文 | 参考从完整数据日志重建索引，未确认到作者原版源码；不把第三方复现称为原版 |
| TiKV Titan | `b21afb5d7a50f960fbf47a19c103ebc6c43636a3`，Apache-2.0 | 本地 GitHub 源码归档；WiscKey 路线的工程参考，本轮只核对 blob 文件同步、目录同步和 manifest 发布顺序 |

## 本轮真正移植的机制

参照 Tsavorite `LogCommitPolicy.cs` 的 MaxParallel、`TsavoriteLog.cs` 的
`ongoingCommitRequests`、`coveredCommits`、`CommittedUntilAddress` 和回调完成顺序，新增
`f2_prefix_pipeline.inc`。C# 依赖的 epoch、allocator、Task 等没有被机械翻译，而是接到
F2 原有的 session、增量 flush、原生日志恢复和哈希索引上。

1. 提交请求按顺序分配序号、追加 Key/Value 或 tombstone，并封入原生日志提交标记。
2. 最多保留 1～4 个准备完毕但尚未向调用者完成的组，单组最多 64 个操作/256 KiB。
3. 准备线程可以在上一组 F_FULLFSYNC 期间准备下一组；不会在全局互斥锁内做慢同步。
4. 同步线程取走已经写完成的组，合并同一文件的同步依赖；一次成功同步可覆盖多个日志提交标记。
5. 准备线程顺序处理完成前沿，先发布对应组的索引，再回复请求。读者不能看到未持久化的新索引头。
6. 组之间同 Key 或同 bucket/tag 的地址链从“最新已准备头”继续，而读者使用“最新已提交头”。
   每组携带自己的 before/after 索引变更，按顺序 CAS 发布。
7. 完整 checkpoint 在所有在途组已同步、发布后执行，根中序号与稳定尾必须一致。
8. 任一准备或同步失败将实例置为失败状态，所有待完成请求返回错误；不会跳过出错区间后继续 ACK。

首轮深度 4 直接对现成的小组发起同步，实验发现它会拆碎同步调用者的批量。第二轮新增显式
`TAYCAN_F2_PIPELINE_SYNC_COLLECT_US=50`：有多个调用者时给下一波请求最多 50 µs 的入场窗口，
再等待已经排队的准备工作完成，或达到深度/checkpoint 栅栏，才同步可覆盖前缀。
这不是无限等大批量：准备组仍有深度和字节上限，checkpoint 栅栏会强制推进，单调用者不做该窗口等待。
同步文件句柄与索引发布顺序保持不变。

这是对提交调度和状态机的移植，不是复制一个类名。同步线程仍执行真实 F_FULLFSYNC。
采用 AIO 不等于允许磁盘 I/O 完成但尚未持久化就回复。

## 不能混淆的边界

- 这不是 KVell 的 shared-nothing 实现，也没有移植其设备掉电原子写假设。
- 这里只有准备与同步的流水线，以及已写完成组的合并同步；当前最多一个同步线程进行持久屏障。
  不应称为四个独立持久提交域或四块磁盘并行。
- 同步 API 每个调用者最多一条请求在途。并发少时，可供下一组准备的请求可能不足。
- 单 Key 线性化与成功即持久化不要求跨 Key 事务，本实现没有增加组事务承诺。
- 自动热转冷迁移、Compaction 和物理数据回收仍未接入此协议；不是完整命题验收。

源码许可证保留在研究副本。本目录对应的 Tsavorite 移植声明见 `Tsavorite-MIT.txt`。
实现和实验详情见 `analysis/f2-source-reuse-20261031/README.md`。
