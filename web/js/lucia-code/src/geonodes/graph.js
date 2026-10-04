import { LuciaError } from '../utils.js';
import { getNodeType } from './nodes.js';

export const GEONODES_GRAPH_VERSION = 1;
export const GEONODES_MAX_NODES = 512;
const NUMERIC = new Set(['float', 'int', 'bool']);
const fail = (message, details) => { throw new LuciaError('LUCIA_GEONODES_GRAPH', message, details); };

export function socketsCompatible(from, to) {
  if (from === 'geometry' || to === 'geometry') return from === to;
  if (from === 'enum' || to === 'enum') return false;
  if (to === 'vector') return from === 'vector' || NUMERIC.has(from);
  return NUMERIC.has(from) && NUMERIC.has(to);
}

// A graph whose GroupInput feeds GroupOutput unchanged: the starting point
// for a new modifier on a mesh.
export function createDefaultGraph() {
  return {
    version: GEONODES_GRAPH_VERSION,
    nodes: [{ id: 'input', type: 'GroupInput', params: {}, position: [0, 0] }, { id: 'output', type: 'GroupOutput', params: {}, position: [320, 0] }],
    links: [{ from: ['input', 'geometry'], to: ['output', 'geometry'] }],
  };
}

// Validates structure, node types, sockets and acyclicity; returns a deep,
// normalized copy (unknown keys dropped) so the graph can be stored safely.
export function validateGraph(graph) {
  if (!graph || typeof graph !== 'object' || Array.isArray(graph)) fail('Geometry node graph must be an object.');
  if (graph.version !== GEONODES_GRAPH_VERSION) fail(`Unsupported geometry node graph version: ${graph.version}`);
  if (!Array.isArray(graph.nodes) || !Array.isArray(graph.links)) fail('Geometry node graph needs nodes and links arrays.');
  if (graph.nodes.length > GEONODES_MAX_NODES) fail(`Geometry node graph exceeds ${GEONODES_MAX_NODES} nodes.`);
  const nodes = new Map();
  for (const node of graph.nodes) {
    if (!node || typeof node.id !== 'string' || !/^[A-Za-z0-9_-]{1,64}$/.test(node.id)) fail('Every node needs a short alphanumeric id.');
    if (nodes.has(node.id)) fail(`Duplicate node id: ${node.id}`);
    const definition = getNodeType(node.type);
    if (!definition) fail(`Unknown node type: ${node.type}`, { id: node.id });
    const params = {};
    for (const [name, value] of Object.entries(node.params || {})) {
      const socket = definition.inputs.find((input) => input.name === name);
      if (!socket || socket.type === 'geometry') fail(`Node ${node.id} has an unknown parameter: ${name}`);
      params[name] = normalizeParam(socket, value, node.id);
    }
    const position = Array.isArray(node.position) && node.position.length === 2 && node.position.every(Number.isFinite) ? [...node.position] : [0, 0];
    nodes.set(node.id, { id: node.id, type: node.type, params, position });
  }
  const outputs = [...nodes.values()].filter((node) => node.type === 'GroupOutput');
  if (outputs.length !== 1) fail('Geometry node graph needs exactly one Group Output.');
  const links = [], linked = new Set();
  for (const link of graph.links) {
    const [fromId, fromSocket] = Array.isArray(link?.from) ? link.from : [], [toId, toSocket] = Array.isArray(link?.to) ? link.to : [];
    const from = nodes.get(fromId), to = nodes.get(toId);
    if (!from || !to) fail('Link references a missing node.', { link });
    const output = getNodeType(from.type).outputs.find((socket) => socket.name === fromSocket), input = getNodeType(to.type).inputs.find((socket) => socket.name === toSocket);
    if (!output || !input) fail(`Link references a missing socket: ${fromId}.${fromSocket} → ${toId}.${toSocket}`);
    if (!socketsCompatible(output.type, input.type)) fail(`Cannot connect ${output.type} to ${input.type} (${fromId}.${fromSocket} → ${toId}.${toSocket}).`);
    const key = `${toId}.${toSocket}`;
    if (linked.has(key)) fail(`Input ${key} has more than one link.`);
    linked.add(key);
    links.push({ from: [fromId, fromSocket], to: [toId, toSocket] });
  }
  const normalized = { version: GEONODES_GRAPH_VERSION, nodes: [...nodes.values()], links };
  topoOrder(normalized);
  return normalized;
}

function normalizeParam(socket, value, id) {
  const bad = () => fail(`Node ${id} parameter ${socket.name} has an invalid ${socket.type} value.`);
  switch (socket.type) {
    case 'float': case 'int': { const n = Number(value); if (!Number.isFinite(n)) bad(); return socket.type === 'int' ? Math.round(n) : n; }
    case 'bool': if (typeof value !== 'boolean') bad(); return value;
    case 'vector': if (!Array.isArray(value) || value.length !== 3 || !value.every(Number.isFinite)) bad(); return value.map(Number);
    case 'enum': if (!socket.options.includes(value)) bad(); return value;
    default: return bad();
  }
}

// Kahn ordering restricted to nodes that reach the Group Output; unreachable
// nodes are skipped (like Blender, they are not evaluated).
export function topoOrder(graph) {
  const incoming = new Map(graph.nodes.map((node) => [node.id, []]));
  for (const link of graph.links) incoming.get(link.to[0]).push(link.from[0]);
  const output = graph.nodes.find((node) => node.type === 'GroupOutput');
  const reachable = new Set(), stack = [output.id];
  while (stack.length) { const id = stack.pop(); if (reachable.has(id)) continue; reachable.add(id); stack.push(...incoming.get(id)); }
  const pending = new Map([...reachable].map((id) => [id, new Set(incoming.get(id))])), order = [];
  const ready = [...reachable].filter((id) => !pending.get(id).size).sort();
  while (ready.length) {
    const id = ready.shift(); order.push(id);
    for (const [other, deps] of pending) if (deps.delete(id) && !deps.size) { ready.push(other); ready.sort(); }
  }
  if (order.length !== reachable.size) fail('Geometry node graph contains a cycle.');
  return order;
}

export function serializeGraph(graph) {
  return JSON.stringify(validateGraph(graph));
}

export function parseGraph(text) {
  let graph;
  try { graph = JSON.parse(String(text)); } catch { fail('Stored geometry node graph is not valid JSON.'); }
  return validateGraph(graph);
}
