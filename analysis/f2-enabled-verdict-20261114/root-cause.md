# 已确认的失败机制

被测源为main 144a5ec，durable compaction开启。

`PrefixCommit`先把Key/Value和末尾校验footer写入HybridLog，异步写完成后才执行SyncDataFiles。
fsync尚未完成时没有对该操作ACK，这一点正确。但崩溃可以让同一次未确认写入的不同文件页
部分保留。恢复扫描遇到一个校验有效的footer，并不说明其前面的全部页都曾同步成功；
当前 `PrefixFindCommittedTail` 在前部记录缺页/头被零填充时抛出
`malformed data record inside committed prefix`，导致整个Open失败，连完整保留的已确认前缀也不能提供服务。

复现实验将第68次100KiB写入停在before-data-sync，前67次成功操作的全部字节保留。
损失前部尚未确认页、保留后部footer时Open稳定返回IOError。全部未确认尾部丢失、全部保留、
尾页丢失均可恢复。错误属于尾部识别/恢复容错边界；不是已经确认的67次操作被实验擦掉，
也不是fsync本身无效。此子实验使用SDK支持且Basic覆盖的大value，超出正式性能固定256B。
不能把这个结果等同于原生SIGKILL失败，也不声称固定256B负载已复现相同缺陷。

本轮按用户要求先给当前实现的验收结论，不将实验分支上的尾部补丁或GC调试改动偷偷合入主干，
不在失败后调整断言使其通过。
