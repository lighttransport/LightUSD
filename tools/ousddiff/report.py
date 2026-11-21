"""Offline HTML report with escaped, paginated data and lazy image views."""
import json
import os
from pathlib import Path

from diagnostics import SUCCESS_STATUSES, failure_codes


def report(data, path):
    path = Path(path).resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    # Embed only case summaries and artifact paths; full machine data stays JSON.
    rows = []
    def relative(value):
        return os.path.relpath(value, path.parent).replace(os.sep, '/') if value else None
    for case in data['cases']:
        pictures = []
        logs = []
        for name, render in case.get('renders', {}).items():
            if render.get('image'):
                pictures.append({'label': name, 'path': relative(render['image'])})
            for attempt in render.get('attempts', []):
                for kind in ('stdout', 'stderr'):
                    if attempt.get(kind):
                        logs.append({'label': name + ' ' + kind, 'path': relative(attempt[kind])})
        for pair in case.get('comparisons', []):
            for kind in ('diff', 'heatmap'):
                if pair.get('metrics', {}).get(kind):
                    pictures.append({'label': '/'.join(pair['renderers']) + ' ' + kind, 'path': relative(pair['metrics'][kind])})
        wall_s = sum(r.get('wall_s') or 0 for r in case.get('renders', {}).values())
        rss_mib = max((r.get('peak_rss_bytes') or 0 for r in case.get('renders', {}).values()), default=0) / 1024**2
        codes = case.get('failure_codes', failure_codes(case)) if case.get('status') not in SUCCESS_STATUSES else []
        rows.append({'failure_codes': codes, 'codes': ', '.join(codes), 'wall_s': round(wall_s, 3), 'rss_mib': round(rss_mib, 1), 'id': case['id'], 'dataset': case.get('dataset', ''), 'status': case.get('status', 'unknown'),
                     'pictures': pictures, 'logs': logs, 'detail': case})
    payload = json.dumps({'summary': data.get('summary', {}), 'capabilities': data.get('capabilities', {}), 'rows': rows, 'failed_ids': data.get('failed_ids', [row['id'] for row in rows if row['status'] not in SUCCESS_STATUSES])}, allow_nan=False).replace('<', '\\u003c').replace('&', '\\u0026')
    template = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>ousddiff report</title>
<style>body{font:15px system-ui;margin:24px;background:#f4f5f7;color:#19202b}input,select,button{padding:7px;margin:4px}table{border-collapse:collapse;width:100%;background:white}td,th{overflow-wrap:anywhere;max-width:40vw;padding:10px;text-align:left;border-bottom:1px solid #ddd}th{cursor:pointer}pre{white-space:pre-wrap;overflow-wrap:anywhere;max-height:460px;overflow:auto}figure{display:inline-block;margin:8px;width:280px}img{width:100%;background:#111}dialog{width:90%;max-height:90%;overflow:auto;border:1px solid #aaa}.fail,.preparation_error{color:#b21d28}.pass{color:#176b36}.summary{display:flex;gap:12px;flex-wrap:wrap;margin:16px 0}.card{padding:12px 18px;background:white;border:1px solid #ddd;border-radius:6px}.card strong{display:block;font-size:24px}</style>
<h1>OpenUSD / lusdrender validation</h1><div id="summary" class="summary"></div><details><summary>Full measurements</summary><pre id="summary-json"></pre></details><details><summary>Backend capability results</summary><pre id="caps"></pre></details>
<input id="search" placeholder="Filter case ID"><select id="dataset"><option value="">All datasets</option></select><select id="status"><option value="">All statuses</option></select><select id="code"><option value="">All failure codes</option></select><button id="download">Download failing IDs</button><button id="previous">Previous</button><button id="next">Next</button><span id="page"></span>
<table><thead><tr><th data-key="id">Case ↕</th><th data-key="dataset">Dataset ↕</th><th data-key="status">Status ↕</th><th data-key="codes">Failure codes ↕</th><th data-key="wall_s">Render wall Σ (s) ↕</th><th data-key="rss_mib">Max RSS (MiB) ↕</th><th>Details</th></tr></thead><tbody id="body"></tbody></table>
<dialog id="detail"><button id="close">Close</button><h2 id="title"></h2><div id="pictures"></div><div id="logs"></div><pre id="json"></pre></dialog>
<script id="data" type="application/json">__DATA__</script><script>
const data=JSON.parse(document.getElementById('data').textContent), el=id=>document.getElementById(id);let page=0,sort='id',direction=1;
el('summary-json').textContent=JSON.stringify(data.summary,null,2);el('caps').textContent=JSON.stringify(data.capabilities,null,2);
for(const [label,value] of [['Completed cases',data.summary.completed??data.rows.length],['Unexpected failures',data.failed_ids.length],['Ungated image comparisons',data.summary.comparison_status_counts?.uncalibrated??0]]){const card=document.createElement('div'),v=document.createElement('strong'),caption=document.createElement('span');card.className='card';v.textContent=value;caption.textContent=label;card.append(v,caption);el('summary').append(card)}
for(const field of ['dataset','status']) for(const value of [...new Set(data.rows.map(r=>r[field]))].sort()){let o=document.createElement('option');o.value=value;o.textContent=value;el(field).append(o)}
for(const value of [...new Set(data.rows.flatMap(r=>r.failure_codes))].sort()){let o=document.createElement('option');o.value=value;o.textContent=value;el('code').append(o)}
el('download').onclick=()=>{const url=URL.createObjectURL(new Blob([JSON.stringify(data.failed_ids,null,2)],{type:'application/json'}));const a=document.createElement('a');a.href=url;a.download='failed_ids.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000)};
function show(r){el('title').textContent=r.id;el('pictures').replaceChildren();el('logs').replaceChildren();for(const p of r.pictures){const f=document.createElement('figure'),i=document.createElement('img'),c=document.createElement('figcaption');i.src=p.path;i.loading='lazy';i.alt=p.label;c.textContent=p.label;f.append(i,c);el('pictures').append(f)}for(const l of r.logs){const a=document.createElement('a');a.href=l.path;a.textContent=l.label;a.style.marginRight='12px';el('logs').append(a)}el('json').textContent=JSON.stringify(r.detail,null,2);el('detail').showModal()}
function draw(){let rows=data.rows.filter(r=>r.id.toLowerCase().includes(el('search').value.toLowerCase())&&(!el('dataset').value||r.dataset===el('dataset').value)&&(!el('status').value||r.status===el('status').value)&&(!el('code').value||r.failure_codes.includes(el('code').value))).sort((a,b)=>direction*(typeof a[sort]==='number'?a[sort]-b[sort]:String(a[sort]).localeCompare(String(b[sort]))));page=Math.min(page,Math.max(0,Math.ceil(rows.length/100)-1));el('body').replaceChildren();for(const r of rows.slice(page*100,page*100+100)){const tr=document.createElement('tr');for(const k of ['id','dataset','status','codes','wall_s','rss_mib']){const td=document.createElement('td');td.textContent=r[k];if(k==='status')td.className=r[k];tr.append(td)}const td=document.createElement('td'),b=document.createElement('button');b.textContent='Inspect images and metrics';b.onclick=()=>show(r);td.append(b);tr.append(td);el('body').append(tr)}el('page').textContent=`${rows.length} cases · page ${page+1}/${Math.max(1,Math.ceil(rows.length/100))}`}
for(const id of ['search','dataset','status','code'])el(id).oninput=()=>{page=0;draw()};el('previous').onclick=()=>{page=Math.max(0,page-1);draw()};el('next').onclick=()=>{page++;draw()};el('close').onclick=()=>el('detail').close();for(const th of document.querySelectorAll('th[data-key]'))th.onclick=()=>{direction=sort===th.dataset.key?-direction:1;sort=th.dataset.key;draw()};draw();
</script></html>'''
    temporary = path.with_suffix('.html.tmp')
    temporary.write_text(template.replace('__DATA__', payload))
    temporary.replace(path)
