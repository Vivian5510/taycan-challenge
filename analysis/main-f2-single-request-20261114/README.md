# main 移除 group commit 的验证

2026-11-14；分支拆分与构建命令见 [设计记录](../../docs/main-without-group-commit-20261114.md)。

同步持久化路径使用 Release、data-log、POSIX AIO、标准 fsync、512 B 对齐，GC 关闭。
[源码/二进制哈希与配置](manifest.json)、[CMake 缓存](CMakeCache.txt)、
[生成与构建记录](build.log)、[保留分支检查](branch-verification.json)均已保留。

- [71 项检查](validation/results.json)：全部 PASS；[输出](validation-runner.log)。
  包括公开 Basic、缩小为 64 Key 的公开 recovery/concurrent、公开 SIGKILL 驱动五轮、
  单调用者及 16 调用者的七个故障边界、两次重开、同 Key 顺序、真实哈希碰撞、
  跨缓冲恢复、损坏拒绝、同步/I/O/应用失败后拒绝后续调用。
- [32 线程诊断](concurrent32.json)：32 个调用者各 256 次 Put/Delete，8,192 次全部成功；
  两次重开逐 Key 验证通过。原始计数为 single_operations=8192、commits=8192、
  data_syncs=8192，证实没有合批。[提交计数](concurrent32-0.stderr.log)。
- [默认 native Basic](native-basic.log)：两个公开用例通过；native 的原生完成语义未改变。
- [链接符号](symbols-crc-only.log)：RocksDB 只留下 CRC32C，没有 WriteThread、WriteBatch、Committer。
- 所有实际测试二进制哈希与最终构建核对一致，核心生成源码与工作区逐字节一致。
  成功数据库已清理，[文件清单](successful-database-manifests.json)保留；日志、失败注入结果未删除。

主诊断预算 4 GiB、32/256 B；Basic 固定二进制/大值用例保持原样。
跨缓冲子实验显式使用 32 MiB/64 KiB value，不冒充正式参数。
真实碰撞检查保留值与恢复断言；多 Key 同批才可能产生的 cross_key_links>0 断言已移除。

本轮只验证提交调度移除，是 macOS ARM64 本机诊断，非 Linux 正式验收；没有跑完整公开
默认规模、2 亿 Key/4 GiB、GC 或 page-cache 丢失。保留分支的审计补丁和未完成 GC 调试
没有合入 main。没有性能基准，不能据这些小测试耗时宣称移除合批后更快。
