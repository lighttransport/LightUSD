// SPDX-License-Identifier: Apache-2.0
import { shaderSource, displayShader, blitShader } from './shaders.js';
import { normalize, sub, cross } from './scene.js';
import { encodeEXR } from './capture.js';
import { bakeDisplacement } from './displacement.js';
import { mayEmit } from './emission.js';

export async function createRenderer(canvas, options = {}) {
  if (!navigator.gpu) throw new Error('WebGPU unavailable: use Chrome on localhost with an enabled GPU');
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) throw new Error('No WebGPU adapter');
  const device = await adapter.requestDevice();
  try { return new MaterialXRenderer(canvas, adapter, device, options); }
  catch (e) { device.destroy(); throw e; }
}
class MaterialXRenderer extends EventTarget {
  constructor(canvas, adapter, device, options) {
    super(); this.canvas = canvas; this.device = device; this.adapter = adapter;
    this.context = canvas.getContext('webgpu'); this.format = navigator.gpu.getPreferredCanvasFormat();
    this.context.configure({ device, format: this.format, alphaMode: 'opaque' });
    this.options = { exposure: 0, resolutionScale: 1, maxSamples: 4096, autoResolution: false, seed: 0, time: 0, frame: 0, textureMaxDimension: 0, textureMaxBytes: 64 * 1024 * 1024, textureStorage: 'half', physicalKernel: 'full' };
    this.mode = 'path-preview'; this.samples = 0; this.generation = 0; this.sceneGeneration = 0; this.disposed = false; this.busy = false;
    this.resources = []; this.errors = [];
    this.stats = { adapter: { vendor: adapter.info?.vendor, architecture: adapter.info?.architecture, device: adapter.info?.device, description: adapter.info?.description, isFallbackAdapter: adapter.info?.isFallbackAdapter }, referenceReady: false, samples: 0 };
    device.addEventListener('uncapturederror', e => this.report(e.error));
    device.lost.then(info => { if (!this.disposed) { this.lost = true; this.report(new Error(`WebGPU device lost: ${info.message}`)); } });
    this.uniform = device.createBuffer({ size: 128, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    this.sceneLayout = device.createBindGroupLayout({ entries: [
      { binding: 0, visibility: GPUShaderStage.COMPUTE | GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: 'uniform' } },
      { binding: 1, visibility: GPUShaderStage.COMPUTE | GPUShaderStage.FRAGMENT, buffer: { type: 'read-only-storage' } },
      { binding: 2, visibility: GPUShaderStage.COMPUTE | GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: 'read-only-storage' } },
      { binding: 3, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
      { binding: 4, visibility: GPUShaderStage.COMPUTE | GPUShaderStage.FRAGMENT, buffer: { type: 'read-only-storage' } },
      { binding: 5, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
      { binding: 6, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
      { binding: 7, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } },
      { binding: 8, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'read-only-storage' } },
    ] });
    this.setOptions(options);
  }
  report(error) { this.errors.push(String(error.message || error)); this.dispatchEvent(new CustomEvent('diagnostic', { detail: this.errors.at(-1) })); }
  check() { if (this.disposed || this.lost) throw new Error('Renderer is disposed or device is lost'); }
  async module(code) {
    const module = this.device.createShaderModule({ code });
    const info = await module.getCompilationInfo();
    const errors = info.messages.filter(m => m.type === 'error');
    if (errors.length) throw new Error(errors.map(m => `${m.lineNum}:${m.linePos} ${m.message}`).join('\n'));
    return module;
  }
  async loadScene(scene) {
    this.check(); const generation = ++this.sceneGeneration;
    this.pendingBuild?.cancel();
    const sourceScene = scene;
    scene = await bakeDisplacement(scene, this.device);
    if (generation !== this.sceneGeneration || this.disposed) return { cancelled: true };
    const packed = await new Promise((resolve, reject) => {
      const worker = new Worker(new URL('./scene-worker.js', import.meta.url), { type: 'module' });
      const cancel = () => { worker.terminate(); resolve(null); };
      this.pendingBuild = { cancel };
      worker.onmessage = ({ data }) => { worker.terminate(); if (generation === this.sceneGeneration) this.pendingBuild = null; if (data.error) reject(new Error(data.error)); else resolve(data.packed); };
      worker.onerror = e => { worker.terminate(); reject(new Error(e.message)); };
      try { worker.postMessage({ scene }); } catch (e) { worker.terminate(); reject(e); }
    });
    if (!packed || generation !== this.sceneGeneration || this.disposed) return { cancelled: true };
    const textures = {}; const shaderOptions = { maxDimension: this.options.textureMaxDimension || undefined, maxBytes: this.options.textureMaxBytes, compact: this.options.textureStorage === 'half', physical: false, physicalKernel: this.options.physicalKernel }; const code = shaderSource(packed.materials, textures, packed.lighting, shaderOptions);
    const module = await this.module(code);
    const layout = this.device.createPipelineLayout({ bindGroupLayouts: [this.sceneLayout] });
    const [compute, raster, displayModule] = await Promise.all([
      this.device.createComputePipelineAsync({ layout, compute: { module, entryPoint: 'trace' } }),
      this.device.createRenderPipelineAsync({ layout, vertex: { module, entryPoint: 'rasterVertex' }, fragment: { module, entryPoint: 'rasterFragment', targets: [{ format: this.format }] }, primitive: { topology: 'triangle-list', cullMode: 'none' }, depthStencil: { format: 'depth24plus', depthWriteEnabled: true, depthCompare: 'less' } }),
      this.module(displayShader),
    ]);
    if (generation !== this.sceneGeneration || this.disposed) return { cancelled: true };
    const display = await this.device.createRenderPipelineAsync({ layout: 'auto', vertex: { module: displayModule, entryPoint: 'vertex' }, fragment: { module: displayModule, entryPoint: 'fragment', targets: [{ format: this.format }] } });
    const blitModule = await this.module(blitShader);
    const blit = await this.device.createRenderPipelineAsync({ layout: 'auto', vertex: { module: blitModule, entryPoint: 'vertex' }, fragment: { module: blitModule, entryPoint: 'fragment', targets: [{ format: this.format }] } });
    if (generation !== this.sceneGeneration || this.disposed) return { cancelled: true };
    const buffer = data => {
      if (data.byteLength > this.device.limits.maxStorageBufferBindingSize) throw new Error('Scene exceeds WebGPU storage-buffer limit');
      const b = this.device.createBuffer({ size: data.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST }); this.device.queue.writeBuffer(b, 0, data); return b;
    };
    const allocated = [];
    try { allocated.push(buffer(packed.nodeData)); allocated.push(buffer(packed.triangleData)); allocated.push(buffer(textures.imageData)); allocated.push(buffer(textures.spectralData)); }
    catch (e) { allocated.forEach(b => b.destroy()); throw e; }
    this.resources.forEach(b => b.destroy()); this.resources = allocated;
    this.scene = packed; this.compute = compute; this.raster = raster; this.displayPipeline = display; this.blit = blit;
    this.physical = null; this.physicalError = null; this.physicalPending = null; let physicalModule = null;
    this.ensurePhysicalPipeline = async () => {
      if (this.physical) return this.physical;
      if (!this.physicalPending) this.physicalPending = (async () => { physicalModule = physicalModule || await this.module(shaderSource(packed.materials, {}, packed.lighting, { ...shaderOptions, physical: true, physicalOnly: true })); return this.device.createComputePipelineAsync({ layout, compute: { module: physicalModule, entryPoint: 'tracePhysical' } }); })().then(p => { if (generation === this.sceneGeneration && !this.disposed) this.physical = p; return p; }, e => { if (generation === this.sceneGeneration) this.physicalError = e; return null; });
      this.physical = await this.physicalPending;
      if (!this.physical) throw this.physicalError || new Error('Physical pipeline compilation failed');
      return this.physical;
    };
    this.sourceScene = sourceScene;
    this.scene.requiresPhysical = textures.requiresPhysical;
    this.scene.requiresSpectral = packed.materials.some(m=>m.mediumMajorant);
    if(this.scene.requiresSpectral && this.mode!=='realtime')this.mode='path-spectral';
    if (textures.requiresPhysical && this.mode === 'path-preview') this.mode = 'path-physical';
    this.setCamera(packed.camera || { origin: [3, 2, 4], target: [0, 1, 0], fov: 45 });
    this.resize(true); this.stats.triangles = packed.triangleCount;
    return { diagnostics: [], referenceReady: false, triangles: packed.triangleCount };
  }
  async setMaterialDocument(document, index = 1) {
    this.check(); if (!this.scene) throw new Error('Load a scene first');
    if (!Number.isInteger(index) || index < 0 || index >= this.scene.materials.length) throw new Error('Invalid material index');
    if (document.displacementOutput || this.scene.materials[index].displacementOutput || mayEmit(document)!==mayEmit(this.scene.materials[index])) {
      const materials = this.sourceScene.materials.slice(); materials[index] = document;
      return this.loadScene({ ...this.sourceScene, materials, camera: this.camera });
    }
    const materials = this.scene.materials.slice(); materials[index] = document;
    const generation = ++this.sceneGeneration;
    const textures = {}; const shaderOptions = { maxDimension: this.options.textureMaxDimension || undefined, maxBytes: this.options.textureMaxBytes, compact: this.options.textureStorage === 'half', physical: false, physicalKernel: this.options.physicalKernel }; const module = await this.module(shaderSource(materials, textures, this.scene.lighting, shaderOptions));
    const layout = this.device.createPipelineLayout({ bindGroupLayouts: [this.sceneLayout] });
    const [compute, raster] = await Promise.all([
      this.device.createComputePipelineAsync({ layout, compute: { module, entryPoint: 'trace' } }),
      this.device.createRenderPipelineAsync({ layout, vertex: { module, entryPoint: 'rasterVertex' }, fragment: { module, entryPoint: 'rasterFragment', targets: [{ format: this.format }] }, primitive: { topology: 'triangle-list' }, depthStencil: { format: 'depth24plus', depthWriteEnabled: true, depthCompare: 'less' } }),
    ]);
    if (generation !== this.sceneGeneration || this.disposed) return { cancelled: true };
    if (textures.imageData.byteLength > this.device.limits.maxStorageBufferBindingSize) throw new Error('Images exceed WebGPU storage-buffer limit');
    const imageBuffer = this.device.createBuffer({ size: textures.imageData.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
    this.device.queue.writeBuffer(imageBuffer, 0, textures.imageData);
    this.resources[2].destroy(); this.resources[2] = imageBuffer;
    this.compute = compute; this.physical = null; this.physicalError = null; this.physicalPending = null; let physicalModule = null; this.raster = raster; this.scene.materials = materials;
    this.ensurePhysicalPipeline = async () => {
      if (this.physical) return this.physical;
      if (!this.physicalPending) this.physicalPending = (async () => { physicalModule = physicalModule || await this.module(shaderSource(materials, {}, this.scene.lighting, { ...shaderOptions, physical: true, physicalOnly: true })); return this.device.createComputePipelineAsync({ layout, compute: { module: physicalModule, entryPoint: 'tracePhysical' } }); })().then(p => { if (generation === this.sceneGeneration && !this.disposed) this.physical = p; return p; }, e => { if (generation === this.sceneGeneration) this.physicalError = e; return null; });
      this.physical = await this.physicalPending;
      if (!this.physical) throw this.physicalError || new Error('Physical pipeline compilation failed');
      return this.physical;
    };
    this.sourceScene = { ...this.sourceScene, materials };
    this.scene.requiresPhysical = textures.requiresPhysical;
    this.scene.requiresSpectral = materials.some(m=>m.mediumMajorant);
    if(this.scene.requiresSpectral && this.mode!=='realtime')this.mode='path-spectral';
    if (textures.requiresPhysical && this.mode === 'path-preview') this.mode = 'path-physical';
    this.resize(true); return { diagnostics: [], referenceReady: false };
  }
  setCamera(camera) {
    this.check();
    for (const k of ['origin', 'target']) if (!Array.isArray(camera[k]) || camera[k].length !== 3 || !camera[k].every(Number.isFinite)) throw new Error(`Invalid camera ${k}`);
    if (Math.hypot(...sub(camera.target, camera.origin)) < 1e-8 || !(camera.fov > 0 && camera.fov < 179)) throw new Error('Invalid camera direction/FOV');
    this.camera = structuredClone(camera); this.resetAccumulation();
  }
  setMode(mode) {
    this.check();
    if (!['path-preview', 'path-physical', 'path-spectral', 'realtime'].includes(mode)) throw new Error('Full reference mode is not implemented; available: path-preview, path-physical, path-spectral, realtime');
    if (mode === 'path-preview' && this.scene?.requiresPhysical) throw new Error('Transmissive materials require path-physical or realtime approximation');
    if (['path-preview','path-physical'].includes(mode) && this.scene?.requiresSpectral)throw new Error('Heterogeneous media require path-spectral');
    if (this.mode !== mode) { this.mode = mode; this.resize(true); this.resetAccumulation(); }
  }
  setOptions(options) {
    this.check();
    const textureKeys = ['textureMaxDimension', 'textureMaxBytes', 'textureStorage', 'physicalKernel'];
    const textureChanged = textureKeys.some(k => Object.hasOwn(options, k) && options[k] !== this.options[k]);
    for (const [k, v] of Object.entries(options)) {
      if (k === 'autoResolution') { if (typeof v !== 'boolean') throw new Error('autoResolution must be boolean'); continue; }
      if (k === 'textureMaxDimension' || k === 'textureMaxBytes') {
        if (!Number.isInteger(v) || v < 0) throw new Error(`${k} must be a non-negative integer`);
        if (k === 'textureMaxDimension' && v !== 0 && (v < 1 || v > 16384)) throw new Error('textureMaxDimension must be 1..16384 or zero');
        if (k === 'textureMaxBytes' && (v < 16 || v > 1024 * 1024 * 1024)) throw new Error('textureMaxBytes is out of range');
        continue;
      }
      if (k === 'textureStorage') { if (!['float', 'half'].includes(v)) throw new Error('textureStorage must be float or half'); continue; }
      if (k === 'physicalKernel') { if (!['full', 'compact'].includes(v)) throw new Error('physicalKernel must be full or compact'); continue; }
      if (!['exposure', 'resolutionScale', 'maxSamples', 'seed', 'time', 'frame'].includes(k) || !Number.isFinite(v)) throw new Error(`Invalid option ${k}`);
      if(k==='seed'&&(!Number.isInteger(v)||v<0||v>0xffffffff))throw new Error('Seed must be a uint32');
      if (k === 'resolutionScale' && (v < 0.1 || v > 1)) throw new Error('Resolution scale must be 0.1–1');
      if (k === 'exposure' && Math.abs(v) > 32) throw new Error('Exposure must be within ±32 stops');
      if (k === 'maxSamples' && (!Number.isInteger(v) || v < 1 || v > 1_000_000)) throw new Error('Invalid sample limit');
    }
    const reseed=options.seed!==undefined&&options.seed!==this.options.seed;
    const animationChanged=(options.time!==undefined&&options.time!==this.options.time)||(options.frame!==undefined&&options.frame!==this.options.frame);
    Object.assign(this.options, options);
    if (textureChanged && this.scene && this.sourceScene) return this.loadScene({ ...this.sourceScene, camera: this.camera });
    if ('resolutionScale' in options) this.resize();if(reseed||animationChanged)this.resetAccumulation();
  }
  resetAccumulation() { this.samples = 0; this.generation++; this.stats.samples = 0; this.transportError = null; }
  resize(force = false) {
    if (!this.scene) return;
    const width = Math.max(1, Math.floor(this.canvas.width * this.options.resolutionScale)), height = Math.max(1, Math.floor(this.canvas.height * this.options.resolutionScale));
    if (!force && width === this.width && height === this.height && this.canvas.width === this.displayWidth && this.canvas.height === this.displayHeight) return;
    if (width * height * 16 > this.device.limits.maxStorageBufferBindingSize) throw new Error('Accumulation exceeds WebGPU buffer limit');
    const pathBytes = ['path-physical', 'path-spectral'].includes(this.mode) ? width * height * 160 : 160;
    if (pathBytes > this.device.limits.maxStorageBufferBindingSize) throw new Error('Path states exceed WebGPU buffer limit; reduce resolution');
    this.paths?.destroy(); this.pathCounters?.destroy(); this.pathReadback?.destroy(); this.moments?.destroy();
    this.paths = this.device.createBuffer({ size: pathBytes, usage: GPUBufferUsage.STORAGE });
    this.pathCounters = this.device.createBuffer({ size: 16, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC });
    this.pathReadback = this.device.createBuffer({ size: 16, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    this.moments = this.device.createBuffer({ size: ['path-physical', 'path-spectral'].includes(this.mode) ? width * height * 16 : 16, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
    this.accumulation?.destroy(); this.depth?.destroy(); this.rasterTarget?.destroy(); this.width = width; this.height = height;
    this.displayWidth = this.canvas.width; this.displayHeight = this.canvas.height;
    this.accumulation = this.device.createBuffer({ size: width * height * 16, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
    this.depth = this.device.createTexture({ size: [width, height], format: 'depth24plus', usage: GPUTextureUsage.RENDER_ATTACHMENT });
    this.rasterTarget = this.device.createTexture({ size: [width, height], format: this.format, usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
    this.blitGroup = this.device.createBindGroup({ layout: this.blit.getBindGroupLayout(0), entries: [{ binding: 0, resource: this.rasterTarget.createView() }, { binding: 1, resource: this.device.createSampler({ minFilter: 'linear', magFilter: 'linear' }) }] });
    this.sceneGroup = this.device.createBindGroup({ layout: this.sceneLayout, entries: [this.uniform, this.resources[0], this.resources[1], this.accumulation, this.resources[2], this.paths, this.pathCounters, this.moments, this.resources[3]].map((buffer, binding) => ({ binding, resource: { buffer } })) });
    this.displayGroup = this.device.createBindGroup({ layout: this.displayPipeline.getBindGroupLayout(0), entries: [this.uniform, this.accumulation].map((buffer, binding) => ({ binding, resource: { buffer } })) });
    this.resetAccumulation();
  }
  async renderStep() {
    this.check(); if (this.busy || !this.scene) return false;
    if(this.transportError)throw this.transportError;
    this.resize(); this.busy = true; let finishRender;this.renderCompletion=new Promise(resolve=>{finishRender=resolve;}); const start = performance.now(); const generation = this.generation;
    try {
      const forward = normalize(sub(this.camera.target, this.camera.origin));
      const right = normalize(cross(forward, Math.abs(forward[1]) > 0.999 ? [0, 0, 1] : [0, 1, 0])); const up = cross(right, forward);
      const tan = Math.tan(this.camera.fov * Math.PI / 360);
      const data = new ArrayBuffer(128), f = new Float32Array(data), u = new Uint32Array(data);
      f.set([...this.camera.origin, 0, ...forward, 0, ...right, tan * this.width / this.height, ...up, tan]);
      u.set([this.width, this.height, this.samples, this.mode === 'realtime' ? 1 : this.mode === 'path-spectral' ? 2 : 0], 16);
      f.set([this.options.exposure, this.canvas.width, this.canvas.height, this.generation], 20);u.set([this.options.seed,0,0,0],24);f.set([this.options.time,this.options.frame,0,0],28); this.device.queue.writeBuffer(this.uniform, 0, data);
      const encoder = this.device.createCommandEncoder();
      const physical = ['path-physical', 'path-spectral'].includes(this.mode);
      const tracing = this.mode !== 'realtime' && this.samples < this.options.maxSamples;
      if (tracing) {
        if (physical) await this.ensurePhysicalPipeline();
        if (physical) encoder.clearBuffer(this.pathCounters);
        const pass = encoder.beginComputePass(); pass.setPipeline(physical ? this.physical : this.compute); pass.setBindGroup(0, this.sceneGroup); pass.dispatchWorkgroups(Math.ceil(this.width / 8), Math.ceil(this.height / 8)); pass.end();
        if (physical) encoder.copyBufferToBuffer(this.pathCounters, 0, this.pathReadback, 0, 16);
      }
      const target = this.context.getCurrentTexture().createView();
      const descriptor = { colorAttachments: [{ view: this.mode === 'realtime' ? this.rasterTarget.createView() : target, clearValue: [0.09, 0.12, 0.16, 1], loadOp: 'clear', storeOp: 'store' }] };
      if (this.mode === 'realtime') descriptor.depthStencilAttachment = { view: this.depth.createView(), depthClearValue: 1, depthLoadOp: 'clear', depthStoreOp: 'store' };
      const pass = encoder.beginRenderPass(descriptor);
      pass.setPipeline(this.mode === 'realtime' ? this.raster : this.displayPipeline);
      pass.setBindGroup(0, this.mode === 'realtime' ? this.sceneGroup : this.displayGroup);
      pass.draw(this.mode === 'realtime' ? this.scene.triangleCount * 3 : 3); pass.end();
      if (this.mode === 'realtime') {
        const blit = encoder.beginRenderPass({ colorAttachments: [{ view: target, loadOp: 'clear', storeOp: 'store' }] });
        blit.setPipeline(this.blit); blit.setBindGroup(0, this.blitGroup); blit.draw(3); blit.end();
      }
      this.device.queue.submit([encoder.finish()]); await this.device.queue.onSubmittedWorkDone();
      if (generation === this.generation && tracing) {
        if (physical) {
          await this.pathReadback.mapAsync(GPUMapMode.READ);
          const counters = new Uint32Array(this.pathReadback.getMappedRange()).slice(); this.pathReadback.unmap();
          this.stats.activePaths = counters[0];
          this.stats.depthTerminated = counters[3];
          if (counters[1] || counters[2]) {this.transportError=new Error(`Physical transport invalid: ${counters[1]} medium stack errors, ${counters[2]} invalid values`);throw this.transportError;}
          if (counters[0] === 0) this.samples++;
        } else this.samples++;
      }
      Object.assign(this.stats, { samples: this.samples, milliseconds: performance.now() - start, width: this.width, height: this.height, mode: this.mode });
      if (this.mode === 'realtime' && this.options.autoResolution) {
        this.averageFrame = (this.averageFrame ?? this.stats.milliseconds) * 0.9 + this.stats.milliseconds * 0.1;
        this.resolutionFrames = (this.resolutionFrames || 0) + 1;
        if (this.resolutionFrames % 30 === 0) {
          const factor = this.averageFrame > 33.3 ? 0.9 : this.averageFrame < 22 ? 1.05 : 1;
          this.options.resolutionScale = Math.min(1, Math.max(0.25, this.options.resolutionScale * factor));
        }
      }
      this.dispatchEvent(new CustomEvent('progress', { detail: { ...this.stats } })); return true;
    } finally { this.busy = false; finishRender(); }
  }
  async capture({ format = 'exr' } = {}) {
    this.check();
    if(this.transportError)throw this.transportError;
    if (this.busy) await this.renderCompletion;
    this.check();if(this.transportError)throw this.transportError;
    if (format === 'png') {
      if (!this.scene) throw new Error('Load a scene first');
      await this.renderStep();
      const blob = await new Promise(resolve => this.canvas.toBlob(resolve, 'image/png'));
      if (!blob) throw new Error('PNG capture failed');
      return { bytes: new Uint8Array(await blob.arrayBuffer()), metadata: { mode: this.mode, referenceReady: false, displayTransformed: true } };
    }
    if (this.mode === 'realtime' || !this.samples) throw new Error('Linear capture requires an accumulated path image');
    const width = this.width, height = this.height, samples = this.samples;
    const captureGeneration=this.generation;
    const hasVariance=['path-physical','path-spectral'].includes(this.mode),pixelBytes=width*height*16;
    const readback = this.device.createBuffer({ size: pixelBytes*(hasVariance?2:1), usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    try {
      const encoder = this.device.createCommandEncoder(); encoder.copyBufferToBuffer(this.accumulation, 0, readback, 0, pixelBytes);
      if(hasVariance)encoder.copyBufferToBuffer(this.moments,0,readback,pixelBytes,pixelBytes);
      this.device.queue.submit([encoder.finish()]);
      await readback.mapAsync(GPUMapMode.READ); const mapped=readback.getMappedRange();const pixels = new Float32Array(mapped.slice(0,pixelBytes));
      if(captureGeneration!==this.generation)throw new Error('Capture invalidated by scene, camera or rendering-mode change');
      const variance=hasVariance?new Float32Array(mapped.slice(pixelBytes)):null,sampleCounts=new Uint32Array(width*height);
      for (let i = 0; i < pixels.length; i += 4) { const n = pixels[i + 3];sampleCounts[i/4]=n; for (let k = 0; k < 3; k++) {pixels[i + k] /= Math.max(1, n);if(variance)variance[i+k]=n>1?Math.max(0,variance[i+k])/(n*(n-1)):NaN;} pixels[i + 3] = 1; }
      const spectral = this.mode === 'path-spectral';
      const metadata = { mode: this.mode, referenceReady: false, colorSpace: spectral ? 'CIE XYZ' : 'linear Rec.709', spectral: spectral ? { wavelengthRange: [360,830], sampling: 'uniform hero wavelength', observer: 'CIE 1931 2 degree, 1 nm table', rgbUplift: 'PBRT v3 bases; illuminant white normalized to CIE Y=1', overrides: this.scene.materials.map(m=>m.spectra || {}) } : null, limitations: [...(spectral ? [] : ['RGB transport']), ...(this.mode === 'path-preview' ? ['12-bounce preview limit'] : ['eight-level medium stack', 'single-scattering GGX', 'authored normals only where connected; geometric fallback']), 'approximate Standard Surface mapping', 'bounded closure composition; approximate MaterialX subsurface and hair fiber lobes', 'scalar bump height fallback; no texture finite differences', 'bounded path ray-cone texture footprint; no full ray differentials', 'linear triangle displacement refinement, not Catmull-Clark', 'RGB graph working space is not ACEScg'], width, height, samples, adapter: this.stats.adapter, provenance: this.scene.provenance };
      metadata.seed=this.options.seed;
      metadata.limitations=metadata.limitations.map(v=>v==='bounded closure composition; approximate MaterialX subsurface and hair fiber lobes'?'16 surface lobes; bounded nested BSDF layers, no exact BSSRDF or Marschner hair':v);
      metadata.limitations=metadata.limitations.map(v=>v==='scalar bump height fallback; no texture finite differences'?'central-difference bump; local planar position/UV offsets, at most two nested derivative nodes':v==='path textures use level zero'?'bounded path ray-cone texture footprint; no full ray differentials':v);
      metadata.variance=hasVariance?'unbiased variance of the mean; NaN when fewer than two samples':null;
      if (format === 'float32') return { pixels, variance, sampleCounts, metadata };
      if (format !== 'exr') throw new Error('Expected exr or float32');
      return { bytes: encodeEXR(width, height, pixels, { channels: spectral ? ['X','Y','Z'] : ['R','G','B'] }), metadata };
    } finally { readback.destroy(); }
  }
  dispose() { if (this.disposed) return; this.disposed = true; this.sceneGeneration++; this.pendingBuild?.cancel(); this.resources.forEach(b => b.destroy()); this.paths?.destroy(); this.pathCounters?.destroy(); this.pathReadback?.destroy(); this.moments?.destroy(); this.accumulation?.destroy(); this.depth?.destroy(); this.rasterTarget?.destroy(); this.uniform.destroy(); this.context.unconfigure(); this.device.destroy(); }
}
