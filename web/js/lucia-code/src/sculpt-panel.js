import { SCULPT_BRUSHES, SCULPT_FALLOFFS } from './sculpt/brushes.js';

const esc = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const LABELS = { draw: 'Draw', smooth: 'Smooth', inflate: 'Inflate', grab: 'Grab', flatten: 'Flatten', pinch: 'Pinch', mask: 'Mask' };

export function renderSculptPanel(app, path) {
  const sculpt = app.sculpt, active = sculpt.active && sculpt.path === path, s = sculpt.settings;
  app.$('#inspector').innerHTML = `<section class="section" id="sculpt-panel"><h3>Sculpt</h3>
    <p class="empty">${active ? 'Left-drag to sculpt · Ctrl inverts · Shift smooths · middle-drag orbits · [ ] resize · 1–7 pick brush. Each stroke is one undo step.' : 'Sculpting deforms authored points with fixed topology. Normals are re-derived after each stroke.'}</p>
    <button id="sculpt-toggle" class="${active ? '' : 'primary'}">${active ? 'Exit sculpt mode' : 'Enter sculpt mode'}</button>
    ${active ? `<div class="sculpt-brushes" role="radiogroup" aria-label="Sculpt brush">${SCULPT_BRUSHES.map((brush, i) => `<button role="radio" aria-checked="${s.brush === brush}" class="${s.brush === brush ? 'active' : ''}" data-sculpt-brush="${brush}" title="${LABELS[brush]} (${i + 1})">${LABELS[brush]}</button>`).join('')}</div>
    <div class="field-grid">
      <label for="sculpt-radius">Radius</label><input id="sculpt-radius" type="number" min="0.0001" step="any" value="${esc(s.radius)}">
      <label for="sculpt-strength">Strength</label><input id="sculpt-strength" type="range" min="0" max="1" step="0.01" value="${esc(s.strength)}">
      <label for="sculpt-falloff">Falloff</label><select id="sculpt-falloff">${SCULPT_FALLOFFS.map((f) => `<option ${f === s.falloff ? 'selected' : ''}>${f}</option>`).join('')}</select>
      <label>Symmetry</label><span class="sculpt-symmetry">${['X', 'Y', 'Z'].map((axis, i) => `<label><input type="checkbox" data-sculpt-axis="${i}" ${s.symmetry[i] ? 'checked' : ''}> ${axis}</label>`).join('')}</span>
      <label for="sculpt-invert">Invert</label><input id="sculpt-invert" type="checkbox" ${s.invert ? 'checked' : ''}>
    </div>
    <div class="stack"><button id="sculpt-mask-clear">Clear mask</button><button id="sculpt-mask-invert">Invert mask</button></div>` : ''}
  </section>`;
  app.$('#sculpt-toggle').onclick = async () => {
    try { if (active) sculpt.exit(); else await sculpt.enter(path); } catch (error) { app.showError(error); }
    app.renderInspector();
  };
  if (!active) return;
  const update = (patch) => { try { sculpt.setSettings(patch); } catch (error) { app.showError(error); } app.renderInspector(); };
  app.$$('[data-sculpt-brush]').forEach((button) => button.onclick = () => update({ brush: button.dataset.sculptBrush }));
  app.$('#sculpt-radius').onchange = (event) => update({ radius: Number(event.target.value) });
  app.$('#sculpt-strength').onchange = (event) => update({ strength: Number(event.target.value) });
  app.$('#sculpt-falloff').onchange = (event) => update({ falloff: event.target.value });
  app.$('#sculpt-invert').onchange = (event) => update({ invert: event.target.checked });
  app.$$('[data-sculpt-axis]').forEach((input) => input.onchange = () => update({ symmetry: s.symmetry.map((value, i) => i === Number(input.dataset.sculptAxis) ? input.checked : value) }));
  app.$('#sculpt-mask-clear').onclick = () => sculpt.clearMask();
  app.$('#sculpt-mask-invert').onclick = () => sculpt.invertMask();
}

// Viewport shortcuts while sculpting. Returns true when the key was handled.
export function handleSculptKey(app, event) {
  const sculpt = app.sculpt;
  if (!sculpt.active || event.target.closest?.('input, select, textarea')) return false;
  const index = Number(event.key) - 1;
  if (Number.isInteger(index) && index >= 0 && index < SCULPT_BRUSHES.length) sculpt.setSettings({ brush: SCULPT_BRUSHES[index] });
  else if (event.key === '[' || event.key === ']') sculpt.setSettings({ radius: Number((sculpt.settings.radius * (event.key === ']' ? 1.15 : 1 / 1.15)).toPrecision(4)) });
  else if (event.key === 'Escape') sculpt.exit();
  else return false;
  if (app.currentInspectorTab === 'sculpt') app.renderInspector();
  return true;
}
