"""Summarize only this round; never relabel prior main-event as caller."""
from pathlib import Path
import collections
import html
import json
import statistics

B = Path(__file__).resolve().parent
r = json.loads((B / 'results.json').read_text())
assert r['status'] in ['PASS', 'FAIL']
if r['status'] == 'PASS':
    assert len(r['validation']) == 12 and all(x['status'] == 'PASS' for x in r['validation'])
    assert len(r['runs']) == 33 and all(x['status'] == 'PASS' for x in r['runs'])
    for x in r['runs']:
        prefix = x['F2_DATA_PREFIX'][0]
        assert prefix['data_syncs'] == prefix['commits'] > 0
        assert x['monitor']['rss_peak_bytes'] <= (4 << 30) * 11 // 10
        assert not (B / 'runs' / x['phase'] / x['case'] / 'db').exists()
names = {'caller': 'caller 主干', 'helio10': 'Helio / 10µs', 'helio50': 'Helio / 50µs'}
workloads = {'contest_mix': '混合读写', 'update': '纯更新', 'read': '热读'}
med = statistics.median
summary = []
totals = {}
for row in r['runs']:
    if row['status'] != 'PASS':
        continue
    progress = [json.loads(line) for line in (B / 'runs' / row['phase'] / row['case'] / 'output.log').read_text().splitlines()
                if line.startswith('{"type":"progress"')]
    stages = {x['stage']: x['completed'] for x in progress if x['final']}
    assert stages['measure'] == row['measurement']['operations']
    if row['workload'] != 'load':
        assert stages['prepare'] == row['num_keys']
    totals[row['case']] = sum(stages.values())

def latency(rows, op):
    values = [x['measurement'][op + '_latency']['p99_us'] for x in rows]
    return med(v for v in values if v is not None) if any(v is not None for v in values) else None

for phase in ['hot', 'million', 'disk-region-read']:
    for workload in (workloads if phase == 'hot' else ['load' if phase == 'million' else 'read']):
        base = {x['repeat']: x for x in r['runs'] if x['phase'] == phase and x['workload'] == workload
                and x['engine'] == 'caller' and x['status'] == 'PASS'}
        for mode in names:
            rows = [x for x in r['runs'] if x['phase'] == phase and x['workload'] == workload
                    and x['engine'] == mode and x['status'] == 'PASS']
            if not rows:
                continue
            rates = [x['measurement']['ops_per_second'] for x in rows]
            result = dict(phase=phase, workload=workload, engine=mode, valid_repeats=len(rows),
                          median_ops_per_second=med(rates), minimum_ops_per_second=min(rates), maximum_ops_per_second=max(rates),
                          repeats_ops_per_second=rates, get_p99_us=latency(rows, 'get'), put_p99_us=latency(rows, 'put'),
                          overall_p99_us=med(x['measurement']['latency']['p99_us'] for x in rows),
                          rss_peak_mib=max(x['monitor']['rss_peak_bytes'] for x in rows) / 2**20,
                          median_setup_seconds=med(x['measurement']['setup_seconds'] for x in rows),
                          median_measured_seconds=med(x['measurement']['measured_seconds'] for x in rows),
                          median_disk_growth_mib=med(x['disk_growth_bytes'] for x in rows) / 2**20,
                          median_disk_growth_bytes_per_measured_op=med(x['disk_growth_bytes'] / x['measurement']['operations'] for x in rows),
                          median_writes_per_group=med(x['whole_run_writes'] / x['F2_DATA_PREFIX'][0]['commits'] for x in rows),
                          median_user_cpu_seconds=med(x['cpu']['user_seconds'] for x in rows),
                          median_system_cpu_seconds=med(x['cpu']['system_seconds'] for x in rows),
                          median_whole_run_cpu_us_per_operation=med(
                              (x['cpu']['user_seconds'] + x['cpu']['system_seconds']) * 1e6 / totals[x['case']] for x in rows),
                          median_average_cpu_cores=med(
                              (x['cpu']['user_seconds'] + x['cpu']['system_seconds']) / x['monitor']['elapsed_seconds'] for x in rows),
                          paired_changes_percent=[100 * (x['measurement']['ops_per_second'] / base[x['repeat']]['measurement']['ops_per_second'] - 1)
                                                  for x in rows if x['repeat'] in base])
            summary.append(result)

def find(phase, workload, mode):
    return next(x for x in summary if (x['phase'], x['workload'], x['engine']) == (phase, workload, mode))

storage = [x['storage'] for x in r['runs'] if x['status'] == 'PASS']
result = dict(status=r['status'], source=r['engines']['caller']['revision'], helio_source=r['engines']['helio50']['revision'],
              validation_statuses=dict(collections.Counter(x['status'] for x in r['validation'])),
              performance_statuses=dict(collections.Counter(x['status'] for x in r['runs'])), summary=summary,
              free_min_gib=min(x['free_min_bytes'] for x in storage) / 2**30 if storage else None,
              database_peak_mib=max(x['database_peak_logical_bytes'] for x in storage) / 2**20 if storage else None)
(B / 'summary.json').write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n')

lines = ['# 恢复后的 caller 主干与 Helio 实验分支对照', '',
         '本报告只使用 2026-11-04 本轮新测数据。caller 来自恢复后的 main `cdbc644`，'
         'Helio 来自 `experiment/helio-integration-audit` 的 `90eedee`。旧的 Boost.Fiber/event '
         '`main-event` 没有参与本轮，也不再称为当前主干。', '',
         '## 构建与身份', '',
         'caller 新建 Release 构建；Helio 使用上轮审计后冻结的 Release 二进制，SHA256 与其提交内原始结果一致，'
         '未重配、未重新编译或修改。双方编译器为 AppleClang 17.0.0，Release 公共优化配置为 -O3 -DNDEBUG，'
         '共享负载源码 SHA256 一致；各自必须的运行时依赖和编译定义不同，实际编译命令保留。'
         'caller使用-std=c++17，Helio原工程使用-std=gnu++17，优化配置相同，不声称全部编译参数逐字一致。'
         '不是此前 caller RelWithDebInfo 与 Helio Release 的对照。', '',
         '共同构建条件：data-log、fsync、POSIX 后端选择、512B 提交块、1MiB 日志页、4KiB overflow 页、'
         'GC 和额外 pipeline 关闭。完整共同参数、源码/生成源码、二进制、动态库和 Boost 静态库哈希见 results.json，'
         '运行前后核对。POSIX 后端选择相同不代表实际执行方式相同：caller 使用 POSIX AIO，'
         'Helio 的已启用 hook 使用原版单 worker 文件线程池，这是待比较实现的一部分。', '',
         '## 测试条件与范围', '',
         'macOS ARM64 原生，同步 KVEngine API，32 个外部调用线程，总预算 4GiB，Key 32B / Value 256B，'
         '相同 seed=20261021、uniform。双方成功返回前完成持久化同步；没有关闭 fsync、提前 ACK 或更改负载断言。', '',
         'caller 使用原有自适应 group collect（上限50µs）；Helio 显式比较10µs、50µs窗口，保留两者，'
         '没有按结果挑一个窗口替代另一负载的配置。它们的收集算法不同，窗口数值相同也不等于相同批次。', '',
         '热负载4096 Key，预热2秒、测量8秒、逐操作延迟，每模式各3轮，正序/反序/轮转交错。'
         '混合为60%读/30%写/10%删除；纯更新均为Put。33个性能单元串行运行。'
         '每模式另完整装载1,000,000唯一Key；disk-region-read先完整装载2,000,000唯一Key，'
         '再预热2秒、随机读8秒，均校验Value内容，后两项各1轮。', '',
         '2M记录超过 F2 日志内存缓冲，但没有清空OS页缓存；disk-region-read不是物理盘冷读。'
         '4096 Key是调度与提交诊断，不是正式2亿Key/4GiB索引落盘验收。', '',
         '## 正确性门槛', '',
         f"本轮状态 `{r['status']}`。验证条目状态：{result['validation_statuses']}；性能状态：{result['performance_statuses']}。", '',
         '三个模式分别运行原SDK basic/concurrent/recovery（2000 Key、4GiB、31读+1写的版本边界用例），'
         '并各执行原SDK SIGKILL协议5次，共15次；所有验证完成后才启动性能矩阵。'
         '这些是开发规模验证，不把12条wrapper记录表述为12个独立故障，亦不宣称通过完整默认规模。', '',
         '## 三轮热负载结果', '',
         '|负载|实现|中位ops/s|最小～最大|各轮相对caller|Get p99 µs|Put p99 µs|RSS峰值MiB|',
         '|---|---|---:|---:|---|---:|---:|---:|']
for x in summary:
    if x['phase'] != 'hot':
        continue
    number = lambda value: '—' if value is None else f'{value:,.1f}'
    lines.append(f"|{workloads[x['workload']]}|{names[x['engine']]}|{x['median_ops_per_second']:,.0f}|"
                 f"{x['minimum_ops_per_second']:,.0f}～{x['maximum_ops_per_second']:,.0f}|"
                 f"{', '.join(f'{v:+.1f}%' for v in x['paired_changes_percent'])}|"
                 f"{number(x['get_p99_us'])}|{number(x['put_p99_us'])}|{x['rss_peak_mib']:.1f}|")
lines += ['', '## 差异解释', '']
for w in workloads:
    try:
        a, b, c = [find('hot', w, m) for m in names]
    except StopIteration:
        continue
    lines += [f"{workloads[w]}：caller中位{a['median_ops_per_second']:,.0f} ops/s；"
              f"Helio10为{b['median_ops_per_second']:,.0f}（相对caller中位数{100*(b['median_ops_per_second']/a['median_ops_per_second']-1):+.1f}%）；"
              f"Helio50为{c['median_ops_per_second']:,.0f}（{100*(c['median_ops_per_second']/a['median_ops_per_second']-1):+.1f}%）。", '']
lines += ['caller的Get由多个SDK调用线程直接执行；Helio把数据访问集中到一个owner。'
          '因此热读包含执行并行度差异，不能把差距全部解释为fiber切换成本。'
          '写入则涉及caller-led与owner提交、外部完成通知、文件交接、合批和读写协调的组合。'
          '本轮比较完整实现，不量化每项因果占比。', '',
          '|负载|实现|整轮写/组|测量期磁盘增长MiB|增长B/测量操作|整轮user CPU秒|整轮system CPU秒|整轮CPU µs/操作|',
          '|---|---|---:|---:|---:|---:|---:|---:|']
for x in summary:
    if x['phase'] == 'hot':
        lines.append(f"|{workloads[x['workload']]}|{names[x['engine']]}|{x['median_writes_per_group']:.2f}|"
                     f"{x['median_disk_growth_mib']:.1f}|{x['median_disk_growth_bytes_per_measured_op']:.1f}|"
                     f"{x['median_user_cpu_seconds']:.2f}|{x['median_system_cpu_seconds']:.2f}|"
                     f"{x['median_whole_run_cpu_us_per_operation']:.2f}|")
lines += ['', '写/组和CPU覆盖装载/预热/测量/关闭，吞吐和p99仅覆盖测量，不能直接换算精确阶段占比。'
          '磁盘增长是逻辑文件字节，不是设备写入量或NAND写放大；相同持续时间不同吞吐产生的总增长不能直接视为效率差异。', '',
          '整轮CPU µs/操作以进程所有子线程user+system CPU时间，除以日志中prepare、warmup、measure各阶段最终操作数之和，'
          '包含初始化/维护/关闭成本；不是测量期单独采集的CPU成本，也不是owner单核时间。'
          '本轮Helio吞吐较低但整轮单位操作CPU消耗也较低，因此不应将吞吐差异解释为所有性能维度均退步。', '',
          '## 完整装载与日志磁盘区读取', '',
          '|场景|实现|唯一Key数|吞吐ops/s|装载setup秒|测量秒|RSS峰值MiB|',
          '|---|---|---:|---:|---:|---:|---:|']
for x in summary:
    if x['phase'] != 'hot':
        lines.append(f"|{x['phase']}|{names[x['engine']]}|{1000000 if x['phase']=='million' else 2000000:,}|"
                     f"{x['median_ops_per_second']:,.0f}|{x['median_setup_seconds']:.3f}|"
                     f"{x['median_measured_seconds']:.3f}|{x['rss_peak_mib']:.1f}|")
lines += ['', 'million为完整同步装载，其耗时在测量秒列，setup为0；disk-region-read的setup是完整2M装载时间。'
          '后两项只有1轮，不能声称稳定的长期稳态优势。', '',
          '## 状态与限制', '',
          '不修改引擎，不合并实验分支，不将旧main-event成绩替代caller。'
          '共享交互主机的三轮短测不足以证明统计显著或跨机器普适结论；没有使用虚拟机，'
          '也不是正式Linux/NVMe、GC/compaction组合或2亿Key/4GiB成绩。', '',
          f"运行中最低可用空间{result['free_min_gib']:.1f}GiB，观察到单库峰值{result['database_peak_mib']:.1f}MiB；"
          '守护20GiB余量和2GiB单库限制，成功数据库删除，日志、RSS、结果与哈希保留。'
          '任何FAIL/未执行状态保留，不自动重跑。', '',
          '[全部原始结果与身份](results.json) · [汇总](summary.json) · [执行脚本](run.py) · '
          '[caller构建命令](caller-configure-command.json)', '']
(B / 'README.md').write_text('\n'.join(lines))

if result['status'] == 'PASS':
    colors = {'caller': 'var(--chart-2)', 'helio10': 'var(--chart-3)', 'helio50': 'var(--chart-1)'}
    parts = ['<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><style>',
             ':root{font-family:var(--font-sans)}body{margin:0;color:var(--foreground)}'
             '.note{font-size:12px;color:var(--muted-foreground);line-height:1.6;margin:8px 0 16px}'
             '.group{margin:18px 0 24px}.label{font-size:14px;font-weight:650;margin-bottom:10px}'
             '.row{display:grid;grid-template-columns:100px minmax(40px,1fr) 70px;gap:8px;align-items:center;margin:8px 0;font-size:12px}'
             '.track{height:16px;background:var(--secondary);border-radius:3px;overflow:hidden}'
             '.bar{height:16px;border-radius:3px}.number{text-align:right;font-variant-numeric:tabular-nums}'
             '@media(max-width:420px){.row{grid-template-columns:93px minmax(30px,1fr) 64px;gap:6px;font-size:11px}}'
             '</style></head><body>',
             '<p class="note">恢复后的 caller 主干 vs Helio 实验分支<br>32 线程 · 4GiB · 32/256B · 同步后 ACK · 双方 Release<br>3轮中位数，单位 ops/s；每组独立刻度</p>']
    chart_groups = [('hot', w, workloads[w]) for w in workloads]
    chart_groups += [('million', 'load', '百万 Key 装载 · 单轮'),
                     ('disk-region-read', 'read', '两百万 Key 日志磁盘区读 · 单轮')]
    for phase, w, title in chart_groups:
        maximum = max(find(phase, w, m)['median_ops_per_second'] for m in names)
        parts.append(f'<section class="group"><div class="label">{title}</div>')
        for m in names:
            x = find(phase, w, m)
            parts.append(f'<div class="row"><span>{html.escape(names[m])}</span><div class="track">'
                         f'<div class="bar" style="width:{x["median_ops_per_second"]/maximum*100:.3f}%;background:{colors[m]}"></div></div>'
                         f'<span class="number">{x["median_ops_per_second"]:,.0f}</span></div>')
        parts.append('</section>')
    parts.append('<p class="note">macOS ARM64 · 热负载4096 Key、三轮交错；百万装载和2M读取各单轮。2秒预热＋8秒测量，装载计完整耗时。保留OS缓存，GC关闭，非正式规模。caller cdbc644，Helio 90eedee；旧 main-event 不参与。</p></body></html>')
    (B / 'comparison.html').write_text(''.join(parts))
print(json.dumps({k: v for k, v in result.items() if k != 'summary'}, ensure_ascii=False))
