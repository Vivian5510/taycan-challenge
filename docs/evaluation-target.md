# 命题评测目标（2026-10-29 更新）

依据用户本次引用的命题要求，后续以这些参数作为目标，机器可读配置位于
[`evaluation-config.json`](../taycan_participant_sdk/evaluation-config.json)。历史报告保留当时参数，不能当作这组参数的评测结果。

2026-11-02 用户进一步确认：故障验收以所提供脚本的 SIGKILL、已确认操作和重开恢复为准，
暂不考虑整机断电。断电模拟已取消，不额外要求 F_FULLFSYNC 或设备掉电缓存测试作为通过条件。
仍须满足原有操作、并发、恢复、内存与空间断言，不能针对测试进程、Key 数或随机种子特化行为。

- 性能数据集为 200,000,000 个唯一 Key，Key 为 16～32 B，Value 定长 256 B。
- 默认用 32 B Key；可显式选择 16～32 B。同一组 4 GB/16 GB 对比必须使用相同 Key 长度、生成器、seed、Key ID 范围和数据规模。
- 内存两档：4 GB 权重 70%，16 GB 权重 30%。当前脚本沿用二进制单位解释，精确为 4,294,967,296 / 17,179,869,184 B；这是本地单位约定，题面若提供精确字节值，应统一更新配置。
- 正确性使用 4 GiB；公开各场景默认 Key 数、轮数、断言和崩溃协议保留。生成的 Key/Value 改为 32/256 B。Basic 自行构造的空值、二进制和长字符串边界用例仍保留。
- 目标入口 RSS 上限为 `floor(mem_limit_bytes × 11 / 10)`，覆盖启动、运行、恢复、关闭，不使用额外测试工具 allowance。
- 70%/30% 仅记录档位权重，缺少完整评分公式时不合成“正式分数”。现有 600 秒测量、10 秒预热、3 轮、1/2/4/8 线程属于本地方法，不由这次引用推定为正式规则。

## 参数预览与执行入口

正确性预览不运行 worker，保留各场景自己的默认数据规模：

```sh
python3 taycan_participant_sdk/tests/run_correctness.py --dry-run
```

在 Linux 上运行指定已构建引擎：

```sh
python3 taycan_participant_sdk/tests/run_correctness.py \
  --build-dir /var/lib/taycan-workspace/build \
  --results-dir /var/lib/taycan-results/correctness-target-new
```

性能两档先预览：

```sh
python3 taycan_participant_sdk/benchmarks/run_performance.py \
  --build-dir /unused --profile contest-4g --dry-run
python3 taycan_participant_sdk/benchmarks/run_performance.py \
  --build-dir /unused --profile contest-16g --dry-run
```

两档锁定 2 亿 Key、所选内存档位、16～32 B Key 和 256 B Value，不接受缩小规模后继续标记为 `contest-*`。额外方法验证需显式选择 `dev` / `reference` 等诊断档，并如实标注。公开原参数重现入口仍为 `tests/run_tests.py`，其历史默认值未变。

## 对当前 F2 原型的含义

1 MiB 日志页可以容纳目标 16～32 B Key + 256 B Value，小于这个范围之外的超大记录能力暂时不影响目标。固定小桶页只影响分配粒度，不能据此断言实现了总内存约束。现有预算公式在 4 GiB 下选择每张主表 1,048,576 个桶、每个日志缓冲 512 MiB；在 16 GiB 下选择 4,194,304 个桶、每个日志缓冲 2 GiB。

两档仍需用同样的 2 亿 Key 验证冷索引迁移、溢出桶增长、Compaction 和恢复期间的 VmRSS。当前自动 Compaction 关闭、逐次 ACK 持久化未补齐；既有 16/2,048 Key 小实验不构成这两档评测通过。4 GiB 档权重更高，应优先在该预算下解决这些问题。
