// SPDX-License-Identifier: Apache-2.0
import { createRenderer } from './src/webgpu-mtlx/renderer.js';
import { syntheticScene } from './src/webgpu-mtlx/scene.js';
import { parseMaterialX, valueCategories } from './src/webgpu-mtlx/graph.js';
const $ = id => document.getElementById(id), canvas = $('view');
const params = new URLSearchParams(location.search);
const width = Number(params.get('width') || 1280), height = Number(params.get('height') || 720);
canvas.width = Math.min(1920, Math.max(32, Number.isFinite(width) ? width : 1280)); canvas.height = Math.min(1080, Math.max(32, Number.isFinite(height) ? height : 720));
const state = window.__webgpuMtlx = { ready: false, errors: [], paused: params.has('manual'), renderer: null };
const error = e => { $('error').textContent = String(e.message || e); state.errors.push(String(e.message || e)); };
function download(bytes, type, name) { const url = URL.createObjectURL(new Blob([bytes], { type })); const a = document.createElement('a'); a.href = url; a.download = name; a.click(); setTimeout(() => URL.revokeObjectURL(url), 1000); }
async function main() {
  const renderer = await createRenderer(canvas); state.renderer = renderer;
  renderer.addEventListener('diagnostic', e => error(e.detail));
  renderer.addEventListener('progress', e => { const s = e.detail; $('status').textContent = `${s.mode} · ${s.samples} spp · ${s.milliseconds.toFixed(1)} ms · ${s.width} × ${s.height} · ${s.triangles} triangles`; });
  let initial, yaw = 0.68, pitch = 0.3, radius = 6, dragging = null, target = [0, 0.8, 0], fov = 42;
  const syncOrbit = camera => { target = camera.target.slice(); fov = camera.fov; const d = camera.origin.map((v, i) => v - target[i]); radius = Math.hypot(...d); yaw = Math.atan2(d[0], d[2]); pitch = Math.asin(d[1] / radius); };
  const orbit = () => renderer.setCamera({ origin: [target[0] + radius * Math.cos(pitch) * Math.sin(yaw), target[1] + radius * Math.sin(pitch), target[2] + radius * Math.cos(pitch) * Math.cos(yaw)], target, fov });
  async function scene() {
    state.ready = false;
    if ($('scene').value === 'shaderball') {
      const { loadShaderBallGeometry } = await import('./src/webgpu-mtlx/usd-scene.js');
      initial = await loadShaderBallGeometry(s => { $('status').textContent = s; });
      renderer.setMode('realtime'); $('mode').value = 'realtime';
    } else initial = syntheticScene($('scene').value);
    await renderer.loadScene(initial); syncOrbit(initial.camera); state.ready = true;
  }
  await scene();
  $('scene').onchange = () => scene().catch(error);
  $('mode').onchange = () => renderer.setMode($('mode').value);
  $('exposure').oninput = () => renderer.setOptions({ exposure: Number($('exposure').value) });
  $('scale').onchange = () => renderer.setOptions($('scale').value === 'auto' ? { autoResolution: true } : { autoResolution: false, resolutionScale: Number($('scale').value) });
  $('pause').onclick = () => { state.paused = !state.paused; $('pause').textContent = state.paused ? 'Resume' : 'Pause'; };
  $('reset').onclick = () => { syncOrbit(initial.camera); renderer.setCamera(initial.camera); };
  $('save').onclick = async () => { try { const capture = await renderer.capture(); download(capture.bytes, 'image/x-exr', 'materialx-preview.exr'); download(JSON.stringify(capture.metadata, null, 2), 'application/json', 'materialx-preview.json'); } catch (e) { error(e); } };
  $('png').onclick = async () => { try { const capture = await renderer.capture({ format: 'png' }); download(capture.bytes, 'image/png', 'materialx-preview.png'); } catch (e) { error(e); } };
  $('material').onchange = async e => { try { const file = e.target.files[0]; if (file) { await renderer.setMaterialDocument(parseMaterialX(await file.text(), { source: file.name })); $('error').textContent = ''; } } catch (e) { error(e); } };
  $('inventory').onclick = async () => {
    try {
      const response = await fetch('/__mtlx/catalog.json'); if (!response.ok) throw new Error('Pinned MaterialX checkout is unavailable');
      const catalog = await response.json(); let count = 0, candidates = 0; const inventory = [];
      for (const file of catalog.files) {
        const r = await fetch(file); if (!r.ok) throw new Error(`Cannot read ${file}`);
        const doc = new DOMParser().parseFromString(await r.text(), 'application/xml');
        for (const def of doc.querySelectorAll('nodedef')) { count++; const category = def.getAttribute('node'); const candidate = valueCategories.has(category); if (candidate) candidates++; inventory.push({ name: def.getAttribute('name'), category, status: candidate ? 'candidate: requires overload validation' : 'unimplemented', source: file }); }
      }
      state.inventory = inventory; $('coverage').textContent = `${catalog.version}: ${count} NodeDefs inventoried; ${candidates} have candidate value emitters. No full-library conformance claim.`;
    } catch (e) { error(e); }
  };
  canvas.onpointerdown = e => { dragging = [e.clientX, e.clientY]; canvas.setPointerCapture(e.pointerId); };
  canvas.onpointerup = canvas.onpointercancel = () => { dragging = null; };
  canvas.onpointermove = e => { if (!dragging) return; yaw -= (e.clientX - dragging[0]) * 0.008; pitch = Math.max(-1.4, Math.min(1.4, pitch + (e.clientY - dragging[1]) * 0.008)); dragging = [e.clientX, e.clientY]; orbit(); };
  canvas.addEventListener('wheel', e => { e.preventDefault(); radius = Math.max(1.2, Math.min(25, radius * Math.exp(e.deltaY * 0.001))); orbit(); }, { passive: false });
  async function frame() { try { if (state.ready && !state.paused) await renderer.renderStep(); } catch (e) { error(e); state.paused = true; } requestAnimationFrame(frame); }
  requestAnimationFrame(frame); window.addEventListener('pagehide', () => renderer.dispose(), { once: true });
}
main().catch(error);
