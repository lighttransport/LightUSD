"""Offline HTML report with escaped, paginated data and lazy image views."""
import json
import os
from pathlib import Path


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
        rows.append({'wall_s': round(wall_s, 3), 'rss_mib': round(rss_mib, 1), 'id': case['id'], 'dataset': case.get('dataset', ''), 'status': case.get('status', 'unknown'),
                     'pictures': pictures, 'logs': logs, 'detail': case})
    payload = json.dumps({'summary': data.get('summary', {}), 'capabilities': data.get('capabilities', {}), 'rows': rows}, allow_nan=False).replace('<', '\\u003c').replace('&', '\\u0026')
    template = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>ousddiff report</title>
<style>body{font:15px system-ui;margin:24px;background:#f4f5f7;color:#19202b}input,select,button{padding:7px;margin:4px}table{border-collapse:collapse;width:100%;background:white}td,th{padding:10px;text-align:left;border-bottom:1px solid #ddd}th{cursor:pointer}pre{white-space:pre-wrap;overflow-wrap:anywhere;max-height:460px;overflow:auto}figure{display:inline-block;margin:8px;width:280px}img{width:100%;background:#111}dialog{width:90%;max-height:90%;overflow:auto;border:1px solid #aaa}.fail,.preparation_error{color:#b21d28}.pass{color:#176b36}</style>
<h1>OpenUSD / lusdrender validation</h1><pre id="summary"></pre><details><summary>Backend capability results</summary><pre id="caps"></pre></details>
<input id="search" placeholder="Filter case ID"><select id="dataset"><option value="">All datasets</option></select><select id="status"><option value="">All statuses</option></select><button id="previous">Previous</button><button id="next">Next</button><span id="page"></span>
<table><thead><tr><th data-key="id">Case ↕</th><th data-key="dataset">Dataset ↕</th><th data-key="status">Status ↕</th><th data-key="wall_s">Render wall Σ (s) ↕</th><th data-key="rss_mib">Max RSS (MiB) ↕</th><th>Details</th></tr></thead><tbody id="body"></tbody></table>
<dialog id="detail"><button id="close">Close</button><h2 id="title"></h2><div id="pictures"></div><div id="logs"></div><pre id="json"></pre></dialog>
<script id="data" type="application/json">__DATA__</script><script>
const data=JSON.parse(document.getElementById('data').textContent), el=id=>document.getElementById(id);let page=0,sort='id',direction=1;
el('summary').textContent=JSON.stringify(data.summary,null,2);el('caps').textContent=JSON.stringify(data.capabilities,null,2);
for(const field of ['dataset','status']) for(const value of [...new Set(data.rows.map(r=>r[field]))].sort()){let o=document.createElement('option');o.value=value;o.textContent=value;el(field).append(o)}
function show(r){el('title').textContent=r.id;el('pictures').replaceChildren();el('logs').replaceChildren();for(const p of r.pictures){const f=document.createElement('figure'),i=document.createElement('img'),c=document.createElement('figcaption');i.src=p.path;i.loading='lazy';i.alt=p.label;c.textContent=p.label;f.append(i,c);el('pictures').append(f)}for(const l of r.logs){const a=document.createElement('a');a.href=l.path;a.textContent=l.label;a.style.marginRight='12px';el('logs').append(a)}el('json').textContent=JSON.stringify(r.detail,null,2);el('detail').showModal()}
function draw(){let rows=data.rows.filter(r=>r.id.toLowerCase().includes(el('search').value.toLowerCase())&&(!el('dataset').value||r.dataset===el('dataset').value)&&(!el('status').value||r.status===el('status').value)).sort((a,b)=>direction*(typeof a[sort]==='number'?a[sort]-b[sort]:String(a[sort]).localeCompare(String(b[sort]))));page=Math.min(page,Math.max(0,Math.ceil(rows.length/100)-1));el('body').replaceChildren();for(const r of rows.slice(page*100,page*100+100)){const tr=document.createElement('tr');for(const k of ['id','dataset','status','wall_s','rss_mib']){const td=document.createElement('td');td.textContent=r[k];if(k==='status')td.className=r[k];tr.append(td)}const td=document.createElement('td'),b=document.createElement('button');b.textContent='Inspect images and metrics';b.onclick=()=>show(r);td.append(b);tr.append(td);el('body').append(tr)}el('page').textContent=`${rows.length} cases · page ${page+1}/${Math.max(1,Math.ceil(rows.length/100))}`}
for(const id of ['search','dataset','status'])el(id).oninput=()=>{page=0;draw()};el('previous').onclick=()=>{page=Math.max(0,page-1);draw()};el('next').onclick=()=>{page++;draw()};el('close').onclick=()=>el('detail').close();for(const th of document.querySelectorAll('th[data-key]'))th.onclick=()=>{direction=sort===th.dataset.key?-direction:1;sort=th.dataset.key;draw()};draw();
</script></html>'''
    temporary = path.with_suffix('.html.tmp')
    temporary.write_text(template.replace('__DATA__', payload))
    temporary.replace(path)
