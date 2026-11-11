// udim — UDIM textures in the Three.js path (LightUSD WASM)
//
// WebGL has no native UDIM. This demo loads a UDIM USD with the tiles passed
// through unchanged (LightUSD "keep-as-is" / sparse mode,
// setCombineUDIMTiles(false)), packs the resolved tiles into a single
// THREE.DataArrayTexture (one layer per tile, the same idea as packing skin
// data into a lookup texture), and a custom GLSL3 ShaderMaterial remaps each
// fragment's multi-tile UV to the right array layer:
//
//   udim = 1001 + floor(u) + 10*floor(v);  layer = LUT[udim];  color = tiles[layer](fract(uv))
//
// A "combine" mode is provided for comparison: it lets the core build a single
// atlas and rebake the mesh UVs (the default lusdzconvert/WebGL path), so the
// two modes should render identically.

import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';

import { loadWasm, rootUsdFromMap, isUsdName, isImageName, buildUSDZWithNewRoot } from './src/usdzconvert.js';
import { meshCopyToGeometry } from './src/gl-upload.js';

import { getCoreInspectorMesh } from './src/lightusd/CoreRenderInspector.js';

const WASM_GLUE = new URL(import.meta.env.PROD ? './assets/lightusd.js' : './src/lightusd/lightusd.js', document.baseURI).href;

// Sample 2x2 UDIM set. 1012 is authored smaller to exercise the resize path.
const SAMPLE_TILES = [
  { udim: 1001, color: '#c0392b', size: 512 }, // u=0,v=0 (bottom-left)
  { udim: 1002, color: '#27ae60', size: 512 }, // u=1,v=0 (bottom-right)
  { udim: 1011, color: '#2980b9', size: 512 }, // u=0,v=1 (top-left)
  { udim: 1012, color: '#f39c12', size: 256 }, // u=1,v=1 (top-right)
];

const MAX_CELL_SIZE = 1024;
// USD texcoords have origin at bottom-left; the array texture has no flipY GPU
// step, so the per-tile rows are flipped in JS to match.
const FLIP_TILES = true;

// ---------------------------------------------------------------------------
// DOM
// ---------------------------------------------------------------------------

const els = {
  view: document.getElementById('view'),
  panel: document.getElementById('panel'),
  drop: document.getElementById('drop'),
  filesInput: document.getElementById('filesInput'),
  btnSample: document.getElementById('btnSample'),
  btnExport: document.getElementById('btnExport'),
  info: document.getElementById('info'),
  log: document.getElementById('log'),
};

function log(msg) { els.log.textContent = msg; }
function setInfo(text) { els.info.textContent = text; }

// ---------------------------------------------------------------------------
// Three.js scene
// ---------------------------------------------------------------------------

let renderer, scene, camera, controls;
let currentGroup = null; // the mesh group currently shown

function initScene() {
  scene = new THREE.Scene();
  scene.background = new THREE.Color(0x15171c);

  const w = window.innerWidth, h = window.innerHeight;
  camera = new THREE.PerspectiveCamera(45, w / h, 0.01, 1000);
  camera.position.set(0, 0, 4);

  renderer = new THREE.WebGLRenderer({ antialias: true });
  renderer.setSize(w, h);
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  els.view.appendChild(renderer.domElement);

  controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true;
  controls.dampingFactor = 0.06;

  scene.add(new THREE.AmbientLight(0xffffff, 0.5));
  const dir = new THREE.DirectionalLight(0xffffff, 1.0);
  dir.position.set(0.4, 0.8, 0.6);
  scene.add(dir);

  window.addEventListener('resize', () => {
    const ww = window.innerWidth, hh = window.innerHeight;
    camera.aspect = ww / hh;
    camera.updateProjectionMatrix();
    renderer.setSize(ww, hh);
  });

  (function animate() {
    requestAnimationFrame(animate);
    controls.update();
    renderer.render(scene, camera);
  })();
}

// Dispose and remove whatever is currently displayed.
function clearGroup() {
  if (!currentGroup) return;
  scene.remove(currentGroup);
  const gl = renderer.getContext();
  currentGroup.traverse((o) => {
    // GLBufferAttribute buffers are owned by us, not THREE — delete explicitly.
    if (o.userData && o.userData.glBuffers) {
      for (const b of o.userData.glBuffers) gl.deleteBuffer(b);
    }
    if (o.geometry) o.geometry.dispose();
    if (o.material) {
      const mats = Array.isArray(o.material) ? o.material : [o.material];
      for (const m of mats) {
        for (const k of Object.keys(m)) {
          const v = m[k];
          if (v && v.isTexture) v.dispose();
        }
        m.dispose();
      }
    }
  });
  currentGroup = null;
}

// Frame a group in the camera.
function frameGroup(group) {
  const box = new THREE.Box3().setFromObject(group);
  if (box.isEmpty()) return;
  const center = box.getCenter(new THREE.Vector3());
  const size = box.getSize(new THREE.Vector3()).length() || 1;
  group.position.sub(center); // recenter at origin
  controls.target.set(0, 0, 0);
  camera.position.set(0, 0, size * 1.4);
  camera.near = size / 100;
  camera.far = size * 100;
  camera.updateProjectionMatrix();
  controls.update();
}

// ---------------------------------------------------------------------------
// WASM
// ---------------------------------------------------------------------------

let native = null;
async function ensureWasm() {
  if (!native) {
    log('Loading WASM…');
    native = await loadWasm(() => import(/* @vite-ignore */ WASM_GLUE));
  }
  return native;
}

// ---------------------------------------------------------------------------
// Sample scene (canvas tiles + USDA)
// ---------------------------------------------------------------------------

function encodePng(canvas) {
  return new Promise((resolve, reject) => {
    canvas.toBlob((blob) => {
      if (!blob) { reject(new Error('toBlob failed')); return; }
      blob.arrayBuffer().then((buf) => resolve(new Uint8Array(buf)), reject);
    }, 'image/png');
  });
}

async function makeTilePng(udim, color, size) {
  const u = (udim - 1001) % 10;
  const v = Math.floor((udim - 1001) / 10);
  const c = document.createElement('canvas');
  c.width = c.height = size;
  const ctx = c.getContext('2d');
  ctx.fillStyle = color;
  ctx.fillRect(0, 0, size, size);
  ctx.strokeStyle = 'rgba(255,255,255,0.9)';
  ctx.lineWidth = Math.max(2, size * 0.02);
  ctx.strokeRect(ctx.lineWidth, ctx.lineWidth, size - 2 * ctx.lineWidth, size - 2 * ctx.lineWidth);
  ctx.fillStyle = '#fff';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.font = `bold ${Math.round(size * 0.22)}px system-ui, sans-serif`;
  ctx.fillText(String(udim), size / 2, size * 0.42);
  ctx.font = `${Math.round(size * 0.1)}px system-ui, sans-serif`;
  ctx.fillText(`u=${u} v=${v}`, size / 2, size * 0.62);
  return await encodePng(c);
}

const SAMPLE_USDA = `#usda 1.0
(
    defaultPrim = "root"
    upAxis = "Y"
    metersPerUnit = 1
)

def Xform "root"
{
    def Mesh "udimQuad"
    {
        int[] faceVertexCounts = [4, 4, 4, 4]
        int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4, 3, 4, 7, 6, 4, 5, 8, 7]
        point3f[] points = [
            (-1, -1, 0), (0, -1, 0), (1, -1, 0),
            (-1, 0, 0), (0, 0, 0), (1, 0, 0),
            (-1, 1, 0), (0, 1, 0), (1, 1, 0)
        ]
        normal3f[] normals = [
            (0, 0, 1), (0, 0, 1), (0, 0, 1),
            (0, 0, 1), (0, 0, 1), (0, 0, 1),
            (0, 0, 1), (0, 0, 1), (0, 0, 1)
        ] (interpolation = "vertex")
        texCoord2f[] primvars:st = [
            (0, 0), (1, 0), (2, 0),
            (0, 1), (1, 1), (2, 1),
            (0, 2), (1, 2), (2, 2)
        ] (interpolation = "vertex")
        rel material:binding = </root/udimMat>
    }

    def Material "udimMat"
    {
        token outputs:surface.connect = </root/udimMat/Surface.outputs:surface>

        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </root/udimMat/DiffTex.outputs:rgb>
            float inputs:metallic = 0
            float inputs:roughness = 0.8
            token outputs:surface
        }

        def Shader "DiffTex"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @tex.<UDIM>.png@
            float2 inputs:st.connect = </root/udimMat/StReader.outputs:result>
            token inputs:wrapS = "clamp"
            token inputs:wrapT = "clamp"
            float3 outputs:rgb
        }

        def Shader "StReader"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            token inputs:varname = "st"
            float2 outputs:result
        }
    }
}
`;

async function buildSampleAssets() {
  const assetMap = new Map();
  assetMap.set('udim.usda', new TextEncoder().encode(SAMPLE_USDA));
  for (const t of SAMPLE_TILES) {
    assetMap.set(`tex.${t.udim}.png`, await makeTilePng(t.udim, t.color, t.size));
  }
  return { assetMap, rootPath: 'udim.usda' };
}

// ---------------------------------------------------------------------------
// Loading helpers
// ---------------------------------------------------------------------------

// USD-relative asset name for an uploaded path (relative to the root USD dir).
function assetNameFor(path, rootDir) {
  return (rootDir && path.startsWith(rootDir)) ? path.slice(rootDir.length) : path;
}

// Import the complete asset store so core composition and tile resolution share
// the root layer's directory, including nested uploaded folders.
function loadUsd(assetMap, rootPath, combine) {
  const usd = new native.RenderStream();
  const store = new native.AssetStore();
  const rootDir = rootPath.includes('/') ? rootPath.slice(0, rootPath.lastIndexOf('/') + 1) : '';
  try {
    store.setBaseWorkingPath(rootDir);
    for (const [path, bytes] of assetMap) store.registerMemoryAsset(path, bytes);
    usd.importAssetStore(store);
    usd.setCombineUDIMTiles(combine);
    usd.setLoadTextureInNative(true);
    const result = usd.begin(assetMap.get(rootPath), rootPath);
    if (!result.success) throw Error(result.error || 'Failed to load USD');
    return { usd, rootDir };
  } catch (error) { usd.delete(); throw error; }
  finally { store.delete(); }
}

// The UVTexture id used for a mesh's diffuse channel, or -1.
// Uses the "legacy" material accessor, which returns a plain object exposing
// `diffuseColorTextureId` directly (the 'json'/'xml' formats serialize to a
// string with a different schema).
function diffuseTextureId(usd, materialId) {
  if (materialId < 0 || materialId >= usd.numMaterials()) return -1;
  try {
    const m = usd.getMaterialWithFormat(materialId, 'legacy');
    if (m && m.diffuseColorTextureId !== undefined && m.diffuseColorTextureId >= 0) {
      return m.diffuseColorTextureId;
    }
  } catch (_) { /* ignore */ }
  return -1;
}

// ---------------------------------------------------------------------------
// Tile decode + DataArrayTexture packing ("like skin texture")
// ---------------------------------------------------------------------------

// Return a canvas/bitmap drawable for a tile: prefer the natively-decoded RGBA
// (getImageCopy gives an owned copy — fine for the CPU-side atlas assembly);
// fall back to decoding the registered PNG bytes in JS.
async function tileSource(usd, tile, assetMap, assetIdentifier) {
  const img = usd.getImageCopy(tile.imageId);
  if (img && img.decoded && img.data && img.width > 0 && img.height > 0) {
    const w = img.width, h = img.height, ch = img.channels || 4;
    const src = img.data;            // owned RGBA copy from getImageCopy()
    const rgba = new Uint8ClampedArray(w * h * 4);
    for (let i = 0; i < w * h; i++) {
      const s = i * ch, d = i * 4;
      if (ch >= 3) {
        rgba[d] = src[s]; rgba[d + 1] = src[s + 1]; rgba[d + 2] = src[s + 2];
        rgba[d + 3] = ch >= 4 ? src[s + 3] : 255;
      } else { // grayscale (1) or gray+alpha (2)
        const g = src[s];
        rgba[d] = g; rgba[d + 1] = g; rgba[d + 2] = g;
        rgba[d + 3] = ch >= 2 ? src[s + 1] : 255;
      }
    }
    const cv = document.createElement('canvas');
    cv.width = w; cv.height = h;
    cv.getContext('2d').putImageData(new ImageData(rgba, w, h), 0, 0);
    return { source: cv, width: w, height: h };
  }

  // Fallback: decode the original bytes (e.g. tiles not decoded natively).
  const name = assetIdentifier.replace('<UDIM>', String(tile.udim));
  const bytes = assetMap.get(name) || assetMap.get(name.split('/').pop());
  if (bytes) {
    const bmp = await createImageBitmap(new Blob([bytes]));
    return { source: bmp, width: bmp.width, height: bmp.height };
  }
  return null;
}

// Build a DataArrayTexture (one layer per tile) + a 100x1 lookup DataTexture
// mapping udim id -> layer+1 (0 = absent).
async function buildTileArray(usd, udimTex, assetMap) {
  const tiles = [...udimTex.tiles].sort((a, b) => a.udim - b.udim);
  const sources = [];
  let cellSize = 1;
  for (const tile of tiles) {
    const s = await tileSource(usd, tile, assetMap, udimTex.assetIdentifier);
    if (!s) { sources.push(null); continue; }
    cellSize = Math.max(cellSize, s.width, s.height);
    sources.push({ tile, ...s });
  }
  cellSize = Math.min(MAX_CELL_SIZE, cellSize);

  const present = sources.filter(Boolean);
  const layerCount = Math.max(1, present.length);
  const data = new Uint8Array(cellSize * cellSize * 4 * layerCount);

  const cell = document.createElement('canvas');
  cell.width = cell.height = cellSize;
  const cctx = cell.getContext('2d', { willReadFrequently: true });

  const lut = new Float32Array(100); // udim 1001..1100, 0 = absent
  const meta = [];
  present.forEach((s, layer) => {
    cctx.save();
    cctx.clearRect(0, 0, cellSize, cellSize);
    if (FLIP_TILES) { cctx.translate(0, cellSize); cctx.scale(1, -1); }
    cctx.drawImage(s.source, 0, 0, cellSize, cellSize);
    cctx.restore();
    s.source.close?.();
    const px = cctx.getImageData(0, 0, cellSize, cellSize).data;
    data.set(px, layer * cellSize * cellSize * 4);
    if (s.tile.udim >= 1001 && s.tile.udim <= 1100) lut[s.tile.udim - 1001] = layer + 1;
    meta.push({ udim: s.tile.udim, u: s.tile.u, v: s.tile.v, w: s.width, h: s.height, layer });
  });

  const arrayTex = new THREE.DataArrayTexture(data, cellSize, cellSize, layerCount);
  arrayTex.format = THREE.RGBAFormat;
  arrayTex.type = THREE.UnsignedByteType;
  arrayTex.colorSpace = THREE.SRGBColorSpace;
  arrayTex.magFilter = THREE.LinearFilter;
  arrayTex.minFilter = THREE.LinearMipmapLinearFilter;
  arrayTex.wrapS = arrayTex.wrapT = THREE.ClampToEdgeWrapping;
  arrayTex.generateMipmaps = true;
  arrayTex.needsUpdate = true;

  const lookupTex = new THREE.DataTexture(lut, 100, 1, THREE.RedFormat, THREE.FloatType);
  lookupTex.magFilter = lookupTex.minFilter = THREE.NearestFilter;
  lookupTex.needsUpdate = true;

  return { arrayTex, lookupTex, cellSize, layerCount, meta };
}

// ---------------------------------------------------------------------------
// Custom UV-remap shader
// ---------------------------------------------------------------------------

const UDIM_VERT = /* glsl */`
out vec2 vUv;
out vec3 vNormalW;
void main() {
  vUv = uv;
  vNormalW = normalize(mat3(modelMatrix) * normal);
  gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
}
`;

const UDIM_FRAG = /* glsl */`
precision highp float;
precision highp sampler2DArray;

uniform sampler2DArray uTiles;
uniform sampler2D      uLut;     // 100x1 R32F: layer+1, 0 = absent
uniform vec3           uLightDir;
uniform vec3           uMagenta;

in vec2 vUv;
in vec3 vNormalW;
out vec4 fragColor;

void main() {
  float tu = floor(vUv.x);
  float tv = floor(vUv.y);

  vec3 base;
  if (tu < 0.0 || tu > 9.0 || tv < 0.0 || tv > 9.0) {
    base = uMagenta;                                   // outside UDIM range
  } else {
    int idx = int(tu) + 10 * int(tv);                 // udim - 1001, 0..99
    float enc = texelFetch(uLut, ivec2(idx, 0), 0).r; // layer+1, or 0
    if (enc < 0.5) {
      base = uMagenta;                                 // tile not present
    } else {
      base = texture(uTiles, vec3(fract(vUv), float(int(enc) - 1))).rgb;
    }
  }

  vec3 n = normalize(vNormalW);
  float ndl = max(dot(n, normalize(uLightDir)), 0.0);
  float hemi = 0.5 + 0.5 * n.y;
  fragColor = vec4(base * (0.35 + 0.45 * hemi + 0.5 * ndl), 1.0);
}
`;

function makeUdimMaterial(arrayTex, lookupTex) {
  return new THREE.ShaderMaterial({
    glslVersion: THREE.GLSL3,
    uniforms: {
      uTiles: { value: arrayTex },
      uLut: { value: lookupTex },
      uLightDir: { value: new THREE.Vector3(0.4, 0.8, 0.6).normalize() },
      uMagenta: { value: new THREE.Color(1, 0, 1) },
    },
    vertexShader: UDIM_VERT,
    fragmentShader: UDIM_FRAG,
  });
}

// ---------------------------------------------------------------------------
// Geometry building lives in ./src/gl-upload.js (meshPtrToGeometry zero-copy +
// meshCopyToGeometry fallback).
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Orchestration
// ---------------------------------------------------------------------------

let lastAssets = null;     // { assetMap, rootPath } for re-load on mode switch
let exportUsd = null;      // original sparse layers and tiles, packaged for export

async function loadAndRender(assetMap, rootPath, mode) {
  await ensureWasm();
  clearGroup();
  exportUsd = null;
  els.btnExport.disabled = true;

  const combine = mode === 'combine';
  const { usd, rootDir } = loadUsd(assetMap, rootPath, combine);

  const group = new THREE.Group();
  const infoLines = [`mode: ${mode}`, `meshes: ${usd.numMeshes()}  materials: ${usd.numMaterials()}  textures: ${usd.numTextures()}`];

  // Discover the (first) sparse UDIM texture for the passthrough path.
  let packed = null, udimTex = null;
  if (!combine && usd.numUDIMTextures() > 0) {
    udimTex = usd.getUDIMTexture(0);
    packed = await buildTileArray(usd, udimTex, assetMap);
    infoLines.push('', `UDIM: ${udimTex.assetIdentifier}`,
      `cell: ${packed.cellSize}px  layers: ${packed.layerCount}`,
      'tiles:');
    for (const t of packed.meta.sort((a, b) => a.udim - b.udim)) {
      infoLines.push(`  ${t.udim} (u=${t.u},v=${t.v})  ${t.w}x${t.h} -> layer ${t.layer}`);
    }
  } else if (combine) {
    infoLines.push('', 'combine: core built a single atlas + rebaked mesh UVs.');
  } else {
    infoLines.push('', 'No UDIM texture in this scene (rendering flat).');
  }

  const gl = renderer.getContext();
  for (let i = 0; i < usd.numMeshes(); i++) {
    const copy = getCoreInspectorMesh(usd, i);
    const matId = copy.materialId ?? -1;
    const geo = meshCopyToGeometry(copy);
    if (!geo) continue;

    const texId = diffuseTextureId(usd, matId);
    let material;
    if (packed && texId >= 0 && usd.getTexture(texId).isUDIM) {
      // Passthrough: custom UV-remap shader over the packed tile array.
      material = makeUdimMaterial(packed.arrayTex, packed.lookupTex);
    } else {
      // Combine mode (atlas) or non-UDIM mesh: a normal lit material.
      material = new THREE.MeshStandardMaterial({ color: 0xcccccc, roughness: 0.8, metalness: 0.0, side: THREE.DoubleSide });
      const map = diffuseMap(usd, texId);
      if (map) { material.map = map; material.color.set(0xffffff); }
    }
    const mesh = new THREE.Mesh(geo, material);
    if (copy.transform?.length === 16) {
      mesh.matrix.fromArray(copy.transform); mesh.matrixAutoUpdate = false;
    }
    group.add(mesh);
  }

  scene.add(group);
  currentGroup = group;
  frameGroup(group);

  setInfo(infoLines.join('\n'));

  if (!combine) {
    exportUsd = buildUSDZWithNewRoot(rootPath, assetMap.get(rootPath),
      [...assetMap].filter(([path])=>path!==rootPath).map(([name,data])=>({name,data})));
    els.btnExport.disabled = false;
  }
  usd.delete();

  log(`Loaded (${mode}).`);
}

// Build a normal THREE texture from a (non-UDIM / atlas) diffuse texture image.
function diffuseMap(usd, texId) {
  if (texId < 0) return null;
  const tex = usd.getTexture(texId);
  if (!tex || tex.textureImageId === undefined || tex.textureImageId < 0) return null;
  const img = usd.getImageCopy(tex.textureImageId);
  if (!img || !img.decoded || !img.data || !img.width) return null;
  const ch = img.channels || 4;
  const w = img.width, h = img.height, src = img.data;
  const rgba = new Uint8ClampedArray(w * h * 4);
  for (let i = 0; i < w * h; i++) {
    const s = i * ch, d = i * 4;
    if (ch >= 3) { rgba[d] = src[s]; rgba[d + 1] = src[s + 1]; rgba[d + 2] = src[s + 2]; rgba[d + 3] = ch >= 4 ? src[s + 3] : 255; }
    else { const g = src[s]; rgba[d] = rgba[d + 1] = rgba[d + 2] = g; rgba[d + 3] = ch >= 2 ? src[s + 1] : 255; }
  }
  const cv = document.createElement('canvas');
  cv.width = w; cv.height = h;
  cv.getContext('2d').putImageData(new ImageData(rgba, w, h), 0, 0);
  const t = new THREE.CanvasTexture(cv);          // CanvasTexture flips Y by default
  t.colorSpace = THREE.SRGBColorSpace;
  t.wrapS = t.wrapT = THREE.ClampToEdgeWrapping;
  t.needsUpdate = true;
  return t;
}

function currentMode() {
  const r = document.querySelector('input[name="mode"]:checked');
  return r ? r.value : 'passthrough';
}

async function loadAssets(assetMap, rootPath) {
  lastAssets = { assetMap, rootPath };
  try {
    await loadAndRender(assetMap, rootPath, currentMode());
  } catch (err) {
    const msg = err && err.message ? err.message : String(err);
    log('ERROR: ' + msg);
    setInfo('ERROR: ' + msg);
  }
}

// ---------------------------------------------------------------------------
// Upload (files / folder / drag-drop)
// ---------------------------------------------------------------------------

async function assetMapFromEntries(entries) {
  const map = new Map();
  for (const { path, file } of entries) {
    map.set(path, new Uint8Array(await file.arrayBuffer()));
  }
  return map;
}

async function handleUpload(entries) {
  const usable = entries.filter(({ path }) => isUsdName(path) || isImageName(path));
  if (!usable.length) { log('No USD or image files found.'); return; }
  const map = await assetMapFromEntries(usable);
  const rootPath = rootUsdFromMap(map);
  if (!rootPath) { log('No root USD (.usd/.usda/.usdc) found in the upload.'); return; }
  await loadAssets(map, rootPath);
}

function entriesFromFileList(files) {
  return Array.from(files).map((file) => ({ path: file.webkitRelativePath || file.name, file }));
}

// ---------------------------------------------------------------------------
// UI wiring
// ---------------------------------------------------------------------------

initScene();

els.btnSample.addEventListener('click', async () => {
  log('Building sample…');
  const { assetMap, rootPath } = await buildSampleAssets();
  await loadAssets(assetMap, rootPath);
});

els.filesInput.addEventListener('change', (e) => handleUpload(entriesFromFileList(e.target.files)));

['dragenter', 'dragover'].forEach((ev) =>
  els.drop.addEventListener(ev, (e) => { e.preventDefault(); els.drop.classList.add('active'); }));
['dragleave', 'drop'].forEach((ev) =>
  els.drop.addEventListener(ev, (e) => { e.preventDefault(); els.drop.classList.remove('active'); }));

els.drop.addEventListener('drop', async (e) => {
  const items = e.dataTransfer.items;
  const entries = [];
  const walk = async (entry, prefix) => {
    if (entry.isFile) {
      const file = await new Promise((res, rej) => entry.file(res, rej));
      entries.push({ path: prefix + entry.name, file });
    } else if (entry.isDirectory) {
      const reader = entry.createReader();
      let batch;
      do {
        batch = await new Promise((res, rej) => reader.readEntries(res, rej));
        for (const c of batch) await walk(c, prefix + entry.name + '/');
      } while (batch.length > 0);
    }
  };
  if (items && items.length && items[0].webkitGetAsEntry) {
    for (const it of items) {
      const entry = it.webkitGetAsEntry();
      if (entry) await walk(entry, '');
    }
    await handleUpload(entries);
  } else {
    await handleUpload(entriesFromFileList(e.dataTransfer.files));
  }
});

document.querySelectorAll('input[name="mode"]').forEach((r) =>
  r.addEventListener('change', () => {
    if (lastAssets) loadAssets(lastAssets.assetMap, lastAssets.rootPath);
  }));

els.btnExport.addEventListener('click', () => {
  if (!exportUsd) return;
  try {
    const data = exportUsd;
    const blob = new Blob([new Uint8Array(data)], { type: 'model/vnd.usdz+zip' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'udim.usdz';
    a.click();
    URL.revokeObjectURL(a.href);
    log('Exported udim.usdz (tiles passed through, root keeps @tex.<UDIM>.png@).');
  } catch (err) {
    log('Export ERROR: ' + (err && err.message ? err.message : err));
  }
});

log('Ready — load the sample or drop a UDIM USD.');
