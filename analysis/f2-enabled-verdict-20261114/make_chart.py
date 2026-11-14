from pathlib import Path
import json,statistics,html
BASE=Path(__file__).resolve().parent
r=json.loads((BASE/'performance/results.json').read_text());excluded=set(json.loads((BASE/'contention.json').read_text())['affected_cells'])
# Only second-round clean matched pairs are shown, so one missing first-round
# observation cannot be mistaken for a two-round median.
labels={'load':'百万 Key 装载','read':'随机读','update':'随机覆盖写','contest_mix':'60% 读 / 30% 写 / 10% 删'}
items=[]
for workload in r['plan']['workloads']:
 pair={engine:next((c for c in r['runs'] if c['engine']==engine and c['workload']==workload and c['repeat']==2 and c['status']=='PASS' and c['cell'] not in excluded),None) for engine in ('f2','rocksdb')}
 if not all(pair.values()):continue
 vals={e:pair[e]['measurement']['ops_per_second'] for e in pair}
 peak=max(vals.values());items.append('<div class="item"><div class="label">'+labels[workload]+'</div>'+''.join('<div class="row"><span>'+('F2' if e=='f2' else 'RocksDB')+'</span><div class="track"><div class="bar '+e+'" style="width:'+str(max(1,100*v/peak))+'%"></div></div><b>'+format(v,',.0f')+'</b></div>' for e,v in vals.items())+'</div>')
page='''<!doctype html><meta charset="utf-8"><style>
:root{font-family:var(--font-sans,system-ui),"PingFang SC",sans-serif;color:var(--foreground)}html,body{margin:0;background:transparent}.legend{font-size:12px;color:var(--muted-foreground);margin:0 0 14px}.item{margin:18px 0}.label{font-weight:600;font-size:14px;margin-bottom:8px}.row{display:flex;gap:10px;align-items:center;margin:7px 0;font-size:12px}.row>span{width:56px}.track{flex:1;min-width:80px;height:18px;background:var(--muted);border-radius:3px}.bar{height:100%;border-radius:3px}.f2{background:var(--chart-1)}.rocksdb{background:var(--chart-2)}.row>b{width:76px;text-align:right;font-variant-numeric:tabular-nums}.note{font-size:11px;color:var(--muted-foreground);line-height:1.6;margin-top:18px}
</style><div class="legend">第二轮串行实测 · ops/s · 每组独立刻度</div>'''+''.join(items)+'''<div class="note">32 线程 · 4 GiB · 100 万 Key · 32/256 B · 同步成功后返回<br>macOS ARM64；本机性能参照，不是正式分数。F2 开启迁移与回收，group commit 已移除。<br>每格新库；预热 2 秒、限时阶段 10 秒。装载计完整百万条耗时。<br>两者缓存策略不同；不代表冷盘 I/O。仅显示完成的第二轮，第一轮受干扰格子已排除。</div>'''
(BASE/'comparison.html').write_text(page)
print(BASE/'comparison.html')
