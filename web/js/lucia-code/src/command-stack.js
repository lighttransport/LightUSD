export class LuciaCommandStack extends EventTarget {
  constructor({ maxCommands = 50, maxBytes = 128 * 1024 * 1024 } = {}) {
    super(); this.undoItems = []; this.redoItems = []; this.maxCommands = maxCommands; this.maxBytes = maxBytes; this.busy = false;
  }
  get canUndo() { return this.undoItems.length > 0 && !this.busy; }
  get canRedo() { return this.redoItems.length > 0 && !this.busy; }
  async execute(command) {
    if (this.busy) return false;
    this.busy = true;
    try {
      command.projectBefore = command.snapshotProject?.();
      command.before = await command.do();
      command.projectAfter = command.snapshotProject?.();
      command.estimatedBytes = new Blob([command.before || '']).size;
      this.undoItems.push(command); this.redoItems.length = 0; this.trim();
    } catch (error) {
      // A command may have partially authored the native stage or project
      // assets before reporting an error. Restore both snapshots before the
      // error reaches the UI so failed operations are observationally atomic.
      try { if (command.before != null) await command.undo(command.before); } catch { /* preserve the original operation error */ }
      command.restoreProject?.(command.projectBefore);
      throw error;
    } finally { this.busy = false; this.emit(); }
    return true;
  }
  async undo() {
    const command = this.undoItems.pop(); if (!command || this.busy) return;
    this.busy = true;
    try { command.after = await command.snapshot(); await command.undo(command.before); command.restoreProject?.(command.projectBefore); this.redoItems.push(command); }
    finally { this.busy = false; this.emit(); }
  }
  async redo() {
    const command = this.redoItems.pop(); if (!command || this.busy) return;
    this.busy = true;
    try { await command.undo(command.after); command.restoreProject?.(command.projectAfter); this.undoItems.push(command); }
    finally { this.busy = false; this.emit(); }
  }
  trim() {
    let bytes = this.undoItems.reduce((n, c) => n + (c.estimatedBytes || 0), 0);
    while (this.undoItems.length > this.maxCommands || bytes > this.maxBytes) bytes -= this.undoItems.shift()?.estimatedBytes || 0;
  }
  clear() { this.undoItems.length = this.redoItems.length = 0; this.emit(); }
  emit() { this.dispatchEvent(new CustomEvent('change')); }
}

export function sessionCommand(session, summary, affectedPaths, operation, project = null) {
  return {
    summary, affectedPaths,
    do: async () => {
      const before = await session.exportUSDA();
      try {
        await operation();
      } catch (error) {
        try { await session.restore(before); } catch { /* preserve the original operation error */ }
        throw error;
      }
      return before;
    },
    snapshot: async () => session.exportUSDA(),
    undo: (source) => session.restore(source),
    snapshotProject: project ? () => ({ assets: cloneAssetMap(project.assets), exportRemap: { ...project.exportRemap } }) : null,
    restoreProject: project ? (state) => { if (!state) return; project.assets = cloneAssetMap(state.assets); project.exportRemap = { ...state.exportRemap }; project.emit(); } : null,
  };
}

function cloneValue(value, seen = new WeakMap()) {
  if (value == null || typeof value !== 'object') return value;
  if (ArrayBuffer.isView(value)) return new value.constructor(value);
  if (value instanceof ArrayBuffer) return value.slice(0);
  if (seen.has(value)) return seen.get(value);
  if (Array.isArray(value)) { const copy = []; seen.set(value, copy); value.forEach((item) => copy.push(cloneValue(item, seen))); return copy; }
  const prototype = Object.getPrototypeOf(value);
  if (prototype !== Object.prototype && prototype !== null) return value;
  const copy = {}; seen.set(value, copy);
  for (const [key, item] of Object.entries(value)) copy[key] = cloneValue(item, seen);
  return copy;
}

function cloneAssetMap(assets) {
  return new Map([...assets || []].map(([path, asset]) => [path, cloneValue(asset)]));
}
