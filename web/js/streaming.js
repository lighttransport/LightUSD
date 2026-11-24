// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-present Light Transport Entertainment, Inc.
//
// Core WebGL2 USD/USDZ preview with browser-side image decoding. RenderStream
// materializes one mesh at a time; GL uploads finish before its scratch is
// reused. Both entry points support USDA, USDC and composed USDZ packages.
// loadBytes retains core scene inspection metadata through texture/mesh upload;
// loadBytesIncremental uploads geometry first and then resolves archive textures.
// Image bytes remain in JS; only texture identifiers enter the native resolver.
// Per-phase statistics report WASM heap reservation, which is distinct from live
// native allocations and browser/GPU memory. end() frees the native scene after
// upload; repeated loads and dispose() release the renderer's GL resources.
//
// import {StreamingUSDRenderer} from './streaming.js';
// const renderer=new StreamingUSDRenderer(canvas); await renderer.init();
// await renderer.loadBytesIncremental(bytes,'scene.usdz');

import { LightUSDLoader } from './src/lightusd/LightUSDLoader.js';
import { createCoreRenderInspector } from './src/lightusd/CoreRenderInspector.js';
import { parseUSDZEntries,isImageName } from './src/usdzconvert.js';

// ---------------------------------------------------------------------------
// Small math helpers (column-major mat4, like WebGL/GLSL).
// ---------------------------------------------------------------------------

const M = {
  ident: () => new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]),
  mul(a, b) {
    const o = new Float32Array(16);
    for (let c = 0; c < 4; c++) {
      for (let r = 0; r < 4; r++) {
        o[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                       a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
      }
    }
    return o;
  },
  perspective(fovy, aspect, near, far) {
    const f = 1 / Math.tan(fovy / 2), nf = 1 / (near - far);
    const o = new Float32Array(16);
    o[0] = f / aspect; o[5] = f; o[10] = (far + near) * nf; o[11] = -1;
    o[14] = 2 * far * near * nf;
    return o;
  },
  lookAt(eye, center, up) {
    const z = norm3(sub3(eye, center));
    const x = norm3(cross3(up, z));
    const y = cross3(z, x);
    const o = new Float32Array(16);
    o[0] = x[0]; o[4] = x[1]; o[8] = x[2]; o[12] = -dot3(x, eye);
    o[1] = y[0]; o[5] = y[1]; o[9] = y[2]; o[13] = -dot3(y, eye);
    o[2] = z[0]; o[6] = z[1]; o[10] = z[2]; o[14] = -dot3(z, eye);
    o[15] = 1;
    return o;
  },
  normalMat3(m) {
    const a=[m[0],m[1],m[2]], b=[m[4],m[5],m[6]], c=[m[8],m[9],m[10]];
    const x=cross3(b,c), y=cross3(c,a), z=cross3(a,b);
    const determinant=dot3(a,x);
    return Math.abs(determinant)>1e-20
      ?new Float32Array([...x,...y,...z].map(value=>value/determinant))
      :new Float32Array([1,0,0,0,1,0,0,0,1]);
  },
};
const sub3 = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const dot3 = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross3 = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const norm3 = (a) => { const l = Math.hypot(a[0], a[1], a[2]) || 1; return [a[0] / l, a[1] / l, a[2] / l]; };

// ---------------------------------------------------------------------------
// Compact GGX PBR shaders (WebGL2 / GLSL ES 3.00).
// ---------------------------------------------------------------------------

const VERT_SRC = `#version 300 es
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUv;
uniform mat4 uModel, uView, uProj;
uniform mat3 uNormalMat;
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;
void main() {
  vec4 wp = uModel * vec4(aPos, 1.0);
  vWorldPos = wp.xyz;
  vNormal = normalize(uNormalMat * aNormal);
  vUv = aUv;
  gl_Position = uProj * uView * wp;
}`;

const FRAG_SRC = `#version 300 es
precision highp float;
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;
out vec4 fragColor;

uniform vec3 uCamPos;
uniform vec3 uLightDir;      // direction TOWARDS the light
uniform vec3 uLightColor;
uniform vec3 uAmbientSky;
uniform vec3 uAmbientGround;

uniform vec3  uBaseColor;
uniform float uMetallic;
uniform float uRoughness;
uniform float uOpacity;
uniform vec3  uEmissive;
uniform float uOcclusion;    // constant AO when no map
uniform int   uDoubleSided;
uniform int   uAlphaCutoffOn;
uniform float uAlphaCutoff;

uniform int uHasBaseMap, uHasNormalMap, uHasMrMap, uHasAoMap, uHasEmissiveMap;
uniform sampler2D uBaseMap, uNormalMap, uMrMap, uAoMap, uEmissiveMap;

const float PI = 3.14159265359;

float distGGX(float NdotH, float a) {
  float a2 = a * a;
  float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
  return a2 / max(PI * d * d, 1e-7);
}
float smithG(float NdotV, float NdotL, float rough) {
  float k = (rough + 1.0) * (rough + 1.0) / 8.0;
  float gv = NdotV / (NdotV * (1.0 - k) + k);
  float gl = NdotL / (NdotL * (1.0 - k) + k);
  return gv * gl;
}
vec3 fresnel(float c, vec3 F0) { return F0 + (1.0 - F0) * pow(clamp(1.0 - c, 0.0, 1.0), 5.0); }
vec3 toLinear(vec3 c) { return pow(c, vec3(2.2)); }

// Tangent-free normal mapping (cotangent frame from screen-space derivatives).
vec3 perturbNormal(vec3 N, vec2 mapN_xy, float mapN_z) {
  vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
  vec2 duv1 = dFdx(vUv), duv2 = dFdy(vUv);
  vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
  vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
  vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
  float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
  mat3 TBN = mat3(T * invmax, B * invmax, N);
  return normalize(TBN * vec3(mapN_xy, mapN_z));
}

void main() {
  vec3 base = uBaseColor;
  float alpha = uOpacity;
  if (uHasBaseMap == 1) { vec4 t = texture(uBaseMap, vUv); base *= toLinear(t.rgb); alpha *= t.a; }
  if (uAlphaCutoffOn == 1 && alpha < uAlphaCutoff) discard;

  float metallic = uMetallic, rough = clamp(uRoughness, 0.04, 1.0);
  if (uHasMrMap == 1) { vec4 mr = texture(uMrMap, vUv); rough = clamp(rough * mr.g, 0.04, 1.0); metallic *= mr.b; }
  float ao = uOcclusion;
  if (uHasAoMap == 1) ao *= texture(uAoMap, vUv).r;

  vec3 N = normalize(vNormal);
  if (uDoubleSided == 1 && !gl_FrontFacing) N = -N;
  vec3 V = normalize(uCamPos - vWorldPos);
  if (uHasNormalMap == 1) {
    vec3 m = texture(uNormalMap, vUv).xyz * 2.0 - 1.0;
    N = perturbNormal(N, m.xy, m.z);
  }

  vec3 F0 = mix(vec3(0.04), base, metallic);
  vec3 L = normalize(uLightDir);
  vec3 H = normalize(V + L);
  float NdotL = max(dot(N, L), 0.0);
  float NdotV = max(dot(N, V), 1e-3);
  float NdotH = max(dot(N, H), 0.0);
  float HdotV = max(dot(H, V), 0.0);

  float D = distGGX(NdotH, rough * rough);
  float G = smithG(NdotV, NdotL, rough);
  vec3  Fr = fresnel(HdotV, F0);
  vec3 spec = (D * G * Fr) / max(4.0 * NdotV * NdotL, 1e-3);
  vec3 kd = (1.0 - Fr) * (1.0 - metallic);
  vec3 direct = (kd * base / PI + spec) * uLightColor * NdotL;

  // Cheap hemispheric ambient stands in for IBL: sky above, ground below.
  float hemi = 0.5 + 0.5 * N.y;
  vec3 ambient = mix(uAmbientGround, uAmbientSky, hemi) * base * ao * (1.0 - 0.4 * metallic);

  vec3 emissive = uEmissive;
  if (uHasEmissiveMap == 1) emissive *= toLinear(texture(uEmissiveMap, vUv).rgb);

  vec3 color = direct + ambient + emissive;
  color = color / (color + vec3(1.0));        // Reinhard tonemap
  color = pow(color, vec3(1.0 / 2.2));         // gamma encode
  fragColor = vec4(color, alpha);
}`;

// ---------------------------------------------------------------------------
// Heap-view + dtype helpers.
// ---------------------------------------------------------------------------

// Build a typed-array VIEW into the live WASM heap for a zero-copy descriptor
// {ptr, length, dtype}. The view is valid only until the heap grows — read it
// (e.g. gl.bufferData) before any call that could allocate WASM memory.
function heapView(native, desc) {
  const buf = native.HEAPU8.buffer;
  const ptr = Number(desc.ptr);
  const n = desc.length;
  switch (desc.dtype) {
    case 'f32': return new Float32Array(buf, ptr, n);
    case 'f64': return new Float64Array(buf, ptr, n);
    case 'u32': return new Uint32Array(buf, ptr, n);
    case 'i32': return new Int32Array(buf, ptr, n);
    case 'u16': return new Uint16Array(buf, ptr, n);
    case 'snorm16': return new Int16Array(buf, ptr, n);
    case 'snorm8': return new Int8Array(buf, ptr, n);
    default: return new Uint8Array(buf, ptr, desc.byteLength);
  }
}

// dtype -> WebGL vertex-attribute type + whether it is normalized.
function glAttrType(gl, dtype) {
  switch (dtype) {
    case 'f32': return { type: gl.FLOAT, normalized: false };
    case 'snorm8': return { type: gl.BYTE, normalized: true };
    case 'snorm16': return { type: gl.SHORT, normalized: true };
    case 'u8': return { type: gl.UNSIGNED_BYTE, normalized: true };
    default: return { type: gl.FLOAT, normalized: false };
  }
}

const fmtMB = (b) => (b / 1048576).toFixed(1) + ' MB';

// ---------------------------------------------------------------------------
// The renderer.
// ---------------------------------------------------------------------------

export class StreamingUSDRenderer {
  constructor(canvas) {
    this.canvas = canvas;
    this.gl = null;
    this.prog = null;
    this.loc = {};        // uniform locations
    this.loader = null;   // LightUSDLoader (owns the WASM Module)
    this.native = null;   // Emscripten Module (HEAPU8 etc.)
    this.drawables = [];  // { vao, count, indexType, material, bbox }
    this.glTextures = []; // GL texture per RenderScene image id (or null)
    this.bbox = null;
    this._raf = 0;
    this._memCb = null;
    this._memTimer = 0;
    // Orbit camera state.
    this.cam = { az: 0.6, el: 0.5, dist: 3, target: [0, 0, 0], fov: 50 * Math.PI / 180 };
    this._inputSize = 0;
  }

  // ---- lifecycle -----------------------------------------------------------

  async init() {
    const gl = this.canvas.getContext('webgl2', { antialias: true, alpha: false });
    if (!gl) throw new Error('WebGL2 is required for the streaming renderer.');
    this.gl = gl;
    gl.enable(gl.DEPTH_TEST);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.clearColor(0.09, 0.10, 0.12, 1.0);
    this.prog = this._buildProgram(VERT_SRC, FRAG_SRC);
    gl.useProgram(this.prog);
    for (const u of ['uModel', 'uView', 'uProj', 'uNormalMat', 'uCamPos', 'uLightDir',
      'uLightColor', 'uAmbientSky', 'uAmbientGround', 'uBaseColor', 'uMetallic',
      'uRoughness', 'uOpacity', 'uEmissive', 'uOcclusion', 'uDoubleSided',
      'uAlphaCutoffOn', 'uAlphaCutoff', 'uHasBaseMap', 'uHasNormalMap', 'uHasMrMap',
      'uHasAoMap', 'uHasEmissiveMap', 'uBaseMap', 'uNormalMap', 'uMrMap', 'uAoMap',
      'uEmissiveMap']) {
      this.loc[u] = gl.getUniformLocation(this.prog, u);
    }
    // Fixed sampler units.
    gl.uniform1i(this.loc.uBaseMap, 0);
    gl.uniform1i(this.loc.uNormalMap, 1);
    gl.uniform1i(this.loc.uMrMap, 2);
    gl.uniform1i(this.loc.uAoMap, 3);
    gl.uniform1i(this.loc.uEmissiveMap, 4);

    this.loader = new LightUSDLoader();
    await this.loader.init();
    this.native = this.loader.native_;
    this._installCameraControls();
    this._startRenderLoop();
    return this;
  }

  heapBytes() { return this.native ? this.native.HEAPU8.buffer.byteLength : 0; }

  onMemory(cb) { this._memCb = cb; }

  setClearColor(color) {
    const c = Array.isArray(color) ? color : [0.09, 0.10, 0.12, 1.0];
    this.gl.clearColor(c[0] ?? 0.09, c[1] ?? 0.10, c[2] ?? 0.12, c[3] ?? 1.0);
  }

  setCamera(opts = {}) {
    if (typeof opts.az === 'number') this.cam.az = opts.az;
    if (typeof opts.el === 'number') this.cam.el = opts.el;
    if (typeof opts.dist === 'number' && opts.dist > 0) this.cam.dist = opts.dist;
    if (Array.isArray(opts.target) && opts.target.length >= 3) {
      this.cam.target = opts.target.slice(0, 3).map(Number);
    }
    if (typeof opts.fov === 'number' && opts.fov > 0) {
      this.cam.fov = opts.fov > Math.PI ? opts.fov * Math.PI / 180 : opts.fov;
    }
    if (typeof opts.near === 'number') this._near = opts.near;
    if (typeof opts.far === 'number') this._far = opts.far;
  }

  frameCamera(opts = {}) { this._frameCamera(opts); }

  getRenderStats() {
    return {
      meshes: this.drawables.length,
      textures: this.glTextures.filter(Boolean).length,
      bbox: this.bbox ? {
        min: this.bbox.min.slice(),
        max: this.bbox.max.slice(),
      } : null,
      camera: {
        az: this.cam.az,
        el: this.cam.el,
        dist: this.cam.dist,
        target: this.cam.target.slice(),
        fov: this.cam.fov,
        near: this._near,
        far: this._far,
      },
    };
  }

  // ---- loading (the low-memory pipeline) -----------------------------------

  // Load a USD/USDZ from bytes. Streams it into the heap, builds the render
  // scene (textures left encoded), uploads every mesh + texture to the GPU, then
  // frees the WASM render scene. Returns { meshes, textures, materials, memory }.
  async loadBytes(bytes, filename = 'scene.usdz') {
    const u8=ArrayBuffer.isView(bytes)
      ?new Uint8Array(bytes.buffer,bytes.byteOffset,bytes.byteLength):new Uint8Array(bytes);
    this._inputSize=u8.length;
    this._disposeScene();
    const mem={input:u8.length,phases:[],incremental:false};
    const snap=label=>this._snapMem(mem,label);
    snap('baseline');
    try {
      this.usd=await createCoreRenderInspector(this.native,u8,filename,{loadTextures:false,omitTexturePayloads:true});
      this._composedTexBytes=new Map();
      if(u8[0]===0x50 && u8[1]===0x4b) {
        const entries=new Map(parseUSDZEntries(u8).map(entry=>[this._normTexPath(entry.name),entry.data]));
        for(let i=0;i<this.usd.numImages();i++) {
          const image=this.usd.getImageCopy(i);
          const bytes=entries.get(this._normTexPath(image.uri));
          if(bytes)this._composedTexBytes.set(i,bytes);
        }
      }
      snap('core scene loaded');
      return await this._uploadRenderScene(mem,snap);
    } catch(error) {this._disposeScene();throw error;}
    finally {this._composedTexBytes=null;}
  }

  // Render an EXTERNALLY composed native instance. The instance must already be
  // through layerToRenderScene() (textures resolved into render_scene_), ideally
  // with setLoadTextureInNative(false) so images stay encoded (JS decodes them
  // off the WASM heap). The renderer takes ownership of `usd` (it is reset()/
  // delete()'d with the scene). Used by the HTTP asset-resolver demo, which
  // composes a remote USD (references/payloads/textures fetched over HTTP) before
  // handing the built render scene here. Returns the same shape as loadBytes().
  async renderComposedNative(usd, label = 'composed', opts = {}) {
    if (!usd) throw new Error('renderComposedNative: no native instance.');
    this._disposeScene();
    this.usd = usd;
    this._inputSize = opts.inputBytes || 0;
    // Map<imageId, Uint8Array> of HTTP-fetched encoded bytes for textures the
    // WASM side could not resolve (buffer_id == -1). Consumed by _uploadImage.
    this._composedTexBytes = opts.textureBytesById || null;

    const mem = { input: this._inputSize, phases: [], composed: true, label };
    const snap = (lbl) => this._snapMem(mem, lbl);
    snap('render scene composed (peak)');

    try {
      return await this._uploadRenderScene(mem, snap);
    } finally {
      this._composedTexBytes = null;
    }
  }

  // Shared upload + free pipeline for a built render_scene_ on `this.usd`:
  // upload every texture (decoded in JS) and every mesh (zero-copy heap views)
  // to the GPU, then reset() the WASM scene. Fills `mem` and returns
  // { meshes, textures, materials, memory }.
  async _uploadRenderScene(mem, snap) {
    const numMeshes = this.usd.numMeshes();
    const numMaterials = this.usd.numMaterials();
    const numImages = this.usd.numImages();

    // Upload every texture (decode encoded bytes in JS, off the WASM heap).
    this.glTextures = new Array(numImages).fill(null);
    const colorImages = this._colorImageIds(numMaterials); // which images are sRGB
    for (let i = 0; i < numImages; i++) {
      // eslint-disable-next-line no-await-in-loop
      this.glTextures[i] = await this._uploadImage(i, colorImages.has(i));
    }
    snap('textures uploaded to GPU');

    // Upload every mesh straight from the heap to GL buffers.
    this.drawables = [];
    this.bbox = null;
    for (let i = 0; i < numMeshes; i++) {
      const d = this._uploadMesh(i, numMaterials);
      if (d) this.drawables.push(d);
    }
    snap('meshes uploaded to GPU');

    // Free the entire WASM-side render scene + input. The GPU keeps the data.
    this.usd.end();
    snap('WASM render scene freed (reset)');

    this._frameCamera();
    mem.peakReserved = Math.max(...mem.phases.map((p) => p.heapReserved));
    mem.summary = {
      inputMB: mem.input / 1048576,
      peakHeapMB: mem.peakReserved / 1048576,
      residentBuffersMB: 0, // after reset, the working set is on the GPU
      ratio: mem.peakReserved / Math.max(1, mem.input),
    };
    this._lastMemory = mem;
    this._emitMemory();
    return { meshes: this.drawables.length, textures: numImages, materials: numMaterials, memory: mem };
  }

  // One memory snapshot (heap reserved + live WASM render buffers) appended to
  // mem.phases, emitted live through onMemory().
  _snapMem(mem, label) {
    const p = { label, heapReserved: this.heapBytes() };
    try {
      const s = this.usd ? this.usd.getMemoryStats() : null;
      p.renderBuffers = s ? s.bufferMemoryBytes : 0;
      p.assetCache = s ? s.assetCacheSizeBytes : 0;
    } catch (_) { p.renderBuffers = 0; p.assetCache = 0; }
    mem.phases.push(p);
    this._emitMemory();
    return p;
  }

  // Keep geometry scratch native and encoded texture data in JS throughout.
  async loadBytesIncremental(bytes, filename = 'scene.usdz') {
    const u8=ArrayBuffer.isView(bytes)
      ?new Uint8Array(bytes.buffer,bytes.byteOffset,bytes.byteLength):new Uint8Array(bytes);
    this._inputSize=u8.length;
    this._disposeScene();
    let crate=u8,sourceURI=filename;
    const texEntries=new Map();
    if(u8[0]===0x50 && u8[1]===0x4b && u8[2]===3 && u8[3]===4) {
      const entries=parseUSDZEntries(u8);
      const root=entries[0];
      if(!root||!/\.(usd|usda|usdc)$/i.test(root.name))throw Error('USDZ has no root layer');
      crate=root.data;sourceURI=root.name;
      for(const entry of entries)texEntries.set(this._normTexPath(entry.name),entry.data);
    }
    this._texEntries = texEntries;
    this._texCache = new Map(); // normalized path -> GL texture

    const mem = { input: u8.length, crate: crate.length, phases: [], incremental: true };
    const snap = (label) => { mem.phases.push({ label, heapReserved: this.heapBytes() }); this._emitMemory(); };
    snap('baseline');

    const rs = new this.native.RenderStream();
    let r;
    try {
      if(texEntries.size) {
        const store=new this.native.AssetStore();
        try {
          const slash=sourceURI.lastIndexOf('/');
          if(slash>=0)store.setBaseWorkingPath(sourceURI.slice(0,slash));
          for(const [name,data] of texEntries) {
            if(name!==sourceURI)store.registerMemoryAsset(name,
              isImageName(name)?new Uint8Array():data);
          }
          rs.importAssetStore(store);
        } finally {store.delete();}
      }
      r=rs.begin(crate,sourceURI);
      if(!r?.success)throw Error(r?.error||'Core render stream failed');
      snap('core layer loaded');

      this.glTextures = [];
      this.drawables = [];
      this.bbox = null;
      const n = r.meshCount;
      for (let i = 0; i < n; i++) {
        const m = rs.getMesh(i);          // materializes ONE mesh into wasm scratch
        if (m && !m.error) {
          const d = this._uploadStreamMesh(m); // upload to GPU before next getMesh
          if (d) this.drawables.push(d);
        }
      }
      snap('all meshes uploaded to GPU');
    } catch(error) {
      this._disposeScene();
      throw error;
    } finally {
      rs.end();   // free the crate + scratch
      rs.delete();
    }
    snap('WASM render scene freed (end)');

    // Resolve textures from the JS-held archive (off the WASM heap): decode each
    // unique referenced image once via createImageBitmap and bind to materials.
    try { await this._resolveStreamTextures(); }
    catch(error) {this._disposeScene();throw error;}
    this._texEntries=null;
    const textured = this.drawables.reduce((a, d) => a + (d.material.baseMap ? 1 : 0), 0);

    this._frameCamera();
    mem.peakReserved = Math.max(...mem.phases.map((p) => p.heapReserved));
    mem.summary = {
      inputMB: mem.input / 1048576,
      crateMB: mem.crate / 1048576,
      peakHeapMB: mem.peakReserved / 1048576,
      ratio: mem.peakReserved / Math.max(1, mem.crate),
      incremental: true,
    };
    this._lastMemory = mem;
    this._emitMemory();
    return { meshes: this.drawables.length, textures: [...this._texCache.values()].filter(Boolean).length, materials: this.drawables.length, memory: mem };
  }

  _normTexPath(p) { return String(p || '').replace(/^[./]+/, ''); }

  // Decode each referenced archive texture once (createImageBitmap, off the WASM
  // heap) and bind to the materials that use it. Runs after geometry upload, so
  // it does not hold up the zero-copy mesh scratch.
  async _resolveStreamTextures() {
    if (!this._texEntries || !this._texEntries.size) return;
    const get = async (path, srgb) => {
      if (!path) return null;
      const assetKey = this._normTexPath(path);
      const key=assetKey+'|'+srgb;
      if (this._texCache.has(key)) return this._texCache.get(key);
      let bytes = this._texEntries.get(assetKey);
      if (!bytes) { // suffix match fallback
        for (const [k, v] of this._texEntries) { if (k.endsWith('/' + assetKey) || assetKey.endsWith('/' + k)) { bytes = v; break; } }
      }
      const tex = bytes ? await this._decodeArchiveTexture(bytes, srgb) : null;
      this._texCache.set(key, tex);
      return tex;
    };
    for (const d of this.drawables) {
      const t = d.material._tex; if (!t) continue;
      d.material.baseMap = await get(t.base, true);
      d.material.normalMap = await get(t.normal, false);
      d.material.mrMap = await get(t.mr, false);
      d.material.aoMap = await get(t.ao, false);
      d.material.emissiveMap = await get(t.emissive, true);
    }
  }

  async _decodeArchiveTexture(bytes, srgb) {
    const gl = this.gl;
    let bmp;
    try { bmp = await createImageBitmap(new Blob([bytes.slice()]), { premultiplyAlpha: 'none', colorSpaceConversion: 'none' }); }
    catch (_) { return null; }
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true); // USD st origin is bottom-left
    gl.texImage2D(gl.TEXTURE_2D, 0, srgb ? gl.SRGB8_ALPHA8 : gl.RGBA8, gl.RGBA, gl.UNSIGNED_BYTE, bmp);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.generateMipmap(gl.TEXTURE_2D);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);
    if (bmp.close) bmp.close();
    return tex;
  }

  // Build a drawable from a RenderStream getMesh() descriptor. The descriptors
  // are zero-copy views into the WASM scratch, valid only until the next
  // getMesh()/end(), so upload happens synchronously here.
  _uploadStreamMesh(m) {
    const gl = this.gl;
    if (!m.points || !m.points.length) return null;
    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);
    this._uploadAttrib(0, m.points, 3);
    this._growBBox(heapView(this.native, m.points),m.worldMatrix);
    if (m.normals && m.normals.length) this._uploadAttrib(1, m.normals, 3);
    else { gl.disableVertexAttribArray(1); gl.vertexAttrib3f(1, 0, 1, 0); }
    if (m.uv0 && m.uv0.length) this._uploadAttrib(2, m.uv0, 2);
    else { gl.disableVertexAttribArray(2); gl.vertexAttrib2f(2, 0, 0); }

    let count, mode = 'arrays';
    if (m.indices && m.indices.length) {
      const ib = this._createBuffer();
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, heapView(this.native, m.indices), gl.STATIC_DRAW);
      count = m.indices.length; mode = 'elements';
    } else {
      count = m.points.length / 3;
    }
    gl.bindVertexArray(null);

    const mt = m.material || {};
    const material = {
      baseColor: Array.isArray(mt.baseColor) ? mt.baseColor : [0.8, 0.8, 0.8],
      metallic: typeof mt.metallic === 'number' ? mt.metallic : 0,
      roughness: typeof mt.roughness === 'number' ? mt.roughness : 0.5,
      opacity: typeof mt.opacity === 'number' ? mt.opacity : 1,
      emissive: Array.isArray(mt.emissive) ? mt.emissive : [0, 0, 0],
      occlusion: typeof mt.occlusion === 'number' ? mt.occlusion : 1,
      alphaCutoff: typeof mt.opacityThreshold === 'number' && mt.opacityThreshold > 0 ? mt.opacityThreshold : -1,
      baseMap: null, normalMap: null, mrMap: null, aoMap: null, emissiveMap: null,
      // Texture asset paths, resolved to GL textures from the JS archive after
      // the geometry loop (see _resolveStreamTextures).
      _tex: {
        base: mt.baseColorTexture, normal: mt.normalTexture,
        mr: mt.roughnessTexture || mt.metallicTexture,
        ao: mt.occlusionTexture, emissive: mt.emissiveTexture,
      },
    };
    return { vao, count, indexType: gl.UNSIGNED_INT, mode, material, doubleSided: !!m.doubleSided, model:m.worldMatrix||M.ident() };
  }

  // Parse getMaterial(matId) (a JSON string) into a normalized shader record.
  // tydra fills `surfaceShader` (UsdPreviewSurface-style) for both
  // UsdPreviewSurface AND MaterialX; an `openPBR` block (base_*) is mapped onto
  // the same fields when no PreviewSurface is present. Texture bindings are
  // RenderScene texture ids (resolve to an image via getTexture().textureImageId).
  _getShader(matId) {
    let raw;
    try { raw = this.usd.getMaterial(matId); } catch (_) { return null; }
    if (!raw || !raw.data) return null;
    let obj;
    try { obj = JSON.parse(raw.data); } catch (_) { return null; }
    const ss = obj.surfaceShader;
    if (ss && obj.hasUsdPreviewSurface !== false) return ss;
    const o = obj.openPBR || obj.openpbr;
    if (o) {
      // tydra serializes OpenPBR grouped by layer (o.base.base_color,
      // o.specular.specular_roughness, o.coat.coat_color, ...). Look a param up
      // by name across the groups, with a flat-structure fallback.
      const GROUPS = ['base', 'specular', 'transmission', 'subsurface', 'sheen',
        'fuzz', 'thin_film', 'coat', 'emission', 'geometry'];
      const find = (name) => {
        if (o[name]) return o[name];
        for (const g of GROUPS) { if (o[g] && o[g][name]) return o[g][name]; }
        return null;
      };
      const texOf = (p) => (p && p.type === 'texture' && typeof p.textureId === 'number' && p.textureId >= 0) ? p.textureId : undefined;
      const valOf = (p) => (p && p.value != null) ? p.value : undefined;

      const baseColor = find('base_color');
      const metal = find('base_metalness');
      const baseRough = find('base_roughness');
      const specRough = find('specular_roughness');
      const coatColor = find('coat_color');
      const coatWeight = find('coat_weight');
      const emis = find('emission_color');
      const norm = find('geometry_normal') || find('normal');

      // "Simple MaterialX shading": the dominant albedo is sometimes authored on
      // the coat layer (e.g. Autodesk standard_surface brass = white base +
      // coat_color image). When base_color has no map, fall back to the
      // coat_color map so the texture is visible in this minimal renderer.
      const useCoat = texOf(baseColor) == null && texOf(coatColor) != null &&
                      (valOf(coatWeight) == null || valOf(coatWeight) > 0);
      const colorTexId = texOf(baseColor) ?? (useCoat ? texOf(coatColor) : undefined);
      // A roughness map may live on the specular/coat layer when base_roughness
      // is a constant. Bind it (the shader's MR sampler reads .g for roughness);
      // it also feeds .b into metalness, which softens a fully-metallic surface —
      // acceptable for this minimal, env-map-less renderer and visually nicer
      // than a flat mirror.
      const roughTexId = texOf(baseRough) ?? texOf(specRough) ?? texOf(find('coat_roughness'));

      return {
        // When the tint comes from the map, neutralize the constant base color.
        diffuseColor: useCoat ? [1, 1, 1] : (valOf(baseColor) || [0.8, 0.8, 0.8]),
        metallic: valOf(metal) ?? 0,
        // With a roughness map bound, let the map drive (uniform = 1); otherwise
        // use the authored scalar (avoid a 0 → perfect-mirror look).
        roughness: roughTexId != null ? 1.0 : (valOf(baseRough) || valOf(specRough) || 0.5),
        opacity: valOf(find('geometry_opacity')) ?? valOf(find('opacity')) ?? 1,
        emissiveColor: valOf(emis) || [0, 0, 0],
        diffuseColorTextureId: colorTexId,
        metallicTextureId: texOf(metal),
        roughnessTextureId: roughTexId,
        normalTextureId: texOf(norm),
        emissiveColorTextureId: texOf(emis),
      };
    }
    return ss || null;
  }

  // Which image ids are sampled as color (sRGB) vs data (linear), from the
  // materials' base-color / emissive bindings.
  _colorImageIds(numMaterials) {
    const color = new Set();
    for (let m = 0; m < numMaterials; m++) {
      const s = this._getShader(m);
      if (!s) continue;
      for (const k of ['diffuseColorTextureId', 'emissiveColorTextureId']) {
        const tid = s[k];
        if (typeof tid === 'number' && tid >= 0) {
          const imgId = this._textureImageId(tid);
          if (imgId >= 0) color.add(imgId);
        }
      }
    }
    return color;
  }

  _textureImageId(texId) {
    try {
      const t = this.usd.getTexture(texId);
      return t && typeof t.textureImageId === 'number' ? t.textureImageId : -1;
    } catch (_) { return -1; }
  }

  // Decode + upload one image to a GL texture. Encoded images (decoded=false)
  // are decoded with createImageBitmap (off the WASM heap); already-decoded
  // images are uploaded straight from the heap view.
  async _uploadImage(imgId, srgb) {
    const gl = this.gl;
    let desc;
    try { desc = this.usd.getImageCopy(imgId); } catch (_) { return null; }
    if (!desc) return null;

    // Image the WASM side left unresolved (buffer_id == -1, no heap bytes) — e.g.
    // an HTTP-referenced texture. Decode externally-supplied encoded bytes
    // (fetched in JS, off the WASM heap) keyed by image id. Same orientation /
    // params as the heap encoded path below.
    if (!desc.data?.byteLength) {
      const ext = this._composedTexBytes && this._composedTexBytes.get(imgId);
      return ext ? this._decodeEncodedTexture(ext, srgb) : null;
    }

    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    const internal = srgb ? gl.SRGB8_ALPHA8 : gl.RGBA8;

    if (desc.decoded) {
      // Raw pixels in the heap. Normalize to RGBA8.
      const view = desc.data;
      const rgba = this._toRGBA8(view, desc.width, desc.height, desc.channels);
      gl.texImage2D(gl.TEXTURE_2D, 0, internal, desc.width, desc.height, 0,
        gl.RGBA, gl.UNSIGNED_BYTE, rgba);
    } else {
      // Encoded bytes (PNG/JPEG/...). Copy out of the heap into a Blob, decode
      // off-heap via the browser, upload. The heap copy is transient (one image).
      const view = desc.data;
      const bytes = view.slice(); // detach from heap before any async/alloc
      let bmp;
      try {
        bmp = await createImageBitmap(new Blob([bytes]), { premultiplyAlpha: 'none', colorSpaceConversion: 'none' });
      } catch (e) {
        gl.deleteTexture(tex);
        return null;
      }
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texImage2D(gl.TEXTURE_2D, 0, internal, gl.RGBA, gl.UNSIGNED_BYTE, bmp);
      bmp.close && bmp.close();
    }
    gl.generateMipmap(gl.TEXTURE_2D);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);
    return tex;
  }

  // Decode encoded image bytes (PNG/JPEG/...) supplied from JS (off the WASM
  // heap) into a GL texture. Same orientation/params as _uploadImage's heap
  // encoded branch (eager geometry: UNPACK_FLIP_Y = false).
  async _decodeEncodedTexture(bytes, srgb) {
    const gl = this.gl;
    let bmp;
    try {
      bmp = await createImageBitmap(new Blob([bytes.slice ? bytes.slice() : bytes]),
        { premultiplyAlpha: 'none', colorSpaceConversion: 'none' });
    } catch (_) { return null; }
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.texImage2D(gl.TEXTURE_2D, 0, srgb ? gl.SRGB8_ALPHA8 : gl.RGBA8,
      gl.RGBA, gl.UNSIGNED_BYTE, bmp);
    bmp.close && bmp.close();
    gl.generateMipmap(gl.TEXTURE_2D);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.REPEAT);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.REPEAT);
    return tex;
  }

  _toRGBA8(src, w, h, channels) {
    if (channels === 4) return src;
    const n = w * h, out = new Uint8Array(n * 4);
    for (let i = 0; i < n; i++) {
      const r = src[i * channels];
      out[i * 4 + 0] = r;
      out[i * 4 + 1] = channels >= 2 ? src[i * channels + 1] : r;
      out[i * 4 + 2] = channels >= 3 ? src[i * channels + 2] : r;
      out[i * 4 + 3] = 255;
    }
    return out;
  }

  // Upload one mesh's geometry (zero-copy heap views -> GL buffers) and resolve
  // its material into a small GPU-ready record.
  _uploadMesh(meshId, numMaterials) {
    const gl = this.gl;
    let m;
    try { m = this.usd.getMesh(meshId); } catch (_) { return null; }
    if (!m || !m.points || !m.points.length) return null;

    const vao = gl.createVertexArray();
    gl.bindVertexArray(vao);

    // Positions (attrib 0). Accumulate the scene bbox while the view is live.
    this._uploadAttrib(0, m.points, 3);
    this._growBBox(heapView(this.native, m.points),m.worldMatrix);

    // Normals (attrib 1) — from the mesh, or computed if absent.
    if (m.normals && m.normals.length) {
      this._uploadAttrib(1, m.normals, 3);
    } else {
      const normals = this._computeNormals(m);
      const nb = this._createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, nb);
      gl.bufferData(gl.ARRAY_BUFFER, normals, gl.STATIC_DRAW);
      gl.enableVertexAttribArray(1);
      gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 0, 0);
    }

    // UV0 (attrib 2) — optional.
    const uv = m.uv0 && m.uv0.length ? m.uv0 : (m.uvSets && m.uvSets.uv0);
    if (uv && uv.length) {
      this._uploadAttrib(2, uv, 2);
    } else {
      gl.disableVertexAttribArray(2);
      gl.vertexAttrib2f(2, 0, 0);
    }

    // Indices — triangulate if the mesh is not already triangle-only.
    let count, indexType = gl.UNSIGNED_INT, mode = 'elements';
    if (m.indices && m.indices.length) {
      let idx = heapView(this.native, m.indices); // u32 view
      if (!m.triangulated && m.faceVertexCounts && m.faceVertexCounts.length) {
        idx = this._triangulate(idx, heapView(this.native, m.faceVertexCounts));
      }
      const ib = this._createBuffer();
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, idx, gl.STATIC_DRAW);
      count = idx.length;
    } else {
      mode = 'arrays';
      count = m.points.length / 3;
    }

    gl.bindVertexArray(null);
    const material = this._resolveMaterial(m.materialId, numMaterials);
    return { vao, count, indexType, mode, material, doubleSided: !!m.doubleSided, model:m.worldMatrix||M.ident() };
  }

  _createBuffer() {
    const buffer=this.gl.createBuffer();
    if(!this._glBuffers)this._glBuffers=new Set();
    this._glBuffers.add(buffer);
    return buffer;
  }

  _uploadAttrib(location, desc, comps) {
    const gl = this.gl;
    const view = heapView(this.native, desc);
    const b = this._createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, b);
    gl.bufferData(gl.ARRAY_BUFFER, view, gl.STATIC_DRAW); // GL copies into GPU now
    const at = glAttrType(gl, desc.dtype);
    gl.enableVertexAttribArray(location);
    gl.vertexAttribPointer(location, desc.comps || comps, at.type, at.normalized, 0, 0);
  }

  _growBBox(positions,model=M.ident()) {
    let bb = this.bbox;
    if (!bb) { bb = this.bbox = { min: [1e30, 1e30, 1e30], max: [-1e30, -1e30, -1e30] }; }
    for (let i = 0; i < positions.length; i += 3) {
      for (let c = 0; c < 3; c++) {
        const v = model[c]*positions[i]+model[4+c]*positions[i+1]+model[8+c]*positions[i+2]+model[12+c];
        if (v < bb.min[c]) bb.min[c] = v;
        if (v > bb.max[c]) bb.max[c] = v;
      }
    }
  }

  _triangulate(indices, fvc) {
    const tris = [];
    let base = 0;
    for (let f = 0; f < fvc.length; f++) {
      const n = fvc[f];
      for (let k = 2; k < n; k++) {
        tris.push(indices[base], indices[base + k - 1], indices[base + k]);
      }
      base += n;
    }
    return new Uint32Array(tris);
  }

  _computeNormals(m) {
    const pos = heapView(this.native, m.points);
    const nv = pos.length / 3;
    const out = new Float32Array(pos.length);
    let idx = m.indices && m.indices.length ? heapView(this.native, m.indices) : null;
    if (idx && !m.triangulated && m.faceVertexCounts && m.faceVertexCounts.length) {
      idx = this._triangulate(idx, heapView(this.native, m.faceVertexCounts));
    }
    const tri = idx || (() => { const a = new Uint32Array(nv); for (let i = 0; i < nv; i++) a[i] = i; return a; })();
    for (let t = 0; t < tri.length; t += 3) {
      const a = tri[t] * 3, b = tri[t + 1] * 3, c = tri[t + 2] * 3;
      const e1 = [pos[b] - pos[a], pos[b + 1] - pos[a + 1], pos[b + 2] - pos[a + 2]];
      const e2 = [pos[c] - pos[a], pos[c + 1] - pos[a + 1], pos[c + 2] - pos[a + 2]];
      const fn = cross3(e1, e2);
      for (const vi of [a, b, c]) { out[vi] += fn[0]; out[vi + 1] += fn[1]; out[vi + 2] += fn[2]; }
    }
    for (let i = 0; i < out.length; i += 3) {
      const l = Math.hypot(out[i], out[i + 1], out[i + 2]) || 1;
      out[i] /= l; out[i + 1] /= l; out[i + 2] /= l;
    }
    return out;
  }

  // Resolve getMaterial(matId) into shader-ready params + bound GL textures.
  // tydra fills the same UsdPreviewSurface-style record for UsdPreviewSurface
  // and MaterialX, so both shade through this path.
  _resolveMaterial(matId, numMaterials) {
    const def = {
      baseColor: [0.8, 0.8, 0.8], metallic: 0.0, roughness: 0.5, opacity: 1.0,
      emissive: [0, 0, 0], occlusion: 1.0, alphaCutoff: -1,
      baseMap: null, normalMap: null, mrMap: null, aoMap: null, emissiveMap: null,
    };
    if (typeof matId !== 'number' || matId < 0 || matId >= numMaterials) return def;
    const mat = this._getShader(matId);
    if (!mat) return def;

    if (Array.isArray(mat.diffuseColor)) def.baseColor = mat.diffuseColor.slice(0, 3);
    if (typeof mat.metallic === 'number') def.metallic = mat.metallic;
    if (typeof mat.roughness === 'number') def.roughness = mat.roughness;
    if (typeof mat.opacity === 'number') def.opacity = mat.opacity;
    if (Array.isArray(mat.emissiveColor)) def.emissive = mat.emissiveColor.slice(0, 3);
    if (typeof mat.occlusion === 'number' && mat.occlusion > 0) def.occlusion = mat.occlusion;
    if (typeof mat.opacityThreshold === 'number' && mat.opacityThreshold > 0) {
      def.alphaCutoff = mat.opacityThreshold;
    }
    const texOf = (tid) => {
      if (typeof tid !== 'number' || tid < 0) return null;
      const imgId = this._textureImageId(tid);
      return imgId >= 0 ? this.glTextures[imgId] || null : null;
    };
    def.baseMap = texOf(mat.diffuseColorTextureId);
    def.normalMap = texOf(mat.normalTextureId);
    def.aoMap = texOf(mat.occlusionTextureId);
    def.emissiveMap = texOf(mat.emissiveColorTextureId);
    // Metallic/roughness may be one packed ORM map or two scalar maps; both work
    // because the shader reads .g (roughness) and .b (metallic), and a grayscale
    // scalar map has r==g==b.
    def.mrMap = texOf(mat.roughnessTextureId) || texOf(mat.metallicTextureId);
    return def;
  }

  // ---- rendering -----------------------------------------------------------

  _frameCamera(opts = {}) {
    if (!this.bbox || this.bbox.min[0] > this.bbox.max[0]) return;
    if (typeof opts.fov === 'number' && opts.fov > 0) {
      this.cam.fov = opts.fov > Math.PI ? opts.fov * Math.PI / 180 : opts.fov;
    }
    if (typeof opts.az === 'number') this.cam.az = opts.az;
    if (typeof opts.el === 'number') this.cam.el = opts.el;
    const c = [0, 1, 2].map((i) => (this.bbox.min[i] + this.bbox.max[i]) / 2);
    this.cam.target = Array.isArray(opts.target) && opts.target.length >= 3
      ? opts.target.slice(0, 3).map(Number) : c;
    const corners = [];
    for (const x of [this.bbox.min[0], this.bbox.max[0]]) {
      for (const y of [this.bbox.min[1], this.bbox.max[1]]) {
        for (const z of [this.bbox.min[2], this.bbox.max[2]]) corners.push([x, y, z]);
      }
    }
    const dir = norm3([
      Math.cos(this.cam.el) * Math.sin(this.cam.az),
      Math.sin(this.cam.el),
      Math.cos(this.cam.el) * Math.cos(this.cam.az),
    ]);
    let right = norm3(cross3([0, 1, 0], dir));
    if (!Number.isFinite(right[0])) right = [1, 0, 0];
    const up = cross3(dir, right);
    let maxX = 1e-3, maxY = 1e-3, maxZ = 0;
    for (const p of corners) {
      const v = sub3(p, this.cam.target);
      maxX = Math.max(maxX, Math.abs(dot3(v, right)));
      maxY = Math.max(maxY, Math.abs(dot3(v, up)));
      maxZ = Math.max(maxZ, Math.abs(dot3(v, dir)));
    }
    const aspect = (this.canvas.width || this.canvas.clientWidth || 1) /
      Math.max(1, (this.canvas.height || this.canvas.clientHeight || 1));
    const tanY = Math.tan(this.cam.fov / 2);
    const tanX = tanY * aspect;
    const fitDist = Math.max(maxX / Math.max(tanX, 1e-3), maxY / Math.max(tanY, 1e-3)) + maxZ;
    this.cam.dist = typeof opts.dist === 'number' && opts.dist > 0
      ? opts.dist : fitDist * (opts.padding ?? 1.1);
    this._near = typeof opts.near === 'number' ? opts.near : Math.max(1e-3, this.cam.dist - maxZ * 2 - 1);
    this._far = typeof opts.far === 'number' ? opts.far : this.cam.dist + maxZ * 2 + 1;
  }

  _camEye() {
    const { az, el, dist, target } = this.cam;
    const ce = Math.cos(el), se = Math.sin(el);
    return [target[0] + dist * ce * Math.sin(az), target[1] + dist * se, target[2] + dist * ce * Math.cos(az)];
  }

  _startRenderLoop() {
    const loop = () => { this._raf = requestAnimationFrame(loop); this._render(); };
    this._raf = requestAnimationFrame(loop);
  }

  _render() {
    const gl = this.gl, canvas = this.canvas;
    const w = canvas.clientWidth | 0, h = canvas.clientHeight | 0;
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    if (!this.drawables.length) return;

    const eye = this._camEye();
    const proj = M.perspective(this.cam.fov, (canvas.width || 1) / (canvas.height || 1),
      this._near || 0.01, this._far || 1000);
    const view = M.lookAt(eye, this.cam.target, [0, 1, 0]);
    const model = M.ident();

    gl.useProgram(this.prog);
    gl.uniformMatrix4fv(this.loc.uModel, false, model);
    gl.uniformMatrix4fv(this.loc.uView, false, view);
    gl.uniformMatrix4fv(this.loc.uProj, false, proj);
    gl.uniformMatrix3fv(this.loc.uNormalMat, false, M.normalMat3(model));
    gl.uniform3fv(this.loc.uCamPos, eye);
    gl.uniform3fv(this.loc.uLightDir, norm3([0.4, 0.8, 0.5]));
    gl.uniform3fv(this.loc.uLightColor, [3.0, 2.9, 2.7]);
    gl.uniform3fv(this.loc.uAmbientSky, [0.35, 0.40, 0.48]);
    gl.uniform3fv(this.loc.uAmbientGround, [0.12, 0.11, 0.10]);

    for (const d of this.drawables) {
      const model=d.model||M.ident();
      gl.uniformMatrix4fv(this.loc.uModel,false,model);
      gl.uniformMatrix3fv(this.loc.uNormalMat,false,M.normalMat3(model));
      const mt = d.material;
      gl.uniform3fv(this.loc.uBaseColor, mt.baseColor);
      gl.uniform1f(this.loc.uMetallic, mt.metallic);
      gl.uniform1f(this.loc.uRoughness, mt.roughness);
      gl.uniform1f(this.loc.uOpacity, mt.opacity);
      gl.uniform3fv(this.loc.uEmissive, mt.emissive);
      gl.uniform1f(this.loc.uOcclusion, mt.occlusion);
      gl.uniform1i(this.loc.uDoubleSided, d.doubleSided ? 1 : 0);
      gl.uniform1i(this.loc.uAlphaCutoffOn, mt.alphaCutoff > 0 ? 1 : 0);
      gl.uniform1f(this.loc.uAlphaCutoff, mt.alphaCutoff > 0 ? mt.alphaCutoff : 0.5);
      this._bindMap(0, mt.baseMap, this.loc.uHasBaseMap);
      this._bindMap(1, mt.normalMap, this.loc.uHasNormalMap);
      this._bindMap(2, mt.mrMap, this.loc.uHasMrMap);
      this._bindMap(3, mt.aoMap, this.loc.uHasAoMap);
      this._bindMap(4, mt.emissiveMap, this.loc.uHasEmissiveMap);
      gl.bindVertexArray(d.vao);
      if (d.mode === 'elements') gl.drawElements(gl.TRIANGLES, d.count, d.indexType, 0);
      else gl.drawArrays(gl.TRIANGLES, 0, d.count);
    }
    gl.bindVertexArray(null);
  }

  _bindMap(unit, tex, hasLoc) {
    const gl = this.gl;
    gl.activeTexture(gl.TEXTURE0 + unit);
    gl.bindTexture(gl.TEXTURE_2D, tex || null);
    gl.uniform1i(hasLoc, tex ? 1 : 0);
  }

  // ---- camera controls -----------------------------------------------------

  _installCameraControls() {
    const el = this.canvas;
    let drag = false, lx = 0, ly = 0;
    el.addEventListener('pointerdown', (e) => { drag = true; lx = e.clientX; ly = e.clientY; el.setPointerCapture(e.pointerId); });
    el.addEventListener('pointerup', (e) => { drag = false; try { el.releasePointerCapture(e.pointerId); } catch (_) {} });
    el.addEventListener('pointermove', (e) => {
      if (!drag) return;
      this.cam.az -= (e.clientX - lx) * 0.01;
      this.cam.el = Math.max(-1.5, Math.min(1.5, this.cam.el + (e.clientY - ly) * 0.01));
      lx = e.clientX; ly = e.clientY;
    });
    el.addEventListener('wheel', (e) => {
      e.preventDefault();
      this.cam.dist *= Math.exp(e.deltaY * 0.001);
    }, { passive: false });
  }

  // ---- memory reporting ----------------------------------------------------

  // Start polling live heap size at `intervalMs` and report through onMemory().
  startMemoryPolling(intervalMs = 500) {
    this.stopMemoryPolling();
    this._memTimer = setInterval(() => this._emitMemory(), intervalMs);
  }
  stopMemoryPolling() { if (this._memTimer) { clearInterval(this._memTimer); this._memTimer = 0; } }

  _emitMemory() {
    if (!this._memCb) return;
    let renderBuffers = 0;
    try { const s = this.usd && this.usd.getMemoryStats(); if (s) renderBuffers = s.bufferMemoryBytes; } catch (_) {}
    this._memCb({
      inputBytes: this._inputSize,
      heapReserved: this.heapBytes(),
      renderBuffers,
      last: this._lastMemory || null,
    });
  }

  // ---- teardown ------------------------------------------------------------

  _disposeScene() {
    const gl = this.gl;
    for (const d of this.drawables) { try { gl.deleteVertexArray(d.vao); } catch (_) {} }
    for (const t of this.glTextures) { if (t) { try { gl.deleteTexture(t); } catch (_) {} } }
    if (this._texCache) { for (const t of this._texCache.values()) { if (t) { try { gl.deleteTexture(t); } catch (_) {} } } }
    for(const buffer of this._glBuffers||[])gl.deleteBuffer(buffer);
    this._glBuffers?.clear();
    this.drawables = []; this.glTextures = []; this.bbox = null;
    this._texCache = null; this._texEntries = null;
    if(this.usd) {
      try {this.usd.end?.();} catch(_) {}
      try {this.usd.delete();} catch(_) {}
      this.usd=null;
    }
  }

  dispose() {
    cancelAnimationFrame(this._raf);
    this.stopMemoryPolling();
    this._disposeScene();
  }

  // ---- GL program helper ---------------------------------------------------

  _buildProgram(vsrc, fsrc) {
    const gl = this.gl;
    const sh = (type, src) => {
      const s = gl.createShader(type);
      gl.shaderSource(s, src); gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) {
        throw new Error('Shader compile error: ' + gl.getShaderInfoLog(s));
      }
      return s;
    };
    const p = gl.createProgram();
    gl.attachShader(p, sh(gl.VERTEX_SHADER, vsrc));
    gl.attachShader(p, sh(gl.FRAGMENT_SHADER, fsrc));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) {
      throw new Error('Program link error: ' + gl.getProgramInfoLog(p));
    }
    return p;
  }
}

// ---------------------------------------------------------------------------
// Demo bootstrap: wire a renderer to a canvas, a file picker / drag-drop, and a
// live memory panel. Call from streaming.html.
// ---------------------------------------------------------------------------

export async function mountStreamingDemo(opts = {}) {
  const canvas = opts.canvas || document.getElementById('gl');
  const panel = opts.panel || document.getElementById('mem-panel');
  const status = opts.status || document.getElementById('status');
  const fileInput = opts.fileInput || document.getElementById('file');

  const renderer = new StreamingUSDRenderer(canvas);
  await renderer.init();

  const setStatus = (s) => { if (status) status.textContent = s; };
  const renderPanel = (m) => {
    if (!panel) return;
    const ratio = m.inputBytes ? (m.heapReserved / m.inputBytes).toFixed(2) : '–';
    const rows = [
      ['Input USD', fmtMB(m.inputBytes)],
      ['WASM heap (reserved peak)', fmtMB(m.heapReserved)],
      ['WASM render buffers (live)', fmtMB(m.renderBuffers)],
      ['Heap / input', ratio + '×'],
    ];
    let html = '<table>' + rows.map(([k, v]) =>
      `<tr><td>${k}</td><td style="text-align:right">${v}</td></tr>`).join('') + '</table>';
    if (m.last && m.last.phases) {
      html += '<div class="phases"><b>Load phases (heap reserved / live buffers)</b><table>' +
        m.last.phases.map((p) =>
          `<tr><td>${p.label}</td><td style="text-align:right">${fmtMB(p.heapReserved)}</td>` +
          `<td style="text-align:right">${fmtMB(p.renderBuffers)}</td></tr>`).join('') +
        '</table></div>';
      const s = m.last.summary;
      if (s && s.incremental) {
        html += `<div class="target"><b>Incremental converter</b> (RenderStream, ` +
          `next-pipeline lazy crate): peak WASM heap ${s.peakHeapMB.toFixed(1)} MB ` +
          `for a ${s.crateMB.toFixed(1)} MB crate → <b>${s.ratio.toFixed(2)}× crate</b> ` +
          `(≈ input). One mesh is decoded at a time; textures are decoded in JS ` +
          `(off the WASM heap) and uploaded to the GPU. Full geometry + UVs + ` +
          `textured UsdPreviewSurface / MaterialX PBR.</div>`;
      } else if (s) {
        html += `<div class="target">Eager path: transient conversion peak ` +
          `${s.peakHeapMB.toFixed(1)} MB for ${s.inputMB.toFixed(1)} MB input → ` +
          `<b>${s.ratio.toFixed(2)}×</b> (builds the whole scene at once; supports ` +
          `textures). Use the incremental path for ≈1× WASM heap.</div>`;
      }
    }
    panel.innerHTML = html;
  };
  renderer.onMemory(renderPanel);
  renderer.startMemoryPolling(500);

  // Default uploads geometry first; standard scene inspection is opt-in
  // via the retained opts.eager / ?eager=1 compatibility switch.
  const useEager = !!opts.eager;
  async function loadFile(file) {
    setStatus(`Loading ${file.name} (${fmtMB(file.size)})…`);
    try {
      const buf = new Uint8Array(await file.arrayBuffer());
      const t0 = performance.now();
      const r = useEager ? await renderer.loadBytes(buf, file.name)
                         : await renderer.loadBytesIncremental(buf, file.name);
      const dt = (performance.now() - t0).toFixed(0);
      const s = r.memory.summary;
      const ratioStr = s.incremental ? `${s.ratio.toFixed(2)}× crate (≈ input)`
                                      : `${s.ratio.toFixed(2)}× input`;
      setStatus(`${file.name}: ${r.meshes} meshes, ${r.textures} textures, ` +
        `${r.materials} materials in ${dt} ms — peak WASM heap ` +
        `${s.peakHeapMB.toFixed(1)} MB (${ratioStr})`);
    } catch (e) {
      setStatus('Error: ' + (e && e.message));
      console.error(e);
    }
  }

  if (fileInput) {
    fileInput.addEventListener('change', (e) => { if (e.target.files[0]) loadFile(e.target.files[0]); });
  }
  // Drag-and-drop onto the canvas.
  const stop = (e) => { e.preventDefault(); e.stopPropagation(); };
  for (const ev of ['dragenter', 'dragover', 'dragleave', 'drop']) canvas.addEventListener(ev, stop);
  canvas.addEventListener('drop', (e) => { const f = e.dataTransfer.files[0]; if (f) loadFile(f); });

  // Optional initial model.
  if (opts.url) {
    setStatus(`Fetching ${opts.url}…`);
    try {
      const resp = await fetch(opts.url);
      const buf = new Uint8Array(await resp.arrayBuffer());
      const name = opts.url.split('/').pop();
      const r = useEager ? await renderer.loadBytes(buf, name)
                         : await renderer.loadBytesIncremental(buf, name);
      const s = r.memory.summary;
      const ratioStr = s.incremental ? `${s.ratio.toFixed(2)}× crate` : `${s.ratio.toFixed(2)}× input`;
      setStatus(`${name}: ${r.meshes} meshes, ${r.textures} textures — peak WASM heap ` +
        `${s.peakHeapMB.toFixed(1)} MB (${ratioStr})`);
    } catch (e) { setStatus('Drop a .usdz/.usdc file to render. ' + (e && e.message || '')); }
  } else {
    setStatus('Drop a .usdz / .usdc / .usda file onto the canvas, or pick one.');
  }

  return renderer;
}

export default StreamingUSDRenderer;
