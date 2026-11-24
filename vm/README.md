# Linux 验证环境

当前日常开发以 F2 原生适配器为主；macOS 只做诊断，Linux x86_64 VM 用于兼容性和脚本验证。
此处 QEMU x86_64 环境保留供目标兼容性检查；不再用于日常性能排名，已停止的长测不自动恢复。

这个目录定义了用于 Taycan 开发和公开测试的 Lima VM。

- `taycan-ubuntu24`：日常主环境，Ubuntu 24.04 LTS、x86_64、GCC 13，稳定版工具链。
- `taycan-centos7`：兼容性环境，CentOS 7、x86_64，匹配参赛包声明的目标环境。

两台 VM 都配置 8 vCPU、24 GiB 内存和 96 GiB 虚拟盘。正式题面要求的兼容目标是
`CentOS 7 / Linux x86_64 / GCC 8.3`，所以 CentOS 7 不是随意选择；日常开发使用
Ubuntu 24.04，避免把 EOL 系统当成主要开发环境。

宿主机是 Apple Silicon，因此两台 x86_64 VM 都使用 QEMU system emulation。它们适合
编译、正确性、RSS、崩溃恢复和索引实验；绝对吞吐和 NVMe P99 不能等同于测评机。

## 创建和启动

在 `taycan-challenge` 目录执行：

主开发环境：

```sh
limactl start --name=taycan-ubuntu24 vm/taycan-ubuntu24-x86_64.yaml
```

兼容性环境：

```sh
limactl start --name=taycan-centos7 vm/taycan-centos7.yaml
```

首次启动会下载对应的 Linux 云镜像并在虚拟机内安装工具链。Ubuntu 镜像用于主开发，
CentOS 7 镜像只用于兼容性检查。查看状态：

```sh
limactl list
limactl shell --workdir=/workspace/taycan-challenge taycan-ubuntu24 sudo /usr/local/bin/taycan-env
```

当前主环境实测：`Linux x86_64`、8 CPU、约 24 GiB 内存、CMake 3.28、GCC 13、Python 3.12。

如果虚拟机启动或初始化失败，查看：

```sh
limactl shell --workdir=/workspace/taycan-challenge taycan-ubuntu24 sudo tail -200 /var/log/cloud-init-output.log
limactl shell --workdir=/workspace/taycan-challenge taycan-ubuntu24 sudo journalctl -u lima-guestagent --no-pager -n 100
```

## 构建和测试

代码目录通过挂载出现在 `/workspace/taycan-challenge`。测试数据库和结果目录必须
放在 VM 本地盘，避免把宿主机挂载层的延迟混入 I/O 结论：

```sh
limactl shell --workdir=/workspace/taycan-challenge taycan-ubuntu24 bash -c '
  cd /workspace/taycan-challenge/code/taycan_participant_sdk
  cmake -S . -B /var/lib/taycan-workspace/build -DCMAKE_BUILD_TYPE=Release
  cmake --build /var/lib/taycan-workspace/build --parallel 8
  python3 /workspace/taycan-challenge/script/taycan/tests/run_tests.py \
    --build-dir /var/lib/taycan-workspace/build \
    --tests basic,recovery,concurrent \
    --results-dir /var/lib/taycan-results/smoke
'
```

构建目录、测试数据库和结果目录放在 VM 的 `/var/lib` 本地盘；代码通过挂载目录访问，
避免把宿主机挂载层的 I/O 延迟当成数据库引擎结果。

公开测试默认 `256 MiB` 索引预算；正式题面还包括 `4 GiB/16 GiB` 两档预算和约
`2 亿 Key` 数据规模。VM 已经给出足够内存，但磁盘和 QEMU 性能不是正式评分硬件。

## 正确性与压测入口

`script/taycan/tests/run_correctness.py` 保留官方参数与验收逻辑；独立的压测程序通过
`-DTAYCAN_BUILD_PERF_BENCHMARKS=ON` 开启。压测命令、压力档位和竞品来源见
[当前性能结论](../docs/performance-status.md)。

性能数据应在 VM 空闲时收集；脚本会检查其他 Taycan 测试进程，默认拒绝与它们同时压测。
小规模工具验证可以显式传 `--allow-shared-host`，该结果会标注资源竞争。
将 `--results-dir` 指向 `/var/lib/taycan-results/`，数据库默认也在该 VM 本地盘目录创建，
不要把性能测试数据库写到 `/workspace` 宿主挂载盘。

## 停止和删除

```sh
limactl stop taycan-ubuntu24
limactl delete taycan-ubuntu24

# 不需要兼容性检查时可删除旧系统 VM
limactl stop taycan-centos7
limactl delete taycan-centos7
```

删除 VM 会删除虚拟盘中的测试数据；代码仍在宿主机挂载目录中。
