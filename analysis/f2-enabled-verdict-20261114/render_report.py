"""Produce an explicit acceptance verdict, keeping all FAIL/NOT_RUN states."""
from pathlib import Path
import collections,json,statistics
ROOT=Path(__file__).resolve().parents[2];BASE=Path(__file__).resolve().parent
rows=[]
for path in [BASE/'public-enabled/results.json',BASE/'public-remaining/results.json']+sorted(BASE.glob('public-*/results.json')):
 if path.exists() and str(path) not in locals().get('seen_paths',set()):
  rows+=json.loads(path.read_text());seen_paths=locals().get('seen_paths',set())|{str(path)}
byname={r['test']:r for r in rows}
case_order=('basic','sequential','random','overwrite','delete','mixed','recovery','checkpoint','concurrent','demand_load','memory_limit','compaction','crash_recovery','compaction_crash')
for name in ('basic','sequential','random','overwrite','delete','mixed','recovery','checkpoint','concurrent','demand_load','memory_limit','compaction','crash_recovery','compaction_crash'):
 if name not in byname:byname[name]=dict(test=name,status='NOT_RUN',reason='Not executed in this round')
model=BASE/'pending-page-loss/results.json';model_result=json.loads(model.read_text()) if model.exists() else None
failed_model=any(x['status']=='FAIL' for x in model_result['cases']) if model_result else None
byname={name:byname[name] for name in case_order}
report=dict(engine_source_unchanged=True,revision='144a5ec34535b2e39b2e80f6e53efbf4a35afe86',overall='NOT_PASS' if failed_model or any(r['status']=='FAIL' for r in rows) else 'INCOMPLETE',scope='macOS ARM64 native development; not official x86_64 Linux scoring',correctness=list(byname.values()),page_loss_model=model_result,performance=[])
perf=BASE/'performance/results.json'
if perf.exists():
 data=json.loads(perf.read_text());report['performance_status']=data['status']
 for workload in data['plan']['workloads']:
  row=dict(workload=workload)
  for engine in ['f2','rocksdb']:
   cells=[r for r in data['runs'] if r['engine']==engine and r['workload']==workload]
   excluded=set(json.loads((BASE/'contention.json').read_text())['affected_cells']) if (BASE/'contention.json').exists() else set()
   passed=[r for r in cells if r['status']=='PASS' and r['cell'] not in excluded];numbers=[r['measurement']['ops_per_second'] for r in passed]
   row[engine]=dict(passed=len(passed),total_planned=data['plan']['repeats'],ops_per_second=statistics.median(numbers) if len(numbers)==data['plan']['repeats'] else None,observations=numbers,statuses=[r['status'] for r in cells],rss_peak_bytes=max((r.get('process',{}).get('rss_peak_bytes',0) for r in passed),default=None))
  report['performance'].append(row)
(BASE/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
lines=['# 启用必要配置后的 F2 验收与性能结论','',f"当前总体结论：**{report['overall']}**。被测主干 `144a5ec`，引擎源码未改动；开启 data-log 同步、durable compaction、热转冷、冷日志与冷索引回收。主诊断预算 4 GiB。",'',
'## 正确性','', '| 场景 | 状态 | Key 数 | RSS峰值 MiB | 耗时 s | 原因/边界 |','|---|---|---:|---:|---:|---|']
for r in byname.values():
 reason=r.get('reason','原场景断言通过' if r['status']=='PASS' else '未执行或可选接口未实现')
 if 'log' in r:
  extra=Path(r['log']).parent/'external-guard.json'
  if extra.exists():reason=json.loads(extra.read_text())['reason']
 if r['test']=='compaction_crash' and (BASE/'public-enabled/compaction_crash-hikb1mnn/external-guard.json').exists():reason='准备阶段超过本地磁盘保护后中止；没有注入崩溃，不算通过，也不能当数据错误'
 lines.append(f"| {r['test']} | {r['status']} | {r.get('parameters',{}).get('num_keys','—')} | {r.get('rss_peak_bytes',0)/(1<<20):.2f} | {r.get('elapsed_seconds',0):.2f} | {reason} |")
lines+=['','使用原生公开 C++ 断言与各场景默认 Key 数/Key Value 大小；仅按用户选择把内存预算设为 4 GiB，严格监控预算×110%。这是本地验收，不是正式 Linux VmRSS。单库 2 GiB、空闲20GiB保护；开发超时与保护中止不冒充引擎断言失败。','',
'## 明确的能力错误','']
if failed_model:lines+=['**未确认尾部部分页丢失时恢复失败。**准备67次已确认操作，在下一次100KiB写入的同步前强杀，再只改变未确认尾部：保留后页/提交标记、丢失前部未同步页。67次已确认操作的全部字节保持不变；重开却报 `malformed data record inside committed prefix`。原确认状态本应仍可恢复，未确认操作允许丢失。该100KiB Value属于SDK公开Basic覆盖的接口范围，超出正式性能负载的256B；本模型没有证明固定256B负载也会以同样方式失败。','',
'四种镜像情形中：全部未确认尾部丢失、全部保留、尾页丢失通过；前页丢失但后页/标记保留失败。此为确定性磁盘镜像模型，不是真实断电或原生SIGKILL脚本；不能说原生SIGKILL也失败。[原始记录](pending-page-loss/results.json)。','']
lines+=['开启配置后的实际 `compaction` 通过100,000 Key×10轮并发更新、空间比例、最终值及重开校验；最终目录372,975,729B，存活逻辑274,130,592B。GC trace 记录19个完整周期及约14.28GB累计旧段删除，磁盘索引也实际参与；不能继续称本轮没有开启回收。','',
'运行期预算尚需大场景验收，不能把小规模RSS未超限等同2亿Key达标。可选SDK Checkpoint 未实现与内部恢复checkpoint是两件事。','',
'## 同条件性能','',
'固定32线程、1,000,000唯一Key、32/256B、4GiB、seed20261021，F2 data-log fsync-after-flush，RocksDB8.9.1 sync=true/WAL开启/无压缩（HAVE_FULLFSYNC=OFF）；相同共享驱动源码，每格新库，2秒预热、10秒测量、2轮反转引擎顺序。装载测完整百万条，失败不重试替换。','',
'RocksDB为结构不合命题的性能参照，不能当合规候选；F2开启回收、移除group commit。F2 F_NOCACHE，RocksDB保留默认buffered SST与block cache，因此不是冷盘算法对照。','',
'| 负载 | F2 ops/s（完成轮） | RocksDB ops/s（完成轮） |','|---|---:|---:|']
for r in report['performance']:
 vals=[]
 for name in ['f2','rocksdb']:
  c=r[name];number=c['ops_per_second'];txt=f'{number:,.0f}' if number is not None else '未形成两轮有效成绩'
  if number is None and c['observations']:txt+='；有效观察'+','.join(f'{v:,.0f}' for v in c['observations'])
  vals.append(txt+f"（{c['passed']}/{c['total_planned']}）")
 lines.append('| '+r['workload']+' | '+' | '.join(vals)+' |')
lines+=['','第一轮受调度重叠影响的四个格子已排除，见[监控与调度记录](README-diagnostic-notes.md)。为便于直接比较，报告另保留[第二轮完整对照](clean-second-round.json)，不把单轮观察伪装成两轮中位数。','','性能完成状态：'+report.get('performance_status','NOT_RUN')+'。这些数据只描述该配置下的本机吞吐，不说明F2已通过命题全部验收。[原始性能](performance/results.json) · [固定计划](comparison-plan.json)。','',
'完整公开原始输出与监控见 [第一组](public-enabled/results.json)、[覆盖/删除/混合/Checkpoint](public-small/results.json)、[随机](public-random/results.json)、[按需加载](public-demand/results.json)、[内存场景预检](public-memory/results.json)。失败数据库原地保留；成功库清理但保留日志/哈希/文件清单。不自动修复再覆盖本轮失败。']
(BASE/'README.md').write_text('\n'.join(lines)+'\n')
print(report['overall'],len(byname),'public statuses',report.get('performance_status'))
