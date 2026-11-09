// SPDX-License-Identifier: Apache-2.0
// Raw C-boundary contracts, including callers that bypass the JS wrappers.
import assert from 'node:assert/strict';

const wasm64 = process.env.LIGHTUSD_WASM64 === '1';
const {default: createModule} = await import(new URL(
  wasm64 ? '../src/lightusd/lightusd_next_64.js' : '../src/lightusd/lightusd_next.js',
  import.meta.url));
const module = await createModule();
const nil = wasm64 ? 0n : 0;
const encoder = new TextEncoder();
const decoder = new TextDecoder();
const allocations = [];
const allocate = bytes => {
  const ptr = module._lightusd_next_alloc(bytes.length);
  assert.notEqual(ptr, nil);
  allocations.push(ptr);
  module.HEAPU8.set(bytes, Number(ptr));
  return ptr;
};
const name = text => {
  const bytes = encoder.encode(text);
  return [allocate(bytes), bytes.length];
};
const stream = module._lightusd_next_create(4);
const wrongKind = module._lightusd_next_create(1);
assert.ok(stream && wrongKind);
try {
  const scene = encoder.encode(`#usda 1.0
(
    defaultPrim = "World_雪"
    comment = "copied ✓"
)
def Xform "World_雪" {
    def Sphere "Ball" {}
}
`);
  assert.equal(module._lightusd_next_render_begin(stream, allocate(scene), scene.length), 1);
  const output = allocate(new Uint8Array(128));
  const outputBytes = () => module.HEAPU8.subarray(Number(output), Number(output) + 128);
  const checkCopy = (copy, expected) => {
    outputBytes().fill(0xa5);
    const required = copy(nil, 0);
    assert.ok(required > 0 && required < 128);
    assert.equal(copy(output, required - 1), required);
    assert.ok(outputBytes().every(byte => byte === 0xa5), 'short copies leave output untouched');
    assert.equal(copy(output, required), required);
    if (expected !== undefined) assert.equal(decoder.decode(outputBytes().subarray(0, required)), expected);
    assert.ok(outputBytes().subarray(required).every(byte => byte === 0xa5), 'copies exclude the NUL');
  };
  checkCopy((out, cap) => module._lightusd_next_render_node_path(stream, 0, out, cap), '/World_雪');
  // Scene strings retain their distinct truncating, NUL-terminated protocol.
  const commentBytes = encoder.encode('copied ✓');
  assert.equal(module._lightusd_next_render_scene_string(stream, 5, nil, 0), commentBytes.length);
  outputBytes().fill(0xa5);
  assert.equal(module._lightusd_next_render_scene_string(stream, 5, output, 4), commentBytes.length);
  assert.deepEqual(Array.from(outputBytes().subarray(0, 4)), [...encoder.encode('cop'), 0]);
  assert.ok(outputBytes().subarray(4).every(byte => byte === 0xa5));
  outputBytes().fill(0xa5);
  assert.equal(module._lightusd_next_render_scene_string(stream, 5, output, commentBytes.length + 1), commentBytes.length);
  assert.deepEqual(outputBytes().subarray(0, commentBytes.length), commentBytes);
  assert.equal(outputBytes()[commentBytes.length], 0);
  assert.ok(outputBytes().subarray(commentBytes.length + 1).every(byte => byte === 0xa5));
  outputBytes().fill(0xa5);
  assert.equal(module._lightusd_next_render_scene_string(stream, 2, nil, 0), 0);
  assert.equal(module._lightusd_next_render_scene_string(stream, 2, output, 128), 0);
  assert.equal(module._lightusd_next_render_node_path(stream, -1, output, 128), -1);
  assert.equal(module._lightusd_next_render_node_path(wrongKind, 0, output, 128), -1);
  assert.ok(outputBytes().every(byte => byte === 0xa5));

  const authored = name('././cache\\folder\\asset.usda');
  const canonical = name('cache/folder/asset.usda');
  const relative = name('./cache/folder/asset.usda');
  const payload = encoder.encode('abc');
  assert.equal(module._lightusd_next_render_stream_asset_start(stream, ...authored, payload.length), 0);
  assert.equal(module._lightusd_next_render_stream_asset_size(stream, ...canonical), payload.length);
  assert.equal(module._lightusd_next_render_stream_asset_append(stream, ...relative, allocate(payload), payload.length), 1);
  assert.equal(module._lightusd_next_render_stream_asset_progress(stream, ...authored), payload.length);
  assert.equal(module._lightusd_next_render_stream_asset_cancel(stream, ...canonical), 1);
  assert.equal(module._lightusd_next_render_stream_asset_size(stream, ...relative), -1);
} finally {
  module._lightusd_next_destroy(stream);
  module._lightusd_next_destroy(wrongKind);
  for (const ptr of allocations) module._lightusd_next_free(ptr);
}
console.log(`ok - bounded string copies and normalized streaming names (${wasm64 ? 'memory64' : 'wasm32'})`);
