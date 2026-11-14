"""Plot only complete two-round pairs, never extrapolate missing cells."""
from pathlib import Path
import json

BASE = Path(__file__).resolve().parent
data = json.loads((BASE / 'confirmation-verification.json').read_text())
labels = dict(load='完整百万 Key 装载', update='随机覆盖写', contest_mix='60% 读 / 30% 写 / 10% 删')
sections = []
for pair in data['workloads']:
    a, b = pair['aio-nocache'], pair['aio-batch2']
    if a['median'] is None or b['median'] is None:
        continue
    peak = max(a['median'], b['median'])
    bars = []
    for name, cell, color in [('每轮最多1个完成回调', a, 'a'), ('每轮最多2个完成回调', b, 'b')]:
        value = cell['median']
        bars.append('<div class="row"><span>' + name + '</span><div class="track"><i class="' + color + '" style="width:' + str(value / peak * 100) + '%"></i></div><b>' + format(value, ',.0f') + '</b></div>')
    sections.append('<section><strong>' + labels[pair['workload']] + '</strong>' + ''.join(bars) + '<div class="delta">候选/基线 ' + format(pair['ratio'], '.3f') + '</div></section>')
html = '''<!doctype html><meta charset="utf-8"><style>
:root{font-family:var(--font-sans,system-ui);color:var(--foreground)}html,body{margin:0;background:transparent}section{margin:18px 0 22px}strong{font-size:14px}.row{display:flex;align-items:center;gap:8px;font-size:12px;margin:9px 0}.row span{width:145px}.row b{width:70px;text-align:right;font-variant-numeric:tabular-nums}.track{flex:1;height:16px;background:var(--muted);border-radius:3px;overflow:hidden}.track i{display:block;height:16px}.a{background:var(--chart-1)}.b{background:var(--chart-2)}.note,.delta{font-size:11px;color:var(--muted-foreground);line-height:1.6}.delta{text-align:right}
@media(max-width:420px){.row span{width:100px}.row b{width:56px}}
</style><div class="note">两轮吞吐中位数 · ops/s · 每组独立刻度</div>''' + ''.join(sections) + '''<div class="note">32线程 · 4 GiB · 100万唯一Key · 32/256B · GC开启<br>均为逐调用fsync后返回；“回调上限”不是group commit。每格新库，第二轮反转次序。<br>Mac ARM64本机对照；两轮不足以给出统计置信结论，不代表正式NVMe分数。</div>'''
(BASE / 'comparison.html').write_text(html)
print(BASE / 'comparison.html')
