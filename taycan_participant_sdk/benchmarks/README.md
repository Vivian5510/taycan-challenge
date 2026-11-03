# 正确性验收与性能压测

当前工作区只保留 F2 原生适配器和 RocksDB 基线。Linux x86_64 是正式兼容性环境；macOS
只能作为 F2 API 的开发诊断环境，不能生成正式验收结论。性能脚本会持续打印当前单元、阶段、
已运行时间、真实完成数、区间吞吐、RSS 和磁盘目录大小，结束时打印 setup/measure/close 分项。
`--progress-interval-seconds N` 设置秒级输出间隔（默认 1）；`0` 关闭进度计数和显示。
进度采集仍有少量开销，精确比较时应统一该参数。

两个入口分别回答“是否正确”和“在固定压力下多快、代价多大”。2026-10-29 用户明确命题为 2 亿 Key、16～32 B Key、256 B 定长 Value、4/16 GB 两档，权重 70%/30%；完整约定见 [评测目标](../../docs/evaluation-target.md)。`tests/run_correctness.py` 使用 4 GiB、默认 32 B Key、256 B Value，保留公开各场景默认 Key 数、断言和崩溃协议，严格按 110% RSS 检查。`tests/run_tests.py` 保留原 SDK 默认参数以便重现。`benchmarks/run_performance.py` 驱动独立压测程序，默认档位为 `contest-4g`。压测成功不代表正式评分或完整正确性通过。

## 编译和运行

在项目根目录构建，开启独立的压测 target：

```sh
cmake -S taycan_participant_sdk -B /var/lib/taycan-workspace/build \
  -DCMAKE_BUILD_TYPE=Release -DTAYCAN_BUILD_PERF_BENCHMARKS=ON
cmake --build /var/lib/taycan-workspace/build --parallel 8
```

以上命令在 Linux 内执行。使用目标参数、公开各场景默认 Key 数运行完整正确性：

```sh
python3 taycan_participant_sdk/tests/run_correctness.py \
  --build-dir /var/lib/taycan-workspace/build \
  --results-dir /var/lib/taycan-results/correctness-default
```

固定压力的吞吐测试和延迟测试分开运行：

```sh
# 默认 reference：100 万 Key，30 秒测量，重复 3 次，扫描 1/2/4/8 线程。
python3 taycan_participant_sdk/benchmarks/run_performance.py \
  --build-dir /var/lib/taycan-workspace/build --profile reference \
  --mode throughput --results-dir /var/lib/taycan-results/perf-throughput

# 独立延迟轮次，默认对每次 API 调用计时。
python3 taycan_participant_sdk/benchmarks/run_performance.py \
  --build-dir /var/lib/taycan-workspace/build --profile reference \
  --workloads read,ycsb_a,contest_mix --mode latency \
  --results-dir /var/lib/taycan-results/perf-latency

# 先看最终参数和运行单元数，不启动进程。macOS 也可执行。
python3 taycan_participant_sdk/benchmarks/run_performance.py \
  --build-dir /unused --profile pressure --dry-run
```

Lima VM 的完整调用前缀为 `limactl shell --workdir=/workspace/taycan-challenge taycan-ubuntu24`。构建产物、结果和数据库均放 VM 本地 `/var/lib`，避免宿主挂载文件系统改变 I/O 结论。每次结果目录必须是新目录。脚本发现其他 Taycan 测试正在运行时会拒绝开始压测；仅验证工具时可以明确使用 `--allow-shared-host`，结果会标注存在竞争，不能用于性能对比。

## 压力配置

| 配置 | Key 数 | mem_limit_bytes | 测量时间 | 预热 | 重复 | 线程 |
|---|---:|---:|---:|---:|---:|---|
| `smoke` | 1,000 | 256 MiB | 1 秒 | 0 | 1 | 1 |
| `dev` | 10,000 | 256 MiB | 15 秒 | 2 秒 | 1 | 1/2/4/8 |
| `reference` | 1,000,000 | 256 MiB | 30 秒 | 5 秒 | 3 | 1/2/4/8 |
| `pressure` | 16,000,000 | 256 MiB | 600 秒 | 10 秒 | 3 | 1/2/4/8 |
| `contest-4g`（默认，权重 70%） | 200,000,000 | 4 GiB | 600 秒 | 10 秒 | 3 | 1/2/4/8 |
| `contest-16g`（权重 30%） | 200,000,000 | 16 GiB | 600 秒 | 10 秒 | 3 | 1/2/4/8 |

`smoke` 只验证工具，`dev` 用于快速定位问题。`reference` 对齐公开 sequential 的百万 Key 数；`pressure` 对齐公开 demand_load 的 1600 万 Key／256 MiB 预算。最后两档使用题面讨论中的规模、预算与混合比例，仍是本地参考配置；线程扫描、预热、重复次数是本地方法，不能称为出题方正式 harness。正式数据和评分程序未公开。

所有档位默认 32 B Key、256 B Value，使用可复现的伪随机字节；负载 seed 固定。`contest-*` 锁定 2 亿 Key、档位预算、16～32 B Key 和 256 B Value，缩小规模会拒绝启动；两档对比须使用相同 Key 长度和 seed。脚本不会因运行缓慢或磁盘不足自动缩小数据集。装载会逐条完成全部 Key，实际耗时单独记录；`load` 的测量是固定 N 次写入，不受上表时间窗口截断。其余 workload 全部装载后再预热和运行固定时长。当前引擎写放大较大，大规模档位可能需要远多于逻辑数据量的磁盘；启动检查只排除连逻辑数据都放不下的情况。

可以显式覆盖规模、预算、线程、窗口和轮次，完整参数会写入结果。例如只比较 50/50 负载的线程扩展：

```sh
python3 taycan_participant_sdk/benchmarks/run_performance.py \
  --build-dir /var/lib/taycan-workspace/build --profile reference \
  --workloads ycsb_a --threads 1,4 --repeats 3 \
  --results-dir /var/lib/taycan-results/perf-ycsb-a
```

## 工作负载

| workload | Get / Put / Delete | 用途 |
|---|---|---|
| `load` | 0 / 100 / 0 | 每个唯一 Key 写一次，单线程时 Key 字节序递增 |
| `read` | 100 / 0 / 0 | 完整装载后读取；任何 NotFound 均失败 |
| `update` | 0 / 100 / 0 | 在已装载的 Key 范围内持续覆盖 |
| `ycsb_a` | 50 / 50 / 0 | 通用 YCSB A 读/更新比例；不是完整 YCSB 协议实现 |
| `contest_mix` | 60 / 30 / 10 | 题面讨论的混合比例，删除后的 NotFound 单独计数 |
| `latency_80_20` | 80 / 20 / 0 | 可选的读多写少诊断负载，不代表同类竞品成绩 |

默认均匀随机选 Key，也可 `--distribution hotset`：90% 请求从 1% 热区均匀抽取，其余从全空间抽取。实际热区占比约 90.1%。这是显式热点模型，不是 Zipf。每次 repeat 使用同一 seed；多线程交错和固定时长结束点不保证得到同一操作历史。

每个 workload × 线程数 × 重复次数都使用独立新数据库，避免上一单元的删除状态和版本污染下一单元。对有预装载的单元，预装载线程数与测量线程数一致，成本单列。线程是直接调用 `KVEngine` 的同步工作线程；没有网络，也没有 RESP pipeline。每个线程最多一个在途调用，属于闭环压力；延迟是 API 调用到返回的时间，包含引擎内部锁等待，不含外部队列等待，不做 coordinated-omission 修正。吞吐包含生成请求、校验 Value 和循环控制的开销。

## 结果口径

结果目录包含 `results.json`、中文 `README.md`，每个单元另有 `output.log` 和 `processes.jsonl`。

- 分开记录 Open、完整预装载、预热、测量、关闭耗时；吞吐为测量期成功操作数除以测量期实际墙钟时间。最后一个在途调用可以超过时限，实际延长时间计入分母。
- 吞吐模式关闭逐次延迟计时，延迟字段为 `null`。延迟模式默认采全部调用；可用 `--sample-every 100` 平均每 100 次采一次，抽样模式与 seed 入报告。
- 延迟用固定内存的直方图，覆盖整段测量；分别报告整体及 Get/Put/Delete 的 p50/p95/p99/p99.9、样本数和精确样本最大值。分位数是直方图桶上界，量化误差小于 1.563%，不是无误差的逐样本排序。样本少时尾分位数不稳定，必须连同样本数看。
- `summary` 汇总每个单元重复运行的吞吐中位数、最小值、最大值和完成轮数。失败及尚未执行的轮次不会算进成功样本。
- `contest-*` 和目标正确性入口 RSS 上限为严格的 `floor(1.1 × mem_limit_bytes)`；历史诊断档仍采用 `floor(1.1 × mem_limit_bytes) + 8 MiB`，不能混用为正式额度。压测默认每 20 ms 采样并记录最大采样间隔；正确性采样间隔保持 2 ms。
- 记录测量前后及关闭后的目录文件逻辑长度，并记录测量期 `/proc/self/io` 的读写字节增量。这些是进程 I/O 诊断数据，不等于 SSD 实际 NAND 写入量或严格物理写放大。
- 保留命令、参数、二进制和当前源码哈希。源码哈希用于追溯当前工作区，二进制哈希才标识本次被测产物；不要在旧二进制下修改源码后声称对应同一构建。
- 成功的数据库默认清理，`--keep-data` 保留；失败数据库和日志始终保留。内存超限、超时、错误返回、Value 损坏、非删除负载漏读、未完整装载均失败。

性能程序没有降低持久化语义的开关：Put/Delete 成功返回必须已持久化。是否满足这个保证仍由实现审查和崩溃测试验证。OS page cache 正常保留，预热后测试不等于冷盘测试；1600 万 Key 也不能自动证明查询一定命中 NVMe。Apple Silicon 上 QEMU x86_64 只用于本地迭代，不用它推断正式机器 QPS。

## 同定位基线与方法参考

主对比对象修正为 **RocksDB（sync=true、WAL 开启）和 Badger（SyncWrites=true、等待事务提交）**，并要求在相同总进程 VmRSS 口径下运行。LMDB 可作为有条件的 B+tree 对照。F2 用于磁盘 ColdIndex 架构参考；Dragonfly、Garnet 和普通 FASTER 的公开配置只作方法或实现参考。

详细的选择依据、内存/持久化条件、原始公开基准与参数陷阱见 [同定位磁盘 KV 基线](../../docs/disk-kv-benchmark-baselines.md)。RocksDB 的 `fillsync` 会自动把写入数除以 1000；Badger 的默认 SyncWrites=false、IndexCacheSize=0 与 1 MiB ValueThreshold 也都需要显式核对，不能直接照搬默认成绩。

RocksDB 适配器已接入，Badger 尚未接入。每个二进制通过 `--describe` 报告实际引擎及版本，单引擎报告写入 `cross_engine_comparison=false`；对比入口生成独立的 `comparison.json`，核对两轮参数相同。配置条件见 [baseline-policy.json](baseline-policy.json)。共享机器上的工具验证不作为正式的性能比较数据。

### 构建 RocksDB 基线并运行同条件对比

Ubuntu 24.04 VM 中安装 `librocksdb-dev`，当前固定使用仓库版本 `8.9.1-2`。基线通过独立构建目录选择适配器，不修改参赛引擎：

```sh
sudo apt-get install librocksdb-dev
cmake -S taycan_participant_sdk -B /var/lib/taycan-workspace/baseline-rocksdb-20261028 \
  -DCMAKE_BUILD_TYPE=Release -DTAYCAN_BUILD_PERF_BENCHMARKS=ON \
  -DCONTEST_ENGINE_DIR=/workspace/taycan-challenge/taycan_participant_sdk/benchmarks/baselines/rocksdb
cmake --build /var/lib/taycan-workspace/baseline-rocksdb-20261028 --parallel 2

python3 taycan_participant_sdk/benchmarks/run_comparison.py \
  --build-dir /var/lib/taycan-workspace/build \
  --baseline-build-dir /var/lib/taycan-workspace/baseline-rocksdb-20261028 \
  --results-dir /var/lib/taycan-results/compare-reference \
  --profile reference --mode throughput
```

正确性入口同样可以用 `--build-dir` 指向 RocksDB 构建，复用公开场景。适配器的 Put/Delete 使用 `sync=true`、WAL 开启、单 Key 调用、关闭压缩；总缓存容量取预算的 5/8，其中 memtable 用 WriteBufferManager 限到 1/4 预算并对同一缓存计费，余量用于其他进程开销。只有外部监控采到的 VmRSS 符合标准才能视为该次内存检查通过，不能仅由这些配置推断。当前适配器最低支持 64 MiB 预算。

单独运行 F2 或 RocksDB 使用相同的 `run_performance.py` 参数；分别保存结果后再比较
`results.json`，避免把控制器时间混入被测引擎。

历史百万 Key 对比队列已取消，不会自动恢复。旧的自研实现及冻结队列控制器已退休；当前只用 F2 原生和 RocksDB 两个构建。`run_comparison.py` 可顺序运行这两个构建并核对参数，但 F2 的原生完成与 RocksDB 同步持久化 ACK 不等价，不能据此直接排名。

2026-10-28 本地修复了公开监控器的一处退出竞态：进程已被主线程回收时，`/proc/<pid>/status` 消失，不再误报监控失败。权限和格式错误仍失败，完全没有 RSS 样本仍失败，超限判定保持原样。原始 tar 包与正在运行的评测快照保留，旧报告中的误报不会被改写为 PASS。

默认压力规模和内存取自 SDK/题面；30 秒窗口可在 F2 C++ 基准核对，pressure 的窗口改为题面讨论中的 600 秒。预热、3 次重复及 1/2/4/8 线程是本地方法。原先的 Dragonfly/Garnet 来源继续保留为历史方法参考，固定版本与新补充的磁盘 KV 来源都记录在 [reference-sources.json](reference-sources.json)。

## 测量工具自身的验证

可选编译专用故障引擎 fixture，它只验证测量工具，绝不链接真实压测或参赛引擎：

```sh
cmake -S taycan_participant_sdk -B /var/lib/taycan-workspace/build \
  -DTAYCAN_BUILD_PERF_BENCHMARKS=ON -DTAYCAN_BUILD_BENCHMARK_TESTS=ON
cmake --build /var/lib/taycan-workspace/build --target taycan_bench_fixture
TAYCAN_BENCH_TEST_BINARY=/var/lib/taycan-workspace/build/taycan_bench_fixture \
  python3 -m unittest discover -s taycan_participant_sdk/benchmarks/tests -v
```

测试验证未装载/漏读不能虚增吞吐、完整 Value 损坏和线程异常必须失败、装载耗时不计入稳态、毫秒/微秒单位正确、禁用采样时不能伪造零延迟，以及参数溢出不会绕过线程数限制。


2026-10-28 用户停止本轮长测后，排队的百万 Key 对比已撤销，自动检查任务已停用。上述自动接续描述保留为历史记录，当前不再等待或启动该任务。构建和参数快照仍保留，没有产生本轮百万 Key 横向成绩。
