// SPDX-License-Identifier: Apache-2.0
// The MCP server product (lightusd_mcp WASM module): tool surface parity with
// the legacy LightUSDLoaderNative MCP methods, end-to-end tool behavior, the
// JSON-RPC transport, and hardening against malformed arguments.
import assert from 'node:assert/strict';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {loadWasm} from '../src/usdzconvert.js';

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const {default: createLightUSDMCP} = await import(new URL(
  wasm64 ? '../src/lightusd/lightusd_mcp_64.js' : '../src/lightusd/lightusd_mcp.js',
  import.meta.url));
const mcp = await createLightUSDMCP();

const b64 = (text) => Buffer.from(text, 'utf8').toString('base64');
const server = new mcp.LightUSDMCPServer();
assert.equal(server.createContext('s'), true);
assert.equal(server.createContext('s'), false);
const call = (tool, args = {}) => JSON.parse(server.toolsCall(tool, args));
const ok = (tool, args) => {
  const r = call(tool, args);
  assert.notEqual(r.isError, true, `${tool}: ${JSON.stringify(r)}`);
  return r;
};

// Tool surface: the legacy tool list minus tools the next product does not
// provide (scripting engine, image processing, an unimplemented stub).
const nextTools = JSON.parse(server.toolsList()).tools.map((t) => t.name).sort();
const combinedUrl = process.env.LIGHTUSD_COMBINED_MODULE
  ? pathToFileURL(path.resolve(process.env.LIGHTUSD_COMBINED_MODULE)).href
  : new URL(wasm64 ? '../src/lightusd/lightusd_combined_64.js'
                   : '../src/lightusd/lightusd_combined.js', import.meta.url).href;
const legacy = await loadWasm(() => import(combinedUrl));
const legacyLoader = new legacy.LightUSDLoaderNative();
try {
  assert.equal(legacyLoader.mcpCreateContext('s'), true);
  const excluded = new Set(['run_script', 'texture_resize', 'texture_repack',
    'load_usd_layer_from_asset']);
  const legacyTools = JSON.parse(legacyLoader.mcpToolsList()).tools
    .map((t) => t.name).filter((n) => !excluded.has(n)).sort();
  assert.deepEqual(nextTools, legacyTools, 'next MCP tool list = legacy minus exclusions');
} finally { legacyLoader.delete(); }

// Stage authoring and queries.
ok('stage_new', {upAxis: 'Z', metersPerUnit: 1});
ok('prim_create', {path: '/World', type_name: 'Xform'});
ok('prim_create', {path: '/World/Mesh', type_name: 'Mesh'});
ok('attr_set', {path: '/World/Mesh', attr_name: 'points',
  value: {type: 'point3f[]', value: [[0, 0, 0], [1, 0, 0], [0, 1, 0]]}});
let r = ok('attr_get', {path: '/World/Mesh', attr_name: 'points'});
assert.deepEqual(r.value.value, [[0, 0, 0], [1, 0, 0], [0, 1, 0]]);
assert.equal(ok('stage_info').upAxis, 'Z');
assert.deepEqual(ok('query_prims_by_type', {type_name: 'Mesh'}).paths, ['/World/Mesh']);
assert.equal(ok('prim_rename', {path: '/World/Mesh', new_name: 'Body'}).path, '/World/Body');
const usda = ok('stage_to_string').usda;
assert.match(usda, /def Mesh "Body"/);
assert.equal(ok('usd_validate').source, 'stage');
const usdz = Buffer.from(ok('usdz_pack').data, 'base64');
assert.equal(usdz.subarray(0, 2).toString(), 'PK');

// File URIs are unavailable in the browser/Node module.
assert.equal(call('stage_load', {uri: 'a.usda'}).isError, true);

// Layers and diff.
const uuid = ok('load_usd_layer_from_data', {name: 'L', data: b64(usda)}).content[0].text;
assert.equal(uuid.length, 36);
r = ok('diff_open', {left: {uuid}, right: {data: b64(usda.replace('Body', 'Other'))}});
assert.ok(ok('diff_text').text.length > 0);

// Malformed arguments are tool errors, and the server keeps working.
assert.equal(call('prim_list', {max_depth: 'deep'}).isError, true);
assert.equal(call('store_asset', {name: 5, data: 'aGk='}).isError, true);
assert.equal(JSON.parse(server.toolsCall('prim_list', '{broken')).error, 'Invalid JSON');
assert.equal(ok('stage_info').upAxis, 'Z');

// JSON-RPC transport.
const rpc = (m) => JSON.parse(server.handleJsonRpc(m));
assert.equal(rpc({jsonrpc: '2.0', id: 1, method: 'initialize', params: {}})
  .result.serverInfo.name, 'lightusd-mcp');
assert.equal(rpc({jsonrpc: '2.0', id: 2, method: 'tools/list'}).result.tools.length,
  nextTools.length);
assert.equal(rpc({jsonrpc: '2.0', id: 3, method: 'tools/call',
  params: {name: 'stage_info', arguments: {}}}).result.structuredContent.upAxis, 'Z');
assert.equal(server.handleJsonRpc({jsonrpc: '2.0', method: 'notifications/initialized'}), '');

// Legacy-named module functions on the default server.
assert.equal(mcp.mcpCreateContext('legacy'), true);
assert.equal(mcp.mcpSelectContext('legacy'), true);
assert.equal(JSON.parse(mcp.mcpToolsCall('stage_new', '{}')).success, true);
assert.ok(JSON.parse(mcp.mcpToolsList()).tools.length === nextTools.length);
assert.deepEqual(JSON.parse(mcp.mcpResourcesList()).resources, []);

server.delete();
assert.throws(() => server.toolsList(), /deleted server/);
console.log(`ok - next MCP server module (${wasm64 ? 'memory64' : 'wasm32'}): ` +
  `${nextTools.length} tools, legacy tool-list parity, JSON-RPC, malformed-argument hardening`);
