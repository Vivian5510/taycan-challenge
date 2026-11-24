# 命题评测目标

本页根据命题文档 `fcABtadeNuBIhN1Y8yLL6cvDT` revision 1470 于 2026-11-20 更新。机器可读配置在 [`evaluation-config.json`](../taycan_participant_sdk/evaluation-config.json)。文档所附 [2026-11-17 开发包](../taycan_participant_sdk_2026_11_17.tar.gz) SHA-256 为 `0badac7a24da3bd2733ee764e48489cf2f2248b18b3087c9f92ff7ef49b47b46`。性能规则以文档第 6 节为准，公开正确性测试的具体默认参数以 SDK 包为准。

2026-11-02 用户确认故障验收按公开脚本的 SIGKILL、已确认操作和重开恢复执行，暂不模拟整机断电。Put/Delete 成功返回前仍必须完成持久化同步，进程崩溃或机器宕机后已确认状态须恢复。

## 引擎要求

- 参赛者独立实现符合 `KVEngine` 接口的哈希 KV 引擎，可在 Linux x86_64 编译运行。
- 哈希桶数量在创建时确定，运行期间不变。可选桶内索引和磁盘索引结构，但须保留完整 Key 或无损表示并精确比较 Key，正确处理哈希冲突。
- 同 Key Put 采用覆盖语义，Delete 不存在的 Key 返回 OK。Put/Delete 成功返回前须完成持久化同步。
- 必须有可持久化的磁盘索引，支持按需加载。启动时按 `mem_limit_bytes` 限制索引与缓存，超阈值自动落盘释放内存。
- 索引访问、加载、更新及落盘对 Get/Put/Delete 透明，支持并发。重启后恢复索引和数据，不能扫描全数据文件重建索引，也不能把全部索引加载进内存。
- 必须实现后台 Compaction，回收覆盖和删除产生的无效数据与索引。
- Checkpoint 是可选加分项。副本需一致、可独立打开，之后原库写入、删除、Compaction 或原目录移除均不能影响副本。
- 数据、索引、WAL、元数据及临时文件必须保存在 Open 指定的 `data_dir` 内。

## 内存与正确性

性能数据集为 200,000,000 个唯一 Key，Key 长 16～32 B，Value 固定 256 B。两个本地混合档各自在目标预算下生成相同数据，保持 Key 长度、seed、生成器和 Key ID 范围一致。性能预算权重为 4 GB 档 70%、16 GB 档 30%。文档没有给出 GB 对应的精确字节数，本地 profile 按 GiB 换算。

公开 SDK 正确性场景的 `mem_limit_bytes` 默认均为 256 MiB，并各自保留包内 Key 数和特殊 Key/Value 边界。开发入口 `tests/run_correctness.py` 只将预算统一设为 4 GiB，默认不覆盖各用例自己的数据尺寸。公开 harness 按 `floor(1.1 × mem_limit_bytes) + 8 MiB` 监控，并对 mixed/compaction 参考状态数组预留每个 Key 4 B。`tests/run_tests.py` 用于重现 SDK 原始 256 MiB 默认值。

必选公开测试包括基础接口、顺序读写、随机读写、覆盖、删除、混合、恢复、并发、按需加载、受限内存持续读写、Compaction 正确性和崩溃恢复。Checkpoint 若未实现可以 SKIP，实现后必须通过对应测试。公开崩溃脚本以外部 SIGKILL 测试已确认 ACK 的状态，不模拟整机断电。

正确性评审还包含官方进一步开展的未公开参数测试和代码审查。通过公开测试不代表最终正确性通过。评审会确认数据与索引实际落盘、恢复完整和 RSS 合规，也会检查是否绕过测试或监控。

## 性能评审

机器要求为 64 核 CPU、NVMe SSD、376 GB RAM。数据准备阶段使用 32 线程、64 GB 内存预算，写入 2 亿条不同 Key；平均写入吞吐须达到 60,000 ops/s。该装载阶段不计入稳态性能评分。

预热阶段执行与正式读写相同的负载，使索引缓存和后台任务稳定，预热细节由评测方统一规定但未公开。稳态阶段使用 32 线程，连续 600 秒随机 Get/Put/Delete，比例为 60/30/10。4 GB 与 16 GB 两档的平均综合吞吐均须达到 50,000 ops/s。

每个性能测试及每轮前评测方清理 OS page cache，正式测量期间不再清理。评分指标包括吞吐、P99 读写延迟和恢复时间；文档还要求统计 P999 读写延迟，但评分公式只给 P99 分配权重。80 分客观分按每档分别归一化：

```text
T = 选手综合吞吐 / 该档合格选手最高综合吞吐
R = 该档合格选手最低 P99 读延迟 / 选手 P99 读延迟
W = 该档合格选手最低 P99 写延迟 / 选手 P99 写延迟
O = 该档合格选手最短恢复时间 / 选手恢复时间
S = 80 × (0.40T + 0.25R + 0.25W + 0.10O)
```

恢复时间从调用 Open 到完成 100 次读、100 次写和 100 次回读。两档配置都须完成评测；内存档权重为 4 GB 70%、16 GB 30%。只有通过正确性并达到两项性能门槛后才参与评分。

加分项共 20 分：Checkpoint +10，正式压测结束时有效数据逻辑字节占数据目录文件总逻辑字节至少 70% 可得 Compaction +5，设计文档 +5。Checkpoint、Compaction 和文档各按其单独验收标准判定。

本地性能入口使用 `contest-load`、`contest-4g` 和 `contest-16g` 测公开门槛，也提供 `contest-recovery-4g` 和 `contest-recovery-16g` 记录 Open 至 100 Get、100 Put、100 回读完成的耗时。恢复探针线程数在命题文档中未规定，本地按单线程测量。10 秒预热和单轮是本地方法；官方预热流程、正式分数归一化数据及最终测试参数未公开，因此本地结果只用于开发判断，不计算官方排名或总分。

## 运行

正确性入口预览和执行：

```sh
python3 taycan_participant_sdk/tests/run_correctness.py --dry-run
python3 taycan_participant_sdk/tests/run_correctness.py \
  --build-dir /var/lib/taycan-workspace/build \
  --results-dir /var/lib/taycan-results/correctness-4g
```

性能档位预览和运行命令见[正确性与性能评测说明](../taycan_participant_sdk/benchmarks/README.md)。开发阶段可以用 `--dry-run` 检查解析后的 Key 数、预算、线程和工作负载，预览不会启动 worker。

## F2 当前状态

当前引擎仍以 F2 为底座。开启 Compaction 和持久化后 ACK 等能力不能凭底座功能推定为符合要求，须用公开测试及官方场景验证。历史小规模结果与 2 亿 Key、4/16 GB 正式性能档无关。
