import * as THREE from 'three';
import { HDRLoader } from 'three/examples/jsm/loaders/HDRLoader.js';
import { EXRLoader } from 'three/examples/jsm/loaders/EXRLoader.js';
import { KTX2Loader } from 'three/examples/jsm/loaders/KTX2Loader.js';

import { LoaderUtils } from "three"
import { convertOpenPBRToMeshPhysicalMaterialLoaded } from './LightUSDMaterialX.js';
import { decodeEXR as decodeEXRWithFallback } from './EXRDecoder.js';
import {
    installTextureColorTransform,
    textureColorTransform
} from './ColorSpaceUtils.js';

/**
 * TextureLoadingManager - Manages delayed/progressive texture loading.
 *
 * This allows the scene to render immediately with basic materials,
 * while textures load in the background with progress reporting.
 *
 * Usage:
 *   const manager = new TextureLoadingManager();
 *
 *   // Queue texture tasks during material setup
 *   manager.queueTexture(material, 'map', textureId, usdScene);
 *
 *   // Start loading after scene is rendered
 *   await manager.startLoading({
 *     onProgress: (info) => console.log(`${info.loaded}/${info.total} textures`),
 *     concurrency: 2,  // Load 2 textures at a time
 *     yieldInterval: 16  // Yield to browser every 16ms
 *   });
 */
class TextureLoadingManager {
    constructor() {
        this.queue = [];           // Pending texture tasks
        this.taskMap = new Map();  // texture key -> task with material bindings
        this.promiseCache = new Map(); // textureId -> in-flight/completed Promise
        this.textureSignatureCache = new Map(); // textureId -> sampler/image signature
        this.loaded = 0;           // Number of loaded textures
        this.failed = 0;           // Number of failed textures
        this.total = 0;            // Total textures to load
        this.isLoading = false;    // Loading in progress
        this.aborted = false;      // Loading was aborted
    }

    /**
     * Queue a texture to be loaded later
     * @param {THREE.Material} material - Target material
     * @param {string} mapProperty - Property name (e.g., 'map', 'normalMap')
     * @param {number} textureId - USD texture ID
     * @param {Object} usdScene - USD scene/loader instance
     * @param {Object} options - Additional options (e.g., normalScale)
     */
    queueTexture(material, mapProperty, textureId, usdScene, options = {}) {
        const cacheKey = LightUSDLoaderUtils.textureCacheKey(
            textureId, usdScene, mapProperty, this.textureSignatureCache);
        const key = `${cacheKey}:${options.sourceFileName || ''}`;
        let task = this.taskMap.get(key);
        if (!task) {
            task = {
                cacheKey,
                mapProperty,
                textureId,
                usdScene,
                options,
                bindings: [],
                status: 'pending'
            };
            this.taskMap.set(key, task);
            this.queue.push(task);
        }
        task.bindings.push({ material, options });
        this.total = this.queue.length;
    }

    /**
     * Get current loading status
     */
    getStatus() {
        return {
            total: this.total,
            loaded: this.loaded,
            failed: this.failed,
            pending: this.total - this.loaded - this.failed,
            percentage: this.total > 0 ? ((this.loaded + this.failed) / this.total) * 100 : 100,
            isLoading: this.isLoading,
            isComplete: this.loaded + this.failed >= this.total && !this.isLoading
        };
    }

    /**
     * Abort loading
     */
    abort() {
        this.aborted = true;
    }

    /**
     * Reset manager for new loading session
     */
    reset() {
        this.queue = [];
        this.taskMap.clear();
        this.promiseCache.clear();
        this.textureSignatureCache.clear();
        this.loaded = 0;
        this.failed = 0;
        this.total = 0;
        this.isLoading = false;
        this.aborted = false;
    }

    /**
     * Start loading queued textures with progress reporting
     * @param {Object} options - Loading options
     * @param {Function} options.onProgress - Progress callback ({loaded, total, percentage, currentTexture})
     * @param {Function} options.onTextureLoaded - Called when each texture loads (material, mapProperty, texture)
     * @param {number} options.concurrency - Number of concurrent loads (default: 1)
     * @param {number} options.yieldInterval - ms between browser yields (default: 16)
     * @returns {Promise<Object>} - Final status {loaded, failed, total}
     */
    async startLoading(options = {}) {
        const {
            onProgress = null,
            onTextureLoaded = null,
            concurrency = LightUSDLoaderUtils.defaultTextureConcurrency(),
            yieldInterval = 16
        } = options;

        if (this.isLoading) {
            console.warn('TextureLoadingManager: Already loading');
            return this.getStatus();
        }

        this.isLoading = true;
        this.aborted = false;
        let lastYieldTime = performance.now();

        // Report initial progress
        if (onProgress) {
            onProgress({
                loaded: 0,
                failed: 0,
                total: this.total,
                pending: this.total,
                percentage: 0,
                currentTexture: null,
                isStart: true,
                isComplete: this.total === 0
            });
        }

        // Yield to allow initial render without textures
        await new Promise(r => requestAnimationFrame(r));

        // Process queue with concurrency control
        const pendingTasks = [...this.queue];
        const activeTasks = new Set();

        const loadTexture = async (task) => {
            if (this.aborted) return;

            task.status = 'loading';
            const { mapProperty, textureId, usdScene, options: taskOptions } = task;

            try {
                const cacheKey = task.cacheKey ||
                    LightUSDLoaderUtils.textureCacheKey(textureId, usdScene, mapProperty, this.textureSignatureCache);
                let promise = this.promiseCache.get(cacheKey);
                if (!promise) {
                    promise = LightUSDLoaderUtils.getTextureFromUSD(
                        usdScene, textureId, mapProperty);
                    this.promiseCache.set(cacheKey, promise);
                }
                const texture = await promise;

                if (texture && !this.aborted) {
                    const colorMetadata = LightUSDLoaderUtils.textureColorMetadata(
                        textureId, usdScene);
                    const authoredColorSpace = LightUSDLoaderUtils.textureSourceColorSpace(
                        textureId, usdScene, colorMetadata);
                    LightUSDLoaderUtils.applyTextureMapDefaults(
                        texture, mapProperty, authoredColorSpace, colorMetadata);
                    for (const binding of task.bindings) {
                        const { material, options: bindingOptions } = binding;
                        material[mapProperty] = texture;
                        installTextureColorTransform(material, mapProperty,
                            textureColorTransform(authoredColorSpace,
                                'lin_rec709_scene', colorMetadata));

                        // Apply special options (e.g., normal map scale)
                        if (bindingOptions.normalScale !== undefined && mapProperty === 'normalMap' && material.normalScale) {
                            material.normalScale.set(bindingOptions.normalScale, bindingOptions.normalScale);
                        }

                        material.needsUpdate = true;
                        if (onTextureLoaded) {
                            onTextureLoaded(material, mapProperty, texture);
                        }
                    }
                    task.status = 'loaded';
                    this.loaded++;
                }
            } catch (err) {
                const isUnsupportedUDIM = err?.name === 'UnsupportedUDIMTextureError';
                const isUnsupportedFormat = err?.name === 'UnsupportedTextureFormatError';
                const detail = {
                    ...LightUSDLoaderUtils.textureDebugInfo(
                        textureId, usdScene, mapProperty, taskOptions.sourceFileName || ''),
                    error: LightUSDLoaderUtils.textureLoadErrorInfo(err)
                };
                console.warn(
                    isUnsupportedUDIM ?
                        `Unsupported UDIM texture for ${mapProperty}; skipping texture fetch` :
                        isUnsupportedFormat ?
                            `Unsupported texture format for ${mapProperty}; skipping texture fetch` :
                            `Failed to load texture ${textureId} for ${mapProperty}`,
                    JSON.stringify(detail)
                );
                task.status = 'failed';
                this.failed++;
            }

            // Report progress
            if (onProgress && !this.aborted) {
                onProgress({
                    loaded: this.loaded,
                    failed: this.failed,
                    total: this.total,
                    pending: this.total - this.loaded - this.failed,
                    percentage: this.total > 0 ? ((this.loaded + this.failed) / this.total) * 100 : 100,
                    currentTexture: `${mapProperty} (${textureId})`,
                    isComplete: this.loaded + this.failed >= this.total
                });
            }

            // Yield to browser periodically
            const now = performance.now();
            if (now - lastYieldTime >= yieldInterval) {
                lastYieldTime = now;
                await new Promise(r => requestAnimationFrame(r));
            }
        };

        // Process with concurrency limit
        while (pendingTasks.length > 0 || activeTasks.size > 0) {
            if (this.aborted) break;

            // Start new tasks up to concurrency limit
            while (pendingTasks.length > 0 && activeTasks.size < concurrency) {
                const task = pendingTasks.shift();
                const promise = loadTexture(task).then(() => {
                    activeTasks.delete(promise);
                });
                activeTasks.add(promise);
            }

            // Wait for at least one task to complete
            if (activeTasks.size > 0) {
                await Promise.race(activeTasks);
            }
        }

        this.isLoading = false;

        // Final progress report
        if (onProgress) {
            onProgress({
                loaded: this.loaded,
                failed: this.failed,
                total: this.total,
                pending: 0,
                percentage: 100,
                currentTexture: null,
                isComplete: true
            });
        }

        return this.getStatus();
    }
}

// Export the manager class
export { TextureLoadingManager };

class LightUSDLoaderUtils extends LoaderUtils {

    // Static reference to LightUSD WASM module for EXR fallback
    static _lightusd = null;

    // Lazily detected WebGL block-compression target. undefined = not probed,
    // null = no supported target. Tests may temporarily replace the detector.
    static _textureCompressionTarget = undefined;
    static sceneTextureCompressionEnabled = true;

    // Basis Universal KTX2 support is lazy so applications that only use the
    // built-in Basis-free uni path never initialize the transcoder.
    // undefined = not probed, null = unavailable/disabled, object = KTX2Loader.
    static _ktx2Loader = undefined;
    static _ownsKTX2Loader = false;
    static _ktx2TranscoderPath = import.meta.env?.DEV
        ? '/node_modules/three/examples/jsm/libs/basis/'
        : '';

    // Yield interval for UI updates (ms)
    static YIELD_INTERVAL_MS = 250;

    static defaultTextureConcurrency() {
        const cores = (typeof navigator !== 'undefined' && Number.isFinite(navigator.hardwareConcurrency))
            ? navigator.hardwareConcurrency
            : 8;
        return Math.max(4, Math.min(16, cores || 8));
    }

    constructor() {
        super();
    }

    /**
     * Yield to browser to allow UI repaint during long-running async operations.
     * Uses requestAnimationFrame for optimal frame timing.
     * @returns {Promise<void>}
     */
    static yieldToUI(mode = 'raf') {
        return new Promise(resolve => {
            if (mode === 'timeout') {
                setTimeout(resolve, 0);
            } else if (typeof requestAnimationFrame === 'function') {
                requestAnimationFrame(() => resolve());
            } else {
                setTimeout(resolve, 0);
            }
        });
    }

    /**
     * Conditional yield - only yields if enough time has passed since last yield
     * @param {Object} state - State object with lastYieldTime property
     * @returns {Promise<void>}
     */
    static async maybeYieldToUI(state, options = null) {
        const mode = options?.yieldMode || 'raf';
        if (mode === 'none') {
            return;
        }
        const now = performance.now();
        const intervalMs = Number.isFinite(options?.yieldIntervalMs) ?
            Math.max(0, options.yieldIntervalMs) : this.YIELD_INTERVAL_MS;
        if (!state.lastYieldTime || (now - state.lastYieldTime) >= intervalMs) {
            state.lastYieldTime = now;
            const yieldStart = performance.now();
            await this.yieldToUI(mode);
            if (options?._debugState) {
                options._debugState.yieldMs += performance.now() - yieldStart;
                options._debugState.yieldCount++;
            }
        }
    }

    /**
     * Set LightUSD WASM module for EXR decoding fallback
     * @param {Object} lightusd - LightUSD WASM module instance
     */
    static setLightUSD(lightusd) {
        LightUSDLoaderUtils._lightusd = lightusd;
    }

    /**
     * Get LightUSD WASM module
     * @returns {Object|null}
     */
    static getLightUSD() {
        return LightUSDLoaderUtils._lightusd;
    }

    static setSceneTextureCompressionEnabled(enabled) {
        LightUSDLoaderUtils.sceneTextureCompressionEnabled = !!enabled;
    }

    static setKTX2TranscoderPath(path) {
        LightUSDLoaderUtils._ktx2TranscoderPath = String(path || '');
        if (LightUSDLoaderUtils._ktx2Loader) {
            LightUSDLoaderUtils._ktx2Loader.setTranscoderPath(
                LightUSDLoaderUtils._ktx2TranscoderPath);
        }
    }

    // Applications may supply a renderer-configured Three.js KTX2Loader. When
    // omitted, getKTX2Loader creates one from a short-lived WebGL capability
    // probe, matching the automatic uni target selection below.
    static setKTX2Loader(loader) {
        if (LightUSDLoaderUtils._ownsKTX2Loader &&
            LightUSDLoaderUtils._ktx2Loader !== loader) {
            LightUSDLoaderUtils._ktx2Loader?.dispose?.();
        }
        LightUSDLoaderUtils._ktx2Loader = loader || null;
        LightUSDLoaderUtils._ownsKTX2Loader = false;
    }

    static getKTX2Loader() {
        if (LightUSDLoaderUtils._ktx2Loader !== undefined) {
            return LightUSDLoaderUtils._ktx2Loader;
        }

        LightUSDLoaderUtils._ktx2Loader = null;
        let canvas = null;
        let gl = null;
        try {
            if (typeof document !== 'undefined' && document.createElement) {
                canvas = document.createElement('canvas');
            } else if (typeof OffscreenCanvas !== 'undefined') {
                canvas = new OffscreenCanvas(1, 1);
            }
            if (!canvas?.getContext) return null;
            gl = canvas.getContext('webgl2', { alpha: false }) ||
                canvas.getContext('webgl', { alpha: false });
            if (!gl) return null;

            const extensions = new Map();
            const getExtension = (name) => {
                if (!extensions.has(name)) extensions.set(name, gl.getExtension(name));
                return extensions.get(name);
            };
            const loader = new KTX2Loader();
            loader.setTranscoderPath(LightUSDLoaderUtils._ktx2TranscoderPath);
            loader.detectSupport({
                extensions: {
                    has: (name) => !!getExtension(name),
                    get: (name) => getExtension(name)
                }
            });
            LightUSDLoaderUtils._ktx2Loader = loader;
            LightUSDLoaderUtils._ownsKTX2Loader = true;
        } catch (error) {
            console.warn('LightUSD Basis KTX2 support is unavailable:', error);
            LightUSDLoaderUtils._ktx2Loader = null;
        } finally {
            gl?.getExtension('WEBGL_lose_context')?.loseContext();
        }
        return LightUSDLoaderUtils._ktx2Loader;
    }

    static disposeKTX2Loader() {
        if (LightUSDLoaderUtils._ownsKTX2Loader) {
            LightUSDLoaderUtils._ktx2Loader?.dispose?.();
        }
        LightUSDLoaderUtils._ktx2Loader = undefined;
        LightUSDLoaderUtils._ownsKTX2Loader = false;
    }

    // Pick the highest-quality RGBA block format available to this browser.
    // A short-lived probe context keeps getTextureFromUSD independent of a
    // particular THREE.WebGLRenderer; the result is cached for the page.
    static detectTextureCompressionTarget() {
        if (this._textureCompressionTarget !== undefined) {
            return this._textureCompressionTarget;
        }
        this._textureCompressionTarget = null;
        if (typeof document === 'undefined' || !document.createElement) {
            return null;
        }

        let gl = null;
        let isWebGL2 = false;
        try {
            const canvas = document.createElement('canvas');
            gl = canvas.getContext('webgl2', { alpha: false });
            isWebGL2 = !!gl;
            if (!gl) gl = canvas.getContext('webgl', { alpha: false });
            if (!gl) return null;

            if (gl.getExtension('EXT_texture_compression_bptc')) {
                this._textureCompressionTarget = {
                    target: 'bc7',
                    linearFormat: THREE.RGBA_BPTC_Format,
                    srgbFormat: 0x8e8d,
                    name: 'BC7'
                };
            } else if (gl.getExtension('WEBGL_compressed_texture_astc')) {
                this._textureCompressionTarget = {
                    target: 'astc4x4',
                    linearFormat: THREE.RGBA_ASTC_4x4_Format,
                    srgbFormat: 0x93d0,
                    name: 'ASTC 4x4'
                };
            } else if (isWebGL2 || gl.getExtension('WEBGL_compressed_texture_etc')) {
                this._textureCompressionTarget = {
                    target: 'etc2rgba',
                    linearFormat: THREE.RGBA_ETC2_EAC_Format,
                    srgbFormat: 0x9279,
                    name: 'ETC2 RGBA'
                };
            }
        } catch (_) {
            this._textureCompressionTarget = null;
        } finally {
            if (gl) gl.getExtension('WEBGL_lose_context')?.loseContext();
        }
        return this._textureCompressionTarget;
    }

    // Compress one native-decoded RGBA8 scene image through the universal uni
    // intermediate and upload device-native blocks. Returns null when the ABI
    // or a suitable WebGL format is unavailable, preserving the DataTexture
    // fallback below.
    static createCompressedSceneTexture(texImage, mapProperty = '') {
        if (!this.sceneTextureCompressionEnabled || !texImage?.decoded ||
            texImage.channels !== 4 || texImage.width <= 0 || texImage.height <= 0 ||
            !texImage.data) {
            return null;
        }
        const lightusd = this._lightusd;
        if (!lightusd || typeof lightusd.compressTextureToUni !== 'function' ||
            typeof lightusd.transcodeTextureUni !== 'function') {
            return null;
        }
        const pick = this.detectTextureCompressionTarget();
        if (!pick) return null;

        const rgba = texImage.data instanceof Uint8Array
            ? texImage.data
            : new Uint8Array(texImage.data);
        if (rgba.byteLength !== texImage.width * texImage.height * 4) return null;

        try {
            // DataTexture.flipY cannot be applied by compressedTexImage2D, so
            // perform the scene loader's usual Y flip before block encoding.
            const uni = lightusd.compressTextureToUni(
                rgba, texImage.width, texImage.height, true);
            if (!uni?.success || !uni.data) return null;
            const blocks = lightusd.transcodeTextureUni(
                uni.data, texImage.width, texImage.height, pick.target);
            if (!blocks?.success || !blocks.data) return null;

            const data = blocks.data instanceof Uint8Array
                ? blocks.data
                : new Uint8Array(blocks.data);
            const isColor = this.textureColorRole(mapProperty) === 'color';
            const texture = new THREE.CompressedTexture(
                [{ data, width: texImage.width, height: texImage.height }],
                texImage.width,
                texImage.height,
                // Three.js accepts its base compressed-format constants here
                // and selects the linear/sRGB GL internal format from
                // texture.colorSpace. Raw COMPRESSED_SRGB_* enums are not
                // recognized as THREE.Texture.format values.
                pick.linearFormat,
                THREE.UnsignedByteType
            );
            texture.colorSpace = isColor
                ? THREE.SRGBColorSpace
                : THREE.NoColorSpace;
            texture.minFilter = THREE.LinearFilter;
            texture.magFilter = THREE.LinearFilter;
            texture.generateMipmaps = false;
            texture.flipY = false;
            texture.userData.lightusdCompression = {
                format: pick.name,
                rgbaBytes: rgba.byteLength,
                uniBytes: Number(uni.byteLength) || uni.data.byteLength,
                gpuBytes: Number(blocks.byteLength) || data.byteLength,
                linearFormat: pick.linearFormat,
                srgbFormat: pick.srgbFormat
            };
            texture.needsUpdate = true;
            return texture;
        } catch (error) {
            console.warn('LightUSD scene texture compression failed; using RGBA8:', error);
            return null;
        }
    }

    // Browser-decoded PNG/JPEG/WebP path. Read the HTML image back once, then
    // use the same uni route as native-decoded USDZ images. Cross-origin or
    // non-DOM images simply keep their original THREE.Texture.
    static compressBrowserSceneTexture(texture, mapProperty = '') {
        const image = texture?.image;
        if (!image || typeof document === 'undefined' || !document.createElement) {
            return null;
        }
        const width = image.naturalWidth || image.videoWidth || image.width;
        const height = image.naturalHeight || image.videoHeight || image.height;
        if (!Number.isFinite(width) || !Number.isFinite(height) ||
            width <= 0 || height <= 0) {
            return null;
        }
        try {
            const canvas = document.createElement('canvas');
            canvas.width = width;
            canvas.height = height;
            const context = canvas.getContext('2d', { willReadFrequently: true });
            if (!context) return null;
            context.drawImage(image, 0, 0, width, height);
            const rgba = context.getImageData(0, 0, width, height).data;
            const compressed = this.createCompressedSceneTexture({
                decoded: true,
                width,
                height,
                channels: 4,
                data: rgba
            }, mapProperty);
            if (compressed) texture.dispose();
            return compressed;
        } catch (_) {
            // A cross-origin image without CORS permission taints the canvas.
            return null;
        }
    }

    // Extract file extension from URI/path
    static getFileExtension(uri) {
        if (!uri || typeof uri !== 'string') return '';

        // Remove query parameters and hash
        const cleanUri = uri.split('?')[0].split('#')[0];

        // Get the last part after the last dot
        const lastDotIndex = cleanUri.lastIndexOf('.');
        if (lastDotIndex === -1 || lastDotIndex === cleanUri.length - 1) {
            return '';
        }

        return cleanUri.substring(lastDotIndex + 1).toLowerCase();
    }

    // Determine MIME type from file extension
    static getMimeTypeFromExtension(extension) {
        const mimeTypes = {
            // Images
            'jpg': 'image/jpeg',
            'jpeg': 'image/jpeg',
            'png': 'image/png',
            'gif': 'image/gif',
            'webp': 'image/webp',
            'bmp': 'image/bmp',
            'tiff': 'image/tiff',
            'tif': 'image/tiff',
            'svg': 'image/svg+xml',
            'ico': 'image/x-icon',
            'ktx2': 'image/ktx2',

            // HDR/EXR formats
            'hdr': 'image/vnd.radiance',
            'exr': 'image/x-exr',
            'rgbe': 'image/vnd.radiance',

            // 3D/USD formats
            'usd': 'model/vnd.usdz+zip',
            'usda': 'model/vnd.usd+ascii',
            'usdc': 'model/vnd.usd+binary',
            'usdz': 'model/vnd.usdz+zip',

            // Other common formats
            'json': 'application/json',
            'xml': 'application/xml',
            'txt': 'text/plain',
            'bin': 'application/octet-stream'
        };

        return mimeTypes[extension.toLowerCase()] || null;
    }

    static isUnsupportedBrowserTextureExtension(extension) {
        return new Set([
            'psd',
            'tga',
            'dds',
            'ktx'
        ]).has(String(extension || '').toLowerCase());
    }

    static classifyKTX2Image(texImage) {
        const uriKTX2 = this.getFileExtension(texImage?.uri || '') === 'ktx2';
        if (!texImage?.data) return uriKTX2 ? { kind: 'unknown' } : null;
        const data = texImage.data instanceof Uint8Array
            ? texImage.data
            : new Uint8Array(texImage.data);
        const identifier = [0xab, 0x4b, 0x54, 0x58, 0x20, 0x32,
            0x30, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a];
        if (data.byteLength < identifier.length ||
            !identifier.every((value, index) => data[index] === value)) {
            return uriKTX2 ? { kind: 'unknown' } : null;
        }
        if (data.byteLength < 80) return { kind: 'unknown' };

        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        const vkFormat = view.getUint32(12, true);
        const supercompression = view.getUint32(44, true);
        const dfdOffset = view.getUint32(48, true);
        const dfdLength = view.getUint32(52, true);
        if (vkFormat !== 0) {
            return { kind: 'standard', vkFormat, supercompression };
        }
        if (dfdLength >= 44 && dfdOffset >= 80 &&
            dfdOffset + dfdLength <= data.byteLength) {
            const colorModel = data[dfdOffset + 12];
            // TinyEXR's corrected private carrier deliberately uses the
            // UNSPECIFIED model. Basis ETC1S/UASTC use Khronos models 163/166.
            if (colorModel === 0) {
                return { kind: 'private-uni', vkFormat, supercompression };
            }
            if (colorModel === 163 || colorModel === 166 ||
                supercompression === 1) {
                return { kind: 'basis', vkFormat, supercompression, colorModel };
            }
            return { kind: 'undefined-format', vkFormat, supercompression,
                colorModel };
        }
        return { kind: 'unknown', vkFormat, supercompression };
    }

    static isKTX2Image(texImage) {
        return this.classifyKTX2Image(texImage) !== null;
    }

    static unsupportedKTX2Error(textureId, uri) {
        const err = new Error(
            `KTX2 texture requires WebGL and Three.js KTX2Loader support: ${uri || `texture ${textureId}`}`);
        err.name = 'UnsupportedTextureFormatError';
        err.textureId = textureId;
        err.textureAssetPath = uri || undefined;
        err.extension = 'ktx2';
        return err;
    }

    static privateUniKTX2Error(textureId, uri) {
        const err = new Error(
            `TinyEXR private uni KTX2 must be decoded by lightusd/textools, not Three.js KTX2Loader: ${uri || `texture ${textureId}`}`);
        err.name = 'UnsupportedTextureFormatError';
        err.textureId = textureId;
        err.textureAssetPath = uri || undefined;
        err.extension = 'ktx2';
        err.ktx2Kind = 'private-uni';
        return err;
    }

    static loadKTX2Texture(source, textureId, uri) {
        const loader = this.getKTX2Loader();
        if (!loader) return Promise.reject(this.unsupportedKTX2Error(textureId, uri));
        if (typeof source === 'string') return loader.loadAsync(source);

        const bytes = source instanceof Uint8Array
            ? source
            : new Uint8Array(source);
        // KTX2Loader transfers the buffer to a worker. Give it an exact owned
        // copy so an Embind heap view cannot be detached or include neighbors.
        const buffer = bytes.slice().buffer;
        return new Promise((resolve, reject) => loader.parse(buffer, resolve, reject));
    }

    // Helper method to determine MIME type
    static getMimeType(texImage) {

        if (texImage.uri) {
            const mime = this.getMimeTypeFromExtension(this.getFileExtension(texImage.uri));
            if (mime != null) {
                return mime;
            }
        }

        // Try to detect from magic bytes if available
        const data = new Uint8Array(texImage.data);
        if (data.length >= 4) {
            // PNG magic bytes: 89 50 4E 47
            if (data[0] === 0x89 && data[1] === 0x50 && data[2] === 0x4E && data[3] === 0x47) {
                return 'image/png';
            }
            // JPEG magic bytes: FF D8 FF
            if (data[0] === 0xFF && data[1] === 0xD8 && data[2] === 0xFF) {
                return 'image/jpeg';
            }
            // WEBP magic bytes: 52 49 46 46 ... 57 45 42 50
            if (data[0] === 0x52 && data[1] === 0x49 && data[2] === 0x46 && data[3] === 0x46) {
                return 'image/webp';
            }
            // EXR magic bytes: 76 2F 31 01
            if (data[0] === 0x76 && data[1] === 0x2F && data[2] === 0x31 && data[3] === 0x01) {
                return 'image/x-exr';
            }
            // HDR magic bytes: "#?" (Radiance format)
            if (data[0] === 0x23 && data[1] === 0x3F) {
                return 'image/vnd.radiance';
            }
        }

        // Default fallback
        return 'image/png';
    }

    static async getTextureFromUSD(usdScene, textureId, mapProperty = '') {
        if (textureId === undefined) return Promise.reject(new Error("textureId undefined"));


        const tex = usdScene.getTexture(textureId);

        const texImage = usdScene.getImageCopy(tex.textureImageId);
        const uri = texImage.uri || '';
        const isUDIMTexture = !!tex.isUDIM || /<UDIM>|%3CUDIM%3E|\.1\d{3}\./i.test(uri);

        if (isUDIMTexture) {
            const err = new Error(`UDIM texture is not supported in this demo: ${uri || `texture ${textureId}`}`);
            err.name = 'UnsupportedUDIMTextureError';
            err.textureId = textureId;
            err.textureAssetPath = uri || undefined;
            return Promise.reject(err);
        }
        const extension = this.getFileExtension(uri);
        const ktx2Info = this.classifyKTX2Image(texImage);
        const isKTX2 = ktx2Info !== null;
        if (!texImage.decoded && ktx2Info?.kind === 'private-uni') {
            return Promise.reject(this.privateUniKTX2Error(textureId, uri));
        }
        if (!texImage.decoded && isKTX2 && !this.getKTX2Loader()) {
            return Promise.reject(this.unsupportedKTX2Error(textureId, uri));
        }
        if (!texImage.decoded && this.isUnsupportedBrowserTextureExtension(extension)) {
            const err = new Error(`Texture format ".${extension}" is not supported by this browser demo: ${uri || `texture ${textureId}`}`);
            err.name = 'UnsupportedTextureFormatError';
            err.textureId = textureId;
            err.textureAssetPath = uri || undefined;
            err.extension = extension;
            return Promise.reject(err);
        }

        // there are 3 states for texture:
        // 1. URI only. Need to fetch texture(file) from URI in JS layer.
        // 2. Texture is loaded from USDZ file, but not yet decoded(Use Three.js or JS library to decode)
        // 3. Texture is decoded and ready to use in Three.js.

        if (texImage.uri && (texImage.bufferId == -1)) {
            // Case 1: URI only
            const lowerUri = uri.toLowerCase();

            if (isKTX2) {
                return this.loadKTX2Texture(uri, textureId, uri)
                    .then((texture) => this.applyTextureSampler(texture, tex));
            } else if (lowerUri.endsWith('.exr')) {
                // EXR: Use EXRLoader
                return new EXRLoader().loadAsync(uri)
                    .then((texture) => this.applyTextureSampler(texture, tex));
            } else if (lowerUri.endsWith('.hdr')) {
                // HDR: Use HDRLoader
                return new HDRLoader().loadAsync(uri)
                    .then((texture) => this.applyTextureSampler(texture, tex));
            } else {
                // Standard image
                return new THREE.TextureLoader().loadAsync(uri)
                    .then((texture) =>
                        this.compressBrowserSceneTexture(texture, mapProperty) || texture)
                    .then((texture) => this.applyTextureSampler(texture, tex));
            }

        } else if (texImage.bufferId >= 0 && texImage.data) {

            if (texImage.decoded) {

                const compressed = this.createCompressedSceneTexture(
                    texImage, mapProperty);
                if (compressed) {
                    return Promise.resolve(this.applyTextureSampler(compressed, tex));
                }

                const image8Array = new Uint8ClampedArray(texImage.data);
                const texture = new THREE.DataTexture(image8Array, texImage.width, texImage.height);
                if (texImage.channels == 1) {
                    texture.format = THREE.RedFormat;
                } else if (texImage.channels == 2) {
                    texture.format = THREE.RGFormat;
                } else if (texImage.channels == 3) {
                    // Recent three.js does not support RGBFormat.
                    return Promise.reject(new Error("RGB image is not supported"));
                } else if (texImage.channels == 4) {
                    texture.format = THREE.RGBAFormat;
                } else {
                    return Promise.reject(new Error("Unsupported image channels: " + texImage.channels));
                }
                texture.flipY = true;
                texture.needsUpdate = true;

                return Promise.resolve(this.applyTextureSampler(texture, tex));

            } else {
                // Case 2: Embedded but not decoded - check format
                try {
                    if (isKTX2) {
                        return this.loadKTX2Texture(texImage.data, textureId, uri)
                            .then((texture) => this.applyTextureSampler(texture, tex));
                    }
                    const mimeType = this.getMimeType(texImage);

                    // Check if HDR/EXR format - use specialized decoders
                    if (mimeType === 'image/x-exr') {
                        // EXR: Use LightUSD fallback decoder
                        const texture = this.decodeEXRFromBuffer(texImage.data, 'float16');
                        if (texture) {
                            texture.flipY = true;
                            return Promise.resolve(this.applyTextureSampler(texture, tex));
                        }
                        // Fallback to Three.js EXRLoader with blob URL
                        const blob = new Blob([texImage.data], { type: mimeType });
                        const blobUrl = URL.createObjectURL(blob);
                        return new EXRLoader().loadAsync(blobUrl)
                            .then((texture) => this.applyTextureSampler(texture, tex))
                            .finally(() => URL.revokeObjectURL(blobUrl));
                    } else if (mimeType === 'image/vnd.radiance') {
                        // HDR: Use LightUSD decoder (faster)
                        const lightusd = LightUSDLoaderUtils._lightusd;
                        if (lightusd && typeof lightusd.decodeHDR === 'function') {
                            const uint8Array = texImage.data instanceof Uint8Array
                                ? texImage.data
                                : new Uint8Array(texImage.data);
                            const result = lightusd.decodeHDR(uint8Array, 'float16');
                            if (result.success) {
                                const texture = new THREE.DataTexture(
                                    result.data,
                                    result.width,
                                    result.height,
                                    THREE.RGBAFormat,
                                    THREE.HalfFloatType
                                );
                                texture.minFilter = THREE.LinearFilter;
                                texture.magFilter = THREE.LinearFilter;
                                texture.flipY = true;
                                texture.needsUpdate = true;
                                return Promise.resolve(this.applyTextureSampler(texture, tex));
                            }
                        }
                        // Fallback to Three.js HDRLoader
                        const blob = new Blob([texImage.data], { type: mimeType });
                        const blobUrl = URL.createObjectURL(blob);
                        return new HDRLoader().loadAsync(blobUrl)
                            .then((texture) => this.applyTextureSampler(texture, tex))
                            .finally(() => URL.revokeObjectURL(blobUrl));
                    } else {
                        // Standard image format
                        const blob = new Blob([texImage.data], { type: mimeType });
                        const blobUrl = URL.createObjectURL(blob);
                        const loader = new THREE.TextureLoader();
                        return loader.loadAsync(blobUrl)
                            .then((texture) =>
                                this.compressBrowserSceneTexture(texture, mapProperty) || texture)
                            .then((texture) => this.applyTextureSampler(texture, tex))
                            .finally(() => URL.revokeObjectURL(blobUrl));
                    }
                } catch (error) {
                    console.error("Failed to decode texture data:", error);
                    return Promise.reject(new Error("Failed to decode texture data"));
                }
            }

        } else {
            return Promise.reject(new Error("Invalid USD texture info"));
        }
    }

    static textureColorRole(mapProperty) {
        switch (mapProperty) {
            case 'map':
            case 'emissiveMap':
            case 'specularColorMap':
            case 'sheenColorMap':
            case 'attenuationColorMap':
                return 'color';
            default:
                return 'data';
        }
    }

    static threeWrapMode(wrap) {
        switch (String(wrap || '').toLowerCase()) {
            case 'repeat':
            case 'tile':
            case 'tileforever':
                return THREE.RepeatWrapping;
            case 'mirror':
            case 'mirroredrepeat':
                return THREE.MirroredRepeatWrapping;
            case 'clamp_to_edge':
            case 'clamp':
            case 'black':
            case 'clamp_to_border':
            default:
                return THREE.ClampToEdgeWrapping;
        }
    }

    static applyTextureSampler(texture, texData = null) {
        if (!texture || !texData) return texture;

        texture.wrapS = this.threeWrapMode(texData.wrapS);
        texture.wrapT = this.threeWrapMode(texData.wrapT);

        if (texData.hasTransform2d) {
            const scaleU = Number.isFinite(texData.txScaleU) ? texData.txScaleU : 1;
            const scaleV = Number.isFinite(texData.txScaleV) ? texData.txScaleV : 1;
            const translateU = Number.isFinite(texData.txTranslationU) ? texData.txTranslationU : 0;
            const translateV = Number.isFinite(texData.txTranslationV) ? texData.txTranslationV : 0;
            const rotation = Number.isFinite(texData.txRotation) ? texData.txRotation : 0;
            texture.repeat.set(scaleU, scaleV);
            texture.offset.set(translateU, translateV);
            texture.rotation = rotation;
            texture.center.set(0, 0);
            texture.matrixAutoUpdate = true;
        }

        texture.needsUpdate = true;
        return texture;
    }

    static textureCacheKey(textureId, usdScene, mapProperty = '', textureSignatureCache = null) {
        const role = this.textureColorRole(mapProperty);
        const signature = this.textureSignature(textureId, usdScene, textureSignatureCache);
        return `${role}:${JSON.stringify(signature)}`;
    }

    static applyTextureMapDefaults(texture, mapProperty, authoredColorSpace = '',
        colorMetadata = null) {
        if (!texture) return texture;
        const transform = textureColorTransform(
            authoredColorSpace, 'lin_rec709_scene', colorMetadata);
        const isColor = transform
            ? transform.colorRole === 'color'
            : this.textureColorRole(mapProperty) === 'color';
        texture.colorSpace = isColor
            ? THREE.SRGBColorSpace
            : THREE.NoColorSpace;
        const compression = texture.userData?.lightusdCompression;
        if (texture.isCompressedTexture && compression) {
            // Keep Three's base format constant; colorSpace controls which GPU
            // compressed internal format WebGLUtils chooses.
            texture.format = compression.linearFormat;
        }
        texture.needsUpdate = true;
        return texture;
    }

    static textureColorMetadata(textureId, usdScene) {
        if (textureId === undefined || textureId === null || textureId < 0 ||
            !usdScene || typeof usdScene.getTexture !== 'function') return null;
        try {
            const texture = usdScene.getTexture(textureId);
            if (!texture || texture.textureImageId === undefined ||
                typeof usdScene.getImageCopy !== 'function') return null;
            return usdScene.getImageCopy(texture.textureImageId) || null;
        } catch (_) {
            return null;
        }
    }

    static textureSourceColorSpace(textureId, usdScene, image = null) {
        image = image || this.textureColorMetadata(textureId, usdScene);
        const resolved = image?.sourceColorSpaceName;
        if (resolved && resolved !== 'auto') return resolved;
        return image?.usdColorSpace || image?.colorSpace || '';
    }

    static createDefaultMaterial() {
        return new THREE.MeshPhysicalMaterial({
            color: new THREE.Color(0.18, 0.18, 0.18),
            emissive: 0x000000,
            metalness: 0.0,
            roughness: 0.5,
            transparent: false,
            depthTest: true,
            side: THREE.FrontSide
        });
    }

    static workingColorToDisplayLinear(rgb, usdScene) {
        if (!Array.isArray(rgb)) return rgb;
        const metadata = usdScene && typeof usdScene.getSceneMetadata === 'function'
            ? usdScene.getSceneMetadata() : null;
        const m = metadata?.workingToDisplayLinear;
        if (!m || m.length !== 9) return rgb;
        return [m[0] * rgb[0] + m[1] * rgb[1] + m[2] * rgb[2],
            m[3] * rgb[0] + m[4] * rgb[1] + m[5] * rgb[2],
            m[6] * rgb[0] + m[7] * rgb[1] + m[8] * rgb[2]];
    }

    static textureSignature(textureId, usdScene, textureSignatureCache = null) {
        if (textureId === undefined || textureId === null || textureId < 0 ||
            !usdScene || typeof usdScene.getTexture !== 'function') {
            return textureId;
        }
        if (textureSignatureCache && textureSignatureCache.has(textureId)) {
            return textureSignatureCache.get(textureId);
        }
        let signature = textureId;
        try {
            const texture = usdScene.getTexture(textureId);
            if (texture && texture.textureImageId !== undefined) {
                signature = {
                    imageId: texture.textureImageId,
                    wrapS: texture.wrapS,
                    wrapT: texture.wrapT,
                    hasTransform2d: !!texture.hasTransform2d,
                    txRotation: texture.txRotation,
                    txScaleU: texture.txScaleU,
                    txScaleV: texture.txScaleV,
                    txTranslationU: texture.txTranslationU,
                    txTranslationV: texture.txTranslationV,
                    isUDIM: !!texture.isUDIM,
                    udimTextureId: texture.udimTextureId,
                    udimUvScaleU: texture.udimUvScaleU,
                    udimUvScaleV: texture.udimUvScaleV,
                    udimUvOffsetU: texture.udimUvOffsetU,
                    udimUvOffsetV: texture.udimUvOffsetV
                };
            }
        } catch (_) {
            signature = textureId;
        }
        if (textureSignatureCache) {
            textureSignatureCache.set(textureId, signature);
        }
        return signature;
    }

    static textureDebugInfo(textureId, usdScene, mapProperty = '', sourceFileName = '') {
        const info = {
            sourceFileName: sourceFileName || undefined,
            mapProperty,
            textureId
        };
        if (textureId === undefined || textureId === null || textureId < 0 ||
            !usdScene || typeof usdScene.getTexture !== 'function') {
            return info;
        }
        try {
            const texture = usdScene.getTexture(textureId);
            if (texture) {
                info.textureImageId = texture.textureImageId;
                info.wrapS = texture.wrapS;
                info.wrapT = texture.wrapT;
                info.isUDIM = !!texture.isUDIM;
                if (texture.udimTextureId !== undefined) {
                    info.udimTextureId = texture.udimTextureId;
                }
            }
            if (texture && texture.textureImageId !== undefined &&
                typeof usdScene.getImageCopy === 'function') {
                const image = usdScene.getImageCopy(texture.textureImageId);
                if (image) {
                    info.textureAssetPath = image.uri || undefined;
                    if (info.textureAssetPath &&
                        /<UDIM>|%3CUDIM%3E|\.1\d{3}\./i.test(info.textureAssetPath)) {
                        info.isUDIM = true;
                        info.udimUnsupported = true;
                    }
                    info.bufferId = image.bufferId;
                    info.decoded = !!image.decoded;
                    info.width = image.width;
                    info.height = image.height;
                    info.channels = image.channels;
                    info.colorSpace = image.colorSpace;
                    info.hasData = !!image.data;
                    info.dataBytes = image.data?.byteLength ?? image.data?.length;
                }
            }
        } catch (error) {
            info.inspectError = error?.message || String(error);
        }
        return info;
    }

    static textureLoadErrorInfo(error) {
        const out = {
            message: error?.message || error?.type || String(error)
        };
        const target = error?.target || error?.currentTarget;
        if (target) {
            out.eventType = error?.type;
            out.requestedSrc = target.src || undefined;
            out.currentSrc = target.currentSrc || undefined;
            out.naturalWidth = target.naturalWidth;
            out.naturalHeight = target.naturalHeight;
        }
        return out;
    }

    static stableMaterialStringify(value, usdScene = null, textureSignatureCache = null) {
        const skipKeys = new Set([
            'name',
            'abs_path',
            'display_name',
            'primName',
            'absPath',
            'displayName'
        ]);
        const visit = (v) => {
            if (Array.isArray(v)) {
                return v.map(visit);
            }
            if (v && typeof v === 'object') {
                const out = {};
                for (const key of Object.keys(v).sort()) {
                    if (!skipKeys.has(key)) {
                        out[key] = key.endsWith('TextureId') ?
                            this.textureSignature(v[key], usdScene, textureSignatureCache) :
                            visit(v[key]);
                    }
                }
                return out;
            }
            return v;
        };
        return JSON.stringify(visit(value));
    }

    static makeMaterialSignature(materialData, preferredMaterialType = 'auto', usdScene = null, textureSignatureCache = null) {
        let parsedMaterial = materialData;
        if (typeof materialData === 'string') {
            try {
                parsedMaterial = JSON.parse(materialData);
            } catch (_e) {
                return null;
            }
        }
        if (!parsedMaterial || typeof parsedMaterial !== 'object') {
            return null;
        }

        const typeInfo = this.getMaterialType(parsedMaterial);
        let shader = null;
        const useFullSignature = preferredMaterialType === 'full' ||
            preferredMaterialType === 'openpbr' ||
            (preferredMaterialType === 'auto' && typeInfo.hasOpenPBR);
        if (useFullSignature) {
            if (typeInfo.hasOpenPBR && parsedMaterial.openPBR) {
                return `openpbr:${this.stableMaterialStringify(parsedMaterial.openPBR, usdScene, textureSignatureCache)}`;
            }
            if (typeInfo.hasUsdPreviewSurface && parsedMaterial.surfaceShader) {
                return `preview-full:${this.stableMaterialStringify(parsedMaterial.surfaceShader, usdScene, textureSignatureCache)}`;
            }
        } else if (preferredMaterialType === 'usdpreviewsurface' || preferredMaterialType === 'preview' ||
            (preferredMaterialType === 'auto' && typeInfo.hasUsdPreviewSurface && !typeInfo.hasOpenPBR)) {
            shader = parsedMaterial.surfaceShader || parsedMaterial;
        } else if (typeInfo.hasUsdPreviewSurface && !typeInfo.hasOpenPBR) {
            shader = parsedMaterial.surfaceShader || parsedMaterial;
        } else {
            return null;
        }

        const fields = [
            'diffuseColor', 'diffuseColorTextureId',
            'emissiveColor', 'emissiveColorTextureId',
            'specularColor', 'specularColorTextureId',
            'metallic', 'metallicTextureId',
            'roughness', 'roughnessTextureId',
            'clearcoat', 'clearcoatTextureId',
            'clearcoatRoughness', 'clearcoatRoughnessTextureId',
            'opacity', 'opacityTextureId',
            'ior', 'iorTextureId',
            'normalTextureId',
            'occlusionTextureId',
            'displacementTextureId',
            'useSpecularWorkflow'
        ];
        const compact = {};
        for (const field of fields) {
            if (Object.prototype.hasOwnProperty.call(shader, field)) {
                compact[field] = field.endsWith('TextureId') ?
                    this.textureSignature(shader[field], usdScene, textureSignatureCache) :
                    shader[field];
            }
        }
        return JSON.stringify(compact);
    }

    //
    // Convert UsdPreviewSureface to MeshPhysicalMaterial
    // - [x] diffuseColor -> color
    // - [x] ior -> ior
    // - [x] clearcoat -> clearcoat
    // - [x] clearcoatRoughness -> clearcoatRoughness
    // - [x] specularColor -> specular
    // - [x] roughness -> roughness
    // - [x] metallic -> metalness
    // - [x] emissiveColor -> emissive
    // - [x] opacity -> opacity (TODO: map to .transmission?)
    // - [x] occlusion -> aoMap
    // - [x] normal -> normalMap
    // - [x] displacement -> displacementMap
    //
    // Options:
    // - textureLoadingManager: TextureLoadingManager instance for delayed texture loading
    //   If provided, textures are queued instead of loaded immediately
    //
    static convertUsdMaterialToMeshPhysicalMaterial(usdMaterial, usdScene, options = {}) {
        const material = new THREE.MeshPhysicalMaterial();
        const textureManager = options.textureLoadingManager || null;
        const materialName = usdMaterial?.name || usdMaterial?.primName ||
            usdMaterial?.displayName || usdMaterial?.absPath ||
            usdMaterial?.abs_path || usdMaterial?.display_name;

        // Helper to load texture immediately or queue for later
        const loadOrQueueTexture = (mapProperty, textureId, textureOptions = {}) => {
            // Opt-out for geometry/structure-focused viewers that don't need
            // textures (avoids fetching — and 404-ing on — unresolved texture
            // URIs).
            if (options.skipTextures) {
                return;
            }
            if (textureManager) {
                // Delayed mode: queue texture for later loading
                textureManager.queueTexture(material, mapProperty, textureId, usdScene, {
                    ...textureOptions,
                    sourceFileName: options.sourceFileName || ''
                });
            } else {
                // Immediate mode: load texture now (original behavior)
                const textureInfo = this.textureDebugInfo(
                    textureId, usdScene, mapProperty, options.sourceFileName);
                if (materialName) {
                    textureInfo.materialName = materialName;
                }
                this.getTextureFromUSD(usdScene, textureId, mapProperty).then((texture) => {
                    const colorMetadata = this.textureColorMetadata(
                        textureId, usdScene);
                    const authoredColorSpace = this.textureSourceColorSpace(
                        textureId, usdScene, colorMetadata);
                    this.applyTextureMapDefaults(texture, mapProperty,
                        authoredColorSpace, colorMetadata);
                    material[mapProperty] = texture;
                    installTextureColorTransform(material, mapProperty,
                        textureColorTransform(authoredColorSpace,
                            'lin_rec709_scene', colorMetadata));
                    material.needsUpdate = true;
                }).catch((err) => {
                    // Deduplicate: scenes can reference the same missing/failing
                    // texture thousands of times (and re-convert on every option
                    // toggle), which floods the console. Log each unique failure
                    // once.
                    const errorInfo = this.textureLoadErrorInfo(err);
                    const key = JSON.stringify({
                        mapProperty,
                        textureAssetPath: textureInfo.textureAssetPath,
                        requestedSrc: errorInfo.requestedSrc,
                        message: errorInfo.message,
                        name: err?.name
                    });
                    const seen = (LightUSDLoaderUtils._textureErrorSeen ||
                        (LightUSDLoaderUtils._textureErrorSeen = new Set()));
                    if (!seen.has(key)) {
                        seen.add(key);
                        const isUnsupportedUDIM = err?.name === 'UnsupportedUDIMTextureError';
                        console.warn(
                            isUnsupportedUDIM ?
                                `unsupported UDIM ${mapProperty} texture; skipping texture fetch (further identical warnings suppressed)` :
                                `failed to load ${mapProperty} texture (further identical errors suppressed)`,
                            {
                                ...textureInfo,
                                error: errorInfo
                            }
                        );
                    }
                });
            }
        };

        // Diffuse color and texture
        material.color = new THREE.Color(0.18, 0.18, 0.18);
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'diffuseColor')) {
            const color = this.workingColorToDisplayLinear(
                usdMaterial.diffuseColor, usdScene);
            material.color = new THREE.Color(color[0], color[1], color[2]);
        }

        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'diffuseColorTextureId')) {
            material.color = new THREE.Color(1, 1, 1);
            loadOrQueueTexture('map', usdMaterial.diffuseColorTextureId);
        }

        // IOR
        material.ior = 1.5;
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'ior')) {
            material.ior = usdMaterial.ior;
        }

        // Clearcoat
        material.clearcoat = 0.0;
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'clearcoat')) {
            material.clearcoat = usdMaterial.clearcoat;
        }

        material.clearcoatRoughness = 0.0;
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'clearcoatRoughness')) {
            material.clearcoatRoughness = usdMaterial.clearcoatRoughness;
        }

        // Workflow selection
        material.useSpecularWorkflow = false;
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'useSpecularWorkflow')) {
            material.useSpecularWorkflow = usdMaterial.useSpecularWorkflow;
        }

        if (material.useSpecularWorkflow) {
            material.specularColor = new THREE.Color(0.0, 0.0, 0.0);
            if (Object.prototype.hasOwnProperty.call(usdMaterial, 'specularColor')) {
                const color = this.workingColorToDisplayLinear(
                    usdMaterial.specularColor, usdScene);
                material.specularColor = new THREE.Color(color[0], color[1], color[2]);
            }
            if (Object.prototype.hasOwnProperty.call(usdMaterial, 'specularColorTextureId')) {
                loadOrQueueTexture('specularColorMap', usdMaterial.specularColorTextureId);
            }
        } else {
            material.metalness = 0.0;
            if (Object.prototype.hasOwnProperty.call(usdMaterial, 'metallic')) {
                material.metalness = usdMaterial.metallic;
            }
            if (Object.prototype.hasOwnProperty.call(usdMaterial, 'metallicTextureId')) {
                loadOrQueueTexture('metalnessMap', usdMaterial.metallicTextureId);
            }
        }

        // Roughness
        material.roughness = 0.5;
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'roughness')) {
            material.roughness = usdMaterial.roughness;
        }
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'roughnessTextureId')) {
            loadOrQueueTexture('roughnessMap', usdMaterial.roughnessTextureId);
        }

        // Emissive
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'emissiveColor')) {
            const color = this.workingColorToDisplayLinear(
                usdMaterial.emissiveColor, usdScene);
            material.emissive = new THREE.Color(color[0], color[1], color[2]);
        }
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'emissiveColorTextureId')) {
            loadOrQueueTexture('emissiveMap', usdMaterial.emissiveColorTextureId);
        }

        // Opacity
        material.opacity = 1.0;
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'opacity')) {
            material.opacity = usdMaterial.opacity;
            if (material.opacity < 1.0) {
                material.transparent = true;
            }
        }
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'opacityTextureId')) {
            loadOrQueueTexture('alphaMap', usdMaterial.opacityTextureId);
        }

        // Ambient Occlusion
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'occlusionTextureId')) {
            loadOrQueueTexture('aoMap', usdMaterial.occlusionTextureId);
        }

        // Normal Map
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'normalTextureId')) {
            loadOrQueueTexture('normalMap', usdMaterial.normalTextureId);
        }

        // Displacement Map
        if (Object.prototype.hasOwnProperty.call(usdMaterial, 'displacementTextureId')) {
            loadOrQueueTexture('displacementMap', usdMaterial.displacementTextureId, { displacementScale: 1.0 });
        }

        return material;
    }

    //
    // Material Type Detection
    //
    // Returns an object describing what material types are available:
    // {
    //   hasOpenPBR: boolean,           // Has OpenPBR (MaterialX) data
    //   hasUsdPreviewSurface: boolean, // Has UsdPreviewSurface data
    //   hasBoth: boolean,              // Has both material types
    //   hasNone: boolean,              // Has no material data
    //   recommended: string            // Recommended type: 'openpbr', 'usdpreviewsurface', or 'none'
    // }
    //
    // Usage:
    //   const materialData = usdScene.getMaterial(materialId, 'json');
    //   const typeInfo = LightUSDLoaderUtils.getMaterialType(materialData);
    //
    static getMaterialType(materialData) {
        // Parse JSON if needed
        let parsedMaterial = materialData;
        if (typeof materialData === 'string') {
            try {
                parsedMaterial = JSON.parse(materialData);
            } catch (e) {
                console.error('Failed to parse material JSON:', e);
                return {
                    hasOpenPBR: false,
                    hasUsdPreviewSurface: false,
                    hasBoth: false,
                    hasNone: true,
                    recommended: 'none'
                };
            }
        }

        if (!parsedMaterial) {
            return {
                hasOpenPBR: false,
                hasUsdPreviewSurface: false,
                hasBoth: false,
                hasNone: true,
                recommended: 'none'
            };
        }

        const hasOpenPBR = !!parsedMaterial.hasOpenPBR;
        const hasUsdPreviewSurface = !!parsedMaterial.hasUsdPreviewSurface;
        const hasBoth = hasOpenPBR && hasUsdPreviewSurface;
        const hasNone = !hasOpenPBR && !hasUsdPreviewSurface;

        // Determine recommended type (prefer OpenPBR when both are available)
        let recommended = 'none';
        if (hasOpenPBR) {
            recommended = 'openpbr';
        } else if (hasUsdPreviewSurface) {
            recommended = 'usdpreviewsurface';
        }

        return {
            hasOpenPBR,
            hasUsdPreviewSurface,
            hasBoth,
            hasNone,
            recommended
        };
    }

    //
    // Get material type as a human-readable string
    //
    // Returns: 'OpenPBR', 'UsdPreviewSurface', 'Both', or 'None'
    //
    static getMaterialTypeString(materialData) {
        const typeInfo = this.getMaterialType(materialData);

        if (typeInfo.hasBoth) return 'Both';
        if (typeInfo.hasOpenPBR) return 'OpenPBR';
        if (typeInfo.hasUsdPreviewSurface) return 'UsdPreviewSurface';
        return 'None';
    }

    //
    // Convert OpenPBR (MaterialX) to MeshPhysicalMaterial
    // Supports all OpenPBR layers: base, specular, transmission, coat, sheen, fuzz, thin_film, emission
    //
    // Usage:
    //   const materialData = usdScene.getMaterial(materialId, 'json');
    //   const material = await LightUSDLoaderUtils.convertOpenPBRMaterialToMeshPhysicalMaterial(materialData, usdScene, options);
    //
    static async convertOpenPBRMaterialToMeshPhysicalMaterial(materialData, usdScene, options = {}) {
        // Parse JSON material data if it's a string
        let parsedMaterial = materialData;
        if (typeof materialData === 'string') {
            try {
                parsedMaterial = JSON.parse(materialData);
            } catch (e) {
                console.error('Failed to parse material JSON:', e);
                return this.createDefaultMaterial();
            }
        }

        // Check if material has OpenPBR data
        if (!parsedMaterial || !parsedMaterial.hasOpenPBR) {
            console.warn('Material does not have OpenPBR data, falling back to UsdPreviewSurface');
            // Fall back to UsdPreviewSurface if available
            if (parsedMaterial && parsedMaterial.hasUsdPreviewSurface) {
                // Extract surfaceShader data from the JSON structure
                const shaderData = parsedMaterial.surfaceShader || parsedMaterial;
                return this.convertUsdMaterialToMeshPhysicalMaterial(shaderData, usdScene, options);
            }
            return this.createDefaultMaterial();
        }

        try {
            // Use the LightUSDMaterialX converter (Loaded version waits for textures(if textureLoadingManager is null))
	            const material = await convertOpenPBRToMeshPhysicalMaterialLoaded(parsedMaterial, usdScene, {
	                envMap: options.envMap || null,
	                envMapIntensity: options.envMapIntensity || 1.0,
	                textureCache: options.textureCache || new Map(),
	                textureLoadingManager: options.textureLoadingManager || null,
	                skipTextures: options.skipTextures || false
	            });

            // Apply sideness based on USD doubleSided attribute
            if (options.doubleSided !== undefined) {
                material.side = options.doubleSided ? THREE.DoubleSide : THREE.FrontSide;
            }

            return material;

        } catch (error) {
            console.error('Failed to convert OpenPBR material:', error);
            return this.createDefaultMaterial();
        }
    }

    //
    // Smart material conversion: automatically selects OpenPBR or UsdPreviewSurface
    //
    // Options:
    //   preferredMaterialType: 'auto' | 'openpbr' | 'usdpreviewsurface'
    //     - 'auto': Prefer OpenPBR when both are available (recommended)
    //     - 'openpbr': Force OpenPBR if available, fallback to UsdPreviewSurface
    //     - 'usdpreviewsurface': Force UsdPreviewSurface if available, fallback to OpenPBR
    //
    // Usage:
    //   const materialData = usdScene.getMaterial(materialId, 'json');
    //   const material = await LightUSDLoaderUtils.convertMaterial(materialData, usdScene, options);
    //
    static async convertMaterial(materialData, usdScene, options = {}) {
        // Get material type info
        const typeInfo = this.getMaterialType(materialData);

        // If no material data, return default
        if (typeInfo.hasNone) {
            return this.createDefaultMaterial();
        }

        // Parse material data for conversion
        let parsedMaterial = materialData;
        if (typeof materialData === 'string') {
            try {
                parsedMaterial = JSON.parse(materialData);
            } catch (e) {
                console.error('Failed to parse material JSON:', e);
                return this.createDefaultMaterial();
            }
        }

        // Determine which material type to use based on preference
        const preferredType = options.preferredMaterialType || 'auto';
        let useOpenPBR = false;
        let useUsdPreviewSurface = false;

        switch (preferredType) {
            case 'auto':
                // Auto mode: prefer OpenPBR when available (including when both are present)
                if (typeInfo.hasOpenPBR) {
                    useOpenPBR = true;
                } else if (typeInfo.hasUsdPreviewSurface) {
                    useUsdPreviewSurface = true;
                }
                break;

            case 'openpbr':
                // Force OpenPBR if available, fallback to UsdPreviewSurface
                if (typeInfo.hasOpenPBR) {
                    useOpenPBR = true;
                } else if (typeInfo.hasUsdPreviewSurface) {
                    useUsdPreviewSurface = true;
                    console.warn('OpenPBR requested but not available, falling back to UsdPreviewSurface');
                }
                break;

            case 'usdpreviewsurface':
                // Force UsdPreviewSurface if available, fallback to OpenPBR
                if (typeInfo.hasUsdPreviewSurface) {
                    useUsdPreviewSurface = true;
                } else if (typeInfo.hasOpenPBR) {
                    useOpenPBR = true;
                    console.warn('UsdPreviewSurface requested but not available, falling back to OpenPBR');
                }
                break;

            default:
                // Unknown preference, use auto behavior
                if (typeInfo.hasOpenPBR) {
                    useOpenPBR = true;
                } else if (typeInfo.hasUsdPreviewSurface) {
                    useUsdPreviewSurface = true;
                }
        }

        // Log material type selection for debugging
        if (typeInfo.hasBoth) {
        }

        // Convert using selected material type
        if (useOpenPBR) {
            return this.convertOpenPBRMaterialToMeshPhysicalMaterial(parsedMaterial, usdScene, options);
        } else if (useUsdPreviewSurface) {
            // Extract surfaceShader data from the JSON structure
            // The JSON format nests shader properties under surfaceShader
            const shaderData = parsedMaterial.surfaceShader || parsedMaterial;
            // Pass options through to support textureLoadingManager
            return this.convertUsdMaterialToMeshPhysicalMaterial(shaderData, usdScene, options);
        }

        return this.createDefaultMaterial();
    }

    static _copyHeapAttribute(desc) {
        const lightusd = LightUSDLoaderUtils._lightusd;
        if (!lightusd || !desc || desc.ptr === undefined || desc.length === undefined) {
            return null;
        }

        const ptr = Number(desc.ptr);
        const length = Number(desc.length);
        if (!Number.isFinite(ptr) || !Number.isFinite(length) || length <= 0) {
            return null;
        }

        switch (desc.dtype) {
            case 'f32':
                return new Float32Array(lightusd.HEAPF32.subarray(ptr >> 2, (ptr >> 2) + length));
            case 'u32':
                return new Uint32Array(lightusd.HEAPU32.subarray(ptr >> 2, (ptr >> 2) + length));
            case 'snorm16':
                return new Int16Array(lightusd.HEAP16.subarray(ptr >> 1, (ptr >> 1) + length));
            case 'snorm8':
            case 'u8':
                return new Int8Array(lightusd.HEAP8.subarray(ptr, ptr + length));
            default:
                return null;
        }
    }

    static _canUseMeshPtr() {
        const lightusd = LightUSDLoaderUtils._lightusd;
        return !!(lightusd &&
            lightusd.HEAPF32 &&
            lightusd.HEAPU32 &&
            lightusd.HEAP16 &&
            lightusd.HEAP8);
    }

    static convertUsdMeshPtrToThreeMesh(mesh, options = {}) {
        if (!mesh || !mesh.points) {
            return null;
        }

        const geometry = new THREE.BufferGeometry();

        const points = this._copyHeapAttribute(mesh.points);
        if (!points) {
            return null;
        }
        geometry.setAttribute('position', new THREE.BufferAttribute(points, 3));

        if (mesh.indices) {
            const indices = this._copyHeapAttribute(mesh.indices);
            if (indices && indices.length > 0) {
                geometry.setIndex(new THREE.BufferAttribute(indices, 1));
            }
        }

        if (mesh.uv0) {
            const uv0 = this._copyHeapAttribute(mesh.uv0);
            if (uv0) {
                geometry.setAttribute('uv', new THREE.BufferAttribute(uv0, 2));
            }
        }

        if (mesh.normals) {
            const normals = this._copyHeapAttribute(mesh.normals);
            if (normals) {
                if (mesh.normals.dtype === 'snorm8' || mesh.normals.dtype === 'snorm16') {
                    geometry.setAttribute('normal', new THREE.BufferAttribute(normals, 3, true));
                } else {
                    geometry.setAttribute('normal', new THREE.BufferAttribute(normals, 3));
                }
            }
        } else {
            geometry.computeVertexNormals();
        }

        if (mesh.vertexColors) {
            const colors = this._copyHeapAttribute(mesh.vertexColors);
            if (colors) {
                const normalized = mesh.vertexColors.dtype === 'u8' || mesh.vertexColors.dtype === 'i8';
                geometry.setAttribute('color', new THREE.BufferAttribute(colors, 3, normalized));
            }
        }

        if (options.computeMissingTangents &&
            geometry.attributes.uv &&
            geometry.attributes.normal) {
            geometry.computeTangents();
        }

        geometry.userData['doubleSided'] = mesh.doubleSided;
        if (Object.prototype.hasOwnProperty.call(mesh, 'submeshes') &&
            mesh.submeshes.length > 0) {
            geometry.userData['submeshes'] = mesh.submeshes;
        }

        return geometry;
    }

    static convertUsdMeshToThreeMesh(mesh, options = {}) {
        const geometry = new THREE.BufferGeometry();
        // IMPORTANT: Copy all typed arrays from WASM heap into JS-owned buffers.
        // The C++ LightUSDLoaderNative object is explicitly deleted via .delete()
        // at the end of processUSDScene to free WASM heap memory. After deletion,
        // typed_memory_view references into render_scene_ become stale (data freed
        // by C++ destructor, overwritten by allocator bookkeeping).

        // Validate WASM buffer health before copying.
        // Emscripten typed_memory_view returns TypedArrays sharing the WASM
        // heap's ArrayBuffer. If memory.grow() is called (heap resize), ALL
        // existing views' backing buffer is detached (byteLength becomes 0).
        // Detect this and re-fetch the mesh from WASM if needed.
        const meshName = mesh.primName || mesh.absPath || '(unknown)';
        const expectedPointsLength = mesh.pointsLength ?? mesh.points?.length ?? 0;
        const expectedIndicesLength = mesh.faceVertexIndicesLength ?? mesh.faceVertexIndices?.length ?? 0;
        if (expectedPointsLength > 0 && mesh.points && mesh.points.buffer && mesh.points.buffer.byteLength === 0) {
          console.error(`[WASM] DETACHED buffer for mesh "${meshName}" points! WASM heap likely grew.`);
        }
        if (expectedIndicesLength > 0 && mesh.faceVertexIndices && mesh.faceVertexIndices.buffer && mesh.faceVertexIndices.buffer.byteLength === 0) {
          console.error(`[WASM] DETACHED buffer for mesh "${meshName}" faceVertexIndices! WASM heap likely grew.`);
        }

        geometry.setAttribute('position', new THREE.BufferAttribute(new Float32Array(mesh.points), 3));

        if (Object.prototype.hasOwnProperty.call(mesh, 'faceVertexIndices')) {
          if (mesh.faceVertexIndices.length >0 ) {
            const indices = new Uint32Array(mesh.faceVertexIndices);
            // Validate: check for out-of-range indices (common corruption indicator)
            const numVertices = mesh.points.length / 3;
            let maxIdx = 0, oobCount = 0, zeroIdxCount = 0;
            for (let i = 0; i < indices.length; i++) {
              if (indices[i] >= numVertices) oobCount++;
              if (indices[i] === 0) zeroIdxCount++;
              if (indices[i] > maxIdx) maxIdx = indices[i];
            }
            if (oobCount > 0) {
              console.error(`[MESH] "${meshName}": ${oobCount}/${indices.length} indices OUT OF RANGE (max idx=${maxIdx}, numVerts=${numVertices})`);
            }
            // High zero-index ratio can indicate corrupted/zeroed-out index data
            const zeroRatio = zeroIdxCount / indices.length;
            if (zeroRatio > 0.3 && indices.length > 100) {
              console.warn(`[MESH] "${meshName}": ${(zeroRatio*100).toFixed(1)}% of indices are 0 (${zeroIdxCount}/${indices.length}) — possible data corruption`);
            }
            geometry.setIndex(new THREE.BufferAttribute(indices, 1));
          }
        }

        if (Object.prototype.hasOwnProperty.call(mesh, 'texcoords')) {
            geometry.setAttribute('uv', new THREE.BufferAttribute(new Float32Array(mesh.texcoords), 2));
        }

        // TODO: uv1

        // faceVarying normals — SNorm8/SNorm16 (normalized int) or Float32
        if (Object.prototype.hasOwnProperty.call(mesh, 'normals')) {
            if (mesh.normalsFormat === 'snorm8') {
                geometry.setAttribute('normal',
                    new THREE.BufferAttribute(new Int8Array(mesh.normals), 3, true));
            } else if (mesh.normalsFormat === 'snorm16') {
                geometry.setAttribute('normal',
                    new THREE.BufferAttribute(new Int16Array(mesh.normals), 3, true));
            } else {
                geometry.setAttribute('normal',
                    new THREE.BufferAttribute(new Float32Array(mesh.normals), 3));
            }
        } else {
            geometry.computeVertexNormals();
        }

        if (Object.prototype.hasOwnProperty.call(mesh, 'vertexColors')) {
            geometry.setAttribute('color', new THREE.BufferAttribute(new Float32Array(mesh.vertexColors), 3));
        }

        // Only compute tangents if we have both UV coordinates and normals
        if (Object.prototype.hasOwnProperty.call(mesh, 'tangents')) {
            geometry.setAttribute('tangent', new THREE.BufferAttribute(new Float32Array(mesh.tangents), 4));
        } else if (options.computeMissingTangents && Object.prototype.hasOwnProperty.call(mesh, 'texcoords') && (Object.prototype.hasOwnProperty.call(mesh, 'normals') || geometry.attributes.normal)) {
            // TODO: try MikTSpace tangent algorithm: https://threejs.org/docs/#examples/en/utils/BufferGeometryUtils.computeMikkTSpaceTangents
            geometry.computeTangents();
        }

        // TODO: vertex opacities(per-vertex alpha)

        // Three.js does not have sideness attribute in Mesh.
        // Store doubleSided param to customData
        if (Object.prototype.hasOwnProperty.call(mesh, 'doubleSided')) {
          geometry.userData['doubleSided'] = mesh.doubleSided;
        } else {
        }

        // Store submesh data for multi-material support (pre-computed in C++)
        if (Object.prototype.hasOwnProperty.call(mesh, 'submeshes') && mesh.submeshes.length > 0) {
          geometry.userData['submeshes'] = mesh.submeshes;
        }

        return geometry;
    }

    static compactIndexedMaterialGroups(geometry, submeshes, materialIdToIndex, materials = null) {
        if (!geometry?.index || !Array.isArray(submeshes) || submeshes.length < 2) {
            return false;
        }
        if (Array.isArray(materials) && materials.some((mat) => mat?.transparent)) {
            return false;
        }

        const indexArray = geometry.index.array;
        if (!indexArray || !Number.isFinite(indexArray.length) || indexArray.length === 0) {
            return false;
        }

        const buckets = new Map();
        let copiedCount = 0;
        for (const submesh of submeshes) {
            const start = submesh.start | 0;
            const count = submesh.count | 0;
            if (count <= 0 || start < 0 || start + count > indexArray.length) {
                return false;
            }
            const matIndex = materialIdToIndex.get(submesh.materialId);
            if (matIndex === undefined) {
                return false;
            }
            if (!buckets.has(matIndex)) {
                buckets.set(matIndex, []);
            }
            buckets.get(matIndex).push({ start, count });
            copiedCount += count;
        }

        if (copiedCount !== indexArray.length || buckets.size >= submeshes.length) {
            return false;
        }

        const reordered = new indexArray.constructor(indexArray.length);
        const groups = [];
        let offset = 0;
        for (const [matIndex, ranges] of buckets.entries()) {
            const groupStart = offset;
            for (const range of ranges) {
                reordered.set(indexArray.subarray(range.start, range.start + range.count), offset);
                offset += range.count;
            }
            groups.push({
                start: groupStart,
                count: offset - groupStart,
                materialIndex: matIndex
            });
        }

        geometry.setIndex(new THREE.BufferAttribute(reordered, 1));
        geometry.clearGroups();
        for (const group of groups) {
            geometry.addGroup(group.start, group.count, group.materialIndex);
        }
        geometry.userData.compactedMaterialGroups = {
            before: submeshes.length,
            after: groups.length
        };
        return true;
    }

    static async setupMesh(mesh /* LightUSDLoaderNative::RenderMesh */, defaultMtl, usdScene, options) {

        const geometryStart = performance.now();
        const geometry = mesh && mesh._meshPtr ?
            this.convertUsdMeshPtrToThreeMesh(mesh, options) :
            this.convertUsdMeshToThreeMesh(mesh, options);
        if (!geometry) {
            const meshName = mesh?.primName || mesh?.absPath || '(unknown mesh)';
            const pointCount = mesh?.vertexCount ?? mesh?.points?.count ?? (mesh?.points?.length ? mesh.points.length / 3 : 0);
            const indexCount = mesh?.indices?.length ?? mesh?.faceVertexIndices?.length ?? 0;
            throw new Error(`Failed to build Three.js geometry for "${meshName}" (points=${pointCount}, indices=${indexCount})`);
        }
        if (options._debugState) {
            options._debugState.geometryMs += performance.now() - geometryStart;
        }

        const normalMtl = new THREE.MeshNormalMaterial();
        const materialCache = options.materialCache || null;
        const materialSignatureCache = options.materialSignatureCache || null;
        const preferredMaterialType = options.preferredMaterialType || 'auto';

        const getMaterialForId = async (matId) => {
            if (matId === undefined || matId < 0) {
                return null;
            }

            const doubleSided = geometry.userData['doubleSided'];
            const cacheKey = `${matId}|${preferredMaterialType}|${doubleSided === true ? 'double' : 'front'}`;
            if (materialCache && materialCache.has(cacheKey)) {
                if (options._debugState) {
                    options._debugState.materialCacheHits++;
                }
                return materialCache.get(cacheKey);
            }

            if (options._debugState) {
                options._debugState.materialCacheMisses++;
            }
            const materialStart = performance.now();
            const result = usdScene.getMaterialWithFormat ?
                usdScene.getMaterialWithFormat(matId, 'json') :
                { error: false, data: JSON.stringify(usdScene.getMaterial(matId)) };
            if (result.error) {
                console.warn(`Failed to get material ${matId} with format: ${result.error}`);
                if (options._debugState) {
                    options._debugState.materialMs += performance.now() - materialStart;
                }
                return null;
            }

            const materialData = typeof result.data === 'string' ?
                JSON.parse(result.data) :
                result.data;

            const signatureStart = performance.now();
            const signature = materialSignatureCache ?
                this.makeMaterialSignature(
                    materialData,
                    options.fastMaterialMode === 'preview' ? 'preview' : preferredMaterialType,
                    usdScene,
                    options.materialSignatureTextureCache || null) :
                null;
            if (options._debugState) {
                options._debugState.materialSignatureMs += performance.now() - signatureStart;
            }
            if (signature && materialSignatureCache) {
                const signatureKey = signature + `|${doubleSided === true ? 'double' : 'front'}`;
                if (materialSignatureCache.has(signatureKey)) {
                    if (options._debugState) {
                        options._debugState.materialSignatureCacheHits++;
                    }
                    const cached = materialSignatureCache.get(signatureKey);
                    if (materialCache) {
                        materialCache.set(cacheKey, cached);
                    }
                    if (options._debugState) {
                        options._debugState.materialMs += performance.now() - materialStart;
                    }
                    return cached;
                }
                if (options._debugState) {
                    options._debugState.materialSignatureCacheMisses++;
                }
            }

            const convertStart = performance.now();
            const material = await this.convertMaterial(materialData, usdScene, {
                preferredMaterialType: options.fastMaterialMode === 'preview' ? 'usdpreviewsurface' : preferredMaterialType,
                envMap: options.envMap || null,
                envMapIntensity: options.envMapIntensity || 1.0,
                textureCache: options.textureCache || new Map(),
                doubleSided,
                textureLoadingManager: options.textureLoadingManager || null,
                skipTextures: options.skipTextures || false,
                sourceFileName: options.sourceFileName || ''
            });
            if (options._debugState) {
                options._debugState.materialConvertMs += performance.now() - convertStart;
            }

            material.envMap = options.envMap || null;
            material.envMapIntensity = options.envMapIntensity || 1.0;
            material.side = doubleSided ? THREE.DoubleSide : THREE.FrontSide;
            material.userData.rawData = materialData;
            material.userData.typeInfo = this.getMaterialType(materialData);
            material.userData.typeString = this.getMaterialTypeString(materialData);
            if (options._debugState) {
                options._debugState.materialMs += performance.now() - materialStart;
            }

            if (materialCache) {
                materialCache.set(cacheKey, material);
            }
            if (materialSignatureCache) {
                const signature = this.makeMaterialSignature(
                    materialData,
                    options.fastMaterialMode === 'preview' ? 'preview' : preferredMaterialType,
                    usdScene,
                    options.materialSignatureTextureCache || null);
                if (signature) {
                    materialSignatureCache.set(signature + `|${doubleSided === true ? 'double' : 'front'}`, material);
                }
            }
            return material;
        };

        let mtl = null;
        const displayColorValues = mesh.displayColor && typeof mesh.displayColor.length === 'number' && mesh.displayColor.length >= 3
            ? Array.from(mesh.displayColor).slice(0, 3).map(Number) : null;
        const displayColor = displayColorValues?.every(Number.isFinite) ? displayColorValues : null;
        const applyDisplayColor = (material) => {
            if (!displayColor || !material?.clone) return material;
            const result = material.clone();
            result.color?.setRGB(displayColor[0], displayColor[1], displayColor[2]);
            return result;
        };

        if (options.overrideMaterial) {
            mtl = defaultMtl || normalMtl
        } else {
            let pbrMaterial = await getMaterialForId(mesh.materialId);
            if (!pbrMaterial) {
                // No valid material - create default material
                pbrMaterial = defaultMtl || new THREE.MeshPhysicalMaterial({
                    color: 0x888888,
                    roughness: 0.5,
                    metalness: 0.0
                });
            }

            // Setting envmap is required for PBR materials to work correctly(e.g. clearcoat)
            pbrMaterial.envMap = options.envMap || null;
            pbrMaterial.envMapIntensity = options.envMapIntensity || 1.0;


            // Sideness is determined by the mesh's USD doubleSided attribute
            if (Object.prototype.hasOwnProperty.call(geometry.userData, 'doubleSided')) {
              if (geometry.userData.doubleSided) {
                pbrMaterial.side = THREE.DoubleSide;
              } else {
                pbrMaterial.side = THREE.FrontSide;
              }
            } else {
              // No doubleSided attribute in USD - default to FrontSide
              pbrMaterial.side = THREE.FrontSide;
            }

            mtl = applyDisplayColor(pbrMaterial || defaultMtl || normalMtl);
        }

        // Handle GeomSubsets (per-face materials)
        if (geometry.userData['submeshes'] && geometry.userData['submeshes'].length > 0) {
            if (options._debugState) {
                options._debugState.multiMaterialMeshes++;
            }
            const submeshes = geometry.userData['submeshes'];

            // Build materials array indexed by materialId
            const materials = [];
            const materialIdToIndex = new Map();

            // First pass: collect unique material IDs
            for (const submesh of submeshes) {
                const matId = submesh.materialId;
                if (!materialIdToIndex.has(matId)) {
                    materialIdToIndex.set(matId, materials.length);
                    materials.push(null); // Placeholder
                }
            }

            // Second pass: load materials
            for (const [matId, matIndex] of materialIdToIndex.entries()) {
                if (matId >= 0) {
                    if (options._debugState) {
                        options._debugState.subsetMaterialRefs++;
                    }
                    materials[matIndex] = applyDisplayColor(await getMaterialForId(matId) || mtl);
                } else {
                materials[matIndex] = mtl; // Use default material
                }
            }

            // Third pass: add geometry groups using pre-computed submesh data
            // (from C++). Many USD files author one GeomSubset per face, which
            // would otherwise become one Three.js draw call per face. For indexed
            // geometry, reorder the index buffer by material and emit one group
            // per material.
            const compactedGroups = options.compactMaterialGroups !== false &&
                this.compactIndexedMaterialGroups(geometry, submeshes, materialIdToIndex, materials);
            if (compactedGroups && options._debugState) {
                const compact = geometry.userData.compactedMaterialGroups;
                options._debugState.compactedGroupMeshes++;
                options._debugState.compactedGroupsBefore += compact.before;
                options._debugState.compactedGroupsAfter += compact.after;
            }
            if (!compactedGroups) {
                for (const submesh of submeshes) {
                    const matIndex = materialIdToIndex.get(submesh.materialId);
                    geometry.addGroup(submesh.start, submesh.count, matIndex);
                }
            }


            // Create mesh with multi-material array
            const meshCreateStart = performance.now();
            const threeMesh = new THREE.Mesh(geometry, materials);
            if (mesh.materialId !== undefined) {
                threeMesh.userData.materialId = mesh.materialId;
            }
            if (displayColor) {
                threeMesh.userData.luciaDisplayColor = displayColor.slice();
            }
            if (options._debugState) {
                options._debugState.meshCreateMs += performance.now() - meshCreateStart;
            }
            return threeMesh;
        } else {
            // Single material mesh
            if (options._debugState) {
                options._debugState.singleMaterialMeshes++;
            }
            const meshCreateStart = performance.now();
            const threeMesh = new THREE.Mesh(geometry, mtl);
            if (mesh.materialId !== undefined) {
                threeMesh.userData.materialId = mesh.materialId;
            }
            if (displayColor) {
                threeMesh.userData.luciaDisplayColor = displayColor.slice();
            }
            if (options._debugState) {
                options._debugState.meshCreateMs += performance.now() - meshCreateStart;
            }
            return threeMesh;
        }
    }


    // arr = float array with 16 elements(row major order)
    static toMatrix4(a) {
        const m = new THREE.Matrix4();

        //m.set(a[0], a[1], a[2], a[3],
        //    a[4], a[5], a[6], a[7],
        //    a[8], a[9], a[10], a[11],
        //    a[12], a[13], a[14], a[15]);
        m.set(a[0], a[4], a[8], a[12],
            a[1], a[5], a[9], a[13],
            a[2], a[6], a[10], a[14],
            a[3], a[7], a[11], a[15]);

        return m;
    }

    /**
     * Count total nodes in USD hierarchy (for progress estimation)
     * @private
     */
    static _countNodes(usdNode) {
        let count = 1;
        if (usdNode.children) {
            for (const child of usdNode.children) {
                count += this._countNodes(child);
            }
        }
        return count;
    }

    /**
     * Count total meshes in USD hierarchy
     * @private
     */
    static _countMeshes(usdNode) {
        let count = usdNode.nodeType === 'mesh' ? 1 : 0;
        if (usdNode.children) {
            for (const child of usdNode.children) {
                count += this._countMeshes(child);
            }
        }
        return count;
    }

    static _initBuildDebug(options, totalNodes, totalMeshes) {
        if (!options._debugState) {
            options._debugState = {
                startMs: performance.now(),
                totalNodes,
                totalMeshes,
                meshCopyMs: 0,
                geometryMs: 0,
                materialMs: 0,
                materialSignatureMs: 0,
                materialConvertMs: 0,
                meshCreateMs: 0,
                transformMs: 0,
                userDataMs: 0,
                childAddMs: 0,
                countMs: 0,
                yieldMs: 0,
                yieldCount: 0,
                materialCacheHits: 0,
                materialCacheMisses: 0,
                materialSignatureCacheHits: 0,
                materialSignatureCacheMisses: 0,
                singleMaterialMeshes: 0,
                multiMaterialMeshes: 0,
                subsetMaterialRefs: 0,
                compactedGroupMeshes: 0,
                compactedGroupsBefore: 0,
                compactedGroupsAfter: 0,
                aggregateMs: 0,
                aggregateInputMeshes: 0,
                aggregateOutputMeshes: 0,
                aggregateSkippedMeshes: 0,
                meshPtrHits: 0,
                meshPtrFallbacks: 0,
                lastProgressMesh: 0
            };
            this._debugBuild(options, 'build:init',
                `nodes=${totalNodes} meshes=${totalMeshes}`);
        }
    }

    static _debugBuild(options, stage, detail = '') {
        const callback = options.onDebugLog || options.debugLog;
        if (!callback && !options.debugBuild) {
            return;
        }

        const state = options._debugState;
        const elapsed = state ? performance.now() - state.startMs : 0;
        const info = {
            stage,
            detail,
            elapsedMs: elapsed,
            state
        };

        if (callback) {
            callback(info);
        } else {
            console.log(`[LightUSDLoaderUtils] ${elapsed.toFixed(1)} ms ${stage}${detail ? ` ${detail}` : ''}`);
        }
    }

    static _debugBuildProgress(options) {
        if (!options._debugState) {
            return;
        }
        const interval = Math.max(1, options.debugLogEveryMeshes || 250);
        const processed = options._progressState ? options._progressState.processedMeshes : 0;
        if (processed === 0 || processed === options._debugState.lastProgressMesh ||
            processed % interval !== 0) {
            return;
        }
        options._debugState.lastProgressMesh = processed;
        this._debugBuild(options, 'build:progress',
            `meshes=${processed}/${options._debugState.totalMeshes}`);
    }

    static _finishBuildDebug(options) {
        if (!options._debugState) {
            return;
        }
        const state = options._debugState;
        const totalMs = performance.now() - state.startMs;
        this._debugBuild(options, 'build:summary',
            `total=${totalMs.toFixed(1)}ms count=${state.countMs.toFixed(1)}ms meshCopy=${state.meshCopyMs.toFixed(1)}ms geometry=${state.geometryMs.toFixed(1)}ms material=${state.materialMs.toFixed(1)}ms materialSignature=${state.materialSignatureMs.toFixed(1)}ms materialConvert=${state.materialConvertMs.toFixed(1)}ms meshCreate=${state.meshCreateMs.toFixed(1)}ms transform=${state.transformMs.toFixed(1)}ms userData=${state.userDataMs.toFixed(1)}ms childAdd=${state.childAddMs.toFixed(1)}ms aggregate=${state.aggregateMs.toFixed(1)}ms/${state.aggregateInputMeshes}->${state.aggregateOutputMeshes} skipped=${state.aggregateSkippedMeshes} yield=${state.yieldMs.toFixed(1)}ms/${state.yieldCount} materialCache=${state.materialCacheHits}/${state.materialCacheMisses} materialSignatureCache=${state.materialSignatureCacheHits}/${state.materialSignatureCacheMisses} meshPtr=${state.meshPtrHits}/${state.meshPtrFallbacks} single=${state.singleMaterialMeshes} multi=${state.multiMaterialMeshes} subsetRefs=${state.subsetMaterialRefs} compactGroups=${state.compactedGroupMeshes}/${state.compactedGroupsBefore}->${state.compactedGroupsAfter}`);
    }

    static _geometryAttributeSignature(geometry) {
        const names = Object.keys(geometry.attributes || {}).sort();
        if (!names.includes('position')) {
            return null;
        }
        const parts = [];
        for (const name of names) {
            const attr = geometry.attributes[name];
            if (!attr || attr.isInterleavedBufferAttribute) {
                return null;
            }
            parts.push([
                name,
                attr.itemSize,
                attr.normalized ? 1 : 0,
                attr.array && attr.array.constructor ? attr.array.constructor.name : ''
            ].join(':'));
        }
        return parts.join('|');
    }

    static _tryAddAggregateGeometry(group, geometry) {
        const attrs = geometry.attributes || {};
        if (!attrs.position || attrs.skinIndex || attrs.skinWeight) {
            return false;
        }
        const pending = [];
        for (const name of Object.keys(attrs)) {
            const attr = attrs[name];
            if (!attr || attr.isInterleavedBufferAttribute) {
                return false;
            }
            const prev = group.attrMeta.get(name);
            if (prev && prev.itemSize !== attr.itemSize) {
                return false;
            }
            if (!prev) {
                pending.push([name, { itemSize: attr.itemSize }]);
            }
        }
        for (const [name, meta] of pending) {
            group.attrMeta.set(name, meta);
        }
        group.geometries.push(geometry);
        return true;
    }

    static _mergeAggregateGeometries(group) {
        const geometries = group.geometries;
        if (!geometries.length || !group.attrMeta.has('position')) {
            return null;
        }

        const attrNames = Array.from(group.attrMeta.keys()).sort();
        const totalVertices = geometries.reduce((sum, geometry) => {
            const position = geometry.attributes.position;
            return sum + (position ? position.count : 0);
        }, 0);
        if (totalVertices === 0) {
            return null;
        }

        const merged = new THREE.BufferGeometry();
        for (const name of attrNames) {
            const meta = group.attrMeta.get(name);
            const itemSize = meta.itemSize;
            const array = new Float32Array(totalVertices * itemSize);
            let vertexOffset = 0;
            for (const geometry of geometries) {
                const attr = geometry.attributes[name];
                const vertexCount = geometry.attributes.position.count;
                if (attr) {
                    array.set(attr.array, vertexOffset * itemSize);
                }
                vertexOffset += vertexCount;
            }
            merged.setAttribute(name, new THREE.BufferAttribute(array, itemSize, false));
        }

        return merged;
    }

    static _reparentChildrenToRoot(mesh, root, rootInverse) {
        while (mesh.children.length > 0) {
            const child = mesh.children[0];
            child.updateMatrixWorld(true);
            const localMatrix = new THREE.Matrix4().multiplyMatrices(rootInverse, child.matrixWorld);
            mesh.remove(child);
            child.matrix.copy(localMatrix);
            child.matrix.decompose(child.position, child.quaternion, child.scale);
            root.add(child);
        }
    }

    static aggregateMeshesByMaterial(root, options = {}) {
        const mode = options.meshAggregation;
        if (!mode || mode === 'off' || mode === false) {
            return { inputMeshes: 0, outputMeshes: 0, skippedMeshes: 0 };
        }

        const aggregateStart = performance.now();
        root.updateMatrixWorld(true);
        const rootInverse = new THREE.Matrix4().copy(root.matrixWorld).invert();
        const groups = new Map();
        let skippedMeshes = 0;

        root.traverse((obj) => {
            if (!obj.isMesh || obj.isSkinnedMesh ||
                Array.isArray(obj.material) || !obj.material || !obj.geometry) {
                if (obj.isMesh) skippedMeshes++;
                return;
            }
            const geometry = obj.geometry;
            if ((geometry.groups && geometry.groups.length > 0) || geometry.morphAttributes &&
                Object.keys(geometry.morphAttributes).length > 0) {
                skippedMeshes++;
                return;
            }

            const sourceGeometry = geometry.index ? geometry.toNonIndexed() : geometry.clone();
            if (!this._geometryAttributeSignature(sourceGeometry)) {
                sourceGeometry.dispose();
                skippedMeshes++;
                return;
            }

            const localMatrix = new THREE.Matrix4().multiplyMatrices(rootInverse, obj.matrixWorld);
            sourceGeometry.applyMatrix4(localMatrix);

            const key = obj.material.uuid;
            let group = groups.get(key);
            if (!group) {
                group = {
                    material: obj.material,
                    geometries: [],
                    meshes: [],
                    attrMeta: new Map()
                };
                groups.set(key, group);
            }
            if (!this._tryAddAggregateGeometry(group, sourceGeometry)) {
                sourceGeometry.dispose();
                skippedMeshes++;
                return;
            }
            group.meshes.push(obj);
        });

        const minMeshes = Math.max(2, options.meshAggregationMinMeshes || 2);
        const aggregateRoot = new THREE.Group();
        aggregateRoot.name = 'AggregatedMeshes';
        aggregateRoot.userData.nodeType = 'mesh-aggregate-root';
        let inputMeshes = 0;
        let outputMeshes = 0;

        for (const group of groups.values()) {
            if (group.meshes.length < minMeshes) {
                for (const geometry of group.geometries) {
                    geometry.dispose();
                }
                skippedMeshes += group.meshes.length;
                continue;
            }

            const mergedGeometry = this._mergeAggregateGeometries(group);
            for (const geometry of group.geometries) {
                geometry.dispose();
            }
            if (!mergedGeometry) {
                skippedMeshes += group.meshes.length;
                continue;
            }

            const aggregateMesh = new THREE.Mesh(mergedGeometry, group.material);
            aggregateMesh.name = `Aggregate_${outputMeshes}`;
            aggregateMesh.userData.nodeType = 'mesh-aggregate';
            aggregateMesh.userData.sourceMeshCount = group.meshes.length;
            aggregateMesh.frustumCulled = false;
            aggregateRoot.add(aggregateMesh);

            for (const mesh of group.meshes) {
                this._reparentChildrenToRoot(mesh, root, rootInverse);
                if (mesh.parent) {
                    mesh.parent.remove(mesh);
                }
                if (mesh.geometry && mesh.geometry.dispose) {
                    mesh.geometry.dispose();
                }
            }
            inputMeshes += group.meshes.length;
            outputMeshes++;
        }

        if (outputMeshes > 0) {
            root.add(aggregateRoot);
        }
        if (options._debugState) {
            options._debugState.aggregateMs += performance.now() - aggregateStart;
            options._debugState.aggregateInputMeshes += inputMeshes;
            options._debugState.aggregateOutputMeshes += outputMeshes;
            options._debugState.aggregateSkippedMeshes += skippedMeshes;
        }
        this._debugBuild(options, 'build:aggregate',
            `mode=${mode} meshes=${inputMeshes}->${outputMeshes} skipped=${skippedMeshes}`);
        return { inputMeshes, outputMeshes, skippedMeshes };
    }

    // Supported options:
    // - 'overrideMaterial' : Override usd material with defaultMtl.
    // - 'onProgress' : Progress callback (info) => void
    //     info: { stage: 'building'|'textures', percentage: number, message: string }
    // - 'debugBuild' : Print aggregated scene-build timing to console.
    // - 'onDebugLog'/'debugLog' : Callback for build timing events.
    //     info: { stage, detail, elapsedMs, state }
    // - 'debugLogEveryMeshes' : Mesh progress interval for debugLog/debugBuild.
    // - 'yieldMode' : 'raf' (default), 'timeout', or 'none'.
    // - 'yieldIntervalMs' : Minimum milliseconds between build yields.
    // - 'meshAggregation' : false/'off' (default) or 'material' to merge static
    //     leaf meshes by shared material and compatible vertex attributes.
    // - 'meshAggregationMinMeshes' : Minimum compatible meshes per aggregate.
    // - '_progressState' : Internal state for progress tracking (auto-created)

    /**
     * Build a Three.js scene graph from a USD node hierarchy
     * Includes browser yields to allow UI updates during scene building.
     *
     * @param {Object} usdNode - USD node from LightUSDLoader
     * @param {THREE.Material} defaultMtl - Default material to use
     * @param {Object} usdScene - USD scene object (LightUSDLoaderNative)
     * @param {Object} options - Build options
     * @param {Function} options.onProgress - Progress callback ({stage, percentage, message}) => void
     * @returns {Promise<THREE.Object3D>} Three.js node
     */
    static async buildThreeNode(usdNode /* LightUSDLoader.Node */, defaultMtl = null, usdScene /* LightUSDLoader.Scene */ = null, options = {})
   /* => THREE.Object3D */ {

        const isRootCall = !options._progressState;

        // Initialize progress tracking on first call (root node)
        if (isRootCall) {
            const countStart = performance.now();
            const totalNodes = this._countNodes(usdNode);
            const totalMeshes = this._countMeshes(usdNode);
            this._initBuildDebug(options, totalNodes, totalMeshes);
            options._debugState.countMs += performance.now() - countStart;
            options._progressState = {
                processedNodes: 0,
                processedMeshes: 0,
                totalNodes: totalNodes,
                totalMeshes: totalMeshes,
                lastYieldTime: 0
            };
            // Report initial progress
            if (options.onProgress) {
                options.onProgress({
                    stage: 'building',
                    percentage: 0,
                    message: `Building scene (0/${totalMeshes} meshes)...`
                });
            }
            // Initial yield to show progress UI
            if ((options.yieldMode || 'raf') !== 'none') {
                const yieldStart = performance.now();
                await this.yieldToUI(options.yieldMode || 'raf');
                if (options._debugState) {
                    options._debugState.yieldMs += performance.now() - yieldStart;
                    options._debugState.yieldCount++;
                }
            }
        }

        var node = new THREE.Group();

        if (usdNode.nodeType == 'xform') {

            // intermediate xform node
            // Apply the USD local transform matrix to the Three.js node
            const transformStart = performance.now();
            const matrix = this.toMatrix4(usdNode.localMatrix);

            // Decompose the matrix into position, rotation, and scale
            // This is necessary for Three.js to properly handle the transform
            node.applyMatrix4(matrix);
            if (options._debugState) {
                options._debugState.transformMs += performance.now() - transformStart;
            }

        } else if (usdNode.nodeType == 'skelroot') {

            // UsdSkelRoot: encapsulation prim for skinned subtree.
            // Its world transform (skelLocalToWorld) positions skinned results in world space.
            // Treated as a group node with transform in Three.js.
            if (usdNode.localMatrix) {
                const transformStart = performance.now();
                const matrix = this.toMatrix4(usdNode.localMatrix);
                node.applyMatrix4(matrix);
                if (options._debugState) {
                    options._debugState.transformMs += performance.now() - transformStart;
                }
            }

        } else if (usdNode.nodeType == 'skeleton') {

            // UsdSkeleton: joint hierarchy prim.
            // Its transform contributes to skelLocalToWorld.
            // Treated as a group node with transform in Three.js.
            if (usdNode.localMatrix) {
                const transformStart = performance.now();
                const matrix = this.toMatrix4(usdNode.localMatrix);
                node.applyMatrix4(matrix);
                if (options._debugState) {
                    options._debugState.transformMs += performance.now() - transformStart;
                }
            }

        } else if (usdNode.nodeType == 'mesh') {

            // contentId is the mesh ID in the USD scene.
            const meshCopyStart = performance.now();
            let mesh = null;
            const useMeshPtr = options.useMeshPtr !== false &&
                LightUSDLoaderUtils._canUseMeshPtr() &&
                usdScene && typeof usdScene.getMeshPtr === 'function';
            if (useMeshPtr) {
                const ptrMesh = usdScene.getMeshPtr(usdNode.contentId);
                if (ptrMesh && ptrMesh.points && ptrMesh.points.length > 0 &&
                    (!ptrMesh.hasSubmeshes || ptrMesh.submeshes)) {
                    mesh = ptrMesh;
                    mesh._meshPtr = true;
                    if (options._debugState) {
                        options._debugState.meshPtrHits++;
                    }
                } else if (options._debugState) {
                    options._debugState.meshPtrFallbacks++;
                }
            }
            if (!mesh) {
                mesh = usdScene.getMeshCopy(usdNode.contentId);
            }
            if (mesh && mesh.materialId === undefined && usdNode.materialId !== undefined) {
                mesh.materialId = usdNode.materialId;
            }
            if (options._debugState) {
                options._debugState.meshCopyMs += performance.now() - meshCopyStart;
            }

            // Update progress before building mesh
            if (options._progressState && options.onProgress) {
                const { processedMeshes, totalMeshes } = options._progressState;
                const percentage = (processedMeshes / Math.max(1, totalMeshes)) * 100;
                options.onProgress({
                    stage: 'building',
                    percentage: percentage,
                    message: `Building mesh ${processedMeshes + 1}/${totalMeshes}: ${usdNode.primName}`
                });
            }

            // Yield to browser before heavy mesh setup
            await this.maybeYieldToUI(options._progressState, options);

            const threeMesh = await this.setupMesh(mesh, defaultMtl, usdScene, options);
            node = threeMesh;

            // Increment mesh counter after building
            if (options._progressState) {
                options._progressState.processedMeshes++;
                this._debugBuildProgress(options);
            }

            // Apply transform to mesh nodes as well
            // Mesh nodes can also have transforms in USD
            if (usdNode.localMatrix) {
                const transformStart = performance.now();
                const matrix = this.toMatrix4(usdNode.localMatrix);
                node.applyMatrix4(matrix);
                if (options._debugState) {
                    options._debugState.transformMs += performance.now() - transformStart;
                }
            }

        } else {
            // Unknown node type - still try to apply transform if available
            if (usdNode.localMatrix) {
                const transformStart = performance.now();
                const matrix = this.toMatrix4(usdNode.localMatrix);
                node.applyMatrix4(matrix);
                if (options._debugState) {
                    options._debugState.transformMs += performance.now() - transformStart;
                }
            }
        }

        const userDataStart = performance.now();
        node.name = usdNode.primName;
        node.userData['primMeta.displayName'] = usdNode.displayName;
        node.userData['primMeta.absPath'] = usdNode.absPath;
        if (usdNode.nodeCategory) node.userData['nodeCategory'] = usdNode.nodeCategory;
        if (usdNode.nodeType) node.userData['nodeType'] = usdNode.nodeType;
        if (usdNode.contentId !== undefined) node.userData['contentId'] = usdNode.contentId;
        if (options._debugState) {
            options._debugState.userDataMs += performance.now() - userDataStart;
        }

        // Update progress after processing this node
        if (options._progressState) {
            options._progressState.processedNodes++;

            // Yield periodically to allow UI updates
            await this.maybeYieldToUI(options._progressState, options);
        }

        if (Object.prototype.hasOwnProperty.call(usdNode, 'children')) {

            // traverse children
            for (const child of usdNode.children) {
                const childNode = await this.buildThreeNode(child, defaultMtl, usdScene, options);
                const addStart = performance.now();
                node.add(childNode);
                if (options._debugState) {
                    options._debugState.childAddMs += performance.now() - addStart;
                }
            }
        }

        if (isRootCall) {
            this.aggregateMeshesByMaterial(node, options);
            this._finishBuildDebug(options);
        }

        return node;
    }

    // ========================================================================
    // DomeLight / Environment Map Utilities
    // ========================================================================

    static MIN_PMREM_SIZE = 64;

    /**
     * Decode half-float (float16) to float32
     */
    static decodeHalfFloat(h) {
        const s = (h & 0x8000) >> 15;
        const e = (h & 0x7C00) >> 10;
        const f = h & 0x03FF;
        if (e === 0) return (s ? -1 : 1) * Math.pow(2, -14) * (f / 1024);
        if (e === 0x1F) return f ? NaN : (s ? -Infinity : Infinity);
        return (s ? -1 : 1) * Math.pow(2, e - 15) * (1 + f / 1024);
    }

    /**
     * Convert RGB [0, 1] to hex color string
     */
    static rgbToHex(r, g, b) {
        const toHex = (c) => {
            const clamped = Math.max(0, Math.min(1, c));
            return Math.round(clamped * 255).toString(16).padStart(2, '0');
        };
        return `#${toHex(r)}${toHex(g)}${toHex(b)}`;
    }

    /**
     * Check if light type is a DomeLight
     */
    static isDomeLight(type) {
        return type === 'dome' || type === 'Dome' || type === 'DomeLight';
    }

    /**
     * Calculate DomeLight intensity from USD light properties
     */
    static calculateDomeLightIntensity(light) {
        const intensity = light.intensity !== undefined ? light.intensity : 1.0;
        // UsdLuxLightAPI defines exposure in stops with a fallback of 0. A
        // previous fallback of 1 doubled every DomeLight with unauthored or
        // explicitly-zero exposure.
        const exposure = light.exposure !== undefined ? light.exposure : 0.0;
        return intensity * Math.pow(2, exposure);
    }

    /**
     * Create a fallback environment texture (solid white)
     */
    static createFallbackEnvTexture() {
        const canvas = document.createElement('canvas');
        canvas.width = this.MIN_PMREM_SIZE;
        canvas.height = this.MIN_PMREM_SIZE;
        const ctx = canvas.getContext('2d');
        ctx.fillStyle = '#ffffff';
        ctx.fillRect(0, 0, this.MIN_PMREM_SIZE, this.MIN_PMREM_SIZE);

        const texture = new THREE.CanvasTexture(canvas);
        texture.mapping = THREE.EquirectangularReflectionMapping;
        texture.colorSpace = THREE.SRGBColorSpace;
        return texture;
    }

    /**
     * Create a solid color texture
     */
    static createSolidColorTexture(color, size) {
        // Decoder values are scene-linear. Keep them as floats while
        // expanding tiny HDR/EXR maps for PMREM; an 8-bit canvas otherwise
        // risks applying (or omitting) a transfer function at the wrong step.
        const data = new Float32Array(size * size * 4);
        for (let i = 0; i < size * size; ++i) {
            data[i * 4 + 0] = color.r;
            data[i * 4 + 1] = color.g;
            data[i * 4 + 2] = color.b;
            data[i * 4 + 3] = 1.0;
        }
        const texture = new THREE.DataTexture(
            data, size, size, THREE.RGBAFormat, THREE.FloatType);
        texture.mapping = THREE.EquirectangularReflectionMapping;
        texture.colorSpace = THREE.LinearSRGBColorSpace;
        texture.minFilter = THREE.LinearFilter;
        texture.magFilter = THREE.LinearFilter;
        texture.generateMipmaps = false;
        texture.needsUpdate = true;
        return texture;
    }

    /**
     * Create a constant color environment map
     */
    static createConstantColorEnvironment(color, colorspace, pmremGenerator) {
        const canvas = document.createElement('canvas');
        canvas.width = 256;
        canvas.height = 256;
        const ctx = canvas.getContext('2d');

        let fillColor = color;
        if (colorspace === 'sRGB' && color.startsWith('#')) {
            // Convert sRGB hex to linear for proper rendering
            const hex = color.replace('#', '');
            const r = parseInt(hex.substring(0, 2), 16) / 255;
            const g = parseInt(hex.substring(2, 4), 16) / 255;
            const b = parseInt(hex.substring(4, 6), 16) / 255;
            const sRGBToLinear = (c) => c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
            fillColor = this.rgbToHex(sRGBToLinear(r), sRGBToLinear(g), sRGBToLinear(b));
        }

        ctx.fillStyle = fillColor;
        ctx.fillRect(0, 0, 256, 256);

        const texture = new THREE.CanvasTexture(canvas);
        texture.mapping = THREE.EquirectangularReflectionMapping;
        texture.colorSpace = THREE.LinearSRGBColorSpace;

        const envMap = pmremGenerator.fromEquirectangular(texture).texture;
        // Preserve the equirectangular source for scene.background. A PMREM
        // CubeUV texture is filtered renderer data and is not a color-faithful
        // visible background.
        envMap.userData.sourceTexture = texture;
        return envMap;
    }

    /**
     * Extract average color from texture data
     */
    static extractAverageColor(texture, width, height) {
        const texData = texture.image?.data;
        if (!texData || width === 0 || height === 0) {
            return { r: 1.0, g: 1.0, b: 1.0 };
        }

        const isHalfFloat = texData instanceof Uint16Array;
        const pixelCount = width * height;
        let sumR = 0, sumG = 0, sumB = 0;

        for (let i = 0; i < pixelCount; i++) {
            if (isHalfFloat) {
                sumR += this.decodeHalfFloat(texData[i * 4 + 0]);
                sumG += this.decodeHalfFloat(texData[i * 4 + 1]);
                sumB += this.decodeHalfFloat(texData[i * 4 + 2]);
            } else {
                sumR += texData[i * 4 + 0];
                sumG += texData[i * 4 + 1];
                sumB += texData[i * 4 + 2];
            }
        }

        return {
            r: sumR / pixelCount,
            g: sumG / pixelCount,
            b: sumB / pixelCount
        };
    }

    /**
     * Ensure texture meets minimum size for PMREM processing
     */
    static ensureMinimumTextureSize(texture) {
        const origWidth = texture.image?.width || 0;
        const origHeight = texture.image?.height || 0;

        if (origWidth >= this.MIN_PMREM_SIZE && origHeight >= this.MIN_PMREM_SIZE) {
            return texture;
        }

        const avgColor = this.extractAverageColor(texture, origWidth, origHeight);
        texture.dispose();

        return this.createSolidColorTexture(avgColor, this.MIN_PMREM_SIZE);
    }

    /**
     * Create a float texture from decoded data
     */
    static createFloatTexture(data, width, height, channels) {
        const floatData = data instanceof Float32Array ? data : new Float32Array(data.buffer);

        let rgbaData;
        if (channels === 4) {
            rgbaData = floatData;
        } else if (channels === 3) {
            rgbaData = new Float32Array(width * height * 4);
            for (let i = 0; i < width * height; i++) {
                rgbaData[i * 4 + 0] = floatData[i * 3 + 0];
                rgbaData[i * 4 + 1] = floatData[i * 3 + 1];
                rgbaData[i * 4 + 2] = floatData[i * 3 + 2];
                rgbaData[i * 4 + 3] = 1.0;
            }
        } else {
            return null;
        }

        return new THREE.DataTexture(rgbaData, width, height, THREE.RGBAFormat, THREE.FloatType);
    }

    /**
     * Create a canvas texture from decoded image data
     */
    static createCanvasTextureFromData(data, width, height, channels) {
        const canvas = document.createElement('canvas');
        canvas.width = width;
        canvas.height = height;
        const ctx = canvas.getContext('2d');
        const imageData = ctx.createImageData(width, height);

        for (let i = 0; i < width * height; i++) {
            const srcIdx = i * channels;
            const dstIdx = i * 4;

            if (channels === 1) {
                imageData.data[dstIdx + 0] = data[srcIdx];
                imageData.data[dstIdx + 1] = data[srcIdx];
                imageData.data[dstIdx + 2] = data[srcIdx];
                imageData.data[dstIdx + 3] = 255;
            } else if (channels === 2) {
                imageData.data[dstIdx + 0] = data[srcIdx];
                imageData.data[dstIdx + 1] = data[srcIdx];
                imageData.data[dstIdx + 2] = data[srcIdx];
                imageData.data[dstIdx + 3] = data[srcIdx + 1];
            } else if (channels === 3) {
                imageData.data[dstIdx + 0] = data[srcIdx + 0];
                imageData.data[dstIdx + 1] = data[srcIdx + 1];
                imageData.data[dstIdx + 2] = data[srcIdx + 2];
                imageData.data[dstIdx + 3] = 255;
            } else if (channels === 4) {
                imageData.data[dstIdx + 0] = data[srcIdx + 0];
                imageData.data[dstIdx + 1] = data[srcIdx + 1];
                imageData.data[dstIdx + 2] = data[srcIdx + 2];
                imageData.data[dstIdx + 3] = data[srcIdx + 3];
            }
        }

        ctx.putImageData(imageData, 0, 0);
        return new THREE.CanvasTexture(canvas);
    }

    /**
     * Create texture from decoded USD image data
     */
    static async createTextureFromDecodedData(data, width, height, channels, colorSpace) {
        try {
            if (!data || !width || !height) return null;

            const isFloat = data instanceof Float32Array || (data.buffer && data.BYTES_PER_ELEMENT === 4);
            let texture;

            if (isFloat) {
                texture = this.createFloatTexture(data, width, height, channels);
            } else {
                texture = this.createCanvasTextureFromData(data, width, height, channels);
            }

            if (!texture) return null;

            texture.mapping = THREE.EquirectangularReflectionMapping;
            texture.colorSpace = (colorSpace === 'sRGB' || colorSpace === 'sRGB_Texture')
                ? THREE.SRGBColorSpace
                : THREE.LinearSRGBColorSpace;
            texture.needsUpdate = true;

            return texture;
        } catch (error) {
            console.error('Error creating texture from decoded data:', error);
            return null;
        }
    }

    /**
     * Decode EXR from buffer with Three.js primary + LightUSD fallback
     * @param {ArrayBuffer|Uint8Array} buffer - EXR data
     * @param {string} [outputFormat='float16'] - Output format
     * @returns {THREE.DataTexture|null}
     */
    static decodeEXRFromBuffer(buffer, outputFormat = 'float16') {
        const result = decodeEXRWithFallback(buffer, LightUSDLoaderUtils._lightusd, {
            outputFormat,
            preferThreeJS: true,
            verbose: false,
        });

        if (!result.success) {
            console.warn('EXR decode failed:', result.error);
            return null;
        }

        // Create Three.js DataTexture from decoded data
        const texture = new THREE.DataTexture(
            result.data,
            result.width,
            result.height,
            THREE.RGBAFormat,
            result.format === 'float16' ? THREE.HalfFloatType : THREE.FloatType
        );
        texture.minFilter = THREE.LinearFilter;
        texture.magFilter = THREE.LinearFilter;
        texture.generateMipmaps = false;
        texture.needsUpdate = true;

        return texture;
    }

    /**
     * Decode environment map from buffer (supports EXR, HDR, and standard image formats)
     * Uses Three.js EXRLoader with LightUSD fallback for EXR files
     */
    static async decodeEnvmapFromBuffer(buffer, uri) {
        try {
            const lowerUri = uri.toLowerCase();

            let texture = null;

            if (lowerUri.endsWith('.exr')) {
                // Use EXR decoder with LightUSD fallback
                texture = this.decodeEXRFromBuffer(buffer, 'float16');

                if (!texture) {
                    // Last resort: try Three.js EXRLoader with blob URL
                    const blob = new Blob([buffer], { type: 'image/x-exr' });
                    const objectUrl = URL.createObjectURL(blob);
                    try {
                        texture = await new EXRLoader().loadAsync(objectUrl);
                    } finally {
                        URL.revokeObjectURL(objectUrl);
                    }
                }
            } else if (lowerUri.endsWith('.hdr')) {
                // HDR uses LightUSD decoder (faster than Three.js)
                const lightusd = LightUSDLoaderUtils._lightusd;
                if (lightusd && typeof lightusd.decodeHDR === 'function') {
                    const uint8Array = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
                    const result = lightusd.decodeHDR(uint8Array, 'float16');
                    if (result.success) {
                        texture = new THREE.DataTexture(
                            result.data,
                            result.width,
                            result.height,
                            THREE.RGBAFormat,
                            THREE.HalfFloatType
                        );
                        texture.minFilter = THREE.LinearFilter;
                        texture.magFilter = THREE.LinearFilter;
                        texture.generateMipmaps = false;
                        texture.needsUpdate = true;
                    }
                }

                if (!texture) {
                    // Fallback to Three.js HDRLoader
                    const blob = new Blob([buffer], { type: 'image/vnd.radiance' });
                    const objectUrl = URL.createObjectURL(blob);
                    try {
                        texture = await new HDRLoader().loadAsync(objectUrl);
                    } finally {
                        URL.revokeObjectURL(objectUrl);
                    }
                }
            } else {
                // Standard image formats
                const mimeType = this.getMimeTypeFromExtension(this.getFileExtension(uri)) || 'application/octet-stream';
                const blob = new Blob([buffer], { type: mimeType });
                const objectUrl = URL.createObjectURL(blob);
                try {
                    texture = await new THREE.TextureLoader().loadAsync(objectUrl);
                } finally {
                    URL.revokeObjectURL(objectUrl);
                }
            }

            if (texture) {
                texture.mapping = THREE.EquirectangularReflectionMapping;
            }

            return texture;
        } catch (error) {
            console.error('Error decoding envmap from buffer:', error);
            return null;
        }
    }

    /**
     * Load DomeLight environment map from USD texture ID
     * @param {Object} light - USD light data
     * @param {Object} usdLoader - USD loader instance
     * @param {number} envmapTextureId - Texture ID
     * @param {string} textureFile - Optional texture file path
     * @param {THREE.PMREMGenerator} pmremGenerator - PMREM generator
     * @returns {Object|null} - { texture, intensity } or null
     */
    static async loadDomeLightFromTextureId(light, usdLoader, envmapTextureId, textureFile, pmremGenerator) {
        try {
            const imageData = usdLoader.getImageCopy(envmapTextureId);
            if (!imageData || !imageData.data || imageData.data.length === 0) {
                console.warn(`DomeLight: No image data found for texture ID ${envmapTextureId}`);
                return null;
            }

            let texture = imageData.decoded
                ? await this.createTextureFromDecodedData(imageData.data, imageData.width, imageData.height, imageData.channels, imageData.colorSpace)
                : await this.decodeEnvmapFromBuffer(imageData.data, imageData.uri || textureFile || '');

            if (!texture) {
                texture = this.createFallbackEnvTexture();
            }

            // Verify texture has valid image data before PMREM processing
            if (!texture || !texture.image) {
                console.warn(`DomeLight: Failed to create valid texture from image ID ${envmapTextureId}`);
                return null;
            }

            texture = this.ensureMinimumTextureSize(texture);

            const pmremResult = pmremGenerator.fromEquirectangular(texture);
            const envMap = pmremResult.texture;

            const intensity = this.calculateDomeLightIntensity(light);

            return {
                texture: envMap,
                sourceTexture: texture,
                intensity,
                name: light.name,
                textureFile,
                envmapTextureId,
                color: light.color,
                exposure: light.exposure
            };
        } catch (error) {
            console.warn(`DomeLight: Failed to load envmap from image index ${envmapTextureId}:`, error.message);
            return null;
        }
    }

    /**
     * Load DomeLight environment map directly from file
     * Uses LightUSD for HDR (faster) and Three.js + LightUSD fallback for EXR
     * @param {Object} light - USD light data
     * @param {string} textureFile - Texture file path
     * @param {THREE.PMREMGenerator} pmremGenerator - PMREM generator
     * @returns {Object|null} - { texture, intensity } or null
     */
    static async loadDomeLightFromFile(light, textureFile, pmremGenerator) {
        try {
            let texture = null;
            const lowerFile = textureFile.toLowerCase();

            // Fetch the file data
            let response;
            try {
                response = await fetch(textureFile);
                if (!response.ok) {
                    console.warn(`DomeLight: Texture file not accessible '${textureFile}' (HTTP ${response.status})`);
                    return null;
                }
                // Check content type - reject HTML responses (likely 404 pages that return 200)
                const contentType = response.headers.get('content-type') || '';
                if (contentType.includes('text/html')) {
                    console.warn(`DomeLight: Invalid content type for '${textureFile}' (got HTML, expected image)`);
                    return null;
                }
            } catch (fetchError) {
                console.warn(`DomeLight: Cannot access texture file '${textureFile}' - ${fetchError.message}`);
                return null;
            }

            const buffer = await response.arrayBuffer();

            if (lowerFile.endsWith('.exr')) {
                // Use EXR decoder with LightUSD fallback
                texture = this.decodeEXRFromBuffer(buffer, 'float16');

                if (!texture) {
                    // Fallback: try Three.js EXRLoader with blob URL
                    try {
                        const blob = new Blob([buffer], { type: 'image/x-exr' });
                        const objectUrl = URL.createObjectURL(blob);
                        try {
                            texture = await new EXRLoader().loadAsync(objectUrl);
                        } finally {
                            URL.revokeObjectURL(objectUrl);
                        }
                    } catch (exrError) {
                        console.warn(`DomeLight: EXR load failed for '${textureFile}' - ${exrError.message}`);
                        return null;
                    }
                }
            } else if (lowerFile.endsWith('.hdr')) {
                // Use LightUSD HDR decoder (faster than Three.js)
                const lightusd = LightUSDLoaderUtils._lightusd;
                if (lightusd && typeof lightusd.decodeHDR === 'function') {
                    const uint8Array = new Uint8Array(buffer);
                    const result = lightusd.decodeHDR(uint8Array, 'float16');
                    if (result.success) {
                        texture = new THREE.DataTexture(
                            result.data,
                            result.width,
                            result.height,
                            THREE.RGBAFormat,
                            THREE.HalfFloatType
                        );
                        texture.minFilter = THREE.LinearFilter;
                        texture.magFilter = THREE.LinearFilter;
                        texture.generateMipmaps = false;
                        texture.needsUpdate = true;
                    }
                }

                if (!texture) {
                    // Fallback to Three.js HDRLoader
                    try {
                        const blob = new Blob([buffer], { type: 'image/vnd.radiance' });
                        const objectUrl = URL.createObjectURL(blob);
                        try {
                            texture = await new HDRLoader().loadAsync(objectUrl);
                        } finally {
                            URL.revokeObjectURL(objectUrl);
                        }
                    } catch (hdrError) {
                        console.warn(`DomeLight: HDR load failed for '${textureFile}' - ${hdrError.message}`);
                        return null;
                    }
                }
            } else {
                console.warn(`DomeLight: Unsupported texture format for '${textureFile}'`);
                return null;
            }

            // Check if texture was loaded and has valid data
            if (!texture) {
                console.warn(`DomeLight: Failed to decode texture for '${textureFile}'`);
                return null;
            }

            texture.mapping = THREE.EquirectangularReflectionMapping;
            const envMap = pmremGenerator.fromEquirectangular(texture).texture;

            const intensity = this.calculateDomeLightIntensity(light);

            return {
                texture: envMap,
                sourceTexture: texture,
                intensity,
                name: light.name,
                textureFile,
                color: light.color,
                exposure: light.exposure
            };
        } catch (error) {
            console.warn(`DomeLight: Unexpected error loading '${textureFile}' - ${error.message}`);
            return null;
        }
    }

    /**
     * Load DomeLight as constant color environment
     * @param {Object} light - USD light data
     * @param {THREE.PMREMGenerator} pmremGenerator - PMREM generator
     * @returns {Object|null} - { texture, intensity, colorHex } or null
     */
    static loadDomeLightAsConstantColor(light, pmremGenerator) {
        if (!light.color || light.color.length < 3) return null;

        const colorHex = this.rgbToHex(light.color[0], light.color[1], light.color[2]);
        const envMap = this.createConstantColorEnvironment(colorHex, 'linear', pmremGenerator);
        const intensity = this.calculateDomeLightIntensity(light);

        return {
            texture: envMap,
            sourceTexture: envMap.userData.sourceTexture,
            intensity,
            colorHex,
            name: light.name,
            color: light.color,
            exposure: light.exposure
        };
    }

    /**
     * Process a single DomeLight from USD
     * @param {Object} light - USD light data
     * @param {Object} usdLoader - USD loader instance
     * @param {THREE.PMREMGenerator} pmremGenerator - PMREM generator
     * @returns {Object|null} - DomeLight result or null
     */
    static async processDomeLight(light, usdLoader, pmremGenerator) {
        const envmapTextureId = light.envmapTextureId;
        const textureFile = light.textureFile || light.texture_file;

        // Try loading from texture ID first
        if (envmapTextureId !== undefined && envmapTextureId >= 0) {
            const result = await this.loadDomeLightFromTextureId(light, usdLoader, envmapTextureId, textureFile, pmremGenerator);
            if (result) return result;
        }

        // Check for Blender-convention constant-color filename (color_RRGGBB.exr)
        // before attempting a network fetch that will fail for embedded USDZ assets
        if (textureFile) {
            const colorMatch = textureFile.match(/color_([0-9A-Fa-f]{6})\.\w+$/);
            if (colorMatch) {
                const hex = '#' + colorMatch[1];
                const envMap = this.createConstantColorEnvironment(hex, 'linear', pmremGenerator);
                const intensity = this.calculateDomeLightIntensity(light);
                console.log(`DomeLight: Using constant color ${hex} from filename '${textureFile}'`);
                return {
                    texture: envMap,
                    sourceTexture: envMap.userData.sourceTexture,
                    intensity,
                    colorHex: hex,
                    name: light.name,
                    textureFile,
                    color: light.color,
                    exposure: light.exposure
                };
            }
        }

        // Fallback: direct file load
        if (textureFile) {
            const result = await this.loadDomeLightFromFile(light, textureFile, pmremGenerator);
            if (result) return result;
        }

        // Final fallback: constant color from light.color
        return this.loadDomeLightAsConstantColor(light, pmremGenerator);
    }

    /**
     * Load DomeLight from USD scene
     * Iterates through all lights and returns the first DomeLight found
     *
     * @param {Object} usdLoader - USD loader instance with numLights() and getLight() methods
     * @param {THREE.PMREMGenerator} pmremGenerator - PMREM generator for environment map processing
     * @returns {Object|null} - DomeLight data { texture, intensity, name, ... } or null
     *
     * Usage:
     *   const domeLightData = await LightUSDLoaderUtils.loadDomeLightFromUSD(usdLoader, pmremGenerator);
     *   if (domeLightData) {
     *       scene.environment = domeLightData.texture;
     *       materials.forEach(m => m.envMapIntensity = domeLightData.intensity);
     *   }
     */
    static async loadDomeLightFromUSD(usdLoader, pmremGenerator) {
        try {
            const numLights = usdLoader.numLights ? usdLoader.numLights() : 0;
            if (numLights === 0) return null;

            for (let i = 0; i < numLights; i++) {
                const light = usdLoader.getLight(i);
                if (light.error) continue;

                if (!this.isDomeLight(light.type)) continue;

                const result = await this.processDomeLight(light, usdLoader, pmremGenerator);
                if (result) return result;
            }

            return null;
        } catch (error) {
            console.warn('Error loading DomeLight from USD:', error);
            return null;
        }
    }

}

export { LightUSDLoaderUtils };
