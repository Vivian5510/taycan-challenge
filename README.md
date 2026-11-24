# Taycan Challenge

当前代码以 F2/FASTER 为底座，在 Linux `data-log` 实验路径验证持久化 ACK、内存控制和并发提交。仓库按源码、脚本、构建产物和实验记录分开放置。

- `code/`：F2 适配实现、官方 SDK 接口/测试源码及小型 C++ 探针。
- `script/`：正确性、性能和实验运行脚本。
- `build/taycan_submission_candidate.zip`：当前源码候选包；未完成正式 Linux 构建和全量验收。
- `build/taycan_design_document.pdf`：独立设计文档候选，仍需结合最终实现复核。
- `analysis/`：开发实验原始结果与日志，体积较大，默认不进 Git。
- `research/`：锁定版本的上游源码。

核心说明只保留三处：

- [整体架构设计](docs/architecture.md)
- [当前性能结论](docs/performance-status.md)
- [最近十项实验与数据索引](docs/recent-experiments/README.md)
- [命题方完整文档（revision 1471）](docs/命题方完整文档.md) · [原始 Docs 快照](docs/命题方完整文档-原始快照.json)

正确性入口：`python3 script/taycan/tests/run_correctness.py --dry-run`。性能入口：`python3 script/taycan/benchmarks/run_performance.py --help`。正式结果必须在命题要求的原生 Linux x86_64 环境、数据规模和持久化配置下重新验收。
