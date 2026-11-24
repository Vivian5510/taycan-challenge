# 实验开发约束

- `main` 保留 Dragonfly 学习实验开始前的 F2 调用线程执行基线。owner 分片、
  Boost.Fiber、Helio runtime、完成事件与队列学习实验只在 `experiment/*` 分支继续。
  2026-11-14 按用户要求，main 同步写路径改为单请求提交；group commit/WriteThread/
  WriteBatch 与 prefix pipeline 保留在 `experiment/f2-group-commit-preserved-20261114`，
  不自动带回 main。
- 不将架构候选直接叠到 main；实验提交说明来源、改动、验证和未完成项，不自动合并或推送。
- 不改变同步 SDK、持久化后 ACK、故障断言或 Key 数来制造通过；小规模诊断须明确标注。
- 性能逐格运行，保留至少 20 GiB 可用空间，单个开发数据库上限 2 GiB。
  成功后保留日志/指标/哈希并清理数据库；原始失败和中断状态不得改写。
- `analysis/`、`build/`、研究 checkout 和数据库默认不进 Git；必要小型报告和清单可显式加入。
- 当前结构和架构见 `docs/architecture.md`；历史主干恢复依据保存在 `archive/docs-legacy/`。
