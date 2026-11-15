# 批量完成事件的可重入性检查

独立小实验向同一QueueIoHandler提交两次4KiB写。确认两个OS写入均完成，尚未轮询回调。
第一个回调必须能通过重入TryComplete驱动第二个已完成回调，然后退出。这与FASTER回调可能
递归发起磁盘读取并在DoThrottling中驱动disk.TryComplete的使用方式有关。

同一测试、同一I/O（修复前）：

- completion=1：两个回调均完成，重入可推进，未超时，返回0。
- completion=2：最终两个回调均被调用，但第一个回调内等第二个时超时，返回1。

源码机制：native_aio_batch.inc先把多个完成job从共享aio_jobs_全部移入TryComplete栈上的
completed数组，再挨个执行回调。第一项回调重入时，后续已经完成的job不在共享队列中，
必须等外层回调返回才重新获得执行机会。若第一个回调等待这些事件，会形成依赖环。
completion=1没有隐藏其他完成事件。

这是已复现的I/O handler可重入性缺陷，但本轮没有证明它就是先前Compaction停滞、
completion=4恢复读不到Key的唯一根因，不能替代对那些失败的定位。

本轮completion=2的两轮吞吐也无收益，因此不会采用批量完成参数。不通过增大超时、
关闭回收或放宽ACK条件绕过测试。后续若继续研究批量完成，应让待分发事件在回调期间
仍可被重入/其他合法poller取到，再重新做FASTER节流、回收与崩溃恢复验证。

## 2026-11-15 修复

按"让待分发事件在回调期间仍可被重入取到"的方向修复了 native_aio_batch.inc：
一次加锁仍收集最多 batch 个就绪 job，但释放锁前把 completed[1..N-1] 逆序推回
aio_jobs_ 队首并标记 inline_completed=true，使重入 TryComplete 能在队列里找到兄弟
事件；外层调用只执行 completed[0] 的回调。

修复后两 variant 均通过（结果见 reentry/results.json）：

- aio-nocache：callbacks=2 sibling=1 timed_out=0 errors=0，返回0。
- aio-batch2：callbacks=2 sibling=1 timed_out=0 errors=0，polls=2，返回0。

修复前 aio-batch2 自旋 1775 万次后超时，修复后仅 2 次 poll 即完成，依赖环消失。

原始数据：[结果](reentry/results.json)、[基线输出](reentry/aio-nocache.stdout.log)、
[候选输出](reentry/aio-batch2.stdout.log)。这是独立I/O组件诊断，不是数据持久化通过证明。
