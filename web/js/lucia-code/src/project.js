import { validPrimPath } from './utils.js';
import { normalizeResolverPlugins } from './usd-doctor.js';

export class LuciaProject extends EventTarget {
  constructor() {
    super();
    this.name = 'Untitled';
    this.source = null;
    this.selectedPath = '/World/Hero';
    this.selectedPaths = new Set([this.selectedPath]);
    this.selectionAnchor = this.selectedPath;
    this.dirty = false;
    this.revision = 0;
    this.domainRevisions = { scene: 0, usd: 0, assets: 0 };
    this.assets = new Map();
    this.resolverPlugins = [];
    this.changes = [];
    this.activity = [];
    this.exportRemap = {};
  }
  select(path, { additive = false, range = null } = {}) {
    if (Array.isArray(range) && range.length) this.selectedPaths = additive ? new Set([...this.selectedPaths, ...range]) : new Set(range);
    else if (!additive) this.selectedPaths = new Set([path]);
    else if (this.selectedPaths.has(path) && this.selectedPaths.size > 1) this.selectedPaths.delete(path);
    else this.selectedPaths.add(path);
    if (!this.selectedPaths.has(path)) this.selectedPath = [...this.selectedPaths].at(-1) || path;
    else this.selectedPath = path;
    if (!additive || !this.selectionAnchor) this.selectionAnchor = path;
    this.emit();
  }
  remapSelection(oldPath, newPath) {
    this.selectedPaths = new Set([...this.selectedPaths].map((path) => path === oldPath || path.startsWith(`${oldPath}/`) ? `${newPath}${path.slice(oldPath.length)}` : path));
    this.selectedPath = this.selectedPath === oldPath || this.selectedPath.startsWith(`${oldPath}/`) ? `${newPath}${this.selectedPath.slice(oldPath.length)}` : this.selectedPath;
  }
  removeSelection(path, fallback = '/') {
    this.selectedPaths = new Set([...this.selectedPaths].filter((selected) => selected !== path && !selected.startsWith(`${path}/`)));
    if (!this.selectedPaths.size) this.selectedPaths.add(fallback);
    if (!this.selectedPaths.has(this.selectedPath)) this.selectedPath = [...this.selectedPaths].at(-1);
  }
  changed(summary, paths = [], domains = ['scene', 'usd', 'assets']) {
    this.dirty = true;
    this.revision++;
    for (const domain of domains) if (Object.hasOwn(this.domainRevisions, domain)) this.domainRevisions[domain]++;
    const entry = { id: crypto.randomUUID(), kind: 'change', summary, paths, at: new Date() };
    this.changes.push(entry);
    this.activity.push(entry);
    this.emit();
  }
  recordAssistantDecision(call, decision) {
    const args = call?.arguments || {};
    const safeDecision = ['proposed', 'declined', 'executed', 'rejected'].includes(decision) ? decision : 'rejected';
    const tool = typeof call?.name === 'string' && call.name ? call.name : 'unknown tool';
    const paths = [args.path, args.targetPath, args.sourcePath, args.transformSourcePath].filter((path) => typeof path === 'string' && validPrimPath(path));
    const entry = { id: crypto.randomUUID(), kind: 'assistant', decision: safeDecision, tool, summary: `Assistant ${safeDecision}: ${tool}`, paths: [...new Set(paths)].sort(), at: new Date() };
    this.activity.push(entry);
    this.emit();
    return entry;
  }
  restoreAssistantActivity(entries = []) {
    if (!Array.isArray(entries)) return 0;
    let restored = 0;
    for (const entry of entries) {
      if (!entry || entry.kind !== 'assistant' || typeof entry.tool !== 'string' || !['proposed', 'declined', 'executed', 'rejected'].includes(entry.decision)) continue;
      const paths = [...new Set((Array.isArray(entry.paths) ? entry.paths : []).filter((path) => typeof path === 'string' && validPrimPath(path)))].sort();
      this.activity.push({ id: crypto.randomUUID(), kind: 'assistant', decision: entry.decision, tool: entry.tool, summary: `Assistant ${entry.decision}: ${entry.tool}`, paths, at: new Date() });
      restored++;
    }
    if (restored) this.emit();
    return restored;
  }
  reset(name, source = null) {
    this.name = name.replace(/\.(usd[acz]?)$/i, '') || 'Untitled';
    this.source = source;
    this.selectedPath = '/World';
    this.selectedPaths = new Set([this.selectedPath]);
    this.selectionAnchor = this.selectedPath;
    this.dirty = false;
    this.revision++;
    for (const domain of Object.keys(this.domainRevisions)) this.domainRevisions[domain]++;
    this.changes = [];
    this.activity = [];
    this.assets.clear();
    this.exportRemap = {};
    this.emit();
  }
  emit() { this.dispatchEvent(new CustomEvent('change')); }
  setResolverPlugins(plugins) { const normalized = normalizeResolverPlugins(plugins); if (JSON.stringify(normalized) === JSON.stringify(this.resolverPlugins)) return this.resolverPlugins; this.resolverPlugins = normalized; this.domainRevisions.usd++; this.emit(); return this.resolverPlugins; }
}
