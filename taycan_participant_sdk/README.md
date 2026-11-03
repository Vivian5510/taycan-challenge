# F2 底座评估与 Taycan SDK

当前实现入口是 [F2 原生适配器](../experiments/faster-native-adapter/README.md)，RocksDB 用作基线。
自研引擎、旁路 WAL F2 和 dashboard 已从活动代码中清除。接口保留在 `include/`，
公开用例和故障协议保留在 `tests/`；旧的完整 SDK 合同在 [历史合同](../analysis/retired-implementations-20261030/sdk-contract.md)。

## 构建

从工程根目录运行，默认构建 F2：

```sh
cmake -S taycan_participant_sdk -B build/f2-native-baseline-arm64-20261030 \
  -DCMAKE_BUILD_TYPE=Release -DTAYCAN_BUILD_PERF_BENCHMARKS=ON
cmake --build build/f2-native-baseline-arm64-20261030 --parallel 4
```

依赖 CMake ≥ 3.24、C++17、TBB；Linux F2 后端另需 libaio 和 libuuid。
目标兼容环境为 CentOS 7 / Linux x86_64 / GCC 8.3，macOS 后端仅作开发诊断。
`CONTEST_ENGINE_DIR` 可以显式指向 `benchmarks/baselines/rocksdb` 以构建 RocksDB。
当前 F2 仍使用原生完成语义，不能把其吞吐当作逐条持久化成绩。

## 运行与终端进度

```sh
# 本机 ARM 诊断；每秒显示阶段、完成数、区间吞吐、RSS 与文件大小。
python3 taycan_participant_sdk/benchmarks/run_performance.py \
  --native-development --build-dir build/f2-native-baseline-arm64-20261030 \
  --profile dev --num-keys 10000 --mem-limit-bytes 4294967296 \
  --key-size 32 --value-size 256 --workloads load,read,contest_mix \
  --threads 1,4 --repeats 1 --results-dir analysis/f2-new-run

# Linux 正确性：保留场景默认规模，4 GiB，32 B Key / 256 B Value。
python3 taycan_participant_sdk/tests/run_correctness.py \
  --build-dir /var/lib/taycan-workspace/f2-native-baseline-20261030 \
  --tests basic,recovery,concurrent --results-dir /var/lib/taycan-results/f2-new-run
```

性能脚本的 `--progress-interval-seconds N` 控制输出间隔，`0` 关闭进度采集。
最终 JSON/README 包含装载、测量、Open/close、RSS 峰值、文件大小、状态和源码/二进制哈希。
FAIL / SKIP / 未执行状态必须保留。完整说明见 [测量脚本](benchmarks/README.md)。

## 接口与约束

- `Open(data_dir, mem_limit_bytes, &engine)` 创建或恢复数据库；失败时输出空指针。
- `Put` 覆盖，`Get` 返回最新值或 NotFound，`Delete` 不存在的 Key 也应返回 OK。
- 同一实例可多线程调用，要求单 Key 线性一致；Key/Value 是字节串，Key 非空。
- 命题要求成功的 Put/Delete 在返回前完成持久化；F2 当前适配尚未满足这一点。
- `CreateCheckpoint` 是可选的独立副本接口；析构必须释放引擎资源。
- 目标数据为同一 2 亿 Key、16–32 B Key、256 B Value，4/16 GiB 两档；VmRSS 按严格 110% 检查。
- 所有数据、索引、临时文件与元数据都必须在指定的数据目录；不能靠测试规模特化或绕过监控。

原包仍保留在工程根目录 `taycan_participant_sdk.tar.gz`。当前工作区不是最终离线提交包。
