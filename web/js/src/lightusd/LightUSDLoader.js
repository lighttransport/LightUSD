import {transferCoreChunks,responseChunks} from './CoreStreamTransfer.js';
import { Loader } from 'three'; // or https://cdn.jsdelivr.net/npm/three/build/three.module.js';
import { parseUSDZEntries } from '../usdzconvert.js';
import { copyWasmArray, markOwnedFloat32Array } from './TypedArrayOwnership.js';

// lightusd module are dynamically imported at LightUSDLoader

// Simple fileLoader both works for nodejs and the browser.
class FileFetcher {
  constructor() {
    this.is_node = typeof process !== "undefined" &&
      process.versions != null &&
      process.versions.node != null;

    this.fs = null;
    this.path = null;
    this.initialized = false;
  }

  async init() {
    if (this.initialized) return;

    if (this.is_node) {
      try {
        const { createRequire } = await import("module");
        const require = createRequire(import.meta.url);
        this.fs = require('fs');
        this.path = require('path');
      } catch (error) {
        console.warn('Failed to initialize Node.js modules:', error);
        this.is_node = false;
      }
    }
    this.initialized = true;
  }

  // Return: Object with arrayBuffer() method that returns Promise<ArrayBuffer>
  async fetch(url) {
    await this.init();

    // Check if this is a blob URL - always use fetch for blob URLs
    const isBlobUrl = url.startsWith('blob:');

    if (this.is_node && !isBlobUrl) {
      // Node.js environment - use fs.readFileSync for file paths
      try {
        if (url.startsWith('file://')) {
          url = url.substring(7); // Remove file:// prefix
        }

        const data = this.fs.readFileSync(url);

        // Return an object with arrayBuffer() method for consistency with browser File API
        // Convert Node.js Buffer to ArrayBuffer
        const arrayBuffer = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);

        return {
          arrayBuffer: async () => arrayBuffer
        };
      } catch (error) {
        throw new Error(`Failed to read file: ${url} - ${error.message}`);
      }
    } else {
      // Browser environment or blob URL - use XMLHttpRequest for better large file handling
      return new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        xhr.open('GET', url, true);
        xhr.responseType = 'arraybuffer';

        xhr.onload = function() {
          if (xhr.status === 200 || xhr.status === 206) {
            const arrayBuffer = xhr.response;
            // Return object with arrayBuffer method for consistency
            resolve({
              arrayBuffer: async () => arrayBuffer
            });
          } else {
            reject(new Error(`Failed to fetch: ${xhr.statusText}`));
          }
        };

        xhr.onerror = function() {
          reject(new Error(`Network error fetching: ${url}`));
        };

        xhr.send();
      });
    }
  }
}

class FetchAssetResolver {
    constructor() {
        this.assetCache = new Map();

        this.fetcher = new FileFetcher();
    }

    async resolveAsync(uri) {
        try {
            const response = await fetch(uri, {
                cache: 'no-store',
                headers: {
                    'Accept': '*/*',
                }
            });
            if (!response.ok && response.status !== 206) {
                throw new Error(`Failed to fetch asset: ${uri}`);
            }
            const data = await response.arrayBuffer();
            this.assetCache.set(uri, data);
            return Promise.resolve([uri, data]);
        } catch (error) {
            console.error(`Error resolving asset ${uri}:`, error);
            throw error;
        }
    }

    getAsset(uri) {
        if (this.assetCache.has(uri)) {
            return this.assetCache.get(uri);
        } else {
            console.warn(`Asset not found in cache: ${uri}`);
            return null;
        }
    }

    hasAsset(uri) {
        return this.assetCache.has(uri);
    }

    setAsset(uri, data) {
        this.assetCache.set(uri, data);
    }

    clearCache() {
        this.assetCache.clear();
    }

}

function nextAnimationFrame() {
    return new Promise((resolve) => {
        // Dedicated workers may expose requestAnimationFrame, but it can be
        // throttled to multi-second intervals when the owning page is not
        // visible (headless/Xvfb and background tabs). Worker conversion only
        // needs to yield to its task queue; reserve rAF for the window where it
        // actually synchronizes UI progress with painting.
        if (typeof document !== 'undefined' &&
            typeof requestAnimationFrame === 'function') {
            requestAnimationFrame(() => resolve());
        } else {
            setTimeout(resolve, 0);
        }
    });
}

function nextCrateProgressLocalPercentage(info = {}) {
    const phase = String(info.phase || '');
    const ratio = Number.isFinite(info.percentage)
        ? Math.max(0, Math.min(100, info.percentage)) / 100
        : 0;
    const ranged = (base, span) => base + ratio * span;
    switch (phase) {
        case 'bootstrap': return 24;
        case 'toc': return 26;
        case 'tokens': return 28;
        case 'strings': return 30;
        case 'fields': return 32;
        case 'fieldsets': return 34;
        case 'specs': return 36;
        case 'paths': return 38;
        case 'stage': return 40;
        case 'stage.prims': return ranged(40, 3);
        case 'stage.properties': return ranged(43, 3);
        case 'stage.hierarchy': return ranged(46, 2);
        case 'complete': return 48;
        default: return ranged(24, 24);
    }
}

export class NextRenderSceneAdapter {
    constructor(native, renderStream, options = {}) {
        this.__backend = 'next';
        this.native = native;
        this.renderStream = renderStream;
        this.filename = options.filename || '';
        this.archiveEntries = options.archiveEntries || new Map();
        this.meshes = options.meshes || [];
        this.points = options.points || [];
        this.curves = options.curves || [];
        this.nodes = options.nodes || [];
        this.lights = options.lights || [];
        this.cameras = options.cameras || [];
        this.pointInstancers = options.pointInstancers || [];
        this.pointInstanceDraws = options.pointInstanceDraws || [];
        this.skeletons = options.skeletons || [];
        this.unsupportedRenderables = options.unsupportedRenderables || [];
        this.animations = options.animations || [];
        this.animationInfos = options.animationInfos || [];
        this.stats = options.stats || {};
        this.materialKeys = new Set();
        this.textureKeys = new Set();
        this.materials = [];
        this.textures = [];
        this._rootNodes = null;
        this.meshCountValue = this.meshes.length;
        this.pointsCountValue = Number.isFinite(options.pointsCount)
            ? options.pointsCount
            : this.points.length;
        this.curvesCountValue = Number.isFinite(options.curvesCount)
            ? options.curvesCount
            : this.curves.length;
        this.nodeCountValue = Number.isFinite(options.nodeCount)
            ? options.nodeCount
            : this.nodes.length;
        this.lightCountValue = Number.isFinite(options.lightCount)
            ? options.lightCount
            : this.lights.length;
        this.cameraCountValue = Number.isFinite(options.cameraCount)
            ? options.cameraCount
            : this.cameras.length;
        this.pointInstancerCountValue = Number.isFinite(options.pointInstancerCount)
            ? options.pointInstancerCount
            : this.pointInstancers.length;
        this.pointInstanceDrawCountValue = Number.isFinite(options.pointInstanceDrawCount)
            ? options.pointInstanceDrawCount
            : this.pointInstanceDraws.length;
        this.skeletonCountValue = Number.isFinite(options.skeletonCount)
            ? options.skeletonCount
            : this.skeletons.length;
        this.animationCountValue = Number.isFinite(options.animationCount)
            ? options.animationCount
            : this.animations.length;
        this.unsupportedRenderableCountValue = Number.isFinite(options.unsupportedRenderableCount)
            ? options.unsupportedRenderableCount
            : this.unsupportedRenderables.length;
        this.sceneMetadata = {
            upAxis: options.upAxis || 'Y',
            metersPerUnit: options.metersPerUnit || 1.0,
            framesPerSecond: Number.isFinite(options.framesPerSecond) && options.framesPerSecond > 0
                ? options.framesPerSecond : undefined,
            timeCodesPerSecond: Number.isFinite(options.timeCodesPerSecond) && options.timeCodesPerSecond > 0
                ? options.timeCodesPerSecond : undefined,
            startTimeCode: Number.isFinite(options.startTimeCode) ? options.startTimeCode : undefined,
            endTimeCode: Number.isFinite(options.endTimeCode) ? options.endTimeCode : undefined
        };
        this.fallbackReason = options.fallbackReason || '';

        const materialByKey = new Map();
        const textureByKey = new Map();
        const addTexturePath = (path, role = '') => {
            if (!path) return;
            const key = this._normTexPath(path);
            if (!key) return;
            this.textureKeys.add(key);
            if (!textureByKey.has(key)) {
                textureByKey.set(key, {
                    index: textureByKey.size,
                    name: key.split('/').pop() || key,
                    assetPath: key,
                    uri: path,
                    role
                });
            }
        };
        const addMaterial = (entry) => {
            if (!entry) return;
            const material = entry.material || entry;
            const texturePaths = entry.texturePaths || {};
            const key = entry.materialKey || material.key ||
                (Number.isFinite(entry.materialId) && entry.materialId >= 0 ? `id:${entry.materialId}` : '');
            const materialKey = key || JSON.stringify({ material, texturePaths });
            this.materialKeys.add(materialKey);
            if (!materialByKey.has(materialKey)) {
                materialByKey.set(materialKey, {
                    index: materialByKey.size,
                    id: Number.isFinite(entry.materialId) ? entry.materialId :
                        (Number.isFinite(material.id) ? material.id : -1),
                    key: materialKey,
                    primPath: material.primPath || '',
                    name: material.name || material.primPath || materialKey,
                    material: { ...material },
                    texturePaths: { ...texturePaths }
                });
            }
            for (const [role, path] of Object.entries(texturePaths)) {
                addTexturePath(path, role);
            }
        };

        for (const mesh of this.meshes) {
            addMaterial({
                material: mesh.material,
                texturePaths: mesh.texturePaths || {},
                materialId: Number.isFinite(mesh.materialId) ? mesh.materialId : -1,
                materialKey: mesh.materialKey || ''
            });
            for (const path of Object.values(mesh.texturePaths || {})) {
                addTexturePath(path);
            }
            for (const material of mesh.materials || []) {
                addMaterial(material);
                for (const path of Object.values(material.texturePaths || {})) {
                    addTexturePath(path);
                }
            }
        }
        this.materials = Array.from(materialByKey.values());
        this.textures = Array.from(textureByKey.values());
        this.materialCountValue = this.materialKeys.size || this.meshCountValue;
        this.textureCountValue = this.textureKeys.size;
    }

    static _isUsdName(name) {
        return /\.(usd|usda|usdc)$/i.test(name || '');
    }

    static _rootEntry(entries) {
        // USDZ defines the first archive entry as its default layer. Do not
        // prefer a later USDC dependency over an earlier USDA root.
        return entries.find((entry) => this._isUsdName(entry.name));
    }

    static async create(native, bytes, filename = 'scene.usdz', options = {}) {
        if (!native || typeof native.RenderStream !== 'function') {
            throw new Error('LightUSD next backend is unavailable in this WASM module.');
        }

        const onProgress = typeof options.onProgress === 'function'
            ? options.onProgress
            : null;
        const progressBase = Number.isFinite(options.progressBase)
            ? options.progressBase
            : 0;
        const progressRange = Number.isFinite(options.progressRange)
            ? options.progressRange
            : 100;
        const checkAbort = () => {
            if (options.signal?.aborted) throw new DOMException('Parsing aborted', 'AbortError');
        };
        checkAbort();
        const report = (stage, localPercentage, message, extra = {}) => {
            checkAbort();
            if (!onProgress) return;
            const p = Math.max(0, Math.min(100, Number(localPercentage) || 0));
            onProgress({
                backend: 'next',
                stage,
                percentage: progressBase + (p / 100) * progressRange,
                localPercentage: p,
                message,
                ...extra
            });
            checkAbort();
        };
        const yieldForProgress = async () => {
            checkAbort();
            if (onProgress || options.signal) await nextAnimationFrame();
            checkAbort();
        };
        const now = () => globalThis.performance?.now?.() ?? Date.now();
        const createStart = now();
        const timings = {
            archiveMs: 0,
            nativeBeginMs: 0,
            animationCopyMs: 0,
            entityCopyMs: 0,
            meshCopyMs: 0,
            nativeMeshGetMs: 0,
            jsMeshCopyMs: 0,
            meshUdimMs: 0,
            meshYieldMs: 0,
            totalMs: 0
        };
        const archiveStart = now();

        const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
        let crate = u8;
        let rootAssetName = '';
        const archiveEntries = new Map();

        report('archive', 0, 'Preparing next backend input...');
        await yieldForProgress();

        if (/\.usdz$/i.test(filename)) {
            report('archive', 4, 'Reading USDZ archive...');
            const entries = parseUSDZEntries(u8);
            const root = this._rootEntry(entries);
            if (!root) {
                throw new Error('LightUSD next backend could not find a USD root layer in the USDZ archive.');
            }
            rootAssetName = this._normTexPathStatic(root.name);
            // RenderStream consumes a root layer, not the surrounding archive.
            // Pass either USDA or USDC bytes directly; archive entries remain
            // available below for textures and dependent USD layers.
            crate = root.data;
            let copiedEntries = 0;
            for (const entry of entries) {
                if (!entry.name.endsWith('/')) {
                    archiveEntries.set(this._normTexPathStatic(entry.name), entry.data);
                }
                copiedEntries++;
                if ((copiedEntries & 255) === 0) {
                    report('archive', 4 + Math.min(16, (copiedEntries / Math.max(1, entries.length)) * 16),
                        `Indexing USDZ assets ${copiedEntries}/${entries.length}`,
                        { archiveCurrent: copiedEntries, archiveTotal: entries.length });
                    await yieldForProgress();
                }
            }
            report('archive', 20, `Indexed USDZ assets ${entries.length}/${entries.length}`,
                { archiveCurrent: entries.length, archiveTotal: entries.length });
        }
        timings.archiveMs = now() - archiveStart;

        const renderStream = new native.RenderStream();
        let beginResult;
        try {
            if (options.maxMemoryLimitMB !== undefined)
                renderStream.setMaxMemoryLimitMB(options.maxMemoryLimitMB);
            if (options.materialDedup !== undefined &&
                typeof renderStream.setMaterialDedup === 'function') {
                renderStream.setMaterialDedup(!!options.materialDedup);
            }
            if (options.mergeMeshes !== undefined &&
                typeof renderStream.setMeshMerge === 'function') {
                renderStream.setMeshMerge(!!options.mergeMeshes);
            }
            if (options.mergeMeshesBakeTransform !== undefined &&
                typeof renderStream.setMeshMergeBakeTransform === 'function') {
                renderStream.setMeshMergeBakeTransform(!!options.mergeMeshesBakeTransform);
            }
            if (options.flattenRenderTree !== undefined &&
                typeof renderStream.setFlattenRenderTree === 'function') {
                renderStream.setFlattenRenderTree(!!options.flattenRenderTree);
            }
            if (options.meshOnly !== undefined &&
                typeof renderStream.setMeshOnly === 'function') {
                renderStream.setMeshOnly(!!options.meshOnly);
            }
            if (options.computeTangents !== undefined &&
                typeof renderStream.setComputeTangents === 'function') {
                renderStream.setComputeTangents(!!options.computeTangents);
            }
            if (options.buildVertexIndices !== undefined &&
                typeof renderStream.setBuildVertexIndices === 'function') {
                renderStream.setBuildVertexIndices(!!options.buildVertexIndices);
            }
            if (options.tangentMethod !== undefined &&
                typeof renderStream.setTangentMethod === 'function') {
                renderStream.setTangentMethod(String(options.tangentMethod));
            }
            // Supply every package entry to the resolver, including images.
            // The root identifier anchors references and authored texture paths.
            if (archiveEntries.size && typeof native.NextAssetStore === 'function') {
                const store = new native.NextAssetStore();
                try {
                    store.setMemoryLimitBytes(Math.min(options.maxMemoryLimitMB ?? 512, 1024) * 1024 * 1024);
                    const slash = rootAssetName.lastIndexOf('/');
                    if (slash >= 0) store.setBaseWorkingPath(rootAssetName.slice(0, slash));
                    for (const [name, bytes] of archiveEntries) {
                        if (name !== rootAssetName) store.registerMemoryAsset(name, bytes);
                    }
                    renderStream.importAssetStore(store);
                } finally { store.delete(); }
            }
            // Variant selection: the next compositor keys overrides by variant
            // SET name (applies to every prim carrying that set).
            if (options.variantSelection && options.variantSelection.variantSet &&
                typeof renderStream.setVariantOverride === 'function') {
                renderStream.setVariantOverride(
                    String(options.variantSelection.variantSet),
                    String(options.variantSelection.variantName ?? ''));
            }
            report('native-load', 24, 'Starting USD crate reader...');
            await yieldForProgress();
            const nativeBeginStart = now();
            // Native callbacks are module-global. Install them only across the
            // synchronous native call, so concurrent async loads cannot retain
            // or restore each other's callbacks across JavaScript yields.
            const previousNextCrateProgress = native.onNextCrateProgress;
            if (onProgress) {
                native.onNextCrateProgress = (info = {}) => {
                    const phase = info.phase || 'crate';
                    const total = Number(info.total);
                    const current = Number(info.current);
                    const count = Number.isFinite(total) && total > 0
                        ? ` ${Math.min(current, total)}/${total}`
                        : '';
                    report('native-load',
                        nextCrateProgressLocalPercentage(info),
                        `Loading USD crate: ${phase}${count}`,
                        { cratePhase: phase, crateCurrent: current, crateTotal: total });
                };
            }
            try {
                beginResult = typeof native._lightusd_wasm_render_begin_named === 'function'
                    ? renderStream.begin(crate, rootAssetName || filename)
                    : renderStream.begin(crate);
            } finally {
                if (onProgress) native.onNextCrateProgress = previousNextCrateProgress;
            }
            if (!beginResult || !beginResult.success) {
                const error = beginResult?.error || renderStream.error?.() || 'RenderStream begin failed';
                throw new Error(error);
            }
            report('native-load', 48, 'Constructed render stream.');
            await yieldForProgress();
            timings.nativeBeginMs = now() - nativeBeginStart;

            // Surface authored variant sets in the legacy extractVariants()
            // shape: [{primPath, variantSets: [{name, selection, options}]}].
            if (typeof options.onVariants === 'function' &&
                typeof renderStream.listVariants === 'function') {
                const byPrim = new Map();
                for (const entry of renderStream.listVariants()) {
                    if (!entry || !entry.primPath) continue;
                    if (!byPrim.has(entry.primPath)) {
                        byPrim.set(entry.primPath, { primPath: entry.primPath, variantSets: [] });
                    }
                    byPrim.get(entry.primPath).variantSets.push({
                        name: entry.setName,
                        selection: entry.selected || '',
                        options: Array.from(entry.variants || [])
                    });
                }
                options.onVariants(Array.from(byPrim.values()));
            }

            const metadata = typeof renderStream.getSceneMetadata === 'function'
                ? renderStream.getSceneMetadata()
                : {};
            const meshOnly = options.meshOnly === true;
            const meshCount = beginResult.meshCount ?? renderStream.meshCount();
            const nodeCount = meshOnly ? 0 : Number.isFinite(beginResult.nodeCount)
                ? beginResult.nodeCount
                : (typeof renderStream.nodeCount === 'function'
                    ? renderStream.nodeCount()
                    : 0);
            const lightCount = meshOnly ? 0 : Number.isFinite(beginResult.lightCount)
                ? beginResult.lightCount
                : (typeof renderStream.lightCount === 'function'
                    ? renderStream.lightCount()
                    : 0);
            const pointsCount = meshOnly ? 0 : Number.isFinite(beginResult.pointsCount)
                ? beginResult.pointsCount
                : (typeof beginResult.points === 'number' ? beginResult.points
                    : (typeof renderStream.numPoints === 'function'
                        ? renderStream.numPoints()
                        : 0));
            const curvesCount = meshOnly ? 0 : Number.isFinite(beginResult.curvesCount)
                ? beginResult.curvesCount
                : (typeof beginResult.curves === 'number' ? beginResult.curves
                    : (typeof renderStream.numCurves === 'function'
                        ? renderStream.numCurves()
                        : 0));
            const cameraCount = meshOnly ? 0 : Number.isFinite(beginResult.cameraCount)
                ? beginResult.cameraCount
                : (typeof renderStream.cameraCount === 'function'
                    ? renderStream.cameraCount()
                    : 0);
            const animationCount = meshOnly ? 0 : Number.isFinite(beginResult.animationCount)
                ? beginResult.animationCount
                : (typeof beginResult.animations === 'number' ? beginResult.animations
                    : (typeof renderStream.numAnimations === 'function'
                        ? renderStream.numAnimations()
                        : 0));
            const pointInstancerCount = meshOnly ? 0 : Number.isFinite(beginResult.pointInstancerCount)
                ? beginResult.pointInstancerCount
                : (typeof renderStream.pointInstancerCount === 'function'
                    ? renderStream.pointInstancerCount()
                    : 0);
            const skeletonCount = meshOnly ? 0 : Number.isFinite(beginResult.skeletonCount)
                ? beginResult.skeletonCount
                : (typeof renderStream.skeletonCount === 'function'
                    ? renderStream.skeletonCount()
                : 0);
            const unsupportedRenderableCount = meshOnly ? 0 : Number.isFinite(
                beginResult.unsupportedRenderableCount)
                ? beginResult.unsupportedRenderableCount
                : (typeof renderStream.unsupportedRenderableCount === 'function'
                    ? renderStream.unsupportedRenderableCount()
                    : 0);
            const pointInstanceDrawCount = meshOnly ? 0 : Number.isFinite(
                beginResult.pointInstanceDrawCount)
                ? beginResult.pointInstanceDrawCount
                : (typeof renderStream.pointInstanceDrawCount === 'function'
                    ? renderStream.pointInstanceDrawCount()
                    : 0);

            const animationCopyStart = now();
            const animations = [];
            for (let i = 0; i < animationCount; i++) {
                const getAnimation = typeof renderStream.getAnimationView === 'function'
                    ? renderStream.getAnimationView.bind(renderStream)
                    : (typeof renderStream.getAnimation === 'function'
                        ? renderStream.getAnimation.bind(renderStream)
                        : null);
                if (getAnimation) {
                    const item = getAnimation(i);
                    if (!item || item.error) {
                        if (item?.error) {
                            console.warn(`NextRenderSceneAdapter: getAnimation(${i}) returned ${item.error}`);
                        }
                        continue;
                    }
                    animations[i] = typeof renderStream.getAnimationView === 'function'
                        ? this._copyAnimationView(native, item)
                        : item;
                }
            }
            timings.animationCopyMs = now() - animationCopyStart;

            const entityCopyStart = now();
            const animationInfos = [];
            if (typeof renderStream.getAllAnimationInfos === 'function') {
                const items = renderStream.getAllAnimationInfos();
                if (Array.isArray(items)) {
                    for (let i = 0; i < items.length; ++i) {
                        if (items[i]) animationInfos[i] = items[i];
                    }
                }
            }
            if (animationInfos.length === 0 &&
                typeof renderStream.getAnimationInfo === 'function') {
                for (let i = 0; i < animationCount; i++) {
                    const item = renderStream.getAnimationInfo(i);
                    if (!item || item.error) {
                        continue;
                    }
                    animationInfos[i] = item;
                }
            }

            const nodes = [];
            for (let i = 0; i < nodeCount; i++) {
                if (typeof renderStream.getNode === 'function') {
                    const item = renderStream.getNode(i);
                    if (!item || item.error) {
                        if (item?.error) {
                            console.warn(`NextRenderSceneAdapter: getNode(${i}) returned ${item.error}`);
                        }
                        continue;
                    }
                    nodes[i] = item;
                }
            }
            const lights = [];
            for (let i = 0; i < lightCount; i++) {
                if (typeof renderStream.getLight === 'function') {
                    const item = renderStream.getLight(i);
                    if (!item || item.error) {
                        if (item?.error) {
                            console.warn(`NextRenderSceneAdapter: getLight(${i}) returned ${item.error}`);
                        }
                        continue;
                    }
                    lights[i] = item;
                }
            }
            const points = [];
            for (let i = 0; i < pointsCount; i++) {
                if (typeof renderStream.getPoints === 'function') {
                    const item = renderStream.getPoints(i);
                    if (!item || item.error) {
                        if (item?.error) {
                            console.warn(`NextRenderSceneAdapter: getPoints(${i}) returned ${item.error}`);
                        }
                        continue;
                    }
                    points[i] = this._copyPoints(native, item, i);
                }
            }
            const curves = [];
            for (let i = 0; i < curvesCount; i++) {
                if (typeof renderStream.getCurves === 'function') {
                    const item = renderStream.getCurves(i);
                    if (!item || item.error) {
                        if (item?.error) {
                            console.warn(`NextRenderSceneAdapter: getCurves(${i}) returned ${item.error}`);
                        }
                        continue;
                    }
                    curves[i] = this._copyCurves(native, item, i);
                }
            }
            const cameras = [];
            for (let i = 0; i < cameraCount; i++) {
                if (typeof renderStream.getCamera === 'function') {
                    const item = renderStream.getCamera(i);
                    if (!item || item.error) {
                        if (item?.error) {
                            console.warn(`NextRenderSceneAdapter: getCamera(${i}) returned ${item.error}`);
                        }
                        continue;
                    }
                    cameras[i] = item;
                }
            }
            const pointInstancers = [];
            for (let i = 0; i < pointInstancerCount; i++) {
                if (typeof renderStream.getPointInstancer === 'function') {
                    const item = renderStream.getPointInstancer(i);
                    if (!item || item.error) {
                        if (item?.error) {
                            console.warn(`NextRenderSceneAdapter: getPointInstancer(${i}) returned ${item.error}`);
                        }
                        continue;
                    }
                    pointInstancers[i] = item;
                }
            }
            const pointInstanceDraws = [];
            for (let i = 0; i < pointInstanceDrawCount; i++) {
                if (typeof renderStream.getPointInstanceDraw === 'function') {
                    const item = renderStream.getPointInstanceDraw(i);
                    if (!item || item.error) {
                        if (item?.error) {
                            console.warn(`NextRenderSceneAdapter: getPointInstanceDraw(${i}) returned ${item.error}`);
                        }
                        continue;
                    }
                    pointInstanceDraws[i] = item;
                }
            }
            const skeletons = [];
            for (let i = 0; i < skeletonCount; i++) {
                if (typeof renderStream.getSkeleton === 'function') {
                    const item = renderStream.getSkeleton(i);
                    if (!item || item.error) {
                        if (item?.error) {
                            console.warn(`NextRenderSceneAdapter: getSkeleton(${i}) returned ${item.error}`);
                        }
                        continue;
                    }
                    skeletons[i] = item;
                }
            }
            const unsupportedRenderables = typeof renderStream.getUnsupportedRenderables === 'function'
                ? renderStream.getUnsupportedRenderables()
                : [];
            timings.entityCopyMs = now() - entityCopyStart;
            const meshCopyStart = now();
            const meshes = [];
            for (let i = 0; i < meshCount; i++) {
                if (i === 0 || (i & 31) === 0) {
                    report('mesh-copy',
                        50 + Math.min(45, (i / Math.max(1, meshCount)) * 45),
                        `Materializing meshes ${i}/${meshCount}`,
                        { meshCurrent: i, meshTotal: meshCount });
                    const yieldStart = now();
                    await yieldForProgress();
                    timings.meshYieldMs += now() - yieldStart;
                }
                const meshGetStart = now();
                const mesh = renderStream.getMesh(i);
                timings.nativeMeshGetMs += now() - meshGetStart;
                if (!mesh || mesh.error) {
                    throw new Error(mesh?.error || `RenderStream mesh ${i} failed`);
                }
                const meshJsCopyStart = now();
                const copiedMesh = this._copyMesh(native, mesh, i);
                if (typeof renderStream.meshColorsBuffer === 'function')
                    copiedMesh.vertexColors = renderStream.meshColorsBuffer(i);
                if (typeof renderStream.meshOpacitiesBuffer === 'function')
                    copiedMesh.opacities = renderStream.meshOpacitiesBuffer(i);
                timings.jsMeshCopyMs += now() - meshJsCopyStart;
                const udimStart = now();
                this._applyUDIMLayout(copiedMesh, archiveEntries);
                timings.meshUdimMs += now() - udimStart;
                meshes.push(copiedMesh);
            }
            report('mesh-copy', 95, `Materialized meshes ${meshCount}/${meshCount}`,
                { meshCurrent: meshCount, meshTotal: meshCount });
            await yieldForProgress();
            timings.meshCopyMs = now() - meshCopyStart;
            const stats = typeof renderStream.getStats === 'function'
                ? renderStream.getStats()
                : {};
            timings.totalMs = now() - createStart;
            stats.timings = timings;
            report('complete', 100, 'Core scene ready.');
            try { renderStream.end(); } catch (_) {}
            try { renderStream.delete(); } catch (_) {}
            return new NextRenderSceneAdapter(native, null, {
                filename,
                archiveEntries,
                meshes,
                points,
                curves,
                nodes,
                lights,
                cameras,
                pointInstancers,
                pointInstanceDraws,
                skeletons,
                unsupportedRenderables,
                nodeCount,
                pointsCount,
                curvesCount,
                lightCount,
                cameraCount,
                pointInstancerCount,
                pointInstanceDrawCount,
                skeletonCount,
                unsupportedRenderableCount,
                animationCount,
                animations,
                animationInfos,
                stats,
                upAxis: metadata.upAxis || 'Y',
                metersPerUnit: (typeof metadata.metersPerUnit === 'number' && metadata.metersPerUnit > 0)
                    ? metadata.metersPerUnit
                    : 1.0,
                framesPerSecond: metadata.framesPerSecond,
                timeCodesPerSecond: metadata.timeCodesPerSecond,
                startTimeCode: metadata.startTimeCode,
                endTimeCode: metadata.endTimeCode
            });
        } catch (error) {
            try { renderStream.end(); } catch (_) {}
            try { renderStream.delete(); } catch (_) {}
            throw error;
        }
    }

    static _copyAnimationView(native, animation) {
        const copyFloat = (value, label) => {
            if (value && Number.isFinite(value.ptr) && Number.isFinite(value.length)) {
                return copyWasmArray(native, value, Float32Array, label);
            }
            return markOwnedFloat32Array(new Float32Array(value || []), label);
        };
        const channels = Array.isArray(animation.channels) ? animation.channels : [];
        const samplers = Array.isArray(animation.samplers)
            ? animation.samplers.map((sampler, index) => {
                const copied = {
                    ...sampler,
                    index: Number.isFinite(sampler?.index) ? sampler.index : index,
                    times: copyFloat(sampler?.times, `animation.samplers[${index}].times`),
                    values: copyFloat(sampler?.values, `animation.samplers[${index}].values`)
                };
                if (sampler?.arrayValues) {
                    copied.arrayValues = copyFloat(
                        sampler.arrayValues,
                        `animation.samplers[${index}].arrayValues`);
                }
                return copied;
            })
            : [];
        const tracks = channels.map((channel, index) => {
            const sampler = samplers[channel?.sampler];
            let type = 'number';
            if (channel?.path === 'Translation' || channel?.path === 'Scale') {
                type = channel.isSkeletal ? 'vector3Array' : 'vector3';
            } else if (channel?.path === 'Rotation') {
                type = channel.isSkeletal ? 'quaternionArray' : 'quaternion';
            } else if (channel?.path === 'Weights') {
                type = channel.isSkeletal ? 'weightArray' : 'number';
            }
            const track = {
                sampler: channel?.sampler ?? index,
                target_node: channel?.target_node ?? -1,
                path: channel?.path || '',
                name: channel?.path || '',
                interpolation: sampler?.interpolation || 'LINEAR',
                times: sampler?.times,
                values: sampler?.values,
                isSkeletal: !!channel?.isSkeletal,
                propertyName: channel?.propertyName || '',
                targetSkeletonId: channel?.skeleton_id ?? -1,
                targetSkeletonPath: channel?.targetSkeletonPath || '',
                jointRemap: channel?.jointRemap || [],
                valueStride: channel?.valueStride ?? sampler?.valueStride ?? 0,
                elementCount: channel?.elementCount ?? sampler?.elementCount ?? 0,
                type
            };
            if (sampler?.arrayValues) track.arrayValues = sampler.arrayValues;
            return track;
        });
        return { ...animation, channels, samplers, tracks };
    }

    static _copyMesh(native, mesh, index) {
        const copy = (desc, Type) => copyWasmArray(native, desc, Type);
        const normalizeMaterial = (source) => {
            const material = source || {};
            const texturePaths = {
                baseColor: material.baseColorTexture || '',
                normal: material.normalTexture || '',
                roughness: material.roughnessTexture || '',
                metallic: material.metallicTexture || '',
                occlusion: material.occlusionTexture || '',
                emissive: material.emissiveTexture || '',
                opacity: material.opacityTexture || ''
            };
            const textureMetadata = material.textureMetadata || {};
            return {
                material: {
                    id: Number.isFinite(material.id) ? material.id : -1,
                    key: material.key || '',
                    primPath: material.primPath || '',
                    baseColor: Array.isArray(material.baseColor) ? material.baseColor : [0.8, 0.8, 0.8],
                    metallic: typeof material.metallic === 'number' ? material.metallic : 0,
                    roughness: typeof material.roughness === 'number' ? material.roughness : 0.5,
                    opacity: typeof material.opacity === 'number' ? material.opacity : 1,
                    emissive: Array.isArray(material.emissive) ? material.emissive : [0, 0, 0],
                    occlusion: typeof material.occlusion === 'number' ? material.occlusion : 1,
                    opacityThreshold: typeof material.opacityThreshold === 'number' ? material.opacityThreshold : -1,
                    textureMetadata,
                    shaderType: material.shaderType || '',
                    materialXConfig: material.materialXConfig || null,
                    materialXJson: material.materialXJson || '',
                    openPBRNodeGraphJson: material.openPBRNodeGraphJson || ''
                },
                texturePaths,
                materialId: Number.isFinite(material.id) ? material.id : -1,
                materialKey: material.key || JSON.stringify({ material, texturePaths, textureMetadata }),
                textureMetadata
            };
        };
        const primary = normalizeMaterial(mesh.material);
        const subsetMaterials = Array.isArray(mesh.materials)
            ? mesh.materials.map((material) => normalizeMaterial(material))
            : [];
        const submeshes = Array.isArray(mesh.submeshes)
            ? mesh.submeshes
                .map((g) => ({
                    start: Number.isFinite(g?.start) ? g.start : 0,
                    count: Number.isFinite(g?.count) ? g.count : 0,
                    materialIndex: Number.isFinite(g?.materialIndex) ? g.materialIndex : 0
                }))
                .filter((g) => g.count > 0)
            : [];
        const blendShapes = Array.isArray(mesh.blendShapes)
            ? mesh.blendShapes.map((shape) => ({
                name: shape?.name || '',
                weight: Number(shape?.weight) || 0,
                pointOffsets: new Float32Array(shape?.pointOffsets || []),
                normalOffsets: new Float32Array(shape?.normalOffsets || []),
                pointIndices: new Uint32Array(shape?.pointIndices || []),
                inbetweens: Array.isArray(shape?.inbetweens)
                    ? shape.inbetweens.map((entry) => ({
                        name: entry?.name || '',
                        weight: Number(entry?.weight) || 0,
                        pointOffsets: new Float32Array(entry?.pointOffsets || [])
                    })) : []
            })) : [];
        return {
            index,
            primName: mesh.primName || `mesh_${index}`,
            primPath: mesh.primPath || '',
            purpose: mesh.purpose || 'default',
            doubleSided: !!mesh.doubleSided,
            points: copy(mesh.points, Float32Array),
            indices: copy(mesh.indices, Uint32Array),
            normals: copy(mesh.normals, Float32Array),
            vertexColors: copy(mesh.vertexColors, Float32Array),
            tangents: copy(mesh.tangents, Float32Array),
            tangentMethod: mesh.tangentMethod || '',
            uv0: copy(mesh.uv0, Float32Array),
            jointIndices: copy(mesh.jointIndices, Uint16Array),
            jointWeights: copy(mesh.jointWeights, Float32Array),
            skel_id: Number.isFinite(mesh.skel_id) ? mesh.skel_id : -1,
            skeletonPath: mesh.skeletonPath || '',
            elementSize: Number.isFinite(mesh.elementSize) ? mesh.elementSize : 0,
            absPath: mesh.primPath || '',
            hasGeomBindTransform: !!mesh.hasGeomBindTransform,
            geomBindTransform: Array.isArray(mesh.geomBindTransform)
                ? mesh.geomBindTransform.slice(0, 16)
                : null,
            localMatrix: Array.isArray(mesh.localMatrix) ? mesh.localMatrix.slice(0, 16) : null,
            worldMatrix: Array.isArray(mesh.worldMatrix) ? mesh.worldMatrix.slice(0, 16) : null,
            material: primary.material,
            texturePaths: primary.texturePaths,
            materialId: Number.isFinite(mesh.materialId) ? mesh.materialId : primary.materialId,
            materialKey: primary.materialKey,
            materials: subsetMaterials,
            submeshes,
            blendShapes
        };
    }

    static _copyPoints(native, points, index) {
        const copy = (desc, Type) => desc instanceof Type
            ? (desc.length ? markOwnedFloat32Array(new Float32Array(desc)) : null)
            : copyWasmArray(native, desc, Type);
        return {
            index,
            name: points.name || `points_${index}`,
            primPath: points.primPath || '',
            pointCount: Number.isFinite(points.pointCount) ? points.pointCount : 0,
            materialId: Number.isFinite(points.materialId) ? points.materialId : -1,
            points: copy(points.points, Float32Array),
            widths: copy(points.widths, Float32Array),
            colors: copy(points.colors, Float32Array),
            hasBounds: !!points.hasBounds,
            bboxMin: Array.isArray(points.bboxMin) ? points.bboxMin.slice(0, 3) : null,
            bboxMax: Array.isArray(points.bboxMax) ? points.bboxMax.slice(0, 3) : null
        };
    }

    static _copyCurves(native, curves, index) {
        const copy = (desc, Type) => desc instanceof Type
            ? (desc.length ? markOwnedFloat32Array(new Float32Array(desc)) : null)
            : copyWasmArray(native, desc, Type);
        return {
            index,
            name: curves.name || `curves_${index}`,
            primPath: curves.primPath || '',
            curveCount: Number.isFinite(curves.curveCount) ? curves.curveCount : 0,
            controlPointCount: Number.isFinite(curves.controlPointCount) ? curves.controlPointCount : 0,
            tessellatedPointCount: Number.isFinite(curves.tessellatedPointCount)
                ? curves.tessellatedPointCount : 0,
            type: curves.type || 'cubic',
            basis: curves.basis || 'bezier',
            wrap: curves.wrap || 'nonperiodic',
            isNurbs: !!curves.isNurbs,
            isHermite: !!curves.isHermite,
            materialId: Number.isFinite(curves.materialId) ? curves.materialId : -1,
            widthsInterpolation: curves.widthsInterpolation || 'constant',
            colorsInterpolation: curves.colorsInterpolation || 'constant',
            opacitiesInterpolation: curves.opacitiesInterpolation || 'constant',
            curveVertexCounts: Array.from(curves.curveVertexCounts || []),
            tessellatedVertexCounts: Array.from(curves.tessellatedVertexCounts || []),
            points: copy(curves.points, Float32Array),
            widths: copy(curves.widths, Float32Array),
            colors: copy(curves.colors, Float32Array),
            tessellatedPoints: copy(curves.tessellatedPoints, Float32Array),
            tessellatedWidths: copy(curves.tessellatedWidths, Float32Array),
            tessellatedColors: copy(curves.tessellatedColors, Float32Array),
            opacities: copy(curves.opacities, Float32Array),
            tessellatedOpacities: copy(curves.tessellatedOpacities, Float32Array),
            hasBounds: !!curves.hasBounds,
            bboxMin: Array.isArray(curves.bboxMin) ? curves.bboxMin.slice(0, 3) : null,
            bboxMax: Array.isArray(curves.bboxMax) ? curves.bboxMax.slice(0, 3) : null
        };
    }

    static _normTexPathStatic(path) {
        return String(path || '').replace(/^[./]+/, '');
    }

    static _isUDIMPath(path) {
        return /<udim>|%\(udim\)d/i.test(String(path || ''));
    }

    static _udimTilePath(path, tileId) {
        return String(path || '')
            .replace(/<udim>/ig, String(tileId))
            .replace(/%\(udim\)d/ig, String(tileId));
    }

    static _findArchiveEntry(entries, path) {
        const key = this._normTexPathStatic(path);
        if (entries.has(key)) return { path: key, bytes: entries.get(key) };
        for (const [candidate, bytes] of entries) {
            if (candidate.endsWith('/' + key) || key.endsWith('/' + candidate)) {
                return { path: candidate, bytes };
            }
        }
        return null;
    }

    static _udimLayout(entries, pattern) {
        if (!this._isUDIMPath(pattern)) return null;
        const tiles = [];
        let maxU = 0;
        let maxV = 0;
        for (let id = 1001; id <= 1100; ++id) {
            const found = this._findArchiveEntry(entries, this._udimTilePath(pattern, id));
            if (!found) continue;
            const u = (id - 1001) % 10;
            const v = Math.floor((id - 1001) / 10);
            maxU = Math.max(maxU, u);
            maxV = Math.max(maxV, v);
            tiles.push({ id, u, v, path: found.path, bytes: found.bytes });
        }
        if (!tiles.length) return null;
        return { pattern, tiles, columns: maxU + 1, rows: maxV + 1 };
    }

    static _applyUDIMLayout(mesh, archiveEntries) {
        if (!mesh?.uv0?.length) return;
        const paths = [
            ...Object.values(mesh.texturePaths || {}),
            ...(mesh.materials || []).flatMap((entry) =>
                Object.values(entry?.texturePaths || {}))
        ];
        const pattern = paths.find((path) => this._isUDIMPath(path));
        const layout = pattern ? this._udimLayout(archiveEntries, pattern) : null;
        if (!layout) return;
        for (let i = 0; i + 1 < mesh.uv0.length; i += 2) {
            const sourceU = mesh.uv0[i];
            const sourceV = mesh.uv0[i + 1];
            const tileU = Math.floor(sourceU);
            const tileV = Math.floor(sourceV);
            mesh.uv0[i] = (tileU + (sourceU - tileU)) / layout.columns;
            mesh.uv0[i + 1] = (tileV + (sourceV - tileV)) / layout.rows;
        }
        mesh.udimLayout = {
            pattern: layout.pattern,
            columns: layout.columns,
            rows: layout.rows,
            tileIds: layout.tiles.map((tile) => tile.id)
        };
    }

    _normTexPath(path) {
        return NextRenderSceneAdapter._normTexPathStatic(path);
    }

    getArchiveTextureBytes(path) {
        const key = this._normTexPath(path);
        if (this.archiveEntries.has(key)) return this.archiveEntries.get(key);
        for (const [candidate, bytes] of this.archiveEntries) {
            if (candidate.endsWith('/' + key) || key.endsWith('/' + candidate)) {
                return bytes;
            }
        }
        return null;
    }

    getArchiveUDIMTiles(path) {
        return NextRenderSceneAdapter._udimLayout(this.archiveEntries, path);
    }

    releaseArchiveTextureBytes() {
        this.archiveEntries.clear();
    }

    releaseBuildData() {
        this.meshes = [];
        this.points = [];
        this.curves = [];
        this.nodes = [];
        this.lights = [];
        this.cameras = [];
        this.pointInstancers = [];
        this.pointInstanceDraws = [];
        this.skeletons = [];
        this.unsupportedRenderables = [];
        this.animations = [];
        this.animationInfos = [];
        this.materials = [];
        this.textures = [];
        this._rootNodes = null;
        this.materialKeys.clear();
        this.textureKeys.clear();
    }

    getSceneMetadata() {
        return this.sceneMetadata;
    }

    getUpAxis() {
        return this.sceneMetadata?.upAxis || 'Y';
    }

    numMeshes() {
        return this.stats?.optimizedMeshes ?? this.meshCountValue;
    }

    numPoints() {
        return this.pointsCountValue || 0;
    }

    numCurves() {
        return this.curvesCountValue || 0;
    }

    numMaterials() {
        return this.stats?.optimizedMaterials ?? this.materialCountValue;
    }

    numTextures() {
        return this.stats?.optimizedTextures ?? this.textureCountValue;
    }

    numImages() {
        return 0;
    }

    numNodes() {
        return this.nodeCountValue || 0;
    }

    numLights() {
        return this.lightCountValue || 0;
    }

    numCameras() {
        return this.cameraCountValue || 0;
    }

    numPointInstancers() {
        return this.pointInstancerCountValue || 0;
    }

    numPointInstanceDraws() {
        return this.pointInstanceDrawCountValue || 0;
    }

    numSkeletons() {
        return this.skeletonCountValue || 0;
    }

    numUnsupportedRenderables() {
        return this.unsupportedRenderableCountValue || 0;
    }

    numAnimations() {
        return this.animationCountValue || 0;
    }

    getMesh(index) {
        return this.getMeshCopy(index);
    }

    getMeshCopy(index) {
        if (!Number.isInteger(index) || index < 0 || index >= this.meshCountValue) {
            return null;
        }
        const mesh = this.meshes[index];
        if (!mesh) return null;
        return {
            ...mesh,
            points: mesh.points ? new Float32Array(mesh.points) : null,
            vertices: mesh.points ? new Float32Array(mesh.points) : null,
            indices: mesh.indices ? new Uint32Array(mesh.indices) : null,
            normals: mesh.normals ? new Float32Array(mesh.normals) : null,
            vertexColors: mesh.vertexColors ? new Float32Array(mesh.vertexColors) : null,
            uv0: mesh.uv0 ? new Float32Array(mesh.uv0) : null,
            uvs: mesh.uv0 ? new Float32Array(mesh.uv0) : null,
            texcoords: mesh.uv0 ? new Float32Array(mesh.uv0) : null,
            jointIndices: mesh.jointIndices ? new Uint16Array(mesh.jointIndices) : null,
            jointWeights: mesh.jointWeights ? new Float32Array(mesh.jointWeights) : null,
            skel_id: Number.isFinite(mesh.skel_id) ? mesh.skel_id : -1,
            skeletonPath: mesh.skeletonPath || '',
            elementSize: Number.isFinite(mesh.elementSize) ? mesh.elementSize : 0,
            absPath: mesh.absPath || mesh.primPath || '',
            hasGeomBindTransform: !!mesh.hasGeomBindTransform,
            geomBindTransform: Array.isArray(mesh.geomBindTransform)
                ? mesh.geomBindTransform.slice(0, 16)
                : null,
            faceVertexIndices: mesh.indices ? new Uint32Array(mesh.indices) : null,
            materialId: Number.isFinite(mesh.materialId) ? mesh.materialId : -1
        };
    }

    getPoints(index) {
        if (!Number.isInteger(index) || index < 0 || index >= this.pointsCountValue) {
            return null;
        }
        const points = this.points[index];
        if (!points) return null;
        return {
            ...points,
            points: points.points ? new Float32Array(points.points) : null,
            widths: points.widths ? new Float32Array(points.widths) : null,
            colors: points.colors ? new Float32Array(points.colors) : null
        };
    }

    getCurves(index) {
        if (!Number.isInteger(index) || index < 0 || index >= this.curvesCountValue) {
            return null;
        }
        const curves = this.curves[index];
        if (!curves) return null;
        return {
            ...curves,
            curveVertexCounts: Array.from(curves.curveVertexCounts || []),
            tessellatedVertexCounts: Array.from(curves.tessellatedVertexCounts || []),
            points: curves.points ? new Float32Array(curves.points) : null,
            widths: curves.widths ? new Float32Array(curves.widths) : null,
            colors: curves.colors ? new Float32Array(curves.colors) : null,
            tessellatedPoints: curves.tessellatedPoints
                ? new Float32Array(curves.tessellatedPoints) : null,
            tessellatedWidths: curves.tessellatedWidths
                ? new Float32Array(curves.tessellatedWidths) : null,
            tessellatedColors: curves.tessellatedColors
                ? new Float32Array(curves.tessellatedColors) : null
        };
    }

    getMaterial(index) {
        if (!Number.isInteger(index) || index < 0 || index >= this.materialCountValue) {
            return null;
        }
        const record = this.materials[index];
        return record ? { ...record.material, id: record.id, key: record.key, primPath: record.primPath } : null;
    }

    getMaterialWithFormat(index, format = 'json') {
        const material = this.getMaterial(index);
        if (!material) {
            return { data: null, error: `Material ${index} not found` };
        }
        if (format !== 'json') {
            return { data: null, error: `Unsupported format: ${format}` };
        }
        return { data: JSON.stringify(material), error: null };
    }

    getTexture(index) {
        if (!Number.isInteger(index) || index < 0 || index >= this.textureCountValue) {
            return null;
        }
        const texture = this.textures[index];
        return texture ? { ...texture } : null;
    }

    getImageCopy() {
        return null;
    }

    _nodeToLegacyTree(nodeId, seen = new Set()) {
        if (!Number.isInteger(nodeId) || nodeId < 0 || nodeId >= this.nodeCountValue) {
            return null;
        }
        if (seen.has(nodeId)) return null;
        seen.add(nodeId);
        const node = this.nodes[nodeId];
        if (!node || node.error) return null;
        const children = [];
        if (Array.isArray(node.children)) {
            for (const childId of node.children) {
                const child = this._nodeToLegacyTree(childId, seen);
                if (child) children.push(child);
            }
        }
        const nodeType = node.type === 'pointInstancer' ? 'pointinstancer' : (node.type || 'xform');
        return {
            index: node.index ?? nodeId,
            primName: node.name || '',
            displayName: node.name || '',
            absPath: node.primPath || '',
            primPath: node.primPath || '',
            nodeType,
            nodeCategory: node.type || nodeType,
            contentId: Number.isFinite(node.dataId) ? node.dataId : -1,
            materialId: Number.isFinite(node.materialId) ? node.materialId : -1,
            localMatrix: Array.isArray(node.localMatrix) ? node.localMatrix.slice(0, 16) : null,
            worldMatrix: Array.isArray(node.worldMatrix) ? node.worldMatrix.slice(0, 16) : null,
            visible: node.visible !== false,
            children
        };
    }

    _computeRootNodes() {
        if (this._rootNodes) return this._rootNodes;
        const roots = [];
        for (let i = 0; i < this.nodeCountValue; ++i) {
            const node = this.nodes[i];
            if (!node || node.error) continue;
            if (!Number.isFinite(node.parentId) || node.parentId < 0) {
                const root = this._nodeToLegacyTree(i);
                if (root) roots.push(root);
            }
        }
        this._rootNodes = roots;
        return roots;
    }

    numRootNodes() {
        return this._computeRootNodes().length;
    }

    getRootNode(index) {
        return this._computeRootNodes()[index] || null;
    }

    getDefaultRootNode() {
        return this.getRootNode(0);
    }

    getAnimation(index) {
        if (!Number.isInteger(index) || index < 0 || index >= this.animationCountValue) {
            return { error: 'invalid animation index' };
        }
        const item = this.animations[index];
        if (!item) {
            return { error: 'missing animation data' };
        }
        return item;
    }

    getAnimationInfo(index) {
        if (!Number.isInteger(index) || index < 0 || index >= this.animationCountValue) {
            return { error: 'invalid animation index' };
        }
        const item = this.animationInfos[index];
        if (!item) {
            return { error: 'missing animation info' };
        }
        return item;
    }

    getAllAnimations() {
        return Array.isArray(this.animations) ? this.animations : [];
    }

    getAllAnimationInfos() {
        return Array.isArray(this.animationInfos) ? this.animationInfos : [];
    }

    getNode(index) {
        if (!Number.isInteger(index) || index < 0 || index >= this.nodeCountValue) {
            return { error: 'invalid node index' };
        }
        const item = this.nodes[index];
        if (!item) {
            return { error: 'missing node data' };
        }
        return item;
    }

    getLight(index) {
        if (!Number.isInteger(index) || index < 0 || index >= this.lightCountValue) {
            return { error: 'invalid light index' };
        }
        const item = this.lights[index];
        if (!item) {
            return { error: 'missing light data' };
        }
        return item;
    }

    getCamera(index) {
        if (!Number.isInteger(index) || index < 0 || index >= this.cameraCountValue) {
            return { error: 'invalid camera index' };
        }
        const item = this.cameras[index];
        if (!item) {
            return { error: 'missing camera data' };
        }
        return item;
    }

    getPointInstancer(index) {
        if (!Number.isInteger(index) || index < 0 || index >= this.pointInstancerCountValue) {
            return { error: 'invalid point instancer index' };
        }
        const item = this.pointInstancers[index];
        if (!item) {
            return { error: 'missing point instancer data' };
        }
        return item;
    }

    getPointInstanceDraw(index) {
        if (!Number.isInteger(index) || index < 0 || index >= this.pointInstanceDrawCountValue) {
            return { error: 'invalid point instance draw index' };
        }
        const item = this.pointInstanceDraws[index];
        if (!item) {
            return { error: 'missing point instance draw data' };
        }
        return item;
    }

    getSkeleton(index) {
        if (!Number.isInteger(index) || index < 0 || index >= this.skeletonCountValue) {
            return { error: 'invalid skeleton index' };
        }
        const item = this.skeletons[index];
        if (!item) {
            return { error: 'missing skeleton data' };
        }
        return item;
    }

    getUnsupportedRenderables() {
        return Array.isArray(this.unsupportedRenderables)
            ? this.unsupportedRenderables
            : [];
    }

    getStats() {
        return this.stats || {};
    }

    delete() {
        this.end();
    }

    end() {
        if (this.renderStream) {
            try { this.renderStream.end(); } catch (_) {}
            try { this.renderStream.delete(); } catch (_) {}
            this.renderStream = null;
        }
        this.meshes = [];
        this.points = [];
        this.curves = [];
        this.nodes = [];
        this.lights = [];
        this.cameras = [];
        this.pointInstancers = [];
        this.pointInstanceDraws = [];
        this.skeletons = [];
        this.unsupportedRenderables = [];
        this.animations = [];
        this.animationInfos = [];
        this.materials = [];
        this.textures = [];
        this._rootNodes = null;
        this.archiveEntries.clear();
        this.materialKeys.clear();
        this.textureKeys.clear();
    }
}

class LightUSDLoader extends Loader {

    /**
     * Constructor for LightUSDLoader
     * @param {*} manager - THREE.js manager
     * @param {Object} options - Configuration options
     * @param {number} options.maxMemoryLimitMB - Maximum memory limit in MB (default: 2048 for WASM32, 8192 for WASM64)
     * @param {boolean} options.useZstdCompressedWasm - Use compressed WASM (default: false)
     * @param {Function} options.onTydraProgress - Callback for Tydra conversion progress ({meshCurrent, meshTotal, stage, meshName, progress}) => void
     * @param {Function} options.onTydraStage - Callback for Tydra stage changes ({stage, message}) => void
     * @param {Function} options.onTydraComplete - Callback for Tydra conversion completion ({meshCount, materialCount, textureCount}) => void
     * @param {Function} options.onLightUSDDebug - Callback for native debug events ({phase, heapBytes, detail, ...}) => void
     * @param {boolean} options.debugMemory - Print native heap debug events to console
     * @param {boolean} options.suppressNativeInfoLogs - Drop native [INFO] stdout logs
     * @param {'next'|'legacy'|'auto'} options.backend - Compatibility setting;
     *   all values use the core WASM/render backend. Legacy requests warn.
     */
    constructor(manager, options = {}) {
        super(manager);

        this.native_ = null;
        this.nextOnlyNative_ = false;
        // Backend names remain accepted for older callers; scenes use core.
        this.backend_ = ['legacy', 'next', 'auto'].includes(options.backend)
            ? options.backend
            : 'next';

        this.assetResolver_ = null;

        // texture loader callback
        // null = Use LightUSD's builtin image loader(C++ native module)
        //this.texLoader = null;


        this.imageCache = {};
        this.textureCache = {};

        this.fetcher = new FileFetcher();

        // Default: do NOT use zstd compressed WASM.
        this.useZstdCompressedWasm_ = options.useZstdCompressedWasm || false;
        this.compressedWasmPath_ = 'lightusd.wasm.zst';

        this.useMemory64_ = false;
        this.compressedWasm64Path_ = 'lightusd_64.wasm.zst';


        // Memory limit in MB - defaults are set by the native module based on WASM architecture
        // (2GB for WASM32, 8GB for WASM64). If not specified, the native default will be used.
        this.maxMemoryLimitMB_ = options.maxMemoryLimitMB;

        // EM_JS synchronous progress callbacks for Tydra conversion
        // These are called directly from C++ during conversion without ASYNCIFY
        this.onTydraProgress_ = options.onTydraProgress || null;
        this.onTydraStage_ = options.onTydraStage || null;
        this.onTydraComplete_ = options.onTydraComplete || null;
        this.onLightUSDDebug_ = options.onLightUSDDebug || null;
        this.debugMemory_ = !!options.debugMemory;
        this.suppressNativeInfoLogs_ = !!options.suppressNativeInfoLogs;
    }

    _getBinarySize(binary) {
        if (binary == null) {
            return null;
        }

        if (typeof binary.byteLength === 'number') {
            return binary.byteLength;
        }

        if (typeof binary.length === 'number') {
            return binary.length;
        }

        if (typeof binary.size === 'number') {
            return binary.size;
        }

        return null;
    }

    _formatDebugMemoryEvent(event) {
        const heapMB = event && typeof event.heapBytes === 'number'
            ? (event.heapBytes / (1024 * 1024)).toFixed(1)
            : 'n/a';
        const inputMB = event && typeof event.inputBytes === 'number' && event.inputBytes > 0
            ? ` input=${(event.inputBytes / (1024 * 1024)).toFixed(1)}MB`
            : '';
        const material = event && event.materialName ? ` material=${event.materialName}` : '';
        const materialCount = event && event.materialsTotal
            ? ` materials=${event.materialsCurrent}/${event.materialsTotal}`
            : '';
        const detail = event && event.detail ? ` ${event.detail}` : '';
        return `[LightUSDDebug] ${event ? event.phase : 'unknown'} heap=${heapMB}MB${inputMB}${materialCount}${material}${detail}`;
    }

    _makeDebugMemoryCallback(extraCallback, printToConsole) {
        return (event) => {
            if (printToConsole) {
                console.debug(this._formatDebugMemoryEvent(event));
            }
            if (this.onLightUSDDebug_) {
                this.onLightUSDDebug_(event);
            }
            if (extraCallback) {
                extraCallback(event);
            }
        };
    }

    _logNativeMemory(usd, label, enabled) {
        if (!enabled || !usd || typeof usd.debugLogMemory !== 'function') {
            return;
        }
        const event = usd.debugLogMemory(label);
        console.debug(this._formatDebugMemoryEvent({
            phase: 'js.' + label,
            heapBytes: event && event.heapBytes,
            detail: label
        }));
    }

    _describeUSDInput(binary, filePath) {
        const details = {};
        const byteLength = this._getBinarySize(binary);

        if (byteLength !== null) {
            details.bytes = byteLength;
        }

        if (typeof filePath === 'string' && filePath.length > 0) {
            if (filePath.startsWith('blob:')) {
                details.sourceType = 'blob';
                details.uri = filePath;
            } else if (/^[a-zA-Z][a-zA-Z\d+.-]*:/.test(filePath)) {
                details.sourceType = 'uri';
                details.uri = filePath;
            } else {
                details.sourceType = 'filename';
                details.filename = filePath;
            }
        } else if (typeof File !== 'undefined' && filePath instanceof File) {
            details.sourceType = 'file';
            details.filename = filePath.name;
            details.bytes = filePath.size;
            if (filePath.type) {
                details.mimeType = filePath.type;
            }
        } else if (typeof Blob !== 'undefined' && filePath instanceof Blob) {
            details.sourceType = 'blob';
            details.bytes = filePath.size;
            if (filePath.type) {
                details.mimeType = filePath.type;
            }
        }

        return details;
    }

    _logFailedUSDInput(binary, filePath, errorText) {
        const details = this._describeUSDInput(binary, filePath);

        if (errorText) {
            details.error = errorText;
        }

        console.log('[LightUSDLoader] Failed USD input:', details);
    }

    // Decompress zstd compressed WASM
    async decompressZstdWasm(compressedPath) {
        try {
            const fzstd = await import('fzstd');

            const wasmURL = new URL(compressedPath, import.meta.url);

            let compressedData;
            if (wasmURL.protocol === 'file:') {
                // Node.js: fetch() does not support file: URLs.
                const { readFile } = await import('node:fs/promises');
                const buf = await readFile(wasmURL);
                compressedData = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
            } else {
                const response = await fetch(wasmURL.href);
                if (!response.ok) {
                    throw new Error(`Failed to fetch compressed WASM: ${response.statusText}`);
                }
                compressedData = await response.arrayBuffer();
            }

            if (compressedData.byteLength < 1024*64) {
                throw new Error('Compressed WASM size is unusually small, may not be valid zstd compressed data.');
            }

            // Check zstd magic number (0x28B52FFD in little-endian)
            const magicBytes = new Uint8Array(compressedData, 0, 4);
            const expectedMagic = [0x28, 0xB5, 0x2F, 0xFD]; // Little-endian representation

            if (compressedData.byteLength < 4 ||
                magicBytes[0] !== expectedMagic[0] ||
                magicBytes[1] !== expectedMagic[1] ||
                magicBytes[2] !== expectedMagic[2] ||
                magicBytes[3] !== expectedMagic[3]) {
                throw new Error('Invalid zstd file: magic number mismatch');
            }

            // Decompress using zstd
            const decompressedData = fzstd.decompress(new Uint8Array(compressedData));

            return decompressedData;
        } catch (error) {
            console.error('Error decompressing zstd WASM:', error);
            throw error;
        }
    }

    // Initialize the native WASM module
    // This is async but the load() method handles it internally with promises
    async init( options = {}) {

        if (options.backend === 'legacy' || options.backend === 'next' || options.backend === 'auto') {
          this.backend_ = options.backend;
        }

        if (Object.prototype.hasOwnProperty.call(options, 'useZstdCompressedWasm')) {
          this.useZstdCompressedWasm_ = options.useZstdCompressedWasm;
        }

        if (Object.prototype.hasOwnProperty.call(options, 'useMemory64')) {
          this.useMemory64_ = options.useMemory64;
        }

        if (Object.prototype.hasOwnProperty.call(options, 'useNextOnlyWasm')) {
            console.warn('[LightUSDLoader] useNextOnlyWasm is deprecated; use backend: \'next\'.');
        }

        if (!this.native_) {

            // WASM module of LightUSD.
            const moduleUrl = new URL(import.meta.url);
            const pageParams = (typeof window !== 'undefined' && window.location)
                ? new URLSearchParams(window.location.search)
                : new URLSearchParams();
            const getParam = (key) => moduleUrl.searchParams.get(key) ?? pageParams.get(key);

            //let initLightUSDNative = null;


            let use_memory64 = this.useMemory64_;
            if (getParam("memory64") == "true") {
              use_memory64 = true;
            }
            // The published module is the core build (lightusd[_64].js). The
            // legacy module is no longer shipped; `backend: 'legacy'` and the
            // historical `wasm=` / `nextWasm=` query switches are ignored.
            if (options.backend === 'legacy' || getParam("backend") === 'legacy' ||
                getParam("wasm") === 'legacy') {
                console.warn('[LightUSDLoader] The legacy backend was removed; using the core module.');
            }
            this.backend_ = 'next';
            const coreUrl = new URL(use_memory64 ? './lightusd_64.js' : './lightusd.js',
                import.meta.url).href;
            const initLightUSDNative = (await import(/* @vite-ignore */ coreUrl)).default;
            this.nextOnlyNative_ = true;

            let wasmBinary = null;

            if (this.useZstdCompressedWasm_) {
                // Load and decompress zstd compressed WASM
                const compressedName =
                    use_memory64 ? this.compressedWasm64Path_ : this.compressedWasmPath_;
                wasmBinary = await this.decompressZstdWasm(compressedName);

            }

            // Initialize with custom WASM binary if decompressed
            const initOptions = {}
            if (wasmBinary) {
              initOptions.wasmBinary = wasmBinary;
            }
            if (this.suppressNativeInfoLogs_) {
              const filterPrint = (message) => {
                if (typeof message === 'string' && message.startsWith('[INFO] ')) {
                  return;
                }
                console.log(message);
              };
              initOptions.print = filterPrint;
              initOptions.printErr = (message) => {
                if (typeof message === 'string' && message.startsWith('[INFO] ')) {
                  return;
                }
                console.error(message);
              };
            }
            //initOptions.locateFile = function(path, scriptDirectory) {
            //  // Redirect WASM file loading to your custom file
            //  if (path.endsWith('.wasm')) {
            //    return './src/lightusd/lightusd_64.wasm';
            //  }
            //  return scriptDirectory + path;
            //}

            // Set up EM_JS synchronous progress callbacks
            // These are called from C++ during Tydra conversion without ASYNCIFY
            if (this.onTydraProgress_) {
              initOptions.onTydraProgress = this.onTydraProgress_;
            }
            if (this.onTydraStage_) {
              initOptions.onTydraStage = this.onTydraStage_;
            }
            if (this.onTydraComplete_) {
              initOptions.onTydraComplete = this.onTydraComplete_;
            }
            if (this.onLightUSDDebug_ || this.debugMemory_) {
              initOptions.onLightUSDDebug = this._makeDebugMemoryCallback(null, this.debugMemory_);
            }

            if (options.locateFeature) initOptions.locateFeature = options.locateFeature;
            this.native_ = await initLightUSDNative(initOptions);
            if (options.preloadEXR && this.native_?.loadFeature) await this.native_.loadFeature('exr');
            if (options.preloadValidator && this.native_?.loadFeature) await this.native_.loadFeature('validator');
            if (!this.native_) {
                throw new Error('LightUSDLoader: Failed to initialize native module.');
            }
        }
        return this;
    }

    /**
     * Set maximum memory limit for USD loading in MB
     * @param {number} limitMB - Memory limit in megabytes
     */
    setMaxMemoryLimitMB(limitMB) {
        if (typeof limitMB !== 'number' || limitMB <= 0) {
            throw new Error('Memory limit must be a positive number');
        }
        this.maxMemoryLimitMB_ = limitMB;
    }

    /**
     * Set progress callback for load operations
     * Note: Due to WASM synchronous execution, progress updates are limited.
     * For true async progress, use loadWithProgressAsync() or Web Workers.
     * @param {Function} callback - Progress callback (progress: 0-1, stage: string) => void
     */
    setProgressCallback(callback) {
        this.progressCallback_ = callback;
    }

    /**
     * Clear the progress callback
     */
    clearProgressCallback() {
        this.progressCallback_ = null;
    }

    /**
     * Set Tydra progress callback for mesh conversion updates
     * This is called synchronously from C++ via EM_JS during scene conversion
     * @param {Function} callback - ({meshCurrent, meshTotal, stage, meshName, progress}) => void
     */
    setTydraProgressCallback(callback) {
        this.onTydraProgress_ = callback;
        // Update native module if already initialized
        if (this.native_) {
            this.native_.onTydraProgress = callback;
        }
    }

    /**
     * Set Tydra stage callback for conversion stage changes
     * @param {Function} callback - ({stage, message}) => void
     */
    setTydraStageCallback(callback) {
        this.onTydraStage_ = callback;
        if (this.native_) {
            this.native_.onTydraStage = callback;
        }
    }

    /**
     * Set Tydra completion callback
     * @param {Function} callback - ({meshCount, materialCount, textureCount}) => void
     */
    setTydraCompleteCallback(callback) {
        this.onTydraComplete_ = callback;
        if (this.native_) {
            this.native_.onTydraComplete = callback;
        }
    }

    /**
     * Get current maximum memory limit in MB
     * @returns {number|undefined} Memory limit in megabytes, or undefined if using native default
     */
    getMaxMemoryLimitMB() {
        return this.maxMemoryLimitMB_;
    }

    /**
     * Get the native default memory limit in MB
     * @returns {Promise<number>} Native default memory limit in megabytes
     */
    async getNativeDefaultMemoryLimitMB() {
        if (!this.native_) {
            await this.init();
        }
        const stream = new this.native_.RenderStream();
        try {return stream.getMaxMemoryLimitMB();} finally {stream.delete();}
    }

    /**
     * Enable or disable bone reduction for skeletal meshes
     * @param {boolean} enabled - Enable bone reduction
     */
    setEnableBoneReduction(enabled) {
        this.enableBoneReduction_ = !!enabled;
    }

    /**
     * Get bone reduction enabled status
     * @returns {boolean} True if bone reduction is enabled
     */
    getEnableBoneReduction() {
        return this.enableBoneReduction_ || false;
    }

    /**
     * Set target bone count for bone reduction
     * @param {number} count - Target number of bone influences per vertex (1-128)
     */
    setTargetBoneCount(count) {
        if (typeof count !== 'number' || count < 1 || count > 128) {
            throw new Error('Target bone count must be between 1 and 128');
        }
        this.targetBoneCount_ = count;
    }

    /**
     * Get target bone count for bone reduction
     * @returns {number} Target bone count (default: 4)
     */
    getTargetBoneCount() {
        return this.targetBoneCount_ || 4;
    }

    /**
     * Enable or disable bone count rounding (round up to standard GPU skinning values)
     * When enabled, bone counts are rounded up to: 4, 8, 16, 32, 48, 64, 80, 96, 128
     * This keeps all bone influences but pads to standard sizes for GPU compatibility
     * @param {boolean} enabled - Enable bone count rounding
     */
    setRoundBoneCount(enabled) {
        this.roundBoneCount_ = !!enabled;
    }

    /**
     * Get bone count rounding setting
     * @returns {boolean} Whether bone count rounding is enabled
     */
    getRoundBoneCount() {
        return this.roundBoneCount_ || false;
    }

    /**
     * Set sphere tessellation subdivision level (0-6, default 4)
     * @param {number} subdivisions - Icosphere subdivision level
     */
    setSphereSubdivisions(subdivisions) {
        this.sphereSubdivisions_ = subdivisions;
    }

    getSphereSubdivisions() {
        return this.sphereSubdivisions_ ?? 4;
    }

    /**
     * Apply configured skinning options to a native USD loader instance.
     * @param {Object} usd - core RenderStream instance
     * @private
     */
    _applySkinningLoadOptions(usd) {
        if (!usd) return;
        if (this.enableBoneReduction_) {
            usd.setEnableBoneReduction(true);
            usd.setTargetBoneCount(this.targetBoneCount_ || 4);
        } else if (this.roundBoneCount_) {
            usd.setRoundBoneCount(true);
        }
        if (this.sphereSubdivisions_ !== undefined) {
            usd.setSphereSubdivisions(this.sphereSubdivisions_);
        }
    }

    /**
     * Apply render-scene optimization options (material/texture dedup, mesh
     * merging, render-tree flattening) to a native loader instance before
     * loadFromBinary() runs the Tydra conversion. Each option is only forwarded
     * when present in `options`, so existing callers are unaffected.
     *
     * @param {Object} usd - native core RenderStream instance
     * @param {Object} options - parse/load options
     * @param {boolean} [options.materialDedup] - deduplicate materials/textures by identity
     * @param {boolean} [options.mergeMeshes] - merge meshes sharing a material
     * @param {boolean} [options.mergeMeshesBakeTransform] - bake transforms into merged vertices
     * @param {boolean} [options.flattenRenderTree] - replace hierarchy with a flat render-only tree
     * @private
     */
    _applyConversionLoadOptions(usd, options = {}) {
        if (!usd) return;
        if (options.materialDedup !== undefined &&
            typeof usd.setNativeMaterialDedup === 'function') {
            usd.setNativeMaterialDedup(!!options.materialDedup);
        }
        if (options.mergeMeshes !== undefined &&
            typeof usd.setNativeMeshMerge === 'function') {
            usd.setNativeMeshMerge(!!options.mergeMeshes);
        }
        if (options.mergeMeshesBakeTransform !== undefined &&
            typeof usd.setNativeMeshMergeBakeTransform === 'function') {
            usd.setNativeMeshMergeBakeTransform(!!options.mergeMeshesBakeTransform);
        }
        if (options.flattenRenderTree !== undefined &&
            typeof usd.setNativeFlattenRenderTree === 'function') {
            usd.setNativeFlattenRenderTree(!!options.flattenRenderTree);
        }
    }

    /**
     * Create a progress event object (GLTFLoader compatible + extended)
     * @private
     */
    _createProgressEvent(loaded, total, stage, message) {
        return {
            // GLTFLoader compatible fields
            loaded: loaded,
            total: total,
            // Extended fields
            stage: stage,
            percentage: total > 0 ? (loaded / total) * 100 : 0,
            message: message
        };
    }

    /**
     * Load a USDZ/USDA/USDC file from a URL as USD Stage(Freezed scene graph)
     * NOTE: for loadAsync(), Use base Loader class's loadAsync() method
     * @param {string} url - URL to load from
     * @param {Function} onLoad - Success callback
     * @param {Function} onProgress - Progress callback with GLTFLoader-compatible event {loaded, total, stage, percentage, message}
     * @param {Function} onError - Error callback
     * @param {Object} options - Loading options
     * @param {number} options.maxMemoryLimitMB - Override memory limit for this load
     */
    load(url, onLoad, onProgress, onError, options = {}) {
        const scope = this;

        // Create a promise chain to handle initialization and loading
        const initPromise = this.native_ ? Promise.resolve() : this.init();

        initPromise
            .then(async () => {
                // Use fetch with progress tracking if onProgress is provided
                if (onProgress) {
                    return scope._fetchWithProgress(url, onProgress);
                } else {
                    // Fallback to simple fetch without progress
                    const response = await scope.fetcher.fetch(url);
                    return await response.arrayBuffer();
                }
            })
            .then((usd_data) => {
                const usd_binary = new Uint8Array(usd_data);

                // Report parsing stage
                if (onProgress) {
                    onProgress(scope._createProgressEvent(0, 1, 'parsing', 'Parsing USD...'));
                }

                scope.parse(usd_binary, url, function (usd) {
                    // Report complete
                    if (onProgress) {
                        onProgress(scope._createProgressEvent(1, 1, 'complete', 'Complete'));
                    }
                    onLoad(usd);
                }, onError, options);

            })
            .catch((error) => {
                console.error('LightUSDLoader: Error loading USD:', error);
                if (onError) {
                    onError(error);
                }
            });
    }

    /**
     * Fetch URL with progress reporting via ReadableStream
     * @private
     */
    async _fetchWithProgress(url, onProgress) {
        const response = await fetch(url);
        if (!response.ok) {
            throw new Error(`Failed to fetch: ${response.statusText}`);
        }

        // Get content length for progress calculation
        const contentLength = response.headers.get('content-length');
        const total = contentLength ? parseInt(contentLength, 10) : 0;

        // If no content-length, fall back to simple fetch
        if (total === 0) {
            onProgress(this._createProgressEvent(0, 0, 'downloading', 'Downloading...'));
            const data = await response.arrayBuffer();
            onProgress(this._createProgressEvent(data.byteLength, data.byteLength, 'downloading', 'Download complete'));
            return data;
        }

        // Read response with progress via ReadableStream
        const reader = response.body.getReader();
        const chunks = [];
        let loaded = 0;

        while (true) {
            const { done, value } = await reader.read();
            if (done) break;

            chunks.push(value);
            loaded += value.length;

            // Report download progress
            const percentage = Math.round((loaded / total) * 100);
            onProgress(this._createProgressEvent(
                loaded,
                total,
                'downloading',
                `Downloading... ${percentage}%`
            ));
        }

        // Combine chunks into single ArrayBuffer
        const result = new Uint8Array(loaded);
        let offset = 0;
        for (const chunk of chunks) {
            result.set(chunk, offset);
            offset += chunk.length;
        }

        return result.buffer;
    }

    /**
     * Parse a USDZ/USDA/USDC binary data
     * @param {ArrayBuffer} binary - Binary USD data
     * @param {string} filePath - Optional file path
     * @param {Function} onLoad - Success callback
     * @param {Function} onError - Error callback
     * @param {Object} options - Parsing options
     * @param {number} options.maxMemoryLimitMB - Override memory limit for this parse
     * @param {boolean} options.debugMemory - Print native heap debug events for this parse
     * @param {Function} options.onLightUSDDebug - Per-parse native debug callback
     * @param {Object} options.variantSelection - Optional variant override {primPath, variantSet, variantName}
     * @param {Function} options.onVariants - Optional callback receiving discovered variant info
     */
    parse(binary /* ArrayBuffer */, filePath /* optional */, onLoad, onError, options = {}) {

        const _onError = function (e) {

            if (onError) {

                onError(e);

            } else {

                console.error(e);

            }

            //scope.manager.itemError( url );
            //scope.manager.itemEnd( url );

        };

        if (!this.native_) {
            console.error('LightUSDLoader: Native module is not initialized.');
            _onError(new Error('LightUSDLoader: Native module is not initialized.'));
            return;
        }

        // Every scene loads through the core module; `backend` values other
        // than 'next' (legacy/auto) are accepted for compatibility and ignored.
        const backend = options.backend || this.backend_ || 'next';
        if (backend !== 'next' && !this.warnedLegacyBackend_) {
            console.warn('[LightUSDLoader] backend=' + backend + ' is not available; using the core module.');
            this.warnedLegacyBackend_ = true;
        }
        NextRenderSceneAdapter.create(this.native_, binary, filePath, {
            ...options, maxMemoryLimitMB: options.maxMemoryLimitMB ?? this.maxMemoryLimitMB_
        })
            .then(onLoad)
            .catch((error) => {
                this._logFailedUSDInput(binary, filePath, error?.message || String(error));
                _onError(error instanceof Error ? error : new Error(String(error)));
            });
        return;
    }

    /** Parse on the core adapter, reporting phases and checking AbortSignal
     * between native work and owned-array copies. Native parsing is synchronous;
     * progress yields let the UI update between materialization phases. */
    async parseWithProgress(binary, filePath, options = {}) {
        if (options.signal?.aborted) throw new DOMException('Parsing aborted', 'AbortError');
        if (!this.native_) await this.init(options);
        return NextRenderSceneAdapter.create(this.native_, binary, filePath, {
            ...options,
            maxMemoryLimitMB: options.maxMemoryLimitMB ?? this.maxMemoryLimitMB_
        });
    }

    /** Whether the initialized module supports the core async adapter. */
    hasAsyncSupport() {
        return typeof this.native_?.RenderStream === 'function';
    }

    /** Parse through the core phase-yielding adapter with optional phase callbacks. */
    async parseAsync(binary, filePath, options = {}) {
        let lastPhase;
        return this.parseWithProgress(binary, filePath, {
            ...options,
            onProgress: options.onProgress || options.onPhaseStart ? info => {
                options.onProgress?.(info);
                if (info.stage !== lastPhase) {
                    lastPhase = info.stage;
                    options.onPhaseStart?.({...info, phase: info.stage, progress: info.percentage / 100});
                }
            } : undefined
        });
    }

    /**
     * Load USD file from URL using C++20 coroutine-based async loading.
     * This method yields to the JavaScript event loop during processing,
     * allowing the browser to repaint.
     *
     * @param {string} url - URL to load from
     * @param {Object} options - Loading options
     * @param {number} options.maxMemoryLimitMB - Override memory limit
     * @param {Function} options.onPhaseStart - Callback when a phase starts ({phase, progress}) => void
     * @param {Function} options.onFetchProgress - Fetch progress callback (loaded, total) => void
     * @returns {Promise<Object>} Parsed USD object
     */
    async loadAsync(url, options = {}) {
        if (!this.native_) {
            await this.init();
        }

        // Fetch the file with progress
        const response = await fetch(url);
        if (!response.ok) {
            throw new Error(`Failed to fetch: ${response.statusText}`);
        }

        // Get content length for progress calculation
        const contentLength = response.headers.get('content-length');
        const total = contentLength ? parseInt(contentLength, 10) : 0;

        // Read response with progress
        const reader = response.body.getReader();
        const chunks = [];
        let loaded = 0;

        while (true) {
            const { done, value } = await reader.read();
            if (done) break;

            chunks.push(value);
            loaded += value.length;

            if (options.onFetchProgress) {
                options.onFetchProgress(loaded, total);
            }
        }

        // Combine chunks
        const binary = new Uint8Array(loaded);
        let offset = 0;
        for (const chunk of chunks) {
            binary.set(chunk, offset);
            offset += chunk.length;
        }

        // Parse using coroutine-based async method
        return this.parseAsync(binary, url, options);
    }

    /**
     * Load USD file from URL with progress reporting
     *
     * @param {string} url - URL to load from
     * @param {Object} options - Loading options
     * @param {number} options.maxMemoryLimitMB - Override memory limit
     * @param {Function} options.onProgress - Progress callback (progressInfo) => void
     * @param {Function} options.onFetchProgress - Fetch progress callback (loaded, total) => void
     * @param {AbortSignal} options.signal - AbortController signal for cancellation
     * @returns {Promise<Object>} Parsed USD object
     */
    async loadWithProgress(url, options = {}) {
        if (!this.native_) {
            await this.init();
        }

        // Check for cancellation before starting
        if (options.signal?.aborted) {
            throw new DOMException('Loading aborted', 'AbortError');
        }

        // Fetch the file with progress
        const response = await fetch(url, { signal: options.signal });
        if (!response.ok) {
            throw new Error(`Failed to fetch: ${response.statusText}`);
        }

        // Get content length for progress calculation
        const contentLength = response.headers.get('content-length');
        const total = contentLength ? parseInt(contentLength, 10) : 0;

        // Read response with progress
        const reader = response.body.getReader();
        const chunks = [];
        let loaded = 0;

        while (true) {
            const { done, value } = await reader.read();
            if (done) break;

            chunks.push(value);
            loaded += value.length;

            if (options.onFetchProgress) {
                options.onFetchProgress(loaded, total);
            }
            if (options.onProgress) {
                options.onProgress({
                    progress: total > 0 ? (loaded / total) * 0.3 : 0, // Fetch is ~30% of total
                    stage: 'fetching',
                    percentage: total > 0 ? (loaded / total) * 30 : 0,
                    message: `Downloading... ${Math.round(loaded / 1024)}KB`
                });
            }
        }

        // Combine chunks
        const totalLength = chunks.reduce((acc, chunk) => acc + chunk.length, 0);
        const binary = new Uint8Array(totalLength);
        let offset = 0;
        for (const chunk of chunks) {
            binary.set(chunk, offset);
            offset += chunk.length;
        }

        // Parse with progress
        const parseOptions = {
            ...options,
            onProgress: options.onProgress ? (info) => {
                // Adjust progress to account for fetch phase
                const adjustedProgress = 0.3 + (info.progress * 0.7);
                options.onProgress({
                    ...info,
                    progress: adjustedProgress,
                    percentage: adjustedProgress * 100,
                    message: info.stage === 'complete' ? 'Complete' : `${info.currentOperation || info.stage}`
                });
            } : undefined
        };

        return this.parseWithProgress(binary, url, parseOptions);
    }

    /**
     * Get the current progress state from a USD loader instance
     * @param {Object} usd - core RenderStream instance
     * @returns {Object} Progress information
     */
    static getProgress(usd) {
        if (usd && typeof usd.getProgress === 'function') {
            return usd.getProgress();
        }
        return null;
    }

    /**
     * Request cancellation of parsing on a USD loader instance
     * @param {Object} usd - core RenderStream instance
     */
    static cancelParsing(usd) {
        if (usd && typeof usd.cancelParsing === 'function') {
            usd.cancelParsing();
        }
    }

    // ============================================================
    // Streaming Transfer Methods for Memory-Efficient Loading
    // ============================================================

    /**
     * Check if ReadableStreamBYOBReader is available in this environment
     * @returns {boolean} True if BYOB reader is supported
     */
    static supportsBYOBReader() {
        try {
            return typeof ReadableStreamBYOBReader !== 'undefined';
        } catch {
            return false;
        }
    }

    /**
     * Stream fetch data directly to WASM memory with minimal JS memory footprint.
     * Uses zero-copy transfer where chunks are written directly to pre-allocated WASM buffer.
     * Each JS chunk is freed immediately after transfer to minimize memory usage.
     *
     * @param {string} url - URL to fetch
     * @param {string} assetPath - Asset path/identifier for the cache
     * @param {Object} options - Options
     * @param {AbortSignal} options.signal - AbortController signal for cancellation
     * @param {Function} options.onProgress - Progress callback (bytesLoaded, totalBytes) => void
     * @param {number} options.chunkSize - Preferred chunk size in bytes (default: 64KB)
     * @param {Object} options.usdInstance - Optional existing core RenderStream instance to use
     * @returns {Promise<{success: boolean, bytesTransferred: number, assetPath: string, usdInstance: Object}>}
     */
    async streamFetchToWasm(url, assetPath, options = {}) {
        if (!this.native_) await this.init();
        if (options.signal?.aborted) throw new DOMException('Stream transfer aborted', 'AbortError');
        const response = await fetch(url, {signal: options.signal});
        if (!response.ok) throw new Error(`Failed to fetch: HTTP ${response.status}`);
        const header = response.headers.get('content-length');
        // Fetch transparently decompresses HTTP content; encoded length then
        // differs from the bytes returned by its body reader.
        const length = header !== null && !response.headers.get('content-encoding') ? Number(header) : null;
        try {return await transferCoreChunks(this, responseChunks(response), assetPath, length, options);}
        catch(error){if(response.body&&!response.body.locked)await response.body.cancel().catch(()=>{});throw error;}
    }

    /**
     * Stream multiple assets to WASM in parallel with memory-efficient transfer.
     * Useful for loading USD files with multiple external references.
     *
     * @param {Array<{url: string, assetPath: string}>} assets - Array of assets to load
     * @param {Object} options - Options
     * @param {AbortSignal} options.signal - AbortController signal for cancellation
     * @param {Function} options.onProgress - Progress callback (completed, total, currentAsset) => void
     * @param {number} options.concurrency - Max concurrent downloads (default: 4)
     * @returns {Promise<Array<{success: boolean, assetPath: string, bytesTransferred: number}>>}
     */
    async streamFetchMultipleToWasm(assets, options = {}) {
        if (!this.native_) {
            await this.init();
        }

        const concurrency = options.concurrency ?? 4;
        if (!Number.isInteger(concurrency) || concurrency < 1) throw RangeError('Invalid concurrency');
        const results = [];
        let completed = 0;

        // Process assets in batches
        for (let i = 0; i < assets.length; i += concurrency) {
            if (options.signal?.aborted) {
                throw new DOMException('Stream transfer aborted', 'AbortError');
            }

            const batch = assets.slice(i, i + concurrency);
            const batchPromises = batch.map(async (asset) => {
                try {
                    const result = await this.streamFetchToWasm(asset.url, asset.assetPath, {
                        signal: options.signal,
                        onProgress: (loaded, total) => {
                            if (options.onAssetProgress) {
                                options.onAssetProgress(asset.assetPath, loaded, total);
                            }
                        }
                    });
                    completed++;
                    if (options.onProgress) {
                        options.onProgress(completed, assets.length, asset.assetPath);
                    }
                    return result;
                } catch (error) {
                    completed++;
                    if (options.onProgress) {
                        options.onProgress(completed, assets.length, asset.assetPath);
                    }
                    return {
                        success: false,
                        assetPath: asset.assetPath,
                        error: error.message
                    };
                }
            });

            const batchResults = await Promise.all(batchPromises);
            results.push(...batchResults);
        }

        return results;
    }

    /**
     * Load USD file with streaming transfer to minimize memory usage.
     * Combines streaming fetch with parsing.
     *
     * @param {string} url - URL to load
     * @param {Object} options - Options
     * @param {AbortSignal} options.signal - AbortController signal
     * @param {Function} options.onProgress - Progress callback
     * @param {number} options.maxMemoryLimitMB - Memory limit for parsing
     * @returns {Promise<Object>} Parsed USD object
     */
    async loadWithStreaming(url, options = {}) {
        options.onProgress?.({progress:0,stage:'streaming',percentage:0,message:'Starting streaming transfer...'});
        const assetPath = url;
        const result = await this.streamFetchToWasm(url, assetPath, {
            ...options, onProgress:(loaded,total)=>options.onProgress?.({
                progress:total?loaded/total*0.3:0,stage:'streaming',
                percentage:total?loaded/total*30:0,message:`Streaming... ${Math.round(loaded/1024)}KB`
            })
        });
        const stream=result.usdInstance;
        try {
            if(options.signal?.aborted)throw new DOMException('Stream transfer aborted','AbortError');
            options.onProgress?.({progress:0.3,stage:'parsing',percentage:30,message:'Parsing USD...'});
            const loaded=stream.beginStreamedAsset(assetPath);
            if(!loaded.success)throw Error(loaded.error||stream.error());
            options.onProgress?.({progress:1,stage:'complete',percentage:100,message:'Complete'});
            return stream;
        }catch(error){if(!options.usdInstance)stream.delete();throw error;}
    }

    async streamFileToWasm(filePath, assetPath, options = {}) {
        if (!this.native_) await this.init();
        if (typeof process === 'undefined' || !process.versions?.node) {
            throw Error('streamFileToWasm is only available in Node.js environment');
        }
        const fs=await import('node:fs');
        const size=(await fs.promises.stat(filePath)).size;
        const chunkSize=options.chunkSize??65536;
        if(!Number.isInteger(chunkSize)||chunkSize<1)throw RangeError('Invalid chunk size');
        const input=fs.createReadStream(filePath,{highWaterMark:chunkSize,signal:options.signal});
        input.on('error',()=>{});
        try{return await transferCoreChunks(this,input,assetPath,size,options);}
        finally{input.destroy();}
    }

    /**
     * Get information about active streaming buffers (for debugging/monitoring)
     * @returns {Promise<Array>} Array of active buffer info
     */
    async getActiveStreamingBuffers() {
        if (!this.native_) {
            await this.init();
        }

        return [...(this.activeCoreTransfers_ || [])].flatMap(stream => stream.getActiveZeroCopyBuffers());
    }

    /**
     * Load a USDZ/USDA/USDC file from a URL as USD Layer(for composition)
     * @param {string} url - URL to load from
     * @param {Function} onLoad - Success callback
     * @param {Function} onProgress - Progress callback
     * @param {Function} onError - Error callback
     * @param {Object} options - Loading options
     * @param {number} options.maxMemoryLimitMB - Override memory limit for this load
     */
    loadAsLayer(url, onLoad, onProgress, onError, options = {}) {
        this.loadAsLayerAsync(url,onProgress,options).then(onLoad,onError||console.error);
    }

    async loadAsLayerAsync(url, onProgress, options = {}) {
        if(!this.native_)await this.init();
        const response=await fetch(url,{signal:options.signal});
        if(!response.ok)throw Error(`Failed to fetch: HTTP ${response.status}`);
        const limit=(options.maxMemoryLimitMB??this.maxMemoryLimitMB_??512)*1024*1024;
        const chunks=[];let size=0;
        for await(const bytes of responseChunks(response)) {
            if(options.signal?.aborted)throw new DOMException('Load aborted','AbortError');
            if(bytes.length>limit-size)throw RangeError('Layer input exceeds memory limit');
            size+=bytes.length;chunks.push(bytes);
            onProgress?.({loaded:size,total:Number(response.headers.get('content-length'))||0});
        }
        let bytes=new Uint8Array(size);let offset=0;
        for(const chunk of chunks){bytes.set(chunk,offset);offset+=chunk.length;}
        if(bytes[0]===0x50&&bytes[1]===0x4b) {
            const entries=parseUSDZEntries(bytes);
            if(!entries[0]||!/\.usd[ac]?$/i.test(entries[0].name))throw Error('USDZ first entry is not a USD root layer');
            bytes=entries[0].data;
        }
        const document=new this.native_.LayerDocument();
        try {
            const result=document.load(bytes);
            if(!result.success)throw Error(result.error||document.error());
            return document;
        }catch(error){document.delete();throw error;}
    }

    loadTest(url,onLoad,onProgress,onError,options={}) {
        this.loadTestAsync(url,onProgress,options).then(onLoad,onError||console.error);
    }

    async loadTestAsync(url,onProgress,options={}) {
        return this.loadWithStreaming(url,{...options,onProgress});
    }

    /**
     * Load USD with full progress reporting (Three.js GLTFLoader compatible)
     * Combines downloading, parsing, and optional scene building with unified progress.
     *
     * @param {string} url - URL to load from
     * @param {Function} onLoad - Success callback (result) => void
     *   - result.usd: The parsed core USD object
     *   - result.scene: Three.js scene (if options.buildScene is true and sceneBuilder provided)
     * @param {Function} onProgress - Progress callback ({loaded, total, stage, percentage, message}) => void
     * @param {Function} onError - Error callback (error) => void
     * @param {Object} options - Loading options
     * @param {number} options.maxMemoryLimitMB - Override memory limit for this load
     * @param {boolean} options.buildScene - If true, build Three.js scene (requires sceneBuilder)
     * @param {Function} options.sceneBuilder - Async function (usd, options) => THREE.Object3D
     *   Typically: LightUSDLoaderUtils.buildThreeNode(usd.getNode(0), null, usd, options)
     * @param {Object} options.sceneBuilderOptions - Options to pass to sceneBuilder
     */
    loadWithFullProgress(url, onLoad, onProgress, onError, options = {}) {
        const scope = this;

        // Progress phase weights
        const DOWNLOAD_WEIGHT = 0.5;  // 0-50%
        const PARSE_WEIGHT = 0.3;     // 50-80%
        const BUILD_WEIGHT = 0.2;     // 80-100%

        const reportProgress = (phase, phaseProgress, message) => {
            if (!onProgress) return;

            let overallProgress = 0;
            let stage = phase;

            switch (phase) {
                case 'downloading':
                    overallProgress = phaseProgress * DOWNLOAD_WEIGHT;
                    break;
                case 'parsing':
                    overallProgress = DOWNLOAD_WEIGHT + (phaseProgress * PARSE_WEIGHT);
                    break;
                case 'building':
                    overallProgress = DOWNLOAD_WEIGHT + PARSE_WEIGHT + (phaseProgress * BUILD_WEIGHT);
                    break;
                case 'complete':
                    overallProgress = 1.0;
                    break;
            }

            onProgress(scope._createProgressEvent(
                overallProgress,
                1,
                stage,
                message || `${stage}... ${Math.round(overallProgress * 100)}%`
            ));
        };

        // Start loading
        const initPromise = this.native_ ? Promise.resolve() : this.init();

        initPromise
            .then(async () => {
                // Phase 1: Download (0-50%)
                reportProgress('downloading', 0, 'Starting download...');

                const response = await fetch(url);
                if (!response.ok) {
                    throw new Error(`Failed to fetch: ${response.statusText}`);
                }

                const contentLength = response.headers.get('content-length');
                const total = contentLength ? parseInt(contentLength, 10) : 0;

                let usd_data;
                if (total > 0 && response.body) {
                    // Stream with progress
                    const reader = response.body.getReader();
                    const chunks = [];
                    let loaded = 0;

                    while (true) {
                        const { done, value } = await reader.read();
                        if (done) break;
                        chunks.push(value);
                        loaded += value.length;
                        reportProgress('downloading', loaded / total, `Downloading... ${Math.round((loaded / total) * 100)}%`);
                    }

                    const result = new Uint8Array(loaded);
                    let offset = 0;
                    for (const chunk of chunks) {
                        result.set(chunk, offset);
                        offset += chunk.length;
                    }
                    usd_data = result;
                } else {
                    // No content-length, simple fetch
                    const buffer = await response.arrayBuffer();
                    usd_data = new Uint8Array(buffer);
                    reportProgress('downloading', 1, 'Download complete');
                }

                return usd_data;
            })
            .then(async (usd_binary) => {
                reportProgress('parsing',0,'Parsing USD...');
                const usd=await scope.parseWithProgress(usd_binary,url,{
                    ...options,onProgress:info=>reportProgress('parsing',info.progress??0,info.message)
                });
                reportProgress('parsing',1,'Parse complete');
                return usd;
            })
            .then(async (usd) => {
                // Phase 3: Build scene (80-100%) - optional
                const result = { usd: usd, scene: null };

                if (options.buildScene && options.sceneBuilder) {
                    reportProgress('building', 0, 'Building scene...');

                    try {
                        // Get root node
                        const rootNode = usd.getNode(0);

                        // Build scene with progress callback
                        const builderOptions = {
                            ...(options.sceneBuilderOptions || {}),
                            onProgress: (info) => {
                                // Forward scene building progress (scale to 80-100%)
                                const buildProgress = info.percentage ? info.percentage / 100 : 0.5;
                                reportProgress('building', buildProgress, info.message || 'Building scene...');
                            }
                        };

                        result.scene = await options.sceneBuilder(rootNode, null, usd, builderOptions);
                        reportProgress('building', 1, 'Scene complete');
                    } catch (buildError) {
                        console.warn('Scene building failed:', buildError);
                        // Continue without scene - USD is still valid
                    }
                }

                reportProgress('complete', 1, 'Complete');
                onLoad(result);
            })
            .catch((error) => {
                console.error('LightUSDLoader: Error in loadWithFullProgress:', error);
                if (onError) {
                    onError(error);
                }
            });
    }

    /**
     * Async version of loadWithFullProgress
     * @returns {Promise<{usd: Object, scene?: THREE.Object3D}>}
     */
    async loadWithFullProgressAsync(url, onProgress, options = {}) {
        const scope = this;
        return new Promise((resolve, reject) => {
            scope.loadWithFullProgress(url, resolve, onProgress, reject, options);
        });
    }

    ///**
    // * Set texture callback
    //  */
    //setTextureLoader(texLoader) {
    //    this.texLoader = texLoader;
    //}



}

export { LightUSDLoader, FetchAssetResolver };
