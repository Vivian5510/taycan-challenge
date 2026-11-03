# Ubuntu 24.04 x86_64 VM 验证记录

日期：2026-10-27。宿主机：Apple Silicon macOS。虚拟化：Lima 2.2.1 + QEMU 11.1.2，
使用 x86_64 system emulation。

## 配置

- VM：`taycan-ubuntu24`
- Ubuntu：24.04 LTS cloud image，固定 release 镜像 SHA256
- Guest：Linux x86_64，8 vCPU，24 GiB RAM，96 GiB 虚拟盘
- CMake：3.28.3
- GCC/G++：13.3.0
- Python：3.12.3
- `/proc/self/status`：可用，RSS 监控入口可运行

## 已验证

- 官方 SDK CMake/Ninja 配置通过。
- 官方 SDK 全部测试二进制编译通过。
- `run_tests.py --list` 列出 14 个场景。
- 未修改的内存模板 smoke test：`basic` PASS；`recovery` 和 `concurrent`
  按预期因模板不支持持久化恢复而 FAIL。

## 监控器说明

`tests/process_runner_test.py` 在该 VM 运行了 8 项自测，其中 7 项通过，
`test_memory_failure_not_accepted_as_injection` 未触发预期异常。该自测依赖
一个很小的内存超限注入窗口，在 QEMU guest 的 RSS 采样条件下没有按预期命中；
官方 `run_tests.py` 的真实场景仍会使用 Linux `/proc` 监控。该结果不能解读为
引擎绕过 RSS，也不修改官方监控代码。

## 限制

这是 x86_64 架构和 Linux `/proc` 兼容环境，但存储设备是宿主机文件上的 QEMU 虚拟盘，
不是评测机 NVMe。吞吐、fsync 延迟、P99 和 Compaction 写放大只能用于相对比较；最终
性能必须在同类 Linux/NVMe 硬件上复测。
