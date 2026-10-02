export class LuciaCommandStack extends EventTarget {
  constructor({ maxCommands = 50, maxBytes = 128 * 1024 * 1024 } = {}) {
    super(); this.undoItems = []; this.redoItems = []; this.maxCommands = maxCommands; this.maxBytes = maxBytes; this.busy = false;
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
      command.estimatedBytes = estimateCommandBytes(command);
      if (command.estimatedBytes > this.maxBytes) throw new Error('Operation exceeds the undo history memory budget. Reduce the asset or atlas size.');
      this.undoItems.push(command); this.redoItems.length = 0; this.trim();
    } catch (error) {
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
    this.busy = true;
    try {
      command.after = await command.snapshot();
      command.restoreProject?.(command.projectBefore);
      await command.undo(command.before);
      this.undoItems.pop(); this.redoItems.push(command);
      command.estimatedBytes = estimateCommandBytes(command);
    } catch (error) {
      command.restoreProject?.(command.projectAfter);
      if (command.after != null) await command.undo(command.after);
      throw error;
    } finally { this.busy = false; this.emit(); }
  }
  async redo() {
    if (this.busy || !this.redoItems.length) return;
    const command = this.redoItems.at(-1);
    this.busy = true;
    try {
      command.restoreProject?.(command.projectAfter);
      await command.undo(command.after);
      this.redoItems.pop(); this.undoItems.push(command); this.trim();
    } catch (error) {
      command.restoreProject?.(command.projectBefore);
      await command.undo(command.before);
      throw error;
    } finally { this.busy = false; this.emit(); }
  }
  trim() {
    while (this.undoItems.length > this.maxCommands || estimateHistoryBytes(this.undoItems) > this.maxBytes) this.undoItems.shift();
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
    undo: (source) => session.restore(source),
    snapshotProject: project ? (cache) => ({ assets: cloneAssetMap(project.assets, cache), exportRemap: { ...project.exportRemap } }) : null,
    restoreProject: project ? (state) => { if (!state) return; project.assets = cloneAssetMap(state.assets); project.exportRemap = { ...state.exportRemap }; project.emit(); } : null,
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

function snapshotBuffer(buffer, cache) {
  const previous = cache?.get(buffer);
  if (previous && previous.byteLength === buffer.byteLength) {
    // Compare words without a callback per byte. In-place modifications get a
    // new snapshot, so sharing never changes an earlier undo state.
    const count = Math.floor(buffer.byteLength / 4);
    const a = new Uint32Array(buffer, 0, count), b = new Uint32Array(previous, 0, count);
    let equal = true;
    for (let i = 0; i < count; i++) if (a[i] !== b[i]) { equal = false; break; }
    if (equal) {
      const aa = new Uint8Array(buffer), bb = new Uint8Array(previous);
      for (let i = count * 4; i < aa.length; i++) if (aa[i] !== bb[i]) { equal = false; break; }
    }
    if (equal) return previous;
  }
  const copy = buffer.slice(0);
  cache?.set(buffer, copy);
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
  return new Map([...assets || []].map(([path, asset]) => [path, cloneValue(asset, seen, cache)]));
}
