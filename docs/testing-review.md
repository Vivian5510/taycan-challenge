# Taycan 参赛包测试规则与脚本解读

核对日期：2026-10-27。对象：已下载的官方 participant SDK 1.0.1。本文按实际代码说明公开测试做什么、如何判定，以及它与正式评测的边界。

2026-10-29 更新：后续目标参数为 2 亿 Key、16～32 B Key、256 B 定长 Value，4/16 GB 两档权重 70%/30%。`tests/run_correctness.py` 已使用 4 GiB、默认 32/256 B 和严格 110% RSS，公开各场景 Key 数与断言保留。下文默认参数描述对应原 SDK 的 `run_tests.py`，作为历史规则核对保留；当前入口见 [评测目标](evaluation-target.md)。

2026-10-28 增加两个明确入口：`tests/run_correctness.py` 原样调用官方公开验收；
`benchmarks/run_performance.py` 单独计量装载、稳态吞吐、操作延迟、RSS 和磁盘开销。
公开测试的固定负载和耗时本来就可用于同机版本对比；新增入口让各阶段的代价更容易定位。
压力档位、来源和运行命令见 [正确性与性能评测说明](../taycan_participant_sdk/benchmarks/README.md)。

## 1. 先明确测试的作用

公开包有 14 个场景名：12 个 C++ GoogleTest 程序场景和 2 个 Python 控制的崩溃恢复场景。Checkpoint 是可选项，其余是基础验收。公开程序没有正式压测的吞吐/P99 评分器；报告中的 elapsed_seconds 是整个场景或进程耗时，不能当作正式性能成绩，也不是单独计量的 Open 延迟。

正式题面另外规定 2 亿 Key、4 GB/16 GB 两档内存、600 秒混合负载，正式数据与评分程序不随包提供。公开 mixed 场景是约 1:1:1 的 Put/Delete/Get，不能把它等同于正式的 60% Get + 30% Put + 10% Delete。

依据：[README](../taycan_participant_sdk/README.md)、[统一入口](../taycan_participant_sdk/tests/run_tests.py)。

## 2. 实际调用关系和参数

调用关系是：先用 CMake 编译；run_tests.py 选场景、填参数、创建结果目录；普通场景由 ProcessRunner 启动相应的 *_test；崩溃场景交给 crash_test.py，进一步启动 crash_worker；被测进程始终由 ProcessRunner 监控。

run_tests.py 实际不会执行 CMake，也不会自动编译。--build-dir 指向已经有二进制文件的目录；所有相对路径按当前工作目录解析。--list 在 Linux 检查之前返回，所以 macOS 也能列场景；正式运行则检查 /proc/self/status。

通用默认值：

- num_keys=100000；key_size=64 B；value_size=128 B；rounds=10。
- mem_limit_bytes=256 MiB；seed=20261021。
- 每个被测进程超时 86400 秒；RSS 采样间隔 2 ms；崩溃试验 5 轮。
- 场景表先覆盖通用参数，最后再应用 CLI 显式覆盖。
- Python 使用 --num-keys、--mem-limit-bytes；C++ 使用 --num_keys、--mem_limit_bytes。
- Python 只转发 num_keys/key_size/value_size/rounds/mem_limit_bytes 等列出的参数，不支持所有 C++ 选项。例如 reader_threads、space_ratio、reclaim_timeout_seconds 仅在直接调试 C++ 时可指定。
- 公共 C++ 参数校验限制 num_keys <= 100000000。实测传入 200000000 会退出 2；正式 2 亿 Key 评测需要另外的正式 harness，不能直接用这个公共选项模拟。
- 基础 basic 测试手工构造空 Value、二进制和长字符串，不受公共 value_size 最小 16 B 限制的约束。

依据：[run_tests.py](../taycan_participant_sdk/tests/run_tests.py)、[test_options.cc](../taycan_participant_sdk/tests/test_options.cc)、[test_main.cc](../taycan_participant_sdk/tests/test_main.cc)。

## 3. 每个公开场景实际检查什么

N 表示 Key 数，r 表示覆盖轮数。下列规模仅是官方默认；缩小规模只用于调试，不构成验收。

### basic

手工用例，2 个 GoogleTest：

- 1 字节 Key、带空字节的 Key、1023 字节 Key。
- Value 从空变长、再缩短到空，最大 100 KiB-1 字节。
- 区分 a、ab、a\0、a\0b，不得按 C 字符串的结尾误截断。
- 不存在读、删除不存在 Key、重复删除。
- 修改或删除相似前缀 Key，其他 Key 必须不受影响。

它能发现 key/value 长度、缓冲复用与空值处理问题，不负责证明持久化。

### sequential

N=100 万，64 B Key、128 B Value。按顺序生成字节序递增 Key，先写 N 条，再全量读 N 条。

### random

N=500 万。用 KeyOrder 无额外置换数组地遍历所有 Key 各写一次；随机读取 N 次；最后还会全量读取 N 次。总计约 1500 万次引擎调用，不只是“写 500 万、读 500 万”。

### overwrite

N=10 万，r=10。每轮覆盖所有 Key，并抽查第一个、中间和最后一个 Key；最后全量验证第 r 版。中间版本也要正确，不能只在关闭时整理。

### delete

N=10 万。每个 Key 执行 Put、Delete、验证 NotFound、重复 Delete；再 Delete 一个从未写入的 Key 并检查 NotFound。Delete 不存在必须返回 OK。

### mixed

N=100 万。先写 N 条，再执行 3N 次混合操作，各分支按随机数模 3 选择。测试用 uint32_t 版本数组记录参考状态，版本 0 代表不存在；最后全量校验。参考数组占 4N 字节，Python 单独给对应额度。

### recovery

N=10 万。先写 v1，再对约 2/3 Key 覆盖 v2、其余删除；正常销毁引擎并重新 Open，全量验证。然后重新插入已删 Key、覆盖部分存活 Key、删除另一部分，再次正常关闭和 Open 后全量验证。它检验恢复后的引擎还可以继续写，再次恢复也稳定。

### concurrent

N=10000，含两个子测试：

1. DisjointKeys：4 个线程各负责不同 Key，对每个 Key 执行 v1 → v2 → 删除 → v3，每步立即读回；最后验证所有 Key 都是 v3。
2. SingleWriterVersionBounds：1 个写线程、默认 4 个读线程。写线程按 64 个 Key 的窗口多轮更新；读线程读窗口里的 Key。Get 前采已完成版本作为下界，Get 后采已开始版本作为上界，读到的版本必须落在两者之间，而且整个 Value 字节都要匹配。最后还会重启验证。

这不是“只要并发不崩就行”，也不是完整的任意并发历史线性一致性检查器。公开代码没有专门对同一 Key 进行多个前台写线程竞争的场景。单 Key 线性一致性仍是接口要求，后续必须补充自测。

### demand_load

N=1600 万，64 B Key、128 B Value，256 MiB 预算：

1. 写入全部 Key。
2. 销毁并重新 Open。
3. 4 个读线程，每个线程都以自己的两种遍历顺序全量读两遍。

因此读取总量是 4 × 2 × 1600 万 = 1.28 亿次。单纯完整 Key 字节数就约 976.6 MiB，明显超过预算，原样保存全部完整 Key 到堆内存不可行。

该程序在同一个进程中销毁/重新 Open，并未主动清理 OS page cache。它通过结果、全程 RSS 和通用超时施压，没有内置“本次 Open 读了多少字节/扫描了哪些文件”的探针。满足“无需全量扫描重建索引”还必须用设计、实现审查和额外 I/O 验证来说明。

### memory_limit

N=2000 万，64 B Key、128 B Value，4 个线程各操作不同 Key：

- 每个新 Key 先写 v1，再写 v2 或删除。
- 每处理 64 个 Key，读取自己线程之前写过的一个较旧 Key。
- 线程结束后随机遍历全量校验。
- 正常销毁、重新 Open，再全量校验。

写/删除调用共 2N，最后两次完整 Get 共 2N，另有约 N/64 次回读。主要同时验证持续写入中的预算、旧 Key 访问、删除状态和恢复。

### compaction

N=10 万，16 B Key、4096 B Value，覆盖 10 轮，2 个前台写线程、1 个前台读线程：

- 两个写线程分别处理不同 Key，部分轮次删除 id%3==0 的 Key；最后一轮也保留这些删除。
- 测试用 64 把锁保护参考状态与同一 Key 的前台操作，所以它并不制造两个前台写者对同一 Key 的竞争。但引擎后台 GC 仍可能与前台操作交错。
- 写完后停止前台读写，保持引擎打开，最多等待 120 秒，每 100 ms 检查空间。
- 对目录递归累计所有普通文件的 file_size：数据、索引、WAL、临时文件都在内。统计逻辑长度，不是磁盘实际分配块数。
- 达到空间要求后全量验证，关闭、重新 Open 再全量验证。

空间要求：

    目录所有文件总长度 <= 3 × 存活 Key/Value 逻辑字节数 + 16 MiB

默认最后存活 66666 条，live_bytes=274130592，目录上限=839168992 字节，约 800.29 MiB。

不能等析构时才回收；索引保留旧文件引用、WAL 不截断或临时文件不清理都会影响这一判定。公开基础空间门槛与正式加分的“有效字节占比至少 70%”不是同一个门槛。

### checkpoint（可选）

N=131072，64 B Key、4096 B Value，两个子测试：

- IndependentCopy：写入后删除部分 Key，创建副本；再修改原库、关闭并移除原库目录；独立打开副本验证快照。副本必须还能继续 Put/Delete，并在再次 Open 后保持自己的更新。
- ConcurrentReadWrite：预先写 v1；1 个写线程按 Key 顺序逐条变更，偶数改为 v2、奇数删除；另一个线程并发读取，主线程创建副本。副本必须是这串有序变更的一个完整前缀，该前缀还必须处于调用前后的允许区间中。不能得到“前面的 Key 是旧状态，后面的反而是新状态”的拼接副本。

未实现时只有精确返回默认的 IOError: Checkpoint is not supported 才会触发 GTEST_SKIP。不同错误文本不会作为可选跳过。公开包未包含题面要求的所有额外 Checkpoint 故障场景，例如创建中崩溃后，半成品副本不能被当作有效空库打开。

### crash_recovery / compaction_crash

两者使用相同的 Python 故障协议；普通恢复准备 1 轮，Compaction 恢复准备 10 轮且 Value 为 4096 B。默认每场景 5 次独立试验，每次都从新数据库开始。

详细协议见下一节。

依据：对应 tests/*_test.cc 和 [默认规模表](../taycan_participant_sdk/tests/run_tests.py)。

## 4. SIGKILL 如何判定“成功的操作不能丢”

每轮分 prepare → serve → kill → verify → 再 verify：

1. prepare 在独立进程中完成初始轮次，正常退出。
2. serve 进程 Open 后发 READY。Python 发一个 G，worker 根据操作编号生成一次 Put/Delete。
3. 接口成功返回后，worker 才通过独立管道发送 ACK n。Python 收到完整 ACK 才发送下一条，因此任意时刻最多一条操作未确认。
4. Python 先执行若干确认操作，最后再发一条但不等 ACK，然后 kill。
5. kill 后排空回复管道，完整 ACK 仍算已成功；截断的半条 ACK 不算。
6. verify 新进程全量检查：所有已确认操作必须保留；仅最后一条未确认操作涉及的那个 Key 允许新/旧两种合法结果。
7. 第一次 verify 返回实际恢复的前缀 n 或 n+1；再启动第二个 verify，必须精确恢复同一个前缀，不得再次摇摆。

故障时机：第 1 次先 SIGSTOP，再发最后一条并 SIGKILL，保证它未执行；第 2 次立即 kill，与执行竞争；后续随机等待 0～10 ms。

worker 的操作编号循环覆盖 Key。第 2、5、8、11…轮会删除约 1/3 Key，随后轮次再插入。这同时检查删除不能复活、覆盖不能倒退。

实现含义：

- ACK 不帮引擎做任何 fsync。同步持久化必须在接口成功返回前完成。
- 不能只在析构时保存；serve 进程被 kill 时不执行正常析构。
- Group Commit 不能只等待“凑够一批请求”：该协议只有一条在途请求，必须有单请求也能前进的提交路径。
- 随机 kill 不保证恰好命中索引发布、Manifest 提交或旧文件删除等某个内部步骤。后续应通过实现自己的故障点扩大覆盖。
- SIGKILL 不模拟断电，内核缓存仍在。通过这个测试不能代替机器故障后的持久化保证及代码审查。

依据：[crash_test.py](../taycan_participant_sdk/tests/crash_test.py)、[crash_worker.cc](../taycan_participant_sdk/tests/crash_worker.cc)。

## 5. RSS、超时和结果判定

普通场景限制：

    floor(mem_limit_bytes × 1.1) + 8 MiB

mixed、compaction 另加 4 × num_keys 字节，补贴测试自己的参考数组。默认常规上限是 303667609 字节，约 289.6 MiB。

ProcessRunner 在外部 Python 线程中默认每 2 ms 读取 /proc/<pid>/status 的 VmRSS。限制覆盖被测进程：堆、缓冲、分配器残留/碎片、线程栈、映射文件页等。普通 read/write 带来的未映射 OS page cache 不计入进程 RSS。公开 runner 没有清理 page cache；正式评测按题面另有预热和缓存控制。

超过额度时立即 SIGKILL 进程组；不能继续跑完以后再算平均。超时、非预期退出、读 RSS 错误、没有有效 RSS 样本都会失败。预期故障注入记 EXPECTED_KILL，原本由内存超限导致的 kill 不会被误算成成功注入。

监控是采样，不是内核 cgroup 硬上限：不保证抓到每个瞬时峰值。报告保存 max_sample_gap_seconds，可检查实际采样间隔。

监控器有 8 个自己的回归检查，覆盖正常、内存超限、超时、意外退出、预期 kill、RSS 读取错误、无样本及超限与故障注入冲突。macOS 上这 8 个都会 SKIP，退出码 0 不能解读为它们通过了。

--rss-slack-bytes 的 help 标注为 provisional，需要原生 Linux 校准；正式口径以出题方最终规则为准，不能依赖所有余量长期可用。

依据：[process_runner.py](../taycan_participant_sdk/tests/process_runner.py)、[process_runner_test.py](../taycan_participant_sdk/tests/process_runner_test.py)。

## 6. 数据如何生成和核对

workload.h 根据 seed、key_id、version 用固定算法生成数据，不依赖标准库随机分布。Key 的前 8 字节通过双射生成，保证测试 Key 唯一；顺序模式使用大端 id 产生字节序递增 Key，其余字节仍有数据内容。

Value 的前 8 字节是 id，接下来 8 字节是 version，剩余字节由固定算法生成。大部分检查比较完整 Value；并发读也会在确认长度和版本范围后重新生成全部字节，检查是否撕裂或损坏。version 只存在于测试生成器中，接口不要求存储引擎支持用户可见的多版本。

KeyOrder 使用与 N 互质的步长遍历所有 id，无需在测试进程里分配大型随机排列数组。

固定数据可复现，但正式评测可以更换 seed、salt、参数与操作组合。实现不能解析这种 id/version 编码来特化查询。公开测试也没有专门构造“引擎所用 Hash/指纹相同”的 Key；完整 Key 比较和碰撞正确性仍需实现和自测。

依据：[workload.h](../taycan_participant_sdk/tests/workload.h)、[test_helper.h](../taycan_participant_sdk/tests/test_helper.h)。

## 7. 日志、命令与容易混淆之处

Linux 上从项目根目录运行：

    cmake -S taycan_participant_sdk -B build/linux -DCMAKE_BUILD_TYPE=Release
    cmake --build build/linux --parallel 4

小规模调试（不能替代默认规模验收）：

    python3 taycan_participant_sdk/tests/run_tests.py --build-dir build/linux \
      --tests basic,recovery,concurrent --num-keys 10000 --seed 42 \
      --results-dir analysis/linux-debug-seed42

恢复/按需加载的默认规模：

    python3 taycan_participant_sdk/tests/run_tests.py --build-dir build/linux \
      --tests recovery,demand_load,memory_limit \
      --results-dir analysis/linux-default-memory

默认全套公开验收（包括可选 Checkpoint 的实现状态检查）：

    python3 taycan_participant_sdk/tests/run_tests.py --build-dir build/linux \
      --results-dir analysis/linux-default-all

单独监控器自测（Linux）：

    python3 taycan_participant_sdk/tests/process_runner_test.py

直接调试某个 C++ 场景，可以传更多 C++ 选项，但没有 Python 的 RSS/超时验收。例如：

    build/linux/compaction_test --num_keys=10000 --reclaim_timeout_seconds=10

run_tests.py 把输出写入 output.log；场景的进程记录在 processes.jsonl，顶层 results.json 记录 PASS/SKIP/FAIL、参数、额度、峰值和耗时。失败保留数据库便于复现，成功清理数据库但保留报告。

--results-dir 按代码实际使用是结果根目录；重复使用同一个目录会重新写 results.json。建议每轮给独立目录。多个 --tests 名称用逗号连接，不能有重复项；源码没有替名称 strip 空格。

CMake 只编译不验收。原 SDK 的 package.sh 只打包；当前 F2 实验工作区已移除旧的打包入口，原始合同保存在 `analysis/retired-implementations-20261030/sdk-contract.md`。公开接口和正确性断言不因 F2 实验而放宽。

## 8. 当前实际验证结果

当前主机为 macOS ARM64，未使用或伪造 Linux RSS 监控。

本次完成：

- 核对 SDK 原压缩包与解压目录的 30 个源码/说明文件：没有修改或缺失。
- Python 脚本语法检查通过。
- --list 正常列出 14 个场景。
- 官方 run_tests.py 正常拒绝在 macOS 上运行，退出 2，提示 requires Linux /proc。
- 监控器自测 8 项全部 SKIP，未宣称其通过。
- 用未修改的内存模板直接运行 12 个 C++ 场景，均缩小为 1000 Key。
- basic、sequential、random、overwrite、delete、mixed 通过；recovery、concurrent、demand_load、memory_limit、compaction 失败；Checkpoint 跳过。
- 失败均符合内存模板没有重启恢复的现状；concurrent 中 DisjointKeys 通过，SingleWriterVersionBounds 在重开后的校验失败。
- 传 --num_keys=200000000 被公共参数校验拒绝，退出 2。

详见 [本次调试报告](../analysis/template-debug-20261027-174551/results.json) 及同目录日志。以上只是验证测试行为，不是官方规模验收，也没有性能含义。

## 9. 实现前应据此确认的约束与后续自测

优先验证顺序建议：接口边界 → 小规模连续恢复 → 多线程读写 → ACK 崩溃恢复 → 受限内存按需加载/持续写入 → Compaction 回收与崩溃 → Checkpoint → 独立性能 benchmark。

额外自测应覆盖：

- 对同一 Key 的多写者竞争以及 Put/Delete/后台搬迁交错。
- 强制 Hash/指纹碰撞，验证完整 Key 比较。
- 记录尾部截断、校验失败、写/同步/元数据发布的 I/O 错误。
- 数据持久化、索引提交、Manifest 发布、文件删除等阶段的确定性故障点。
- 重启读取字节数与延迟随数据量增长的关系，证明复用了索引而非全量扫描重建。
- 后台持续回收时的吞吐和 P99，避免只在停写后表现良好。
- 创建 Checkpoint 中崩溃、半成品副本拒绝 Open，以及原目录删除后的副本持续读写。

正式压测脚本、线程数、具体随机分布、预热长度和多轮指标聚合方式尚未在公开包中给出；正式数据也不提供。因此公开 PASS 只证明基础自测结果，不能用来推断最终排名。


## 2026-10-28 监控器退出竞态修复

原始长测的 sequential 日志显示全量写读校验通过、进程退出码 0，但外部采样线程与主线程的 `wait()` 发生竞态：主线程先回收进程，采样线程再读 `/proc/<pid>/status` 得到 ENOENT，仍可能因同时调用 `Popen.poll()` 的时序而误记 monitor_error。原始快照结果继续保留 FAIL，不能事后改成完整验收通过。

本地 `read_rss` 将 ENOENT/ESRCH 作为该次采样已无进程地址空间处理；权限、格式等监控错误仍然失败，没有任何有效 RSS 样本仍然失败。新增对应回归，内存上限、超时与预期 SIGKILL 协议保持原样。正在运行的固定 runner 快照不改，之后的新轮次才使用修复。
