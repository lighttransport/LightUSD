import { LuciaError } from './utils.js';

export function createLayerPropertyDelta(before, after) {
  const changes = [];
  const walk = (left, right, path) => {
    if (JSON.stringify(left) === JSON.stringify(right)) return true;
    if (path.at(-1) === 'properties') {
      for (const name of new Set([...Object.keys(left || {}), ...Object.keys(right || {})])) {
        if (JSON.stringify(left?.[name]) !== JSON.stringify(right?.[name]))
          changes.push({ path: [...path, name], before: structuredClone(left?.[name]), after: structuredClone(right?.[name]) });
      }
      return changes.length <= 256;
    }
    if (!left || !right || typeof left !== 'object' || typeof right !== 'object' || Array.isArray(left) || Array.isArray(right)) return false;
    return [...new Set([...Object.keys(left), ...Object.keys(right)])].every(key => walk(left[key], right[key], [...path, key]));
  };
  return walk(before, after, []) && changes.length ? { changes } : null;
}
export function applyLayerPropertyDelta(layer, delta, direction) {
  if (!['undo', 'redo'].includes(direction) || !Array.isArray(delta?.changes) || delta.changes.length > 256)
    throw new LuciaError('LUCIA_HISTORY', 'Invalid property delta.');
  const result = structuredClone(layer), undo = direction === 'undo';
  for (const change of delta.changes) {
    const { path } = change;
    if (!Array.isArray(path) || path.length < 3 || path.at(-2) !== 'properties' || path.some(key => typeof key !== 'string' || ['__proto__', 'prototype', 'constructor'].includes(key)))
      throw new LuciaError('LUCIA_HISTORY', 'Invalid property path.');
    let parent = result;
    for (const key of path.slice(0, -1)) {
      if (!Object.hasOwn(parent, key)) {
        if (key !== 'properties') throw new LuciaError('LUCIA_HISTORY_STALE', 'The property owner no longer exists.');
        parent[key] = {};
      }
      parent = parent[key];
    }
    const key = path.at(-1), expected = undo ? change.after : change.before;
    if (JSON.stringify(parent[key]) !== JSON.stringify(expected)) throw new LuciaError('LUCIA_HISTORY_STALE', 'The property differs from its undo record.');
    const value = undo ? change.before : change.after;
    if (value === undefined) delete parent[key]; else parent[key] = structuredClone(value);
  }
  return result;
}
