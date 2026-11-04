# main 恢复到 Dragonfly 学习实验之前

按用户要求恢复调用线程直接进入 F2 的实现。恢复前 main 首次 Git 提交已经包含
owner/Boost.Fiber/Helio 借鉴代码，因此从保存的源码恢复，并以新的 Git 提交记录，
不重写历史。恢复前 main 备份为 `archive/main-with-dfly-experiments-20261104`（cc28437）；
完整 Helio 实验在 `experiment/helio-integration-audit`（90eedee）以及其余实验分支保留。

恢复边界：`f2-open-source-wait-20261103` 完成、Dragonfly 线程架构学习与实现实验开始前。
8 个文件从历史源码恢复，10 个实验文件从 main 移除；精确来源及前后 SHA256 见
[恢复清单](restore-manifest.json)。先前独立 pipeline、GC 与持久化/恢复修复维持边界前状态。

重新生成的 FASTER 源码共 73 个文件，与当时 caller 版本保存的构建清单逐项核对，
没有缺失、新增或不同文件；见[生成源码核对](generated-source-check.json)。
13 个本地内核实现文件另外逐项核对历史哈希；见[kernel-source-check.json](kernel-source-check.json)。
重新构建后的 engine identity 也与历史 caller 一致：data-log、caller-led group commit、
fsync、adaptive yield 上限100µs、AIO completion batch=1，无 owner/Boost.Fiber/Helio。
二进制符号检查确认不存在这些实验 backend/runtime；见[验证汇总](verification-summary.json)。

## 本次验证

- 新建构建目录 `build/f2-pre-dfly-main-20261104`，RelWithDebInfo。
- 显式 data-log / posix-aio / fsync / 512B，GC 和额外 pipeline 关闭；历史 CMake 默认值未改变。
- 原 SDK basic、concurrent、recovery 程序：2000 Key、32B Key/256B Value、4GiB，全部 PASS。
  concurrent 的版本边界用例采用31读+1写。
- 未修改的 `run_data_log_validation.py`：71 项检查全部 PASS，覆盖错误拒绝成功、同 Key
  覆盖/删除恢复、跨页、提交标记内容碰撞、真实 hash 冲突、尾部/数据/恢复根损坏，以及
  各持久化阶段的强杀和连续两次恢复。其中包含原 SDK 随机 SIGKILL 5 次。
- 历史边界脚本仍显式使用64 Key，跨页探针用32MiB；它们是边界诊断，不是正式规模测试。
  没有将这71条记录表述为71次故障或命题全量验收。
- 本轮没有重新跑吞吐；旧报告的 main-event 属于另一个冻结的 Boost.Fiber 实验配置，
  不能继续标成当前 main 成绩。

完整命令、RSS 与结果见[验证结果](validation/results.json)、
[71项原始结果](validation/boundaries/results.json)、[配置命令](configure-command.json)。
所有成功数据库已清理，只保留日志、结果、哈希。最低可用空间 153.7GiB，
观察到单库峰值 257.0MiB。

首次配置因 Ninja 不在 PATH 失败，随后使用此前已有 Ninja 的绝对路径配置成功，
记录分别保留为 configure-attempt-no-ninja.log 和 configure.log；没有隐去该工具配置失败。
引擎与测试没有失败后重跑。

当前仍未验收 GC 组合、2亿 Key/4GiB 和正式 Linux 环境。
[基线说明和构建入口](../../docs/main-pre-dfly-baseline.md)。
