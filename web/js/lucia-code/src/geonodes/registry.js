import { LuciaError } from '../utils.js';

// Socket types: geometry, float, int, bool, vector, enum. float/vector
// inputs flagged `field: true` also accept per-element fields.
export const SOCKET_TYPES = new Set(['geometry', 'float', 'int', 'bool', 'vector', 'enum']);
const nodeTypes = new Map();

export function registerNode(type, definition) {
  if (!/^[A-Za-z][A-Za-z0-9]*$/.test(String(type))) throw new LuciaError('LUCIA_GEONODES_REGISTRY', `Invalid node type name: ${type}`);
  if (nodeTypes.has(type)) throw new LuciaError('LUCIA_GEONODES_REGISTRY', `Node type already registered: ${type}`);
  const { label = type, category = 'Utility', inputs = [], outputs = [], evaluate } = definition || {};
  if (typeof evaluate !== 'function') throw new LuciaError('LUCIA_GEONODES_REGISTRY', `Node ${type} needs an evaluate function.`);
  for (const socket of [...inputs, ...outputs]) if (!socket?.name || !SOCKET_TYPES.has(socket.type)) throw new LuciaError('LUCIA_GEONODES_REGISTRY', `Node ${type} has an invalid socket.`);
  nodeTypes.set(type, Object.freeze({ type, label, category, inputs, outputs, evaluate }));
}

export function getNodeType(type) {
  return nodeTypes.get(type) || null;
}

export function listNodeTypes() {
  return [...nodeTypes.values()].map(({ type, label, category, inputs, outputs }) => ({ type, label, category, inputs, outputs }));
}
