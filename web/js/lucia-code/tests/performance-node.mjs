// Reproducible workloads; report timings without machine-specific thresholds.
import assert from 'node:assert/strict';
import {PNG} from 'pngjs';
import {LuciaCommandStack,sessionCommand,estimateHistoryBytes} from '../src/command-stack.js';
import factory from '../../src/lightusd/lightusd_next.js';
import {bakeUDIMAtlas} from '../../src/udim-bake.js';
if(!globalThis.CustomEvent)globalThis.CustomEvent=class extends Event{};

const project={assets:new Map([['large.png',{bytes:new Uint8Array(32*1048576)}]]),exportRemap:{},emit(){}},
  source='#usda 1.0\n'+' '.repeat(1048576),session={exportUSDA:()=>source,restore(){}},
  stack=new LuciaCommandStack({maxBytes:128*1048576});
globalThis.gc?.();
const memoryBefore=process.memoryUsage().arrayBuffers,start=performance.now();
for(let i=0;i<4;i++)await stack.execute(sessionCommand(session,'Scene-only edit',[],async()=>{},project));
globalThis.gc?.();
const memoryAfter=process.memoryUsage().arrayBuffers;
assert.equal(stack.undoItems.length,4);
const snapshots=new Set(stack.undoItems.flatMap(c=>[c.projectBefore.assets.get('large.png').bytes.buffer,c.projectAfter.assets.get('large.png').bytes.buffer]));
assert.equal(snapshots.size,1);
console.log(JSON.stringify({workload:'four scene edits, 32 MiB unchanged asset',elapsedMs:performance.now()-start,
  retainedArrayBufferMiB:(memoryAfter-memoryBefore)/1048576,historyMiB:estimateHistoryBytes(stack.undoItems)/1048576}));

const native=await factory(),tile=new PNG({width:1024,height:1024});
for(let i=0;i<tile.data.length;i+=4)tile.data.set([128,64,32,255],i);
const encoded=new Uint8Array(PNG.sync.write(tile)),keys=['tile.1001.png','tile.1002.png'];
let fetches=0,fetchedBytes=0;
const tileSource={keys,fetch:async()=>{fetches++;fetchedBytes+=encoded.byteLength;return encoded.slice();}};
const bakeStart=performance.now();
const atlas=await bakeUDIMAtlas(native,tileSource,'tile.<UDIM>.png',{udimBake:'grid',udimThumbnails:true,udimMemoryBudgetBytes:256*1048576});
assert.equal(fetches,4);assert.equal(atlas.layout.width,2048);assert.equal(atlas.layout.height,1024);
assert.ok(atlas.thumbnail?.length);assert.ok(PNG.sync.read(Buffer.from(atlas.thumbnail)).width<=256);
console.log(JSON.stringify({workload:'two 1024px tiles, bounded atlas and thumbnail',elapsedMs:performance.now()-bakeStart,
  fetches,fetchedBytes,atlasBytes:atlas.data.length,thumbnailBytes:atlas.thumbnail.length,wasmHeapMiB:native.HEAPU8.byteLength/1048576}));
{
  // Sculpt dab budget: ~100k-vertex mesh, interactive strokes must stay responsive.
  const { SculptMesh } = await import('../src/sculpt/sculpt-mesh.js');
  const { SculptStroke } = await import('../src/sculpt/brushes.js');
  const { getNodeType } = await import('../src/geonodes/index.js');
  const mesh = getNodeType('MeshGrid').evaluate({ sizeX: 2, sizeY: 2, verticesX: 317, verticesY: 317 }).mesh.mesh;
  const buildStart = performance.now(), sculptMesh = new SculptMesh(mesh), buildMs = performance.now() - buildStart;
  const stroke = new SculptStroke(sculptMesh, { brush: 'draw', radius: 0.1, strength: 0.5, symmetry: [true, false, false] });
  const dabStart = performance.now();
  for (let i = 0; i < 100; i++) stroke.dab([-0.5 + i * 0.01, 0, 0]);
  const dabMs = (performance.now() - dabStart) / 100;
  assert.ok(sculptMesh.changed());
  console.log(JSON.stringify({ workload: 'sculpt 100k-vertex grid, mirrored draw dabs', buildMs, perDabMs: dabMs }));
}
