import { createDefaultGraph, validateGraph } from './geonodes/graph.js';
import { getNodeType, listNodeTypes } from './geonodes/nodes.js';
import { GeoNodesRunner, sourceInputHash, triangulateAuthoredMesh } from './geonodes/runner.js';
import { LuciaError } from './utils.js';

const esc = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const isCancellation = (error) => error?.code === 'LUCIA_CANCELLED';

// Resolve the prim that owns a modifier: selecting the generated output
// edits its source's graph.
export function geoNodesOwner(session, path) {
  return session.getGeomNodesOutputInfo(path)?.source || path;
}

export class LuciaGeoNodesEditor {
  constructor(app) { this.app = app; this.runner = new GeoNodesRunner(); this.path = null; this.graph = null; this.source = null; this.status = ''; this.error = null; this.timer = null; }

  get outputPaths() { const { meshPath, instancerPath } = this.app.session.geomNodesOutputPaths(this.path); return [meshPath, instancerPath]; }

  async open(path) {
    const authored = await this.app.session.getAuthoredMesh(path);
    if (!authored?.points || authored.animated) throw new LuciaError('LUCIA_GEONODES_SOURCE', 'Geometry nodes need a mesh with untimed authored points.');
    this.close();
    this.path = path;
    this.source = triangulateAuthoredMesh(authored);
    this.graph = this.app.session.getGeomNodesGraph(path) || createDefaultGraph();
    this.mount(); this.schedulePreview(0);
  }

  // Undo/redo or other edits may change the source mesh under an open editor.
  async refreshSource() {
    const path = this.path; if (!path) return;
    const authored = await this.app.session.getAuthoredMesh(path).catch(() => null);
    if (path !== this.path) return;
    if (!authored?.points || authored.animated) { this.close(); return; }
    this.source = triangulateAuthoredMesh(authored); this.schedulePreview(0);
  }

  close() {
    this.endDrag(true);
    clearTimeout(this.timer); this.runner.reset();
    this.app.bridge?.clearPreview();
    this.app.$('#geonodes-editor')?.remove();
    this.path = null; this.graph = null; this.source = null; this.error = null; this.status = '';
  }

  // ------------------------------------------------------------------ graph edits
  edit(mutate) {
    const draft = structuredClone(this.graph);
    mutate(draft);
    try { this.graph = validateGraph(draft); this.error = null; }
    catch (error) { this.error = error.message; this.renderStatus(); return false; }
    this.renderCanvas(); this.schedulePreview();
    return true;
  }

  addNode(type) {
    const definition = getNodeType(type); if (!definition) return;
    let n = 1; while (this.graph.nodes.some((node) => node.id === `${type.toLowerCase()}${n}`)) n++;
    const canvas = this.app.$('#geonodes-canvas'), x = (canvas?.scrollLeft || 0) + 40 + (this.graph.nodes.length % 5) * 24, y = (canvas?.scrollTop || 0) + 40 + (this.graph.nodes.length % 5) * 24;
    this.edit((graph) => graph.nodes.push({ id: `${type.toLowerCase()}${n}`, type, params: {}, position: [x, y] }));
  }

  // ------------------------------------------------------------------ evaluation
  schedulePreview(delay = 200) {
    clearTimeout(this.timer);
    this.timer = setTimeout(() => this.preview(), delay);
  }

  async preview() {
    if (!this.path) return;
    const path = this.path;
    this.status = 'Evaluating…'; this.renderStatus();
    try {
      const result = await this.runner.evaluate(this.graph, this.source, { preview: true });
      if (path !== this.path) return;
      this.app.bridge.setPreviewMesh(path, result.positions, result.indices, this.outputPaths);
      this.status = `${result.stats.vertices.toLocaleString()} vertices · ${result.stats.triangles.toLocaleString()} triangles${result.stats.instances ? ` · ${result.stats.instances} instances (realized on commit)` : ''}`;
      this.error = null;
    } catch (error) {
      if (isCancellation(error)) return;
      this.app.bridge.clearPreview(); this.error = error.message; this.status = '';
    }
    this.renderStatus();
  }

  async commit() {
    clearTimeout(this.timer);
    const path = this.path, graph = this.graph, source = this.source, outputPaths = this.outputPaths;
    const ok = await this.app.runMutation(`Geometry Nodes: ${path}`, [path, ...outputPaths], async () => {
      const result = await this.runner.evaluate(graph, source);
      return this.app.session.commitGeomNodes(path, graph, result, { inputHash: result.inputHash, graphKey: result.key });
    }, ['scene', 'usd'], { comparison: false });
    // The user may have discarded or reopened the editor on another prim meanwhile.
    if (ok && this.path === path) this.close();
    return ok;
  }

  // ------------------------------------------------------------------ DOM
  mount() {
    const viewport = this.app.$('#viewport');
    viewport.insertAdjacentHTML('beforeend', `<section id="geonodes-editor" class="geonodes-editor" aria-label="Geometry node editor">
      <header class="geonodes-toolbar"><strong>Geometry Nodes</strong><code>${esc(this.path)}</code>
        <select id="geonodes-add" aria-label="Add node"><option value="">Add node…</option>${groupedOptions()}</select>
        <span id="geonodes-status" class="empty" role="status"></span><span class="top-spacer"></span>
        <button id="geonodes-commit" class="primary">Commit</button><button id="geonodes-close">Discard</button></header>
      <div id="geonodes-canvas" class="geonodes-canvas"><svg id="geonodes-links" class="geonodes-links"></svg><div id="geonodes-nodes"></div></div></section>`);
    this.app.$('#geonodes-add').onchange = (event) => { if (event.target.value) this.addNode(event.target.value); event.target.value = ''; };
    this.app.$('#geonodes-commit').onclick = () => this.commit();
    this.app.$('#geonodes-close').onclick = () => { this.close(); this.app.renderInspector(); };
    this.renderCanvas(); this.renderStatus();
  }

  renderStatus() {
    const status = this.app.$('#geonodes-status'); if (!status) return;
    status.textContent = this.error ? `Error: ${this.error}` : this.status;
    status.classList.toggle('issue-error', Boolean(this.error));
    const commit = this.app.$('#geonodes-commit'); if (commit) commit.disabled = Boolean(this.error);
  }

  renderCanvas() {
    this.endDrag(true);
    const container = this.app.$('#geonodes-nodes'); if (!container) return;
    const linked = new Set(this.graph.links.map((link) => `${link.to[0]}.${link.to[1]}`));
    container.innerHTML = this.graph.nodes.map((node) => {
      const definition = getNodeType(node.type);
      const inputs = definition.inputs.map((socket) => {
        const key = `${node.id}.${socket.name}`, isLinked = linked.has(key), value = node.params[socket.name] ?? socket.default;
        const control = isLinked || socket.type === 'geometry' ? '' : paramControl(node.id, socket, value);
        return `<div class="gn-socket gn-in">${socket.type === 'enum' ? '' : `<i class="gn-dot gn-${socket.type}" data-in="${esc(key)}" title="${isLinked ? 'Click to disconnect' : socket.type}"></i>`}<span>${esc(socket.name)}</span>${control}</div>`;
      }).join('');
      const outputs = definition.outputs.map((socket) => `<div class="gn-socket gn-out"><span>${esc(socket.name)}</span><i class="gn-dot gn-${socket.type}" data-out="${esc(`${node.id}.${socket.name}`)}" title="Drag to an input"></i></div>`).join('');
      return `<div class="gn-node" data-node="${esc(node.id)}" style="left:${node.position[0]}px;top:${node.position[1]}px"><div class="gn-header" data-drag="${esc(node.id)}">${esc(definition.label)}${node.type === 'GroupOutput' ? '' : `<button class="gn-delete" data-delete="${esc(node.id)}" aria-label="Delete node">×</button>`}</div>${outputs}${inputs}</div>`;
    }).join('');
    this.bindCanvas(); this.drawLinks();
  }

  bindCanvas() {
    const app = this.app;
    app.$$('#geonodes-nodes [data-delete]').forEach((button) => button.onclick = () => this.edit((graph) => { const id = button.dataset.delete; graph.nodes = graph.nodes.filter((node) => node.id !== id); graph.links = graph.links.filter((link) => link.from[0] !== id && link.to[0] !== id); }));
    app.$$('#geonodes-nodes [data-param]').forEach((input) => input.onchange = () => {
      const [id, name, component] = input.dataset.param.split('.'), node = this.graph.nodes.find((candidate) => candidate.id === id), socket = getNodeType(node.type).inputs.find((s) => s.name === name);
      this.edit((graph) => {
        const target = graph.nodes.find((candidate) => candidate.id === id);
        if (socket.type === 'vector') { const current = [...(target.params[name] ?? socket.default ?? [0, 0, 0])]; current[Number(component)] = Number(input.value); target.params[name] = current; }
        else target.params[name] = socket.type === 'bool' ? input.checked : socket.type === 'enum' ? input.value : Number(input.value);
      });
    });
    app.$$('#geonodes-nodes [data-in]').forEach((dot) => dot.onclick = () => { const [id, name] = dot.dataset.in.split('.'); this.edit((graph) => { graph.links = graph.links.filter((link) => !(link.to[0] === id && link.to[1] === name)); }); });
    app.$$('#geonodes-nodes [data-drag]').forEach((header) => header.onpointerdown = (event) => {
      if (event.target.closest('button')) return;
      const id = header.dataset.drag, element = header.parentElement, start = [event.clientX, event.clientY], origin = [parseFloat(element.style.left), parseFloat(element.style.top)];
      const move = (e) => { element.style.left = `${Math.max(0, origin[0] + e.clientX - start[0])}px`; element.style.top = `${Math.max(0, origin[1] + e.clientY - start[1])}px`; this.drawLinks(); };
      const up = () => { this.endDrag(); const node = this.graph?.nodes.find((candidate) => candidate.id === id); if (node) node.position = [parseFloat(element.style.left), parseFloat(element.style.top)]; };
      this.beginDrag(move, up);
    });
    app.$$('#geonodes-nodes [data-out]').forEach((dot) => dot.onpointerdown = (event) => {
      event.preventDefault();
      const from = dot.dataset.out.split('.'), svg = app.$('#geonodes-links'), start = this.socketCenter(dot);
      const line = document.createElementNS('http://www.w3.org/2000/svg', 'path'); line.setAttribute('class', 'gn-link gn-link-pending'); svg.appendChild(line);
      const move = (e) => { const box = app.$('#geonodes-canvas').getBoundingClientRect(), canvas = app.$('#geonodes-canvas'); line.setAttribute('d', curve(start, [e.clientX - box.left + canvas.scrollLeft, e.clientY - box.top + canvas.scrollTop])); };
      const up = (e) => {
        this.endDrag(); line.remove();
        if (e.type === 'pointercancel' || !this.graph) return;
        const target = document.elementFromPoint(e.clientX, e.clientY)?.closest?.('[data-in]');
        if (!target) return;
        const to = target.dataset.in.split('.');
        this.edit((graph) => { graph.links = graph.links.filter((link) => !(link.to[0] === to[0] && link.to[1] === to[1])); graph.links.push({ from, to }); });
      };
      this.beginDrag(move, up, () => line.remove());
    });
  }

  // One window-level drag at a time; ended by pointerup/pointercancel, by a
  // re-render, or by close(), so no listener outlives its editor.
  beginDrag(move, up, abort = null) {
    this.endDrag();
    this.drag = { move, up, abort };
    window.addEventListener('pointermove', move); window.addEventListener('pointerup', up); window.addEventListener('pointercancel', up);
  }

  endDrag(abort = false) {
    const drag = this.drag; if (!drag) return;
    this.drag = null;
    window.removeEventListener('pointermove', drag.move); window.removeEventListener('pointerup', drag.up); window.removeEventListener('pointercancel', drag.up);
    if (abort) drag.abort?.();
  }

  socketCenter(dot) {
    const canvas = this.app.$('#geonodes-canvas'), box = canvas.getBoundingClientRect(), rect = dot.getBoundingClientRect();
    return [rect.left + rect.width / 2 - box.left + canvas.scrollLeft, rect.top + rect.height / 2 - box.top + canvas.scrollTop];
  }

  drawLinks() {
    const svg = this.app.$('#geonodes-links'); if (!svg) return;
    svg.innerHTML = this.graph.links.map((link) => {
      const out = this.app.$(`#geonodes-nodes [data-out="${CSS.escape(link.from.join('.'))}"]`), input = this.app.$(`#geonodes-nodes [data-in="${CSS.escape(link.to.join('.'))}"]`);
      return out && input ? `<path class="gn-link" d="${curve(this.socketCenter(out), this.socketCenter(input))}"/>` : '';
    }).join('');
  }
}

function curve([x1, y1], [x2, y2]) {
  const dx = Math.max(40, Math.abs(x2 - x1) / 2);
  return `M${x1},${y1} C${x1 + dx},${y1} ${x2 - dx},${y2} ${x2},${y2}`;
}

function groupedOptions() {
  const groups = new Map();
  for (const type of listNodeTypes()) if (type.type !== 'GroupOutput') (groups.get(type.category) || groups.set(type.category, []).get(type.category)).push(type);
  return [...groups].map(([category, types]) => `<optgroup label="${esc(category)}">${types.map((type) => `<option value="${esc(type.type)}">${esc(type.label)}</option>`).join('')}</optgroup>`).join('');
}

function paramControl(id, socket, value) {
  const key = `${id}.${socket.name}`;
  switch (socket.type) {
    case 'float': case 'int': return `<input type="number" step="${socket.type === 'int' ? 1 : 'any'}" value="${esc(value ?? 0)}" data-param="${esc(key)}" aria-label="${esc(socket.name)}">`;
    case 'bool': return `<input type="checkbox" ${value ? 'checked' : ''} data-param="${esc(key)}" aria-label="${esc(socket.name)}">`;
    case 'enum': return `<select data-param="${esc(key)}" aria-label="${esc(socket.name)}">${socket.options.map((option) => `<option ${option === value ? 'selected' : ''}>${esc(option)}</option>`).join('')}</select>`;
    case 'vector': return value == null ? '<small>position</small>' : `<span class="gn-vec-inputs">${[0, 1, 2].map((i) => `<input type="number" step="any" value="${esc(value[i])}" data-param="${esc(`${key}.${i}`)}" aria-label="${esc(`${socket.name} ${'xyz'[i]}`)}">`).join('')}</span>`;
    default: return '';
  }
}

// Inspector section for the selected prim.
export async function renderGeoNodesPanel(app, selectedPath) {
  const session = app.session, path = geoNodesOwner(session, selectedPath);
  let graph = null, error = null;
  try { graph = session.getGeomNodesGraph(path); } catch (e) { error = e.message; }
  const { meshPath, instancerPath } = session.geomNodesOutputPaths(path);
  const info = session.getGeomNodesOutputInfo(meshPath) || session.getGeomNodesOutputInfo(instancerPath);
  const editing = app.geoNodes.path === path;
  app.$('#inspector').innerHTML = `<section class="section" id="geonodes-panel"><h3>Geometry Nodes</h3>
    <p class="empty">Non-destructive procedural modifier. The graph is stored on <code>${esc(path)}</code>; the result is written to <code>${esc(meshPath)}</code> and, for instances, a PointInstancer <code>${esc(instancerPath)}</code> (enable <em>realizeInstances</em> on Group Output to bake them into the mesh). The source is hidden.</p>
    ${error ? `<p class="issue-error">${esc(error)}</p>` : ''}
    <p id="geonodes-state" class="empty">${graph ? `${graph.nodes.length} nodes · ${graph.links.length} links` : 'No modifier on this prim.'}</p>
    <div class="stack">
      <button id="geonodes-edit" class="primary">${editing ? 'Editor open' : graph ? 'Edit graph' : 'Add Geometry Nodes'}</button>
      ${graph ? '<button id="geonodes-reevaluate">Re-evaluate</button><button id="geonodes-apply">Apply</button><button id="geonodes-remove">Remove modifier</button>' : ''}
    </div></section>`;
  app.$('#geonodes-edit').disabled = editing;
  app.$('#geonodes-edit').onclick = async () => { try { await app.geoNodes.open(path); app.renderInspector(); } catch (e) { app.showError(e); } };
  if (!graph) return;
  // Staleness: the output records the source stamp it was evaluated from.
  session.getAuthoredMesh(path).then((authored) => {
    const state = app.$('#geonodes-state'); if (!state || !authored?.points) return;
    let stale = true; try { stale = !info || info.inputHash !== sourceInputHash(triangulateAuthoredMesh(authored)); } catch {}
    state.textContent += !info ? ' · output missing' : stale ? ' · STALE: source changed since last evaluation' : ' · up to date';
    state.classList.toggle('issue-warning', stale);
  }).catch(() => {});
  const evaluateCommitted = async () => { const authored = await session.getAuthoredMesh(path); return app.geoNodes.runner.evaluate(graph, triangulateAuthoredMesh(authored)); };
  app.$('#geonodes-reevaluate').onclick = () => app.runMutation(`Geometry Nodes: ${path}`, [path, meshPath, instancerPath], async () => { const result = await evaluateCommitted(); return session.commitGeomNodes(path, graph, result, { inputHash: result.inputHash, graphKey: result.key }); }, ['scene', 'usd'], { comparison: false });
  app.$('#geonodes-apply').onclick = async () => { if (await app.confirmAction(`Apply geometry nodes on ${path}?`, 'The evaluated mesh replaces the source geometry; the graph and generated output are removed. The operation is undoable.')) await app.runMutation(`Apply geometry nodes: ${path}`, [path], async () => session.applyGeomNodes(path, await evaluateCommitted()), ['scene', 'usd']); };
  app.$('#geonodes-remove').onclick = async () => { if (await app.confirmAction(`Remove geometry nodes from ${path}?`, 'The graph and generated output are removed and the source is shown again. The operation is undoable.')) await app.runMutation(`Remove geometry nodes: ${path}`, [path], () => session.removeGeomNodes(path), ['scene', 'usd']); };
}
