import { applySourceDelta, createSourceDelta } from './source-delta.js';

export class LuciaCommandStack extends EventTarget {
  constructor({ maxCommands = 50, maxBytes = 128 * 1024 * 1024 } = {}) {
    super();
    if (!Number.isSafeInteger(maxCommands) || maxCommands < 1 || !Number.isSafeInteger(maxBytes) || maxBytes < 1)
      throw new RangeError('Undo history limits must be positive safe integers.');
    this.undoItems = []; this.redoItems = []; this.maxCommands = maxCommands; this.maxBytes = maxBytes; this.busy = false;
    this.assetSnapshots = new WeakMap();
  }
  get canUndo() { return this.undoItems.length > 0 && !this.busy; }
  get canRedo() { return this.redoItems.length > 0 && !this.busy; }
  async execute(command) {
    if (this.busy) return false;
    this.busy = true;
    try {
      command.projectBefore = command.snapshotProject?.(this.assetSnapshots);
      command.before = await command.do();
      command.projectAfter = command.snapshotProject?.(this.assetSnapshots);
      command.after = await command.snapshot?.();
      await command.compact?.();
      command.estimatedBytes = estimateCommandBytes(command);
      if (command.estimatedBytes > this.maxBytes) throw new Error('Operation exceeds the undo history memory budget. Reduce the asset or atlas size.');
      this.undoItems.push(command); this.redoItems.length = 0; this.trim();
    } catch (error) {
      this.assetSnapshots = new WeakMap();
      // A command may have partially authored the native stage or project
      // assets before reporting an error. Restore both snapshots before the
      // error reaches the UI so failed operations are observationally atomic.
      command.restoreProject?.(command.projectBefore);
      try { if (command.before != null) await command.undo(command.before); } catch { /* preserve the original operation error */ }
      throw error;
    } finally { this.busy = false; this.emit(); }
    return true;
  }
  async undo() {
    if (this.busy || !this.undoItems.length) return;
    const command = this.undoItems.at(-1);
    let recovery, projectRecovery;
    this.busy = true;
    try {
      recovery = await command.snapshot();
      projectRecovery = command.snapshotProject?.(this.assetSnapshots);
      if (!command.compacted) command.after = recovery;
      command.restoreProject?.(command.projectBefore);
      await command.undo(command.before);
      this.undoItems.pop(); this.redoItems.push(command);
      command.estimatedBytes = estimateCommandBytes(command);
    } catch (error) {
      command.restoreProject?.(projectRecovery || command.projectAfter);
      if (command.compacted) {
        if (recovery != null && await command.snapshot() !== recovery) await command.restoreSource(recovery);
      } else if (command.after != null) await command.undo(command.after);
      throw error;
    } finally { this.busy = false; this.emit(); }
  }
  async redo() {
    if (this.busy || !this.redoItems.length) return;
    const command = this.redoItems.at(-1);
    let recovery, projectRecovery;
    this.busy = true;
    try {
      if (command.compacted) recovery = await command.snapshot();
      projectRecovery = command.snapshotProject?.(this.assetSnapshots);
      command.restoreProject?.(command.projectAfter);
      await command.undo(command.after);
      this.redoItems.pop(); this.undoItems.push(command); this.trim();
    } catch (error) {
      command.restoreProject?.(projectRecovery || command.projectBefore);
      if (command.compacted) {
        if (recovery != null && await command.snapshot() !== recovery) await command.restoreSource(recovery);
      } else await command.undo(command.before);
      throw error;
    } finally { this.busy = false; this.emit(); }
  }
  trim() {
    while (this.undoItems.length > this.maxCommands || estimateHistoryBytes(this.undoItems) > this.maxBytes) this.undoItems.shift();
  }
  prune(keep = 0) {
    if (this.busy) throw new Error('Cannot prune history during an operation.');
    if (!Number.isSafeInteger(keep) || keep < 0) throw new RangeError('History retention must be a non-negative integer.');
    this.undoItems.splice(0, Math.max(0, this.undoItems.length - keep));
    this.redoItems.length = 0;
    this.assetSnapshots = new WeakMap();
    this.emit();
  }
  clear() { this.undoItems.length = this.redoItems.length = 0; this.assetSnapshots = new WeakMap(); this.emit(); }
  emit() { this.dispatchEvent(new CustomEvent('change')); }
}

export function sessionCommand(session, summary, affectedPaths, operation, project = null) {
  const command = {
    summary, affectedPaths,
    do: async () => {
      const before = await session.exportUSDA();
      try {
        await operation();
      } catch (error) {
        command.restoreProject?.(command.projectBefore);
        try { await session.restore(before); } catch { /* preserve the original operation error */ }
        throw error;
      }
      return before;
    },
    snapshot: async () => session.exportUSDA(),
    restoreSource: (source) => session.restore(source),
    undo: async (state) => state?.propertyDelta ? session.restorePropertyHistory(state.propertyDelta, state.direction) : session.restore(typeof state === 'string' ? state :
      await applySourceDelta(await session.exportUSDA(), state.delta, state.direction)),
    compact: async () => {
      const propertyDelta = await session.compactPropertyHistory?.(command.before, command.after);
      if (propertyDelta) {
        command.before = { propertyDelta, direction: 'undo' };
        command.after = { propertyDelta, direction: 'redo' };
        command.compacted = true;
        return;
      }
      const delta = await createSourceDelta(command.before, command.after);
      // Large replacements are still represented losslessly by the same
      // record; small property edits no longer retain two full stages.
      command.before = { delta, direction: 'undo' };
      command.after = { delta, direction: 'redo' };
      command.compacted = true;
    },
    snapshotProject: project ? (cache) => ({ name: project.name, assets: cloneAssetMap(project.assets, cache), provenance: structuredClone(project.provenance || null), exportRemap: { ...project.exportRemap } }) : null,
    restoreProject: project ? (state) => { if (!state) return; project.name = state.name; project.assets = cloneAssetMap(state.assets); project.provenance = structuredClone(state.provenance || null); project.exportRemap = { ...state.exportRemap }; project.emit(); } : null,
  };
  return command;
}

export function estimateCommandBytes(command) {
  return estimateHistoryBytes([command]);
}
export function estimateHistoryBytes(commands) {
  const seen = new Set();
  const visit = value => {
    if (typeof value === 'string') return new Blob([value]).size;
    if (!value || typeof value !== 'object' || seen.has(value)) return 0;
    seen.add(value);
    if (ArrayBuffer.isView(value)) return visit(value.buffer);
    if (value instanceof ArrayBuffer) return value.byteLength;
    if (value instanceof Map) return [...value].reduce((n, pair) => n + visit(pair), 0);
    return Object.values(value).reduce((n, item) => n + visit(item), 0);
  };
  return visit(commands.map(command => [command.before, command.after, command.projectBefore, command.projectAfter]));
}

function equalBuffers(left, right) {
  if (left.byteLength !== right.byteLength) return false;
  const count = Math.floor(left.byteLength / 4);
  const a = new Uint32Array(left, 0, count), b = new Uint32Array(right, 0, count);
  for (let i = 0; i < count; i++) if (a[i] !== b[i]) return false;
  const aa = new Uint8Array(left), bb = new Uint8Array(right);
  for (let i = count * 4; i < aa.length; i++) if (aa[i] !== bb[i]) return false;
  return true;
}
function snapshotBuffer(buffer, cache) {
  const previous = cache?.get(buffer);
  if (previous && equalBuffers(buffer, previous)) return previous;
  // Content addressing also shares independently allocated equal assets and
  // restored runtime buffers. Hash collisions require exact equality, and weak
  // references keep this index from retaining evicted undo payloads.
  let key, candidates;
  if (cache && typeof WeakRef === 'function') {
    cache.content ||= new Map();
    let hash = 2166136261;
    const words = new Uint32Array(buffer, 0, Math.floor(buffer.byteLength / 4));
    for (const word of words) { hash ^= word; hash = Math.imul(hash, 16777619); }
    const bytes = new Uint8Array(buffer);
    for (let i = words.length * 4; i < bytes.length; i++) { hash ^= bytes[i]; hash = Math.imul(hash, 16777619); }
    key = `${buffer.byteLength}:${hash >>> 0}`;
    candidates = (cache.content.get(key) || []).filter(reference => reference.deref());
    for (const reference of candidates) {
      const snapshot = reference.deref();
      if (snapshot && equalBuffers(buffer, snapshot)) { cache.set(buffer, snapshot); return snapshot; }
    }
  }
  const copy = buffer.slice(0);
  cache?.set(buffer, copy);
  if (candidates) {
    if (cache.content.size >= 4096 && !cache.content.has(key)) cache.content.delete(cache.content.keys().next().value);
    cache.content.set(key, [...candidates.slice(-3), new WeakRef(copy)]);
  }
  return copy;
}

function cloneValue(value, seen = new WeakMap(), cache = null) {
  if (value == null || typeof value !== 'object') return value;
  if (seen.has(value)) return seen.get(value);
  if (ArrayBuffer.isView(value)) {
    const buffer = cloneValue(value.buffer, seen, cache);
    const copy = value instanceof DataView ? new DataView(buffer, value.byteOffset, value.byteLength)
      : new value.constructor(buffer, value.byteOffset, value.length);
    seen.set(value, copy);
    return copy;
  }
  if (value instanceof ArrayBuffer) { const copy = snapshotBuffer(value, cache); seen.set(value, copy); return copy; }
  if (Array.isArray(value)) { const copy = []; seen.set(value, copy); value.forEach((item) => copy.push(cloneValue(item, seen, cache))); return copy; }
  const prototype = Object.getPrototypeOf(value);
  if (prototype !== Object.prototype && prototype !== null) return value;
  const copy = {}; seen.set(value, copy);
  for (const [key, item] of Object.entries(value)) copy[key] = cloneValue(item, seen, cache);
  return copy;
}

function cloneAssetMap(assets, cache = null) {
  const seen = new WeakMap();
  // Interned history bytes may be shared by unrelated assets. Restore each
  // asset into independent mutable storage while preserving its own views.
  return new Map([...assets || []].map(([path, asset]) => [path, cloneValue(asset, cache ? seen : new WeakMap(), cache)]));
}
