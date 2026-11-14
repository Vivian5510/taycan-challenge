# 原始结果复核与范围

- `public-batch4` 的 Compaction 失败在写入返回 `IOError` 和读校验，尚未到空间断言；普通 crash_recovery 第一轮 verify 中 Key 491 为 NotFound，前序已确认操作数171592。此前进度消息将它说成空间断言失败不准确，以原始日志为准。
- `public-enabled` 是 `aio-buffered`，观察到第12次GC复制完成后长时间无文件/事件进展；两次线程栈显示GC等待独占维护门，writer等待共享门，reader在Pending I/O中。人工停止精确worker，原FAIL:-9保留。已确认停滞现象，尚未证明缓存开关是根因。
- `public-candidate` 是 `hot-inline-nocache`，约75秒时人工停止，最终GC事件仍有第14周期complete/serial594970。这个结果只能称中止，没有足够证据称它与前一配置相同死锁，也不能据此断言hot-inline本身有错误。此前进度消息的归因过强；不把它计为通过或已证实数据错误。
- `selected.json` 是逐轮选项，不代表全部历史结果都属于当前候选。筛选四组、buffered专项、hot-inline专项、completion=4和completion=2各有独立路径与二进制清单。
- completion=2指一个poll最多取出两个已完成I/O回调；它不把两个Put合并为一个持久提交。成功写入仍逐次封口、同步、发布索引。
- completion=2的单独Compaction运行原公开C++用例，100000Key/16B/4096B/10轮/4GiB，返回0并验证最终值和重开；该单独运行没有外部RSS采样，不能据它声明Linux VmRSS验收通过。SIGKILL5轮与71项专项使用外部RSS监控。
- 保留全部原始失败与中止；未再运行的原场景不得算通过。completion=4失败、completion=2通过一次回归不证明问题仅由上限造成；正式采用仍需并发/GC恢复回归和固定性能复验。
