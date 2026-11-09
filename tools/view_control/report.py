"""Offline comparison artifact; no service or external resources required."""
import html
import json
from pathlib import Path
import shutil


def write_report(directory, reference, candidate, metrics, reference_label="usdview / reference", candidate_label="lusdview / candidate"):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    for source, name in ((reference, 'reference.png'), (candidate, 'candidate.png')):
        target = directory/name
        if Path(source).resolve() != target.resolve():
            shutil.copyfile(source, target)
    summary = html.escape(json.dumps(metrics, indent=2))
    page = '''<!doctype html><html lang="en"><meta charset="utf-8">
<title>Viewer comparison</title>
<style>body{background:#222;color:#eee;font:16px system-ui;margin:24px}img{max-width:100%}
.pair{display:flex;gap:16px}.pair figure{margin:0;flex:1}.stack{position:relative;display:inline-block;max-width:100%}
.stack img{display:block}.stack .top{position:absolute;inset:0;width:100%;height:100%}
label{display:block;margin:16px 0}pre{white-space:pre-wrap}button,select,input{font:inherit}</style>
<h1>Viewer comparison</h1><p>Original emitted pixels; no registration, resizing or exposure fitting.</p>
<div class="pair"><figure><figcaption>REFERENCE_LABEL</figcaption><img src="reference.png"></figure>
<figure><figcaption>CANDIDATE_LABEL</figcaption><img src="candidate.png"></figure></div>
<label>Comparison <select id="mode"><option>Overlay</option><option>Wipe</option></select>
<input id="amount" type="range" min="0" max="100" value="50" aria-label="Comparison amount"></label>
<div class="stack"><img src="reference.png"><img class="top" id="top" src="candidate.png"></div>
<div class="pair"><figure><figcaption>Difference</figcaption><img src="diff.png"></figure>
<figure><figcaption>Heatmap</figcaption><img src="heatmap.png"></figure></div>
<details><summary>Metrics</summary><pre>METRICS</pre></details>
<script>const mode=document.getElementById('mode'),amount=document.getElementById('amount'),topImage=document.getElementById('top');
function update(){const wipe=mode.value==='Wipe';topImage.style.opacity=wipe?1:amount.value/100;
topImage.style.clipPath=wipe?'inset(0 '+(100-amount.value)+'% 0 0)':'none';}
mode.onchange=amount.oninput=update;update();</script></html>'''
    target = directory/'index.html'
    target.write_text(page.replace('METRICS', summary).replace('REFERENCE_LABEL', html.escape(reference_label)).replace('CANDIDATE_LABEL', html.escape(candidate_label)), encoding='utf-8')
    return str(target)
