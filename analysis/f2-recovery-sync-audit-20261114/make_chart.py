from pathlib import Path
import json
BASE=Path(__file__).resolve().parent
r=json.loads((BASE/'sync-performance/results.json').read_text());cells={c['cell']:c for c in r['runs']}
rows=[]
for title,f2,rocks in [('1线程：两边逐次同步','f2-t1-default-audit','rocksdb-t1-default-audit'),('32线程：各自默认提交方式','f2-t32-default-audit','rocksdb-t32-default-audit'),('32线程：RocksDB限制合批','f2-t32-default-audit','rocksdb-t32-group1-audit')]:
 a=cells[f2];b=cells[rocks];peak=max(a['measurement']['ops_per_second'],b['measurement']['ops_per_second']);bars=[]
 for name,c in [('F2',a),('RocksDB',b)]:
  v=c['measurement']['ops_per_second'];count=c['syscalls']['hot_calls' if name=='F2' else 'wal_calls']
  bars.append('<div class="row"><span>'+name+'</span><div class="track"><i class="'+('f2' if name=='F2' else 'rocks')+'" style="width:'+str(100*v/peak)+'%"></i></div><b>'+format(v,',.0f')+'</b></div><div class="sub">'+name+'：'+format(count,',')+' 次真实数据/WAL同步</div>')
 rows.append('<section><strong>'+title+'</strong>'+''.join(bars)+'</section>')
html='''<!doctype html><meta charset="utf-8"><style>
:root{font-family:var(--font-sans,system-ui);color:var(--foreground)}html,body{background:transparent;margin:0}section{margin:16px 0 22px}strong{font-size:14px}.row{display:flex;align-items:center;gap:10px;font-size:12px;margin:8px 0 2px}.row span{width:60px}.row b{width:75px;text-align:right;font-variant-numeric:tabular-nums}.track{height:17px;background:var(--muted);flex:1;border-radius:3px;overflow:hidden}.track i{display:block;height:17px}.f2{background:var(--chart-1)}.rocks{background:var(--chart-2)}.sub,.note{font-size:11px;color:var(--muted-foreground);line-height:1.6}.sub{margin-left:70px}.note{margin-top:15px}
</style><div class="note">完整装载吞吐 · ops/s · 每组独立刻度</div>'''+''.join(rows)+'''<div class="note">百万唯一 Key · 32/256 B · 4 GiB · 同一驱动与真实 fsync · 单轮诊断<br>两者均等待同步成功才返回；默认 RocksDB 约 16 个调用共用一次 WAL 同步。<br>无插桩 32 线程独立观察：F2 12,734，RocksDB 141,338 ops/s。插桩会影响分组调度。<br>macOS ARM64，本机诊断；不代表正式 NVMe 分数或物理断电保证。</div>'''
(BASE/'comparison.html').write_text(html)
