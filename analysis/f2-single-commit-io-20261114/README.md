# F2 单请求 I/O 调优

恢复修复已合入 main `26e4466`。本轮性能试验在独立分支进行，主干默认仍为一次完成回调、逐调用 fsync，无 group commit。

## 四配置筛选

百万唯一 Key、32 线程、32/256 B、4 GiB、seed=20261021；各格新库，GC 与冷热迁移开启、8 MiB 段。所有编译先结束，逐格装载完整百万条。筛选单轮仅用于选择下一项实验。

| 配置 | 装载 ops/s | 后续验证 |
|---|---:|---|
| aio-nocache | 11,997 | 本轮基线 |
| aio-buffered | 16,854 | GC 发布等待停滞，人工停止；未通过完整回归 |
| hot-inline-nocache | 13,352 | 人工中止，证据不能判定相同死锁；未通过完整回归 |
| hot-inline-buffered | 14,749 | 仅筛选，未采用 |

四配置前置门槛12/12通过，但它只是 Basic 固定用例、1000 Key Recovery/Concurrent 诊断，不能代替完整验收。

## AIO 完成回调上限候选

completion=2/4 是一次 poll 取出的已完成 I/O 回调上限，不是持久化 group commit。每次成功写入仍独立封口、同步后发布索引。使用已有完成回调批量实现和显式构建参数；没有把默认改成2或4。

- completion=4：Compaction 写入返回 IOError，普通 SIGKILL verify 读不到一个已确认 Key，拒绝采用。不是空间断言失败。
- completion=2：原公开 Compaction 100000 Key、16/4096 B、10轮、4GiB通过空间/内容/重开；该单独运行未采样 RSS。公开 SIGKILL 100000 Key、16/128 B、4GiB 五轮通过；71项诊断通过。
- completion=2 的真实 fsync 暂停时不 ACK，EIO 返回失败；ACK 后强杀重开正确。相同四种未确认尾部页丢失模型通过。
- 主干内核改动没有混入上述候选。默认IO/缓存/完成上限保持不变。

[原始日志解释与更正](evidence-corrections.md)包含人工中止、工具前置错误和归因边界；原FAIL和中止不改写成PASS。

## completion=2 两轮确认

基线 aio-nocache 与 aio-batch2：每格新库完整装载百万 Key；update、contest_mix 预热2秒、测量10秒。两轮反转引擎次序，所有参数、驱动、持久化条件固定。

| 负载 | 基线完成轮 | 基线中位 ops/s | 候选完成轮 | 候选中位 ops/s | 候选/基线 |
|---|---:|---:|---:|---:|---:|
| load | 2/2 | 10,479 | 2/2 | 10,457 | 0.998 |
| update | 2/2 | 10,355 | 2/2 | 8,721 | 0.842 |
| contest_mix | 2/2 | 23,481 | 2/2 | 22,961 | 0.978 |

确认矩阵状态：PASS。单轮不计算最终收益，缺格不当通过。

## hot-inline补跑

compaction: FAIL；timeout。
百万次写入完成后，栈采样主线程在最终DBHelper::check/Get的Pending路径，GC等待独占维护门；没有把最初75秒的人工中止当作失败证据。当前完整补跑使用固定600秒开发时限。

本机 macOS ARM64 开发对照；不是正式 Linux/NVMe、2亿 Key 验收。此前受容量限制的正式规模没有被重新标为通过。

[固定计划](plan.json) · [原始确认结果](confirm/results.json) · [计数/哈希核对](confirmation-verification.json) · [候选专项](validation-batch2/results.json) · [候选SIGKILL](batch2-crash-runner.log) · [采用门槛](retention-policy.json)。

## 当前采用结论

DO_NOT_ADOPT_COMPLETION_BATCH2

completion=2 两轮没有收益，并在回调重入组件诊断中失败，不采用。主干默认不变。[可重入性证据](reentry-findings.md)。
