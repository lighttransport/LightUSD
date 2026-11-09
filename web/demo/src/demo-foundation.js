import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { HDRLoader } from 'three/examples/jsm/loaders/HDRLoader.js';
import { RectAreaLightUniformsLib } from 'three/examples/jsm/lights/RectAreaLightUniformsLib.js';
import { GUI } from 'lil-gui';

import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import { LightUSDLoaderUtils } from 'lightusd/LightUSDLoaderUtils.js';
import { LightUSDComposer } from 'lightusd/LightUSDComposer.js';
import {
  NodeGraphOptimizationLevel,
  analyzeNodeGraph,
  optimizeNodeGraph,
  setLightUSD as setMaterialXLightUSD
} from 'lightusd/LightUSDMaterialX.js';
import { MtlxMaterialConverter } from 'lightusd/LightUSDOpenPBR_WebGL.js';
import { extractSkinnedMeshData } from 'lightusd/USDSceneSkinningData.js';
import { buildSkeletonDataFromUSD } from 'lightusd/USDSkeletonData.js';
import { applyUSDSceneSkinningPipeline } from 'lightusd/USDSceneSkinningPipeline.js';
import { getUSDSceneMetadata } from 'lightusd/USDSceneMetadata.js';
import { buildNodeIndexMap } from 'lightusd/USDAnimationConverter.js';
import { extractUSDSceneAnimations } from 'lightusd/USDSceneAnimationPipeline.js';
import {
  buildNextThreeNode,
  isNextScene,
  nextCountsFromScene,
  readNextSceneMeta
} from 'lightusd-next-demo-utils';
import { Report } from './app-report.js';
import { applyUSDMaterialFeatures } from './usd-material-features.js';

RectAreaLightUniformsLib.init();

function bytesLabel(bytes) {
  if (!Number.isFinite(bytes)) return '';
  if (bytes > 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  if (bytes > 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${bytes} B`;
}

function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
}

function escapeHTML(value) {
  return String(value)
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;');
}

const BACKEND_CHOICES = ['next', 'legacy', 'auto'];

function selectedBackend() {
  const value = new URLSearchParams(window.location.search).get('backend');
  return BACKEND_CHOICES.includes(value) ? value : 'next';
}

function setBackendAndReload(backend) {
  const url = new URL(window.location.href);
  url.searchParams.set('backend', BACKEND_CHOICES.includes(backend) ? backend : 'next');
  window.location.href = url.toString();
}

function normalizedAssetKey(value) {
  return String(value || '').replaceAll('\\', '/').replace(/^\.\//, '').replace(/^\//, '');
}

export async function initDemo(config) {
  const root = document.getElementById('demo-root') || document.body;
  const app = new DemoApp(config, root);
  window.__lightusdDemoApp = app;
  await app.start();
  return app;
}

class DemoApp {
  constructor(config, root) {
    this.config = {
      useDefaultLights: true,
      preferredMaterialType: 'auto',
      ...config
    };
    this.root = root;
    this.lastRenderTime = performance.now();
    this.loader = null;
    this.currentUsd = null;
    this.currentLabel = '';
    this.currentSourceBytes = null;
    this.currentSourceName = '';
    this.currentSourceUrl = '';
    this.envMap = null;
    this.environmentSource = null;
    this.domeEnvironment = null;
    this.mixer = null;
    this.actions = [];
    this.skeletonHelpers = [];
    this.usdLights = [];
    this.nodeGraphStats = null;
    this.sceneChangedCallbacks = new Set();
    this.params = {
      grid: true,
      defaultLights: this.config.useDefaultLights !== false,
      envIntensity: 1.4,
      ambient: 0.35,
      keyLight: 2.0,
      play: true,
      speed: 1.0,
      showSkeleton: !!this.config.showSkeleton
    };
    this.params.environment = this.config.defaultEnvironment || 'goegap';
    this.params.backend = this.config.requiredBackend || selectedBackend();
  }

  async start() {
    document.title = `${this.config.title} - LightUSD Demo`;
    this.createShell();
    this.setupThree();
    this.setupEvents();
    this.setupGUI();
    await this.loadDefaultEnvironment();
    await this.loadDefaultAsset();
  }

  createShell() {
    this.root.innerHTML = `
      <div class="demo-shell">
        <header class="demo-toolbar">
          <div>
            <a class="demo-back" href="./">Demos</a>
            <h1>${escapeHTML(this.config.title)}</h1>
            <p>${escapeHTML(this.config.subtitle || '')}</p>
          </div>
          <div class="demo-actions">
            <button id="open-file" type="button">${escapeHTML(this.config.openFileLabel || 'Open USD')}</button>
            ${this.config.enableFolderInput ? '<button id="open-folder" type="button">Open Export Folder</button>' : ''}
            <button id="load-default" type="button">Load Sample</button>
            <button id="fit-scene" type="button">Fit</button>
          </div>
        </header>
        <main class="demo-main">
          <section class="viewport-wrap">
            <div id="viewport" class="viewport"></div>
            <div id="drop-hint" class="drop-hint">Drop USDA, USDC, USD, or USDZ</div>
              <div id="status" class="status">Initializing...</div>
              <div id="lightusd-loader" class="lightusd-loader hidden">
                <div class="loader-progress"><div class="loader-bar" style="width:10%"></div></div>
                <div class="loader-text">Loading LightUSD WASM…</div>
              </div>
            </section>
          <aside class="info-panel">
            <h2>Controls</h2>
            <div id="gui-container" class="gui-container"></div>
            <h2>Scene</h2>
            <dl id="scene-stats"></dl>
            <h2>Notes</h2>
            <div id="notes"></div>
          </aside>
        </main>
        <input id="file-input" type="file" accept=".usd,.usda,.usdc,.usdz" hidden>
        ${this.config.enableFolderInput ? '<input id="folder-input" type="file" webkitdirectory multiple hidden>' : ''}
      </div>
    `;

    this.viewport = this.root.querySelector('#viewport');
    this.statusEl = this.root.querySelector('#status');
    this.statsEl = this.root.querySelector('#scene-stats');
    this.notesEl = this.root.querySelector('#notes');
    this.guiContainer = this.root.querySelector('#gui-container');
    this.dropHint = this.root.querySelector('#drop-hint');
    this.fileInput = this.root.querySelector('#file-input');
    this.root.querySelector('#open-file').addEventListener('click', () => this.fileInput.click());
    this.folderInput = this.root.querySelector('#folder-input');
    this.root.querySelector('#open-folder')?.addEventListener('click', () => this.folderInput?.click());
    this.root.querySelector('#load-default').addEventListener('click', () => this.loadDefaultAsset());
    this.root.querySelector('#fit-scene').addEventListener('click', () => this.fitScene());
    this.setNotes([
      'Drag and drop a USD file anywhere on the viewport.',
      'Use orbit controls to navigate. Press F to fit the scene.',
      this.config.materialModeLabel ? `Material mode: ${this.config.materialModeLabel}.` : null
    ].filter(Boolean));
  }

  setupThree() {
    this.scene = new THREE.Scene();
    this.scene.background = new THREE.Color(0x0e0e10);

    this.world = new THREE.Group();
    this.world.name = 'LightUSDDemoWorld';
    this.scene.add(this.world);

    this.lightGroup = new THREE.Group();
    this.lightGroup.name = 'Lights';
    this.scene.add(this.lightGroup);

    this.grid = new THREE.GridHelper(10, 20, 0x44444a, 0x26262b);
    this.grid.position.y = -0.01;
    this.scene.add(this.grid);

    this.camera = new THREE.PerspectiveCamera(50, 1, 0.01, 1000);
    this.camera.position.set(3, 2.2, 4);

    this.renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false });
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;
    this.renderer.toneMapping = THREE.ACESFilmicToneMapping;
    this.renderer.toneMappingExposure = 1.0;
    this.renderer.shadowMap.enabled = true;
    this.viewport.appendChild(this.renderer.domElement);

    this.pmremGenerator = new THREE.PMREMGenerator(this.renderer);

    this.controls = new OrbitControls(this.camera, this.renderer.domElement);
    this.controls.enableDamping = true;
    this.controls.dampingFactor = 0.08;

    this.ambientLight = new THREE.AmbientLight(0xffffff, this.params.ambient);
    this.keyLight = new THREE.DirectionalLight(0xffffff, this.params.keyLight);
    this.keyLight.position.set(4, 6, 5);
    this.fillLight = new THREE.DirectionalLight(0x9fb7ff, 0.5);
    this.fillLight.position.set(-4, 3, -3);
    this.lightGroup.add(this.ambientLight, this.keyLight, this.fillLight);

    window.addEventListener('resize', () => this.resize());
    this.resize();
    this.renderer.setAnimationLoop(() => this.render());
  }

  setupEvents() {
    this.fileInput.addEventListener('change', () => {
      const file = this.fileInput.files?.[0];
      if (file) this.loadFile(file);
      this.fileInput.value = '';
    });
    this.folderInput?.addEventListener('change', () => {
      const files = Array.from(this.folderInput.files || []);
      if (files.length) this.loadFolder(files);
      this.folderInput.value = '';
    });

    this.viewport.addEventListener('dragenter', (event) => {
      event.preventDefault();
      this.dropHint.classList.add('active');
    });
    this.viewport.addEventListener('dragover', (event) => {
      event.preventDefault();
      this.dropHint.classList.add('active');
    });
    this.viewport.addEventListener('dragleave', () => {
      this.dropHint.classList.remove('active');
    });
    this.viewport.addEventListener('drop', (event) => {
      event.preventDefault();
      this.dropHint.classList.remove('active');
      const file = event.dataTransfer?.files?.[0];
      if (file) this.loadFile(file);
    });

    window.addEventListener('keydown', (event) => {
      if (event.key.toLowerCase() === 'f') {
        this.fitScene();
      }
    });
  }

  setupGUI() {
    this.gui = new GUI({ title: this.config.title, container: this.guiContainer });
    this.gui.add({ open: () => this.fileInput.click() }, 'open').name('Open USD');
    this.gui.add({ sample: () => this.loadDefaultAsset() }, 'sample').name('Load sample');
    this.gui.add({ fit: () => this.fitScene() }, 'fit').name('Fit scene (F)');
    this.gui.add(this.params, 'backend', BACKEND_CHOICES).name('Backend').onChange(setBackendAndReload);
    this.gui.add(this.params, 'grid').name('Grid').onChange((value) => {
      this.grid.visible = value;
    });
    this.gui.add(this.params, 'defaultLights').name('Default lights').onChange((value) => {
      this.lightGroup.visible = value;
    });
    this.gui.add(this.params, 'ambient', 0, 3, 0.01).name('Ambient').onChange((value) => {
      this.ambientLight.intensity = value;
    });
    this.gui.add(this.params, 'keyLight', 0, 8, 0.01).name('Key light').onChange((value) => {
      this.keyLight.intensity = value;
    });
    this.gui.add(this.params, 'envIntensity', 0, 8, 0.01).name('Env intensity').onChange(() => {
      this.applyEnvironmentToMaterials();
    });
    if (this.config.environmentSelector) {
      this.gui.add(this.params, 'environment', {
        'Off': 'off',
        'USD DomeLight': 'dome',
        'Goegap HDRI': 'goegap'
      }).name('Environment').onChange(() => this.applySelectedEnvironment());
    }

    if (this.config.enableSkinning) {
      this.gui.add(this.params, 'showSkeleton').name('Skeleton').onChange((value) => {
        for (const helper of this.skeletonHelpers) helper.visible = value;
      });
    }

    if (this.config.enableAnimation) {
      const anim = this.gui.addFolder('Animation');
      anim.add(this.params, 'play').name('Play');
      anim.add(this.params, 'speed', 0, 4, 0.01).name('Speed');
    }

    if (this.config.enableExport) {
      const exp = this.gui.addFolder('Export');
      exp.add({ USDA: () => this.exportCurrent('usda') }, 'USDA').name('Download USDA');
      exp.add({ USDC: () => this.exportCurrent('usdc') }, 'USDC').name('Download USDC');
      exp.add({ USDZ: () => this.exportCurrent('usdz') }, 'USDZ').name('Download USDZ');
    }
  }

  async ensureLoader() {
    if (this.loader) return this.loader;

    this.loader = new LightUSDLoader(null, { maxMemoryLimitMB: 512 });
    try {
      await Report.steps('Initializing LightUSD', [
        { label: 'Loading WASM module', run: () => this.loader.init({
          useZstdCompressedWasm: false,
          useMemory64: false,
          backend: this.params.backend
        })},
      ]);
    } catch (e) {
      Report.err(e, 'WASM initialization failed').action('Retry', () => this.ensureLoader());
      throw e;
    }
    LightUSDLoaderUtils.setLightUSD(this.loader.native_);
    setMaterialXLightUSD(this.loader.native_);
    this.loader.setMaxMemoryLimitMB(512);
    if (this.config.enableSkinning) {
      this.loader.setRoundBoneCount(true);
    }
    Report.done();
    return this.loader;
  }

  async loadDefaultEnvironment() {
    try {
      const texture = await new HDRLoader().loadAsync('./assets/textures/goegap_1k.hdr');
      texture.mapping = THREE.EquirectangularReflectionMapping;
      this.environmentSource?.dispose?.();
      this.envMap?.dispose?.();
      this.environmentSource = texture;
      this.envMap = this.pmremGenerator.fromEquirectangular(texture).texture;
      if (!this.config.environmentSelector || this.params.environment === 'goegap') {
        this.scene.environment = this.envMap;
        this.scene.background = this.environmentSource;
      } else {
        this.applySelectedEnvironment();
      }
    } catch (error) {
      console.warn('Default HDR environment unavailable:', error);
    }
  }

  async loadDefaultAsset() {
    if (!this.config.defaultAsset) return;
    await this.loadURL(this.config.defaultAsset, this.config.title);
  }

  async loadURL(url, label = url) {
    await this.ensureLoader();
    this.setStatus(`Loading ${label}...`);
    try {
      const data = await this.fetchBytes(url, label);
      Report.progress(20, 'Downloaded USD');
      this.currentSourceBytes = new Uint8Array(data);
      this.currentSourceName = url.split(/[?#]/)[0].split('/').pop() || 'scene.usd';
      this.currentSourceUrl = url;
      let usd;
      if (this.config.useComposition) {
        usd = this.params.backend === 'legacy'
          ? await this.loadComposedLayer(url)
          : await this.loadComposedNext(data, url);
      } else if (this.config.useLayerExport && this.params.backend === 'legacy') {
        usd = await this.loadLayerForExport(url);
      } else {
        Report.progress(40, 'Parsing USD');
        usd = await this.parseUSD(data, this.currentSourceName);
      }
      Report.progress(60, 'Building scene');
      await this.displayUSD(usd, label);
      Report.done();
    } catch (error) {
      console.error(error);
      this.setStatus(`Failed: ${error.message}`);
      Report.err(error, 'Loading failed').action('Retry', () => this.loadURL(url, label));
    }
  }

  async loadFile(file) {
    await this.ensureLoader();
    this.setStatus(`Reading ${file.name} (${bytesLabel(file.size)})...`);
    try {
      const data = new Uint8Array(await file.arrayBuffer());
      this.currentSourceBytes = new Uint8Array(data);
      this.currentSourceName = file.name;
      this.currentSourceUrl = '';
      const usd = this.config.useLayerExport && this.params.backend === 'legacy'
        ? this.parseLayerForExport(data, file.name)
        : await this.parseUSD(data, file.name);
      await this.displayUSD(usd, file.name);
      Report.done();
    } catch (error) {
      console.error(error);
      this.setStatus(`Failed: ${error.message}`);
      Report.err(error, `Loading ${file.name}`).action('Retry', () => this.loadFile(file));
    }
  }

  async loadFolder(files) {
    await this.ensureLoader();
    const byKey = new Map();
    for (const file of files) {
      const relative = normalizedAssetKey(file.webkitRelativePath || file.name);
      byKey.set(relative, file);
      byKey.set(normalizedAssetKey(file.name), file);
    }
    this.localExportFiles = byKey;
    const hero = files.find((file) => /(^|\/)MetaHuman_Hero\.usda$/i.test(file.webkitRelativePath || file.name));
    if (!hero) {
      throw new Error('The selected folder does not contain MetaHuman_Hero.usda.');
    }
    this.setStatus(`Reading MetaHuman export folder (${files.length} files)...`);
    try {
      const data = new Uint8Array(await hero.arrayBuffer());
      this.currentSourceBytes = new Uint8Array(data);
      this.currentSourceName = hero.name;
      this.currentSourceUrl = '';
      const usd = await this.loadComposedNextFromFiles(data, hero.webkitRelativePath || hero.name, byKey);
      await this.displayUSD(usd, 'MetaHuman export folder');
      Report.done();
    } catch (error) {
      console.error(error);
      this.setStatus(`Failed: ${error.message}`);
      Report.err(error, 'Loading MetaHuman export folder');
    }
  }

  async fetchBytes(url, label = url) {
    const response = await fetch(url);
    if (!response.ok) throw new Error(`HTTP ${response.status} loading ${label}`);
    const total = Number(response.headers.get('content-length')) || 0;
    if (!response.body) return new Uint8Array(await response.arrayBuffer());
    const reader = response.body.getReader();
    const chunks = [];
    let loaded = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
      loaded += value.length;
      const suffix = total ? `${bytesLabel(loaded)} / ${bytesLabel(total)}` : bytesLabel(loaded);
      this.setStatus(`Downloading ${label}: ${suffix}`);
    }
    const bytes = new Uint8Array(loaded);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.length;
    }
    return bytes;
  }

  nextParseOptions(extra = {}) {
    return {
      backend: this.params.backend,
      maxMemoryLimitMB: 512,
      materialDedup: false,
      mergeMeshes: false,
      mergeMeshesBakeTransform: false,
      flattenRenderTree: false,
      ...extra
    };
  }

  parseUSD(data, filename, extra = {}) {
    return new Promise((resolve, reject) => {
      this.loader.parse(data, filename, resolve, reject, this.nextParseOptions(extra));
    });
  }

  async loadComposedLayer(url) {
    const layer = await this.loader.loadAsLayerAsync(url);
    const composer = new LightUSDComposer();
    composer.setLayer(layer);
    composer.setUSDLoader(this.loader);
    composer.setBaseWorkingPath('./assets');
    composer.setAssetSearchPaths(['./assets']);
    await composer.progressiveComposition();
    const composed = composer.getLayer();
    composed.layerToRenderScene();
    return composed;
  }

  async loadComposedNext(rootBytes, rootUrl) {
    const Session = this.loader.native_?.NextFlattenSession;
    if (typeof Session !== 'function') {
      throw new Error('NextFlattenSession is unavailable in this WASM build.');
    }
    const session = new Session();
    const rootName = rootUrl.split(/[?#]/)[0].split('/').pop() || 'root.usda';
    const begin = session.begin(rootBytes, rootName, true);
    if (!begin?.success) {
      session.delete?.();
      throw new Error(begin?.error || 'Failed to start next composition.');
    }
    try {
      for (let iteration = 0; iteration < 64; iteration++) {
        const step = session.step(null);
        if (!step?.success) throw new Error(step?.error || 'Next composition failed.');
        if (step.status === 'need-layer') {
          const dependency = await this.fetchDependencyLayer(step.key, rootUrl);
          const provided = session.provideLayer(step.key, dependency);
          if (!provided?.success) {
            throw new Error(provided?.error || `Failed to provide ${step.key}.`);
          }
          continue;
        }
        if (step.status === 'done' && step.data) {
          return this.parseUSD(new Uint8Array(step.data), `${rootName}.flattened.usdc`);
        }
        throw new Error(`Unexpected next composition status: ${step.status}`);
      }
      throw new Error('Next composition exceeded the dependency-layer limit.');
    } finally {
      session.end();
      session.delete?.();
    }
  }

  async loadComposedNextFromFiles(rootBytes, rootName, filesByKey) {
    const Session = this.loader.native_?.NextFlattenSession;
    if (typeof Session !== 'function') throw new Error('NextFlattenSession is unavailable in this WASM build.');
    const session = new Session();
    const rootKey = normalizedAssetKey(rootName);
    const rootDirectory = rootKey.includes('/') ? rootKey.slice(0, rootKey.lastIndexOf('/') + 1) : '';
    const begin = session.begin(rootBytes, rootKey.split('/').pop(), true);
    if (!begin?.success) {
      session.delete?.();
      throw new Error(begin?.error || 'Failed to start local composition.');
    }
    try {
      for (let iteration = 0; iteration < 64; iteration++) {
        const step = session.step(null);
        if (!step?.success) throw new Error(step?.error || 'Local composition failed.');
        if (step.status === 'need-layer') {
          const key = normalizedAssetKey(step.key);
          const file = filesByKey.get(key) || filesByKey.get(normalizedAssetKey(rootDirectory + key));
          if (!file) throw new Error(`Missing referenced USD layer: ${step.key}`);
          const provided = session.provideLayer(step.key, new Uint8Array(await file.arrayBuffer()));
          if (!provided?.success) throw new Error(provided?.error || `Failed to provide ${step.key}.`);
          continue;
        }
        if (step.status === 'done' && step.data) {
          return this.parseUSD(new Uint8Array(step.data), `${rootKey}.flattened.usdc`);
        }
        throw new Error(`Unexpected local composition status: ${step.status}`);
      }
      throw new Error('Local composition exceeded the dependency-layer limit.');
    } finally {
      session.end();
      session.delete?.();
    }
  }

  async fetchDependencyLayer(key, rootUrl) {
    const candidates = [
      new URL(String(key), new URL(rootUrl, document.baseURI)).href,
      new URL(String(key), document.baseURI).href
    ];
    let lastError = null;
    for (const url of [...new Set(candidates)]) {
      try {
        return await this.fetchBytes(url, key);
      } catch (error) {
        lastError = error;
      }
    }
    throw lastError || new Error(`Could not resolve dependency layer ${key}.`);
  }

  async loadLayerForExport(url) {
    const layer = await this.loader.loadAsLayerAsync(url);
    layer.layerToRenderScene();
    return layer;
  }

  parseLayerForExport(data, filename) {
    const layer = new this.loader.native_.LightUSDLoaderNative();
    layer.setMaxMemoryLimitMB(512);
    this.loader._applySkinningLoadOptions?.(layer);
    const ok = layer.loadAsLayerFromBinary(data, filename);
    if (!ok) {
      throw new Error(layer.error?.() || `Failed to load ${filename} as USD layer.`);
    }
    layer.layerToRenderScene();
    return layer;
  }

  async displayUSD(usd, label) {
    this.clearSceneObjects();
    this.currentUsd = usd;
    this.currentLabel = label;
    this.applySceneUpAxis(usd);

    if (this.config.useUsdLux) {
      await this.applyDomeLight(usd);
    } else {
      this.scene.environment = this.envMap;
    }

    if (isNextScene(usd)) await this.hydrateNextExternalAssets(usd);
    const builtScene = await this.buildThreeRoot(usd);
    const sceneRoot = builtScene.node;

    if (this.config.materialBackend === 'nodegraph') {
      this.applyNodeGraphMaterials(sceneRoot);
    }

    if (this.config.enableSkinning || this.config.enableAnimation) {
      await this.applySkinningAndAnimation(usd, sceneRoot, builtScene.nodeIndexMap);
    } else {
      this.world.add(sceneRoot);
    }

    if (this.config.useUsdLux) {
      this.addUSDLights(usd);
    }

    this.applyEnvironmentToMaterials();
    this.updateStats(usd, label);
    if (this.config.enableMaterialGraph) {
      this.showMaterialGraph(usd);
    } else if (this.config.enableExport) {
      this.showExportNotes(usd);
    }
    this.fitScene();
    this.setStatus(`Loaded ${label}`);
    for (const callback of this.sceneChangedCallbacks) {
      try { callback({ usd, label, root: sceneRoot }); } catch (error) { console.error(error); }
    }
    // Scene extensions may need the copied mesh/curve arrays to build custom
    // GPU representations. Release them only after synchronous callbacks have
    // consumed the adapter data.
    if (!this.config.preserveBuildData && isNextScene(usd) && typeof usd.releaseBuildData === 'function') {
      usd.releaseBuildData();
    }
  }

  onSceneChanged(callback) {
    this.sceneChangedCallbacks.add(callback);
    return () => this.sceneChangedCallbacks.delete(callback);
  }

  async buildThreeRoot(usd) {
    if (isNextScene(usd)) {
      const built = buildNextThreeNode(usd, {
        skipTextures: false,
        lazyTextures: true,
        releaseBuildData: false,
        onProgress: (info) => this.setStatus(info.message || 'Building next scene...')
      });
      if (built.textureManager) {
        await built.textureManager.startLoading({
          concurrency: LightUSDLoaderUtils.defaultTextureConcurrency(),
          yieldInterval: 16,
          onTextureLoaded: (material, texture, task) => {
            for (const binding of task?.bindings || []) {
              if (binding.material === material) applyUSDMaterialFeatures(material, binding.mapProperty);
            }
          },
          onProgress: (info) => this.setStatus(
            `Loading textures ${info.loaded + info.failed}/${info.total}`)
        });
      }
      built.node.traverse((object) => {
        const materials = object.material ? (Array.isArray(object.material) ? object.material : [object.material]) : [];
        materials.forEach((material) => applyUSDMaterialFeatures(material));
      });
      return built;
    }
    const root = new THREE.Group();
    root.name = 'USD Scene';
    const defaultMaterial = LightUSDLoaderUtils.createDefaultMaterial();
    defaultMaterial.envMap = this.config.environmentSelector ? this.scene.environment : (this.scene.environment || this.envMap);
    const options = {
      overrideMaterial: false,
      preferredMaterialType: this.config.preferredMaterialType,
      envMap: this.config.environmentSelector ? this.scene.environment : (this.scene.environment || this.envMap),
      envMapIntensity: this.params.envIntensity,
      textureCache: new Map(),
      onProgress: (info) => this.setStatus(info.message || 'Building scene...')
    };

    const rootCount = usd.numRootNodes ? Math.max(usd.numRootNodes(), 1) : 1;
    for (let i = 0; i < rootCount; i++) {
      const usdNode = rootCount > 1 && usd.getRootNode ? usd.getRootNode(i) : usd.getDefaultRootNode();
      const threeNode = await LightUSDLoaderUtils.buildThreeNode(usdNode, defaultMaterial, usd, options);
      root.add(threeNode);
    }
    root.traverse((object) => {
      const materials = object.material ? (Array.isArray(object.material) ? object.material : [object.material]) : [];
      materials.forEach((material) => applyUSDMaterialFeatures(material));
    });
    return { node: root, nodeIndexMap: null };
  }

  async hydrateNextExternalAssets(usd) {
    if (!this.currentSourceUrl || !usd?.archiveEntries) return;
    const records = Array.isArray(usd.textures) ? usd.textures : [];
    let indexedTextures = [];
    let assetIndexBase = null;
    if (this.config.externalAssetIndex) {
      try {
        const indexUrl = new URL(this.config.externalAssetIndex, document.baseURI);
        const response = await fetch(indexUrl);
        if (response.ok) {
          const index = await response.json();
          indexedTextures = Array.isArray(index?.textures) ? index.textures : [];
          assetIndexBase = new URL('.', indexUrl);
        }
      } catch (error) {
        console.warn('[demo] external asset index unavailable', error);
      }
    }
    for (const record of records) {
      const assetPath = record?.assetPath || record?.uri;
      const key = normalizedAssetKey(assetPath);
      if (!key || usd.archiveEntries.has(key)) continue;
      const indexedMatches = indexedTextures.filter((candidate) => {
        const normalized = normalizedAssetKey(candidate);
        return normalized === key || normalized.endsWith(`/${key}`);
      });
      const candidates = indexedMatches.map((candidate) => new URL(candidate, assetIndexBase).href);
      candidates.push(
        new URL(assetPath, new URL(this.currentSourceUrl, document.baseURI)).href,
        new URL(assetPath, document.baseURI).href
      );
      for (const url of [...new Set(candidates)]) {
        try {
          const response = await fetch(url);
          if (!response.ok) continue;
          // Vite's SPA fallback answers an unknown image URL with index.html
          // and status 200. Never cache that document as texture bytes.
          const contentType = response.headers.get('content-type') || '';
          if (/text\/html|application\/json/i.test(contentType)) continue;
          usd.archiveEntries.set(key, new Uint8Array(await response.arrayBuffer()));
          break;
        } catch {
          // Try the next resolver candidate.
        }
      }
    }
  }

  getSceneUpAxis(usd) {
    if (isNextScene(usd)) return String(readNextSceneMeta(usd).upAxis || 'Y').toUpperCase();
    try {
      const metadata = getUSDSceneMetadata(usd);
      const axis = metadata.fileUpAxis || metadata.upAxis || (usd?.getUpAxis ? usd.getUpAxis() : 'Y');
      return String(axis || 'Y').toUpperCase();
    } catch {
      return usd?.getUpAxis ? String(usd.getUpAxis() || 'Y').toUpperCase() : 'Y';
    }
  }

  applySceneUpAxis(usd) {
    this.currentUpAxis = this.getSceneUpAxis(usd);
    this.world.rotation.set(0, 0, 0);
    if (this.currentUpAxis === 'Z') {
      this.world.rotation.x = -Math.PI / 2;
    }
    this.world.updateMatrixWorld(true);
  }

  applyNodeGraphMaterials(sceneRoot) {
    const converter = new MtlxMaterialConverter();
    let converted = 0;
    let inspected = 0;

    sceneRoot.traverse((object) => {
      if (!object.isMesh || !object.material) return;
      const originalMaterials = Array.isArray(object.material) ? object.material : [object.material];
      const nextMaterials = originalMaterials.map((material) => {
        inspected++;
        const materialData = material.userData?.openPBRData || material.userData?.rawData;
        const openPBR = materialData?.openPBR || materialData?.openPBRShader || materialData;
        const nodeGraph = openPBR?.nodeGraph || materialData?.nodeGraph;
        if (!nodeGraph?.nodegraph) return material;

        const optimized = optimizeNodeGraph(nodeGraph, NodeGraphOptimizationLevel.STANDARD);
        const convertedMaterial = converter.createMaterial({
          ...materialData,
          openPBR: {
            ...openPBR,
            nodeGraph: optimized
          }
        }, {
          loadTextures: false
        });
        convertedMaterial.name = material.name || convertedMaterial.name;
        convertedMaterial.side = material.side;
        this.copyTextureMaps(material, convertedMaterial);
        if ('envMap' in convertedMaterial) convertedMaterial.envMap = this.config.environmentSelector ? this.scene.environment : (this.scene.environment || this.envMap);
        if ('envMapIntensity' in convertedMaterial) convertedMaterial.envMapIntensity = this.params.envIntensity;
        convertedMaterial.userData = {
          ...material.userData,
          materialBackend: 'MaterialX WebGL2 NodeGraph',
          optimizedNodeGraph: optimized
        };
        converted++;
        return convertedMaterial;
      });
      object.material = Array.isArray(object.material) ? nextMaterials : nextMaterials[0];
    });

    this.nodeGraphStats = { converted, inspected };
  }

  copyTextureMaps(sourceMaterial, targetMaterial) {
    const textureMapNames = [
      'map',
      'normalMap',
      'roughnessMap',
      'metalnessMap',
      'emissiveMap',
      'alphaMap',
      'aoMap',
      'bumpMap',
      'displacementMap',
      'clearcoatMap',
      'clearcoatNormalMap',
      'clearcoatRoughnessMap',
      'sheenColorMap',
      'sheenRoughnessMap',
      'specularColorMap',
      'iridescenceMap',
      'iridescenceThicknessMap',
      'transmissionMap',
      'thicknessMap'
    ];

    for (const mapName of textureMapNames) {
      if (!targetMaterial[mapName] && sourceMaterial[mapName]) {
        targetMaterial[mapName] = sourceMaterial[mapName];
      }
    }

    if (sourceMaterial.normalScale && !targetMaterial.normalScale) {
      targetMaterial.normalScale = sourceMaterial.normalScale.clone?.() || sourceMaterial.normalScale;
    }
    targetMaterial.needsUpdate = true;
  }

  async applySkinningAndAnimation(usd, threeNode, nextNodeIndexMap = null) {
    const metadata = getUSDSceneMetadata(usd);
    const skinningData = extractSkinnedMeshData(usd, { logger: console, verbose: false });
    const skeletonData = buildSkeletonDataFromUSD(usd, {
      logger: console,
      hasSkinnedMeshData: skinningData.hasSkinnedMeshData
    });

    const nodeIndexMap = nextNodeIndexMap || buildNodeIndexMap(threeNode);
    const skinningResult = applyUSDSceneSkinningPipeline({
      threeNode,
      characterGroup: this.world,
      helperScene: this.world,
      skeletonDataArray: skeletonData.skeletonDataArray,
      allSkinnedMeshUSDData: skinningData.allSkinnedMeshUSDData,
      skinnedMeshDataByName: skinningData.skinnedMeshDataByName,
      usdScene: usd,
      showMesh: true,
      showSkeleton: this.params.showSkeleton,
      useWASMBoneTexture: false,
      logger: console
    });
    this.skeletonHelpers = skinningResult.skeletonHelpers || [];

    if (this.config.enableAnimation) {
      const animData = extractUSDSceneAnimations(usd, {
        boneMaps: skeletonData.boneMaps,
        nodeIndexMap,
        timeCodesPerSecond: metadata.timeCodesPerSecond,
        logger: console
      });
      const clips = [...animData.usdAnimations, ...animData.usdNodeAnimations];
      if (clips.length > 0) {
        this.mixer = new THREE.AnimationMixer(this.world);
        this.mixer.timeScale = metadata.timeCodesPerSecond;
        this.actions = clips.map((clip) => {
          const action = this.mixer.clipAction(clip);
          action.play();
          return action;
        });
        this.setNotes([
          `${clips.length} animation clip(s) loaded.`,
          'Use the GUI Animation folder to pause or change playback speed.'
        ]);
      }
    }
  }

  async applyDomeLight(usd) {
    try {
      const dome = await LightUSDLoaderUtils.loadDomeLightFromUSD(usd, this.pmremGenerator);
      if (dome?.texture) {
        this.domeEnvironment = dome;
        if (this.config.environmentSelector) {
          this.applySelectedEnvironment();
          return;
        }
        this.scene.environment = dome.texture;
        this.scene.background = dome.sourceTexture || dome.texture;
        this.params.envIntensity = dome.intensity || this.params.envIntensity;
        return;
      }
    } catch (error) {
      console.warn('DomeLight load failed:', error);
    }
    this.domeEnvironment = null;
    if (this.config.environmentSelector) this.applySelectedEnvironment();
    else {
      this.scene.environment = this.envMap;
      this.scene.background = this.environmentSource || new THREE.Color(0x0e0e10);
    }
  }

  applySelectedEnvironment() {
    if (!this.config.environmentSelector) return;
    const mode = this.params.environment;
    if (mode === 'dome' && this.domeEnvironment?.texture) {
      this.scene.environment = this.domeEnvironment.texture;
      this.scene.background = this.domeEnvironment.sourceTexture || new THREE.Color(0x0e0e10);
      this.params.envIntensity = this.domeEnvironment.intensity || 1;
    } else if (mode === 'goegap' && this.envMap) {
      this.scene.environment = this.envMap;
      this.scene.background = this.environmentSource || new THREE.Color(0x0e0e10);
    } else {
      this.scene.environment = null;
      this.scene.background = new THREE.Color(0x0e0e10);
    }
    this.applyEnvironmentToMaterials();
  }

  addUSDLights(usd) {
    if (!usd?.numLights) return;
    const numLights = usd.numLights();
    for (let i = 0; i < numLights; i++) {
      const lightData = usd.getLight(i);
      if (!lightData || lightData.error) continue;
      const type = String(lightData.type || '').toLowerCase();
      if (type === 'dome' || type === 'domelight') continue;

      let intensity = lightData.intensity ?? 1;
      if (lightData.exposure) intensity *= Math.pow(2, lightData.exposure);
      const color = new THREE.Color(
        lightData.color?.[0] ?? 1,
        lightData.color?.[1] ?? 1,
        lightData.color?.[2] ?? 1
      );
      const position = new THREE.Vector3(
        lightData.position?.[0] ?? 0,
        lightData.position?.[1] ?? 0,
        lightData.position?.[2] ?? 0
      );
      const quaternion = new THREE.Quaternion();
      if (lightData.transform?.length === 16) {
        const matrix = new THREE.Matrix4();
        matrix.fromArray(lightData.transform);
        matrix.decompose(position, quaternion, new THREE.Vector3());
      }

      let light = null;
      if (type === 'point' || type === 'sphere') {
        if (lightData.shapingConeAngle && lightData.shapingConeAngle < 90) {
          light = new THREE.SpotLight(color, intensity, 0, THREE.MathUtils.degToRad(lightData.shapingConeAngle), lightData.shapingConeSoftness || 0);
          light.target.position.set(0, 0, -1);
          light.add(light.target);
        } else {
          light = new THREE.PointLight(color, intensity);
        }
      } else if (type === 'distant' || type === 'directional') {
        light = new THREE.DirectionalLight(color, intensity);
        light.position.copy((lightData.direction ? new THREE.Vector3(...lightData.direction) : new THREE.Vector3(0, -1, 0)).multiplyScalar(-5));
      } else if (type === 'rect' || type === 'disk') {
        light = new THREE.RectAreaLight(color, intensity, lightData.width || lightData.radius || 1, lightData.height || lightData.radius || 1);
      }

      if (!light) continue;
      light.name = lightData.name || `USDLight_${i}`;
      if (type !== 'distant' && type !== 'directional') {
        light.position.copy(position);
        light.quaternion.copy(quaternion);
      }
      this.world.add(light);
      this.usdLights.push(light);
    }
  }

  showMaterialGraph(usd) {
    const lines = [];
    const numMaterials = usd.numMaterials ? usd.numMaterials() : 0;
    for (let i = 0; i < numMaterials; i++) {
      try {
        const result = usd.getMaterialWithFormat(i, 'json');
        const data = result?.data ? JSON.parse(result.data) : usd.getMaterial(i);
        const label = data.name || data.displayName || `Material ${i}`;
        const open = data.openPBR || data.openPBRShader || data.openPBRSurface || data;
        const nodeGraph = open?.nodeGraph || data.nodeGraph;
        if (nodeGraph?.nodegraph) {
          const analysis = analyzeNodeGraph(nodeGraph);
          const optimized = optimizeNodeGraph(nodeGraph, NodeGraphOptimizationLevel.STANDARD);
          const nodes = nodeGraph.nodegraph.nodes?.length || 0;
          const outputs = nodeGraph.nodegraph.outputs?.length || 0;
          const optimizedNodes = optimized.nodegraph?.nodes?.length || nodes;
          const reduction = analysis.potentialReduction;
          const analysisText = analysis.error || (
            reduction
              ? `${reduction.patternsFound} pattern(s), ${reduction.identitiesFound} identity op(s), ${reduction.nodesRemovable} removable node(s).`
              : 'No optimizer analysis available.'
          );
          lines.push(
            `<strong>${escapeHTML(label)}</strong><br>` +
            `<code>${nodes} node(s), ${outputs} output(s), ${optimizedNodes} after optimize. ` +
            `${escapeHTML(analysisText)}</code>`
          );
        } else {
          const keys = Object.keys(open || {}).slice(0, 12);
          lines.push(`<strong>${escapeHTML(label)}</strong><br><code>${escapeHTML(keys.join(' -> ') || 'material data')}</code>`);
        }
      } catch (error) {
        lines.push(`Material ${i}: ${escapeHTML(error.message)}`);
      }
    }
    if (this.nodeGraphStats) {
      lines.unshift(
        `<strong>Renderer</strong><br><code>${this.nodeGraphStats.converted} of ${this.nodeGraphStats.inspected} mesh material slot(s) using MaterialX WebGL2 node graph evaluation.</code>`
      );
    }
    if (lines.length > 0) {
      this.setNotes(lines);
    }
  }

  exportCurrent(kind) {
    if (!this.currentUsd) {
      this.setStatus('No USD scene loaded.');
      return;
    }
    try {
      if (isNextScene(this.currentUsd)) {
        this.exportCurrentNext(kind);
        return;
      }
      if (kind === 'usda') {
        const data = typeof this.currentUsd.exportAsUSDA === 'function'
          ? this.currentUsd.exportAsUSDA()
          : this.currentUsd.layerToString?.();
        if (!data) throw new Error(this.currentUsd.error?.() || 'USDA export returned empty data.');
        downloadBlob(new Blob([data], { type: 'text/plain' }), 'lightusd-export.usda');
      } else if (kind === 'usdc') {
        if (typeof this.currentUsd.exportAsUSDC !== 'function') {
          this.setStatus('USDC export is not available in this WASM build.');
          return;
        }
        const bytes = new Uint8Array(this.currentUsd.exportAsUSDC());
        downloadBlob(new Blob([bytes], { type: 'application/octet-stream' }), 'lightusd-export.usdc');
      } else if (kind === 'usdz') {
        if (typeof this.currentUsd.exportAsUSDZ !== 'function') {
          this.setStatus('USDZ export is not available in this WASM build.');
          return;
        }
        const bytes = new Uint8Array(this.currentUsd.exportAsUSDZ());
        downloadBlob(new Blob([bytes], { type: 'model/vnd.usdz+zip' }), 'lightusd-export.usdz');
      }
      this.setStatus(`Exported ${kind.toUpperCase()}.`);
    } catch (error) {
      console.error(error);
      this.setStatus(`Export failed: ${error.message}`);
    }
  }

  exportCurrentNext(kind) {
    const Exporter = this.loader.native_?.NextUSDZConverterNative;
    if (typeof Exporter !== 'function' || !this.currentSourceBytes) {
      throw new Error('The next exporter or original USD bytes are unavailable.');
    }
    const exporter = new Exporter();
    try {
      if (!exporter.loadFromBinary(this.currentSourceBytes, this.currentSourceName || 'scene.usd')) {
        throw new Error(exporter.error?.() || 'Next exporter could not load the scene.');
      }
      if (kind === 'usda') {
        const data = exporter.exportAsUSDA();
        if (!data) throw new Error(exporter.error?.() || 'USDA export failed.');
        downloadBlob(new Blob([data], { type: 'text/plain' }), 'lightusd-export.usda');
      } else {
        const method = kind === 'usdc' ? 'exportAsUSDC' : 'exportAsUSDZ';
        const bytes = new Uint8Array(exporter[method]());
        if (!bytes.length) throw new Error(exporter.error?.() || `${kind.toUpperCase()} export failed.`);
        const type = kind === 'usdz' ? 'model/vnd.usdz+zip' : 'application/octet-stream';
        downloadBlob(new Blob([bytes], { type }), `lightusd-export.${kind}`);
      }
      this.setStatus(`Exported ${kind.toUpperCase()}.`);
    } finally {
      exporter.delete?.();
    }
  }

  showExportNotes(usd) {
    const hasNextExporter = isNextScene(usd) &&
      typeof this.loader.native_?.NextUSDZConverterNative === 'function' &&
      !!this.currentSourceBytes;
    const hasUSDA = hasNextExporter || typeof usd.exportAsUSDA === 'function' || typeof usd.layerToString === 'function';
    const hasUSDC = hasNextExporter || typeof usd.exportAsUSDC === 'function';
    const hasUSDZ = hasNextExporter || typeof usd.exportAsUSDZ === 'function';
    this.setNotes([
      hasUSDA ? 'USDA export is available for the loaded layer.' : 'USDA export is not available for the loaded scene.',
      hasUSDC && hasUSDZ
        ? 'USDC and USDZ export bindings are available in this WASM build.'
        : 'USDC and USDZ buttons are kept for newer WASM builds; this local build exposes USDA layer export only.'
    ]);
  }

  applyEnvironmentToMaterials() {
    const env = this.config.environmentSelector ? this.scene.environment : (this.scene.environment || this.envMap);
    this.world.traverse((object) => {
      const materials = object.material ? (Array.isArray(object.material) ? object.material : [object.material]) : [];
      for (const material of materials) {
        if ('envMap' in material) material.envMap = env;
        if ('envMapIntensity' in material) material.envMapIntensity = this.params.envIntensity;
        material.needsUpdate = true;
      }
    });
  }

  clearSceneObjects() {
    if (this.currentUsd && typeof this.currentUsd.releaseBuildData === 'function') {
      this.currentUsd.releaseBuildData();
    }
    this.world.clear();
    this.world.rotation.set(0, 0, 0);
    for (const light of this.usdLights) {
      this.scene.remove(light);
    }
    this.usdLights = [];
    for (const helper of this.skeletonHelpers) {
      this.scene.remove(helper);
    }
    this.skeletonHelpers = [];
    this.nodeGraphStats = null;
    if (this.mixer) this.mixer.stopAllAction();
    this.mixer = null;
    this.actions = [];
    this.scene.background = new THREE.Color(0x0e0e10);
    this.scene.environment = this.config.environmentSelector ? null : this.envMap;
  }

  updateStats(usd, label) {
    const nextCounts = isNextScene(usd) ? nextCountsFromScene(usd) : null;
    const countValue = (getter) => {
      if (typeof getter !== 'function') {
        return Number.isFinite(getter) ? getter : 0;
      }
      try {
        return Number.isFinite(getter.call(usd)) ? getter.call(usd) : 0;
      } catch {
        return 0;
      }
    };

    const arrayValue = (getter) => {
      if (typeof getter !== 'function') {
        return null;
      }
      try {
        const value = getter.call(usd);
        if (Array.isArray(value)) return value.length;
        if (Number.isFinite(value)) return value;
      } catch {
        // ignore
      }
      return null;
    };

    const unsupportedCountFromNum = countValue(
      usd?.numUnsupportedRenderables && usd.numUnsupportedRenderables.bind(usd)
    );
    const unsupportedRenderables = Number.isFinite(unsupportedCountFromNum)
      ? unsupportedCountFromNum
      : arrayValue(
          usd?.getUnsupportedRenderables && usd.getUnsupportedRenderables.bind(usd)
        ) || 0;

    const countNumber = (getter) => {
      const value = countValue(getter);
      return Number.isFinite(value) ? value : 0;
    };

    const stats = {
      Source: label,
      Backend: isNextScene(usd) ? 'next' : 'legacy',
      'Up axis': this.currentUpAxis || this.getSceneUpAxis(usd),
      Meshes: nextCounts?.meshes ?? countNumber(usd?.numMeshes && usd.numMeshes.bind(usd)),
      Materials: nextCounts?.materials ?? countNumber(usd?.numMaterials && usd.numMaterials.bind(usd)),
      Textures: nextCounts?.textures ?? countNumber(usd?.numTextures && usd.numTextures.bind(usd)),
      Images: countNumber(usd?.numImages && usd.numImages.bind(usd)),
      Lights: nextCounts?.lights ?? countNumber(usd?.numLights && usd.numLights.bind(usd)),
      Cameras: nextCounts?.cameras ?? countNumber(usd?.numCameras && usd.numCameras.bind(usd)),
      Nodes: nextCounts?.nodes ?? countNumber(usd?.numNodes && usd.numNodes.bind(usd)),
      PointInstancers: nextCounts?.pointInstancers ?? countNumber(usd?.numPointInstancers && usd.numPointInstancers.bind(usd)),
      PointInstanceDraws: nextCounts?.pointInstanceDraws ?? countNumber(usd?.numPointInstanceDraws && usd.numPointInstanceDraws.bind(usd)),
      Skeletons: nextCounts?.skeletons ?? countNumber(usd?.numSkeletons && usd.numSkeletons.bind(usd)),
      Animations: nextCounts?.animations ?? countNumber(usd?.numAnimations && usd.numAnimations.bind(usd)),
      UnsupportedRenderables: unsupportedRenderables
    };
    this.statsEl.innerHTML = Object.entries(stats)
      .map(([key, value]) => `<dt>${escapeHTML(key)}</dt><dd>${escapeHTML(value)}</dd>`)
      .join('');
  }

  setNotes(lines) {
    this.notesEl.innerHTML = lines.map((line) => `<p>${line}</p>`).join('');
  }

  setStatus(message) {
    this.statusEl.textContent = message;
  }

  fitScene() {
    const box = new THREE.Box3().setFromObject(this.world);
    if (box.isEmpty()) return;
    const size = box.getSize(new THREE.Vector3());
    const center = box.getCenter(new THREE.Vector3());
    const maxDim = Math.max(size.x, size.y, size.z, 0.1);
    const fitHeightDistance = maxDim / (2 * Math.atan((Math.PI * this.camera.fov) / 360));
    const fitWidthDistance = fitHeightDistance / this.camera.aspect;
    const distance = Math.max(fitHeightDistance, fitWidthDistance) * 1.45;
    const direction = new THREE.Vector3(0.8, 0.55, 1).normalize();
    this.camera.position.copy(center).add(direction.multiplyScalar(distance));
    this.camera.near = Math.max(distance / 100, 0.001);
    this.camera.far = distance * 100;
    this.camera.updateProjectionMatrix();
    this.controls.target.copy(center);
    this.controls.update();
  }

  resize() {
    const rect = this.viewport.getBoundingClientRect();
    const width = Math.max(1, rect.width);
    const height = Math.max(1, rect.height);
    this.camera.aspect = width / height;
    this.camera.updateProjectionMatrix();
    this.renderer.setSize(width, height, false);
  }

  render() {
    const now = performance.now();
    const delta = Math.min((now - this.lastRenderTime) / 1000, 0.1);
    this.lastRenderTime = now;
    if (this.mixer && this.params.play) {
      this.mixer.update(delta * this.params.speed);
    }
    this.controls.update();
    if (typeof this.renderOverride === 'function') this.renderOverride();
    else this.renderer.render(this.scene, this.camera);
  }
}
