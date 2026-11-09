import * as THREE from 'three';
import { decodeEXR as decodeEXRWithFallback } from './EXRDecoder.js';

/**
 * LightUSD MaterialX / OpenPBR Material Utilities
 *
 * This module provides utilities for converting MaterialX OpenPBR materials
 * from USD files to Three.js MeshPhysicalMaterial.
 *
 * Key features:
 * - OpenPBR parameter extraction from JSON format (flat and grouped)
 * - Texture loading from USD scene with caching (including EXR/HDR)
 * - Full OpenPBR to MeshPhysicalMaterial conversion
 * - Support for all OpenPBR layers: base, specular, transmission, subsurface,
 *   coat, sheen, fuzz, thin_film, emission, geometry
 */

// Reference to LightUSD WASM module for HDR/EXR decoding
let _lightusd = null;

/**
 * Set LightUSD WASM module for texture decoding
 * @param {Object} lightusd - LightUSD WASM module
 */
export function setLightUSD(lightusd) {
    _lightusd = lightusd;
}

// ============================================================================
// OpenPBR Parameter Mapping to Three.js MeshPhysicalMaterial
// ============================================================================

/**
 * Mapping from OpenPBR parameter names to Three.js material properties
 */
const OPENPBR_TO_THREEJS_MAP = {
    // Base layer
    'base_color': { property: 'color', type: 'color' },
    'base_weight': { property: null, type: 'scalar' }, // Not directly mapped
    'base_metalness': { property: 'metalness', type: 'scalar' },
    'base_roughness': { property: null, type: 'scalar' }, // Use specular_roughness instead
    'base_diffuse_roughness': { property: null, type: 'scalar' }, // Oren-Nayar, not in Three.js

    // Specular layer
    'specular_weight': { property: null, type: 'scalar' },
    'specular_color': { property: 'specularColor', type: 'color' },
    'specular_roughness': { property: 'roughness', type: 'scalar' },
    'specular_ior': { property: 'ior', type: 'scalar' },
    'specular_ior_level': { property: null, type: 'scalar' },
    'specular_anisotropy': { property: 'anisotropy', type: 'scalar' },
    'specular_rotation': { property: 'anisotropyRotation', type: 'scalar' },

    // Transmission
    'transmission_weight': { property: 'transmission', type: 'scalar' },
    'transmission_color': { property: 'attenuationColor', type: 'color' },
    'transmission_depth': { property: 'attenuationDistance', type: 'scalar' },

    // Subsurface
    'subsurface_weight': { property: null, type: 'scalar' }, // Complex SSS not in Three.js
    'subsurface_color': { property: null, type: 'color' },
    'subsurface_radius': { property: null, type: 'color' },
    'subsurface_scale': { property: null, type: 'scalar' },

    // Coat (clearcoat)
    'coat_weight': { property: 'clearcoat', type: 'scalar' },
    'coat_color': { property: null, type: 'color' }, // Not directly supported
    'coat_roughness': { property: 'clearcoatRoughness', type: 'scalar' },
    'coat_ior': { property: null, type: 'scalar' },

    // Sheen
    'sheen_weight': { property: 'sheen', type: 'scalar' },
    'sheen_color': { property: 'sheenColor', type: 'color' },
    'sheen_roughness': { property: 'sheenRoughness', type: 'scalar' },

    // Fuzz (similar to sheen in Three.js)
    'fuzz_weight': { property: 'sheen', type: 'scalar' },
    'fuzz_color': { property: 'sheenColor', type: 'color' },
    'fuzz_roughness': { property: 'sheenRoughness', type: 'scalar' },

    // Thin film (iridescence)
    'thin_film_weight': { property: 'iridescence', type: 'scalar' },
    'thin_film_thickness': { property: 'iridescenceThicknessRange', type: 'scalar' },
    'thin_film_ior': { property: 'iridescenceIOR', type: 'scalar' },

    // Emission
    'emission_color': { property: 'emissive', type: 'color' },
    'emission_luminance': { property: 'emissiveIntensity', type: 'scalar' },

    // Geometry
    'opacity': { property: 'opacity', type: 'scalar' },
    'geometry_opacity': { property: 'opacity', type: 'scalar' },
    'normal': { property: null, type: 'normal' },
    'geometry_normal': { property: null, type: 'normal' },
};

/**
 * Mapping from OpenPBR parameters to Three.js texture map names
 */
const OPENPBR_TEXTURE_MAP = {
    'base_color': 'map',
    'base_metalness': 'metalnessMap',
    'specular_roughness': 'roughnessMap',
    'specular_color': 'specularColorMap',
    'normal': 'normalMap',
    'geometry_normal': 'normalMap',
    'emission_color': 'emissiveMap',
    'opacity': 'alphaMap',
    'geometry_opacity': 'alphaMap',
    'coat_roughness': 'clearcoatRoughnessMap',
    'coat_normal': 'clearcoatNormalMap',
    'sheen_color': 'sheenColorMap',
    'sheen_roughness': 'sheenRoughnessMap',
    'thin_film_thickness': 'iridescenceThicknessMap',
};

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Extract scalar or color value from OpenPBR parameter
 * Handles both direct values and {type, value} wrapper format
 */
function extractValue(param) {
    if (param === undefined || param === null) return undefined;
    if (typeof param === 'number') return param;
    if (Array.isArray(param)) return param;
    if (typeof param === 'object') {
        if (param.value !== undefined) return param.value;
        if (param.type === 'value') return param.value;
    }
    return param;
}

/**
 * Check if parameter has a texture
 */
function hasTexture(param) {
    if (!param || typeof param !== 'object') return false;
    return param.textureId !== undefined && param.textureId >= 0;
}

/**
 * Get texture ID from parameter
 */
function getTextureId(param) {
    if (!hasTexture(param)) return -1;
    return param.textureId;
}

/**
 * Create THREE.Color from RGB array with optional color space handling
 */
function createColor(rgb, colorSpace = 'srgb', usdScene = null) {
    if (!rgb || !Array.isArray(rgb)) return new THREE.Color(1, 1, 1);
    const metadata = usdScene && typeof usdScene.getSceneMetadata === 'function'
        ? usdScene.getSceneMetadata() : null;
    const m = metadata?.workingToDisplayLinear;
    const converted = m && m.length === 9
        ? [m[0] * rgb[0] + m[1] * rgb[1] + m[2] * rgb[2],
            m[3] * rgb[0] + m[4] * rgb[1] + m[5] * rgb[2],
            m[6] * rgb[0] + m[7] * rgb[1] + m[8] * rgb[2]]
        : rgb;
    const color = new THREE.Color(converted[0], converted[1], converted[2]);
    return color;
}

/**
 * Extract normal map strength (`normal_map_scale`) from OpenPBR geometry data.
 */
function extractNormalMapScale(geometryData) {
    if (!geometryData || typeof geometryData !== 'object') return 1.0;
    const scale = extractValue(geometryData.normal_map_scale);
    return (typeof scale === 'number' && Number.isFinite(scale)) ? scale : 1.0;
}

/**
 * Resolve normal parameter (`normal` or `geometry_normal`) from OpenPBR geometry data.
 */
function extractNormalParam(geometryData) {
    if (!geometryData || typeof geometryData !== 'object') return undefined;
    return geometryData.normal !== undefined ? geometryData.normal : geometryData.geometry_normal;
}

/**
 * Apply normal map strength to Three.js material.
 */
function applyNormalScale(material, normalScale) {
    const scale = (typeof normalScale === 'number' && Number.isFinite(normalScale)) ? normalScale : 1.0;
    material.normalScale = new THREE.Vector2(scale, scale);
}

/**
 * Apply normal map texture in async (await) conversion path.
 */
async function applyNormalMapLoaded(material, normalParam, normalScale, usdScene, textureCache, textureManager = null, skipTextures = false) {
    if (skipTextures || !(normalParam !== undefined && usdScene && hasTexture(normalParam))) {
        return;
    }

    const textureId = getTextureId(normalParam);
    applyNormalScale(material, normalScale);

    if (textureManager) {
        textureManager.queueTexture(material, 'normalMap', textureId, usdScene, {
            normalScale
        });
        return;
    }

    const texture = await loadTextureFromUSD(usdScene, textureId, textureCache);
    if (texture) {
        material.normalMap = texture;
        material.userData.textures.normalMap = { textureId, texture };
    }
}

/**
 * Apply normal map texture in legacy fire-and-forget conversion path.
 */
function applyNormalMapLegacy(material, normalParam, normalScale, usdScene, textureCache, skipTextures = false) {
    if (skipTextures || !(normalParam !== undefined && usdScene && hasTexture(normalParam))) {
        return;
    }

    const textureId = getTextureId(normalParam);
    applyNormalScale(material, normalScale);

    loadTextureFromUSD(usdScene, textureId, textureCache).then((texture) => {
        if (texture) {
            material.normalMap = texture;
            material.userData.textures.normalMap = { textureId, texture };
            material.needsUpdate = true;
        }
    }).catch((err) => {
        console.error('Failed to load normal texture:', err);
    });
}

/**
 * Extract normal map strength (`normal_map_scale`) from grouped getter access.
 */
function extractNormalMapScaleFromGetter(getParam) {
    if (typeof getParam !== 'function') return 1.0;
    const scale = extractValue(getParam('normal_map_scale', 'geometry'));
    return (typeof scale === 'number' && Number.isFinite(scale)) ? scale : 1.0;
}

/**
 * Resolve normal parameter (`normal` or `geometry_normal`) from grouped getter access.
 */
function extractNormalParamFromGetter(getParam) {
    if (typeof getParam !== 'function') return undefined;
    return getParam('normal', 'geometry') ?? getParam('geometry_normal', 'geometry');
}

/**
 * Apply OpenPBR normal map using grouped getter access.
 * `loadTextureFn` can be overridden by callers that own custom texture loading behavior.
 */
async function applyOpenPBRNormalMapFromGetter(
    material,
    getParam,
    usdScene,
    textureCache = null,
    loadTextureFn = loadTextureFromUSD
) {
    const normalParam = extractNormalParamFromGetter(getParam);
    if (loadTextureFn === null || !(normalParam !== undefined && usdScene && hasTexture(normalParam))) {
        return;
    }

    const textureId = getTextureId(normalParam);
    const texture = await loadTextureFn(usdScene, textureId, textureCache);
    if (texture) {
        const normalScale = extractNormalMapScaleFromGetter(getParam);
        material.normalMap = texture;
        applyNormalScale(material, normalScale);
        material.userData.textures.normalMap = { textureId, texture };
    }
}

/**
 * Detect MIME type from image data
 */
function getMimeType(imgData) {
    if (imgData.uri) {
        const ext = imgData.uri.split('.').pop().toLowerCase().split('?')[0];
        const mimeTypes = {
            'png': 'image/png',
            'jpg': 'image/jpeg',
            'jpeg': 'image/jpeg',
            'webp': 'image/webp',
            'gif': 'image/gif',
            'exr': 'image/x-exr',
            'hdr': 'image/vnd.radiance'
        };
        if (mimeTypes[ext]) return mimeTypes[ext];
    }

    // Check magic bytes
    if (imgData.data && imgData.data.length >= 4) {
        const data = new Uint8Array(imgData.data);
        // PNG magic: 0x89 0x50 0x4E 0x47
        if (data[0] === 0x89 && data[1] === 0x50 && data[2] === 0x4E && data[3] === 0x47) return 'image/png';
        // JPEG magic: 0xFF 0xD8 0xFF
        if (data[0] === 0xFF && data[1] === 0xD8 && data[2] === 0xFF) return 'image/jpeg';
        // WEBP magic: RIFF....WEBP
        if (data[0] === 0x52 && data[1] === 0x49 && data[2] === 0x46 && data[3] === 0x46) return 'image/webp';
        // EXR magic: 0x76 0x2F 0x31 0x01
        if (data[0] === 0x76 && data[1] === 0x2F && data[2] === 0x31 && data[3] === 0x01) return 'image/x-exr';
        // HDR magic: "#?" (Radiance format)
        if (data[0] === 0x23 && data[1] === 0x3F) return 'image/vnd.radiance';
    }

    return 'image/png';
}

/**
 * Check if MIME type is HDR format (EXR or HDR)
 */
function isHDRFormat(mimeType) {
    return mimeType === 'image/x-exr' || mimeType === 'image/vnd.radiance';
}

/**
 * Decode HDR/EXR texture data and create Three.js DataTexture
 * @param {Uint8Array|ArrayBuffer} data - Image data
 * @param {string} mimeType - MIME type
 * @returns {THREE.DataTexture|null}
 */
async function decodeHDRTexture(data, mimeType) {
    const buffer = data instanceof ArrayBuffer ? data : data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);

    if (mimeType === 'image/x-exr') {
        const exr = _lightusd?.loadFeature ? await _lightusd.loadFeature('exr') : _lightusd;
        // Use EXR decoder with LightUSD fallback
        const result = decodeEXRWithFallback(buffer, exr, {
            outputFormat: 'float16',
            preferThreeJS: true,
        });

        if (result.success) {
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
            texture.flipY = true;
            texture.needsUpdate = true;
            return texture;
        }
    } else if (mimeType === 'image/vnd.radiance') {
        // Use LightUSD HDR decoder (faster)
        if (_lightusd && typeof _lightusd.decodeHDR === 'function') {
            const uint8Array = data instanceof Uint8Array ? data : new Uint8Array(buffer);
            const result = _lightusd.decodeHDR(uint8Array, 'float16');

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
                texture.generateMipmaps = false;
                texture.flipY = true;
                texture.needsUpdate = true;
                return texture;
            }
        }
    }

    return null;
}

// ============================================================================
// Texture Loading
// ============================================================================

const UNSUPPORTED_BROWSER_TEXTURE_EXTENSIONS = new Set([
    'psd', 'psb', 'tif', 'tiff', 'tx', 'tex', 'ies'
]);

function getTextureExtension(uri = '') {
    const clean = String(uri).split(/[?#]/, 1)[0].replace(/@$/, '');
    const match = /\.([A-Za-z0-9]+)$/.exec(clean);
    return match ? match[1].toLowerCase() : '';
}

function isUnsupportedBrowserTextureURI(uri = '') {
    return UNSUPPORTED_BROWSER_TEXTURE_EXTENSIONS.has(getTextureExtension(uri));
}

function warnUnsupportedTexture(textureId, uri, reason = '') {
    console.warn(`Unsupported texture format for texture ${textureId}; skipping texture load`, {
        textureId,
        uri: uri || '',
        extension: getTextureExtension(uri),
        reason
    });
}

function threeWrapMode(wrap) {
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

function applyTextureSampler(texture, texData = null) {
    if (!texture || !texData) return texture;

    texture.wrapS = threeWrapMode(texData.wrapS);
    texture.wrapT = threeWrapMode(texData.wrapT);

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

function textureCacheKey(textureId, texData = null) {
    return JSON.stringify({
        textureId,
        imageId: texData?.textureImageId,
        wrapS: texData?.wrapS,
        wrapT: texData?.wrapT,
        hasTransform2d: !!texData?.hasTransform2d,
        txRotation: texData?.txRotation,
        txScaleU: texData?.txScaleU,
        txScaleV: texData?.txScaleV,
        txTranslationU: texData?.txTranslationU,
        txTranslationV: texData?.txTranslationV
    });
}

/**
 * Load texture from USD scene
 * @param {Object} usdScene - USD scene object with getTexture/getImage methods
 * @param {number} textureId - Texture ID
 * @param {Map} cache - Optional texture cache
 * @returns {Promise<THREE.Texture|null>}
 */
async function loadTextureFromUSD(usdScene, textureId, cache = null) {
    if (textureId === undefined || textureId < 0) return null;

    try {
        const texData = usdScene.getTexture(textureId);
        if (!texData || texData.textureImageId === undefined || texData.textureImageId < 0) {
            console.warn(`Texture ${textureId} has no valid image data`);
            return null;
        }

        const cacheKey = textureCacheKey(textureId, texData);
        if (cache && cache.has(cacheKey)) {
            return cache.get(cacheKey);
        }

        const imgData = usdScene.getImageCopy(texData.textureImageId);
        if (!imgData) {
            console.warn(`Image ${texData.textureImageId} not found`);
            return null;
        }
        const imageURI = imgData.uri || '';
        if (isUnsupportedBrowserTextureURI(imageURI)) {
            warnUnsupportedTexture(textureId, imageURI, 'browser TextureLoader cannot decode this source format');
            return null;
        }

        let texture = null;

        // Case 1: URI only - try to find embedded alternative first
        if (imgData.uri && (imgData.bufferId === -1 || imgData.bufferId === undefined)) {
            // LightUSD may create duplicate image entries: one with URI reference (bufferId=-1)
            // and one with embedded data (bufferId>=0). Try to find the embedded version.
            const filename = imageURI.replace(/^\.\//, ''); // Remove leading ./
            let foundEmbedded = false;

            if (typeof usdScene.numImages === 'function') {
                const numImages = usdScene.numImages();
                for (let i = 0; i < numImages; i++) {
                    const altImg = usdScene.getImageCopy(i);
                    if (altImg.bufferId >= 0 && altImg.uri === filename) {
                        if (isUnsupportedBrowserTextureURI(altImg.uri || filename)) {
                            warnUnsupportedTexture(textureId, altImg.uri || filename, 'embedded image keeps an unsupported source format');
                            return null;
                        }
                        // Found embedded version - use it instead
                        const altImgData = altImg;
                        if (altImgData.data) {
                            if (altImgData.decoded) {
                                const image8Array = new Uint8ClampedArray(altImgData.data);
                                texture = new THREE.DataTexture(image8Array, altImgData.width, altImgData.height);
                                if (altImgData.channels === 1) texture.format = THREE.RedFormat;
                                else if (altImgData.channels === 2) texture.format = THREE.RGFormat;
                                else if (altImgData.channels === 4) texture.format = THREE.RGBAFormat;
                                texture.flipY = true;
                                texture.needsUpdate = true;
                            } else {
                                const mimeType = getMimeType(altImgData);
                                // Check if HDR format - use specialized decoder
                                if (isHDRFormat(mimeType)) {
                                    texture = await decodeHDRTexture(altImgData.data, mimeType);
                                } else {
                                    const blob = new Blob([altImgData.data], { type: mimeType });
                                    const blobUrl = URL.createObjectURL(blob);
                                    try {
                                        const loader = new THREE.TextureLoader();
                                        texture = await loader.loadAsync(blobUrl);
                                    } finally {
                                        URL.revokeObjectURL(blobUrl);
                                    }
                                }
                            }
                            foundEmbedded = true;
                            break;
                        }
                    }
                }
            }

            // Fall back to loading from URI if no embedded version found
            if (!foundEmbedded) {
                const loader = new THREE.TextureLoader();
                texture = await loader.loadAsync(imageURI);
            }
        }
        // Case 2 & 3: Embedded texture
        else if (imgData.bufferId >= 0 && imgData.data) {
            if (imgData.decoded) {
                // Already decoded - create DataTexture
                const image8Array = new Uint8ClampedArray(imgData.data);
                texture = new THREE.DataTexture(image8Array, imgData.width, imgData.height);

                if (imgData.channels === 1) texture.format = THREE.RedFormat;
                else if (imgData.channels === 2) texture.format = THREE.RGFormat;
                else if (imgData.channels === 4) texture.format = THREE.RGBAFormat;
                else {
                    console.warn(`Unsupported channel count for texture ${textureId}: ${imgData.channels}`);
                    return null;
                }

                texture.flipY = true;
                texture.needsUpdate = true;
            } else {
                // Needs decoding - check format and decode
                const mimeType = getMimeType(imgData);

                // Check if HDR format - use specialized decoder
                if (isHDRFormat(mimeType)) {
                    texture = await decodeHDRTexture(imgData.data, mimeType);
                } else {
                    // Standard image - use Blob and TextureLoader
                    const blob = new Blob([imgData.data], { type: mimeType });
                    const blobUrl = URL.createObjectURL(blob);
                    try {
                        const loader = new THREE.TextureLoader();
                        texture = await loader.loadAsync(blobUrl);
                    } finally {
                        URL.revokeObjectURL(blobUrl);
                    }
                }
            }
        }

        texture = applyTextureSampler(texture, texData);

        if (texture && cache) {
            cache.set(cacheKey, texture);
        }

        return texture;

    } catch (error) {
        console.warn(`Failed to load texture ${textureId}; skipping texture`, {
            textureId,
            error,
            expectedForUnsupportedFormats: true
        });
        return null;
    }
}

// ============================================================================
// OpenPBR to MeshPhysicalMaterial Conversion
// ============================================================================

/**
 * Convert OpenPBR material data to Three.js MeshPhysicalMaterial
 * Waits for all textures to load before returning the material.
 *
 * @param {Object} materialData - OpenPBR material data from USD
 * @param {Object} usdScene - USD scene for texture loading
 * @param {Object} options - Conversion options
 * @returns {Promise<THREE.MeshPhysicalMaterial>}
 */
async function convertOpenPBRToMeshPhysicalMaterialLoaded(materialData, usdScene = null, options = {}) {
    const material = new THREE.MeshPhysicalMaterial();

    // Store texture references for later management
    material.userData.textures = {};
    material.userData.materialType = 'OpenPBR';
    material.userData.openPBRData = materialData;

    // Get OpenPBR data - support multiple formats
    let pbr = null;

    // Check for grouped format first
    if (materialData.openPBR) {
        pbr = materialData.openPBR;
        material.userData.format = 'grouped';
    }
    // Check for flat format
    else if (materialData.base_color !== undefined ||
             materialData.base_metalness !== undefined ||
             materialData.specular_roughness !== undefined) {
        pbr = { flat: materialData };
        material.userData.format = 'flat';
    }
    // Check for openPBRShader format
    else if (materialData.openPBRShader) {
        pbr = { flat: materialData.openPBRShader };
        material.userData.format = 'flat';
    }

    if (!pbr) {
        console.warn('No OpenPBR data found in material');
        return material;
    }

	    // Texture cache and delayed loading manager
	    const textureCache = options.textureCache || new Map();
	    const textureManager = options.textureLoadingManager || null;
	    const skipTextures = options.skipTextures || false;

    // Helper to apply parameter with optional texture
    const applyParam = async (paramName, paramValue, group = null) => {
        const mapping = OPENPBR_TO_THREEJS_MAP[paramName];
        if (!mapping || !mapping.property) return;

        const value = extractValue(paramValue);

        // Apply scalar or color value
        if (mapping.type === 'color' && Array.isArray(value)) {
            material[mapping.property] = createColor(value, 'linear', usdScene);
        } else if (mapping.type === 'scalar' && typeof value === 'number') {
            material[mapping.property] = value;
        }

        // Load and apply texture if present
	        if (!skipTextures && usdScene && hasTexture(paramValue)) {
            const texMapName = OPENPBR_TEXTURE_MAP[paramName];
            if (texMapName) {
                const textureId = getTextureId(paramValue);
                if (texMapName === 'map') {
                    material.color = new THREE.Color(1, 1, 1);
                }

                // If textureLoadingManager is provided, queue texture for later loading
                if (textureManager) {
                    textureManager.queueTexture(material, texMapName, textureId, usdScene);
                } else {
                    // Load immediately (original behavior)
                    const texture = await loadTextureFromUSD(usdScene, textureId, textureCache);
                    if (texture) {
                        material[texMapName] = texture;
                        material.userData.textures[texMapName] = {
                            textureId: textureId,
                            texture: texture
                        };
                        material.needsUpdate = true;
                    }
                }
            }
        }
    };

    // Process flat format
    if (pbr.flat) {
        const flat = pbr.flat;

        // Base layer
        await applyParam('base_color', flat.base_color);
        await applyParam('base_metalness', flat.base_metalness);

        // Specular layer
        await applyParam('specular_roughness', flat.specular_roughness);
        await applyParam('specular_ior', flat.specular_ior);
        await applyParam('specular_color', flat.specular_color);
        await applyParam('specular_anisotropy', flat.specular_anisotropy);

        // Transmission
        await applyParam('transmission_weight', flat.transmission_weight);
        await applyParam('transmission_color', flat.transmission_color);

        // Coat
        await applyParam('coat_weight', flat.coat_weight);
        await applyParam('coat_roughness', flat.coat_roughness);

        // Sheen/Fuzz
        if (flat.sheen_weight !== undefined) {
            await applyParam('sheen_weight', flat.sheen_weight);
            await applyParam('sheen_color', flat.sheen_color);
            await applyParam('sheen_roughness', flat.sheen_roughness);
        } else if (flat.fuzz_weight !== undefined) {
            await applyParam('fuzz_weight', flat.fuzz_weight);
            await applyParam('fuzz_color', flat.fuzz_color);
            await applyParam('fuzz_roughness', flat.fuzz_roughness);
        }

        // Thin film (iridescence)
        if (flat.thin_film_weight !== undefined) {
            const weight = extractValue(flat.thin_film_weight);
            if (weight > 0) {
                material.iridescence = weight;
                const thickness = extractValue(flat.thin_film_thickness) || 500;
                material.iridescenceThicknessRange = [100, thickness];
                material.iridescenceIOR = extractValue(flat.thin_film_ior) || 1.5;
            }
        }

        // Emission
        if (flat.emission_color !== undefined) {
            const emissionColor = extractValue(flat.emission_color);
            if (emissionColor && Array.isArray(emissionColor)) {
                material.emissive = createColor(emissionColor, 'linear', usdScene);
            }
            // Load emission texture
            if (!skipTextures && usdScene && hasTexture(flat.emission_color)) {
                const textureId = getTextureId(flat.emission_color);
                if (textureManager) {
                    textureManager.queueTexture(material, 'emissiveMap', textureId, usdScene);
                } else {
                    const texture = await loadTextureFromUSD(usdScene, textureId, textureCache);
                    if (texture) {
                        material.emissiveMap = texture;
                        material.userData.textures.emissiveMap = { textureId, texture };
                    }
                }
            }
        }
        if (flat.emission_luminance !== undefined) {
            material.emissiveIntensity = extractValue(flat.emission_luminance) || 0;
        }

        // Geometry - opacity/alpha
        const opacityParam = flat.opacity !== undefined ? flat.opacity : flat.geometry_opacity;
        if (opacityParam !== undefined) {
            const opacityValue = extractValue(opacityParam);
            if (typeof opacityValue === 'number') {
                material.opacity = opacityValue;
                material.transparent = opacityValue < 1.0;
            }
            if (!skipTextures && usdScene && hasTexture(opacityParam)) {
                const textureId = getTextureId(opacityParam);
                // For alpha maps, we need to set transparent=true even in delayed mode
                material.transparent = true;
                if (textureManager) {
                    textureManager.queueTexture(material, 'alphaMap', textureId, usdScene);
                } else {
                    const texture = await loadTextureFromUSD(usdScene, textureId, textureCache);
                    if (texture) {
                        material.alphaMap = texture;
                        material.userData.textures.alphaMap = { textureId, texture };
                    }
                }
            }
        }

        const normalParam = extractNormalParam(flat);
        const normalScale = extractNormalMapScale(flat);
        await applyNormalMapLoaded(material, normalParam, normalScale, usdScene, textureCache, textureManager, skipTextures);
    }

    // Process grouped format
    else {
        // Base layer
        if (pbr.base) {
            await applyParam('base_color', pbr.base.base_color);
            await applyParam('base_metalness', pbr.base.base_metalness);
        }

        // Specular layer
        if (pbr.specular) {
            await applyParam('specular_roughness', pbr.specular.specular_roughness);
            await applyParam('specular_ior', pbr.specular.specular_ior);
            await applyParam('specular_color', pbr.specular.specular_color);
            await applyParam('specular_anisotropy', pbr.specular.specular_anisotropy);
        }

        // Transmission
        if (pbr.transmission) {
            await applyParam('transmission_weight', pbr.transmission.transmission_weight);
            await applyParam('transmission_color', pbr.transmission.transmission_color);
        }

        // Coat
        if (pbr.coat) {
            await applyParam('coat_weight', pbr.coat.coat_weight);
            await applyParam('coat_roughness', pbr.coat.coat_roughness);
        }

        // Sheen
        if (pbr.sheen) {
            await applyParam('sheen_weight', pbr.sheen.sheen_weight);
            await applyParam('sheen_color', pbr.sheen.sheen_color);
            await applyParam('sheen_roughness', pbr.sheen.sheen_roughness);
        }

        // Fuzz
        if (pbr.fuzz) {
            await applyParam('fuzz_weight', pbr.fuzz.fuzz_weight);
            await applyParam('fuzz_color', pbr.fuzz.fuzz_color);
            await applyParam('fuzz_roughness', pbr.fuzz.fuzz_roughness);
        }

        // Thin film
        if (pbr.thin_film) {
            const weight = extractValue(pbr.thin_film.thin_film_weight);
            if (weight > 0) {
                material.iridescence = weight;
                const thickness = extractValue(pbr.thin_film.thin_film_thickness) || 500;
                material.iridescenceThicknessRange = [100, thickness];
                material.iridescenceIOR = extractValue(pbr.thin_film.thin_film_ior) || 1.5;
            }
        }

        // Emission
        if (pbr.emission) {
            const emissionColor = extractValue(pbr.emission.emission_color);
            if (emissionColor && Array.isArray(emissionColor)) {
                material.emissive = createColor(emissionColor, 'linear', usdScene);
            }
            if (!skipTextures && usdScene && hasTexture(pbr.emission.emission_color)) {
                const textureId = getTextureId(pbr.emission.emission_color);
                if (textureManager) {
                    textureManager.queueTexture(material, 'emissiveMap', textureId, usdScene);
                } else {
                    const texture = await loadTextureFromUSD(usdScene, textureId, textureCache);
                    if (texture) {
                        material.emissiveMap = texture;
                        material.userData.textures.emissiveMap = { textureId, texture };
                    }
                }
            }
            if (pbr.emission.emission_luminance !== undefined) {
                material.emissiveIntensity = extractValue(pbr.emission.emission_luminance) || 0;
            }
        }

        // Geometry
        if (pbr.geometry) {
            const opacityParam = pbr.geometry.opacity !== undefined ? pbr.geometry.opacity : pbr.geometry.geometry_opacity;
            if (opacityParam !== undefined) {
                const opacityValue = extractValue(opacityParam);
                if (typeof opacityValue === 'number') {
                    material.opacity = opacityValue;
                    material.transparent = opacityValue < 1.0;
                }
                if (!skipTextures && usdScene && hasTexture(opacityParam)) {
                    const textureId = getTextureId(opacityParam);
                    material.transparent = true;
                    if (textureManager) {
                        textureManager.queueTexture(material, 'alphaMap', textureId, usdScene);
                    } else {
                        const texture = await loadTextureFromUSD(usdScene, textureId, textureCache);
                        if (texture) {
                            material.alphaMap = texture;
                            material.userData.textures.alphaMap = { textureId, texture };
                        }
                    }
                }
            }

            const normalParam = extractNormalParam(pbr.geometry);
            const normalScale = extractNormalMapScale(pbr.geometry);
            await applyNormalMapLoaded(material, normalParam, normalScale, usdScene, textureCache, textureManager, skipTextures);
        }
    }

    // Apply environment map if provided
    if (options.envMap) {
        material.envMap = options.envMap;
        material.envMapIntensity = options.envMapIntensity || 1.0;
    }

    // Set material name
    if (materialData.name) {
        material.name = materialData.name;
    }

    return material;
}

/**
 * Convert OpenPBR material data to Three.js MeshPhysicalMaterial (legacy pattern)
 * Returns material immediately, textures load asynchronously in the background.
 * This matches the behavior of convertUsdMaterialToMeshPhysicalMaterial.
 *
 * @param {Object} materialData - OpenPBR material data from USD
 * @param {Object} usdScene - USD scene for texture loading
 * @param {Object} options - Conversion options
 * @returns {THREE.MeshPhysicalMaterial} Material (textures load asynchronously)
 */
function convertOpenPBRToMeshPhysicalMaterial(materialData, usdScene = null, options = {}) {
    const material = new THREE.MeshPhysicalMaterial();

    // Store texture references for later management
    material.userData.textures = {};
    material.userData.materialType = 'OpenPBR';
    material.userData.openPBRData = materialData;

    // Get OpenPBR data - support multiple formats
    let pbr = null;

    // Check for grouped format first
    if (materialData.openPBR) {
        pbr = materialData.openPBR;
        material.userData.format = 'grouped';
    }
    // Check for flat format
    else if (materialData.base_color !== undefined ||
             materialData.base_metalness !== undefined ||
             materialData.specular_roughness !== undefined) {
        pbr = { flat: materialData };
        material.userData.format = 'flat';
    }
    // Check for openPBRShader format
    else if (materialData.openPBRShader) {
        pbr = { flat: materialData.openPBRShader };
        material.userData.format = 'flat';
    }

    if (!pbr) {
        console.warn('No OpenPBR data found in material');
        return material;
    }

	    // Texture cache
	    const textureCache = options.textureCache || new Map();
	    const skipTextures = options.skipTextures || false;

    // Helper to apply parameter value (sync) and queue texture loading (async)
    const applyParam = (paramName, paramValue) => {
        const mapping = OPENPBR_TO_THREEJS_MAP[paramName];
        if (!mapping || !mapping.property) return;

        const value = extractValue(paramValue);

        // Apply scalar or color value immediately
        if (mapping.type === 'color' && Array.isArray(value)) {
            material[mapping.property] = createColor(value, 'linear', usdScene);
        } else if (mapping.type === 'scalar' && typeof value === 'number') {
            material[mapping.property] = value;
        }

        // Queue texture loading (fire-and-forget)
	        if (!skipTextures && usdScene && hasTexture(paramValue)) {
            const texMapName = OPENPBR_TEXTURE_MAP[paramName];
            if (texMapName) {
                if (texMapName === 'map') {
                    material.color = new THREE.Color(1, 1, 1);
                }
                loadTextureFromUSD(usdScene, getTextureId(paramValue), textureCache).then((texture) => {
                    if (texture) {
                        material[texMapName] = texture;
                        material.userData.textures[texMapName] = {
                            textureId: getTextureId(paramValue),
                            texture: texture
                        };
                        material.needsUpdate = true;
                    }
                }).catch((err) => {
                    console.error(`Failed to load texture for ${paramName}:`, err);
                });
            }
        }
    };

    // Process flat format
    if (pbr.flat) {
        const flat = pbr.flat;

        // Base layer
        applyParam('base_color', flat.base_color);
        applyParam('base_metalness', flat.base_metalness);

        // Specular layer
        applyParam('specular_roughness', flat.specular_roughness);
        applyParam('specular_ior', flat.specular_ior);
        applyParam('specular_color', flat.specular_color);
        applyParam('specular_anisotropy', flat.specular_anisotropy);

        // Transmission
        applyParam('transmission_weight', flat.transmission_weight);
        applyParam('transmission_color', flat.transmission_color);

        // Coat
        applyParam('coat_weight', flat.coat_weight);
        applyParam('coat_roughness', flat.coat_roughness);

        // Sheen/Fuzz
        if (flat.sheen_weight !== undefined) {
            applyParam('sheen_weight', flat.sheen_weight);
            applyParam('sheen_color', flat.sheen_color);
            applyParam('sheen_roughness', flat.sheen_roughness);
        } else if (flat.fuzz_weight !== undefined) {
            applyParam('fuzz_weight', flat.fuzz_weight);
            applyParam('fuzz_color', flat.fuzz_color);
            applyParam('fuzz_roughness', flat.fuzz_roughness);
        }

        // Thin film (iridescence)
        if (flat.thin_film_weight !== undefined) {
            const weight = extractValue(flat.thin_film_weight);
            if (weight > 0) {
                material.iridescence = weight;
                const thickness = extractValue(flat.thin_film_thickness) || 500;
                material.iridescenceThicknessRange = [100, thickness];
                material.iridescenceIOR = extractValue(flat.thin_film_ior) || 1.5;
            }
        }

        // Emission
        if (flat.emission_color !== undefined) {
            const emissionColor = extractValue(flat.emission_color);
            if (emissionColor && Array.isArray(emissionColor)) {
                material.emissive = createColor(emissionColor, 'linear', usdScene);
            }
            // Load emission texture (fire-and-forget)
            if (!skipTextures && usdScene && hasTexture(flat.emission_color)) {
                loadTextureFromUSD(usdScene, getTextureId(flat.emission_color), textureCache).then((texture) => {
                    if (texture) {
                        material.emissiveMap = texture;
                        material.userData.textures.emissiveMap = { textureId: getTextureId(flat.emission_color), texture };
                        material.needsUpdate = true;
                    }
                }).catch((err) => {
                    console.error('Failed to load emission texture:', err);
                });
            }
        }
        if (flat.emission_luminance !== undefined) {
            material.emissiveIntensity = extractValue(flat.emission_luminance) || 0;
        }

        // Geometry - opacity/alpha
        const opacityParam = flat.opacity !== undefined ? flat.opacity : flat.geometry_opacity;
        if (opacityParam !== undefined) {
            const opacityValue = extractValue(opacityParam);
            if (typeof opacityValue === 'number') {
                material.opacity = opacityValue;
                material.transparent = opacityValue < 1.0;
            }
            if (!skipTextures && usdScene && hasTexture(opacityParam)) {
                loadTextureFromUSD(usdScene, getTextureId(opacityParam), textureCache).then((texture) => {
                    if (texture) {
                        material.alphaMap = texture;
                        material.transparent = true;
                        material.userData.textures.alphaMap = { textureId: getTextureId(opacityParam), texture };
                        material.needsUpdate = true;
                    }
                }).catch((err) => {
                    console.error('Failed to load opacity texture:', err);
                });
            }
        }

        const normalParam = extractNormalParam(flat);
        const normalScale = extractNormalMapScale(flat);
        applyNormalMapLegacy(material, normalParam, normalScale, usdScene, textureCache, skipTextures);
    }

    // Process grouped format
    else {
        // Base layer
        if (pbr.base) {
            applyParam('base_color', pbr.base.base_color);
            applyParam('base_metalness', pbr.base.base_metalness);
        }

        // Specular layer
        if (pbr.specular) {
            applyParam('specular_roughness', pbr.specular.specular_roughness);
            applyParam('specular_ior', pbr.specular.specular_ior);
            applyParam('specular_color', pbr.specular.specular_color);
            applyParam('specular_anisotropy', pbr.specular.specular_anisotropy);
        }

        // Transmission
        if (pbr.transmission) {
            applyParam('transmission_weight', pbr.transmission.transmission_weight);
            applyParam('transmission_color', pbr.transmission.transmission_color);
        }

        // Coat
        if (pbr.coat) {
            applyParam('coat_weight', pbr.coat.coat_weight);
            applyParam('coat_roughness', pbr.coat.coat_roughness);
        }

        // Sheen
        if (pbr.sheen) {
            applyParam('sheen_weight', pbr.sheen.sheen_weight);
            applyParam('sheen_color', pbr.sheen.sheen_color);
            applyParam('sheen_roughness', pbr.sheen.sheen_roughness);
        }

        // Fuzz
        if (pbr.fuzz) {
            applyParam('fuzz_weight', pbr.fuzz.fuzz_weight);
            applyParam('fuzz_color', pbr.fuzz.fuzz_color);
            applyParam('fuzz_roughness', pbr.fuzz.fuzz_roughness);
        }

        // Thin film
        if (pbr.thin_film) {
            const weight = extractValue(pbr.thin_film.thin_film_weight);
            if (weight > 0) {
                material.iridescence = weight;
                const thickness = extractValue(pbr.thin_film.thin_film_thickness) || 500;
                material.iridescenceThicknessRange = [100, thickness];
                material.iridescenceIOR = extractValue(pbr.thin_film.thin_film_ior) || 1.5;
            }
        }

        // Emission
        if (pbr.emission) {
            const emissionColor = extractValue(pbr.emission.emission_color);
            if (emissionColor && Array.isArray(emissionColor)) {
                material.emissive = createColor(emissionColor, 'linear', usdScene);
            }
            if (!skipTextures && usdScene && hasTexture(pbr.emission.emission_color)) {
                loadTextureFromUSD(usdScene, getTextureId(pbr.emission.emission_color), textureCache).then((texture) => {
                    if (texture) {
                        material.emissiveMap = texture;
                        material.userData.textures.emissiveMap = { textureId: getTextureId(pbr.emission.emission_color), texture };
                        material.needsUpdate = true;
                    }
                }).catch((err) => {
                    console.error('Failed to load emission texture:', err);
                });
            }
            if (pbr.emission.emission_luminance !== undefined) {
                material.emissiveIntensity = extractValue(pbr.emission.emission_luminance) || 0;
            }
        }

        // Geometry
        if (pbr.geometry) {
            const opacityParam = pbr.geometry.opacity !== undefined ? pbr.geometry.opacity : pbr.geometry.geometry_opacity;
            if (opacityParam !== undefined) {
                const opacityValue = extractValue(opacityParam);
                if (typeof opacityValue === 'number') {
                    material.opacity = opacityValue;
                    material.transparent = opacityValue < 1.0;
                }
                if (!skipTextures && usdScene && hasTexture(opacityParam)) {
                    loadTextureFromUSD(usdScene, getTextureId(opacityParam), textureCache).then((texture) => {
                        if (texture) {
                            material.alphaMap = texture;
                            material.transparent = true;
                            material.userData.textures.alphaMap = { textureId: getTextureId(opacityParam), texture };
                            material.needsUpdate = true;
                        }
                    }).catch((err) => {
                        console.error('Failed to load opacity texture:', err);
                    });
                }
            }

            const normalParam = extractNormalParam(pbr.geometry);
            const normalScale = extractNormalMapScale(pbr.geometry);
            applyNormalMapLegacy(material, normalParam, normalScale, usdScene, textureCache, skipTextures);
        }
    }

    // Apply environment map if provided
    if (options.envMap) {
        material.envMap = options.envMap;
        material.envMapIntensity = options.envMapIntensity || 1.0;
    }

    // Set material name
    if (materialData.name) {
        material.name = materialData.name;
    }

    return material;
}

// ============================================================================
// MaterialX NodeGraph Optimizer
// ============================================================================

/**
 * Optimization levels for NodeGraph processing
 */
const NodeGraphOptimizationLevel = {
    NONE: 0,           // No optimization
    BASIC: 1,          // Remove identity ops only
    STANDARD: 2,       // Pattern detection + identity removal
    AGGRESSIVE: 3      // All optimizations + constant folding
};

// Helper functions for value comparison
function _approxEqual(a, b, epsilon = 0.0001) {
    if (typeof a === 'number' && typeof b === 'number') {
        return Math.abs(a - b) < epsilon;
    }
    if (Array.isArray(a) && Array.isArray(b) && a.length === b.length) {
        return a.every((v, i) => _approxEqual(v, b[i], epsilon));
    }
    return a === b;
}

function _isWhite(value) {
    return Array.isArray(value) && value.length === 3 && _approxEqual(value, [1, 1, 1]);
}

function _isBlack(value) {
    return Array.isArray(value) && value.length === 3 && _approxEqual(value, [0, 0, 0]);
}

function _isOne(value) {
    return _approxEqual(value, 1);
}

function _isZero(value) {
    return _approxEqual(value, 0);
}

function _buildNodeMap(nodes) {
    const map = new Map();
    for (const node of nodes) {
        map.set(node.name, node);
    }
    return map;
}

function _getInputInfo(node, inputName) {
    if (!node.inputs) return null;
    const input = node.inputs.find(i => i.name === inputName);
    if (!input) return null;
    if (input.nodename) {
        return { type: 'connection', nodename: input.nodename, output: input.output || 'out' };
    }
    return { type: 'value', value: input.value };
}

/**
 * Detect Invert pattern: constant -> subtract([1,1,1], x) -> mix
 */
function _detectInvertPattern(nodes, nodeMap) {
    const patterns = [];
    for (const mixNode of nodes) {
        if (mixNode.category !== 'mix_color3') continue;
        const bgInput = _getInputInfo(mixNode, 'bg');
        const fgInput = _getInputInfo(mixNode, 'fg');
        const mixInput = _getInputInfo(mixNode, 'mix');
        if (!bgInput || !fgInput || bgInput.type !== 'connection' || fgInput.type !== 'connection') continue;

        const subtractNode = nodeMap.get(fgInput.nodename);
        if (!subtractNode || subtractNode.category !== 'subtract_color3') continue;

        const in1 = _getInputInfo(subtractNode, 'in1');
        const in2 = _getInputInfo(subtractNode, 'in2');
        if (!in1 || in1.type !== 'value' || !_isWhite(in1.value)) continue;
        if (!in2 || in2.type !== 'connection' || in2.nodename !== bgInput.nodename) continue;

        const factor = mixInput && mixInput.type === 'value' ? mixInput.value : 1;
        patterns.push({
            type: 'invert',
            outputNode: mixNode.name,
            inputNode: bgInput.nodename,
            subtractNode: subtractNode.name,
            factor: factor,
            nodesToRemove: [subtractNode.name],
            replacement: {
                name: mixNode.name,
                category: 'invert_color3',
                type: 'ND_invert_color3',
                inputs: [
                    { name: 'in', nodename: bgInput.nodename, output: 'out' },
                    { name: 'amount', type: 'float', value: factor }
                ]
            }
        });
    }
    return patterns;
}

/**
 * Detect Brightness/Contrast pattern: multiply -> add -> subtract -> max
 */
function _detectBrightnessContrastPattern(nodes, nodeMap) {
    const patterns = [];
    for (const maxNode of nodes) {
        if (maxNode.category !== 'max_color3') continue;
        const maxIn2 = _getInputInfo(maxNode, 'in2');
        if (!maxIn2 || maxIn2.type !== 'value' || !_isBlack(maxIn2.value)) continue;

        const maxIn1 = _getInputInfo(maxNode, 'in1');
        if (!maxIn1 || maxIn1.type !== 'connection') continue;

        const subtractNode = nodeMap.get(maxIn1.nodename);
        if (!subtractNode || subtractNode.category !== 'subtract_color3') continue;

        const subIn1 = _getInputInfo(subtractNode, 'in1');
        const subIn2 = _getInputInfo(subtractNode, 'in2');
        if (!subIn1 || subIn1.type !== 'connection') continue;

        const addNode = nodeMap.get(subIn1.nodename);
        if (!addNode || addNode.category !== 'add_color3') continue;

        const addIn1 = _getInputInfo(addNode, 'in1');
        const addIn2 = _getInputInfo(addNode, 'in2');

        let multiplyNodeName = null;
        let brightness = [0, 0, 0];

        if (addIn2?.type === 'connection') {
            const maybeMultiply = nodeMap.get(addIn2.nodename);
            if (maybeMultiply?.category === 'multiply_color3') {
                multiplyNodeName = addIn2.nodename;
                brightness = addIn1?.type === 'value' ? addIn1.value : [0, 0, 0];
            }
        }
        if (!multiplyNodeName && addIn1?.type === 'connection') {
            const maybeMultiply = nodeMap.get(addIn1.nodename);
            if (maybeMultiply?.category === 'multiply_color3') {
                multiplyNodeName = addIn1.nodename;
                brightness = addIn2?.type === 'value' ? addIn2.value : [0, 0, 0];
            }
        }
        if (!multiplyNodeName) continue;

        const multiplyNode = nodeMap.get(multiplyNodeName);
        const mulIn1 = _getInputInfo(multiplyNode, 'in1');
        const mulIn2 = _getInputInfo(multiplyNode, 'in2');

        let inputNodeName = null;
        let contrast = [1, 1, 1];

        if (mulIn1?.type === 'connection') {
            inputNodeName = mulIn1.nodename;
            contrast = mulIn2?.type === 'value' ? mulIn2.value : [1, 1, 1];
        } else if (mulIn2?.type === 'connection') {
            inputNodeName = mulIn2.nodename;
            contrast = mulIn1?.type === 'value' ? mulIn1.value : [1, 1, 1];
        }
        if (!inputNodeName) continue;

        patterns.push({
            type: 'brightness_contrast',
            outputNode: maxNode.name,
            inputNode: inputNodeName,
            brightness: brightness,
            contrast: contrast,
            nodesToRemove: [subtractNode.name, addNode.name, multiplyNodeName],
            replacement: {
                name: maxNode.name,
                category: 'brightness_contrast_color3',
                type: 'ND_brightness_contrast_color3',
                inputs: [
                    { name: 'in', nodename: inputNodeName, output: 'out' },
                    { name: 'brightness', type: 'color3f', value: brightness },
                    { name: 'contrast', type: 'color3f', value: contrast }
                ]
            }
        });
    }
    return patterns;
}

/**
 * Detect HSV Adjust pattern: combine3 -> hsvadjust
 */
function _detectHSVAdjustPattern(nodes, nodeMap) {
    const patterns = [];
    for (const hsvNode of nodes) {
        if (hsvNode.category !== 'hsvadjust_color3') continue;
        const amountInput = _getInputInfo(hsvNode, 'amount');
        const inInput = _getInputInfo(hsvNode, 'in');
        if (!amountInput || amountInput.type !== 'connection' || !inInput) continue;

        const combineNode = nodeMap.get(amountInput.nodename);
        if (!combineNode || combineNode.category !== 'combine3_vector3') continue;

        const in1 = _getInputInfo(combineNode, 'in1');
        const in2 = _getInputInfo(combineNode, 'in2');
        const in3 = _getInputInfo(combineNode, 'in3');
        // ND_hsvadjust_color3 uses a direct hue offset, while the optimized
        // ND_hsv_adjust_color3 form uses 0.5 as its neutral hue input.
        // Preserve the authored offset when changing node variants.
        const hueInput = (in1?.value ?? 0) + 0.5;

        patterns.push({
            type: 'hsv_adjust',
            outputNode: hsvNode.name,
            inputNode: inInput.type === 'connection' ? inInput.nodename : null,
            hue: hueInput,
            saturation: in2?.type === 'value' ? in2.value : 1,
            value: in3?.type === 'value' ? in3.value : 1,
            nodesToRemove: [combineNode.name],
            replacement: {
                name: hsvNode.name,
                category: 'hsv_adjust_color3',
                type: 'ND_hsv_adjust_color3',
                inputs: inInput.type === 'connection' ? [
                    { name: 'in', nodename: inInput.nodename, output: 'out' },
                    { name: 'hue', type: 'float', value: hueInput },
                    { name: 'saturation', type: 'float', value: in2?.value ?? 1 },
                    { name: 'value', type: 'float', value: in3?.value ?? 1 }
                ] : [
                    { name: 'in', type: 'color3f', value: inInput.value },
                    { name: 'hue', type: 'float', value: hueInput },
                    { name: 'saturation', type: 'float', value: in2?.value ?? 1 },
                    { name: 'value', type: 'float', value: in3?.value ?? 1 }
                ]
            }
        });
    }
    return patterns;
}

// ============================================================================
// Extract Channel Optimization Patterns
// ============================================================================

/**
 * Detect swizzle patterns: extract channels and recombine in different order
 * e.g., extract(R,G,B) -> combine(B,G,R) = swizzle(BGR)
 * This can be replaced with a single swizzle node or marked for shader optimization
 */
function _detectSwizzlePattern(nodes, nodeMap) {
    const patterns = [];

    for (const combineNode of nodes) {
        if (combineNode.category !== 'combine3_color3' && combineNode.category !== 'combine3_vector3') continue;

        const in1 = _getInputInfo(combineNode, 'in1');
        const in2 = _getInputInfo(combineNode, 'in2');
        const in3 = _getInputInfo(combineNode, 'in3');

        if (!in1?.nodename || !in2?.nodename || !in3?.nodename) continue;

        const extract1 = nodeMap.get(in1.nodename);
        const extract2 = nodeMap.get(in2.nodename);
        const extract3 = nodeMap.get(in3.nodename);

        // All must be extract operations from the same source
        const isExtract = (n) => n?.category === 'extract_color3' || n?.category === 'extract_vector3' ||
                                 n?.category === 'separate3_color3' || n?.category === 'separate3_vector3';

        if (!isExtract(extract1) || !isExtract(extract2) || !isExtract(extract3)) continue;

        const src1 = _getInputInfo(extract1, 'in');
        const src2 = _getInputInfo(extract2, 'in');
        const src3 = _getInputInfo(extract3, 'in');

        if (!src1?.nodename || src1.nodename !== src2?.nodename || src1.nodename !== src3?.nodename) continue;

        // Get channel indices
        const idx1 = _getInputInfo(extract1, 'index')?.value ?? (in1.output === 'outx' ? 0 : in1.output === 'outy' ? 1 : in1.output === 'outz' ? 2 : -1);
        const idx2 = _getInputInfo(extract2, 'index')?.value ?? (in2.output === 'outx' ? 0 : in2.output === 'outy' ? 1 : in2.output === 'outz' ? 2 : -1);
        const idx3 = _getInputInfo(extract3, 'index')?.value ?? (in3.output === 'outx' ? 0 : in3.output === 'outy' ? 1 : in3.output === 'outz' ? 2 : -1);

        // Identity (0,1,2) is handled by separate_combine_passthrough
        if (idx1 === 0 && idx2 === 1 && idx3 === 2) continue;

        // Check for valid swizzle pattern
        const indices = [idx1, idx2, idx3];
        if (indices.some(i => i < 0 || i > 2)) continue;

        const swizzleMap = { 0: 'R', 1: 'G', 2: 'B' };
        const swizzle = indices.map(i => swizzleMap[i]).join('');

        patterns.push({
            type: 'swizzle',
            outputNode: combineNode.name,
            inputNode: src1.nodename,
            swizzle: swizzle,
            extractNodes: [extract1.name, extract2.name, extract3.name],
            nodesToRemove: [], // Don't remove, mark for shader optimization
            replacement: null,
            optimizationHint: `Can be optimized to single swizzle: .${swizzle.toLowerCase()}`
        });
    }

    return patterns;
}

/**
 * Detect channel isolation to grayscale: extract(channel) -> combine(x,x,x)
 * e.g., extract(R) -> combine(R,R,R) = grayscale from red channel
 */
function _detectChannelToGrayscalePattern(nodes, nodeMap) {
    const patterns = [];

    for (const combineNode of nodes) {
        if (combineNode.category !== 'combine3_color3' && combineNode.category !== 'combine3_vector3') continue;

        const in1 = _getInputInfo(combineNode, 'in1');
        const in2 = _getInputInfo(combineNode, 'in2');
        const in3 = _getInputInfo(combineNode, 'in3');

        // All three inputs must come from the same node (same channel duplicated)
        if (!in1?.nodename || in1.nodename !== in2?.nodename || in1.nodename !== in3?.nodename) continue;

        // Check if outputs are the same (same channel)
        const out1 = in1.output || 'out';
        const out2 = in2.output || 'out';
        const out3 = in3.output || 'out';

        if (out1 !== out2 || out2 !== out3) continue;

        const sourceNode = nodeMap.get(in1.nodename);
        if (!sourceNode) continue;

        // Check if source is an extract operation
        const isExtract = sourceNode.category === 'extract_color3' || sourceNode.category === 'extract_vector3';
        const channelMap = { 'outx': 'R', 'outy': 'G', 'outz': 'B', 'out': '?' };
        const channel = channelMap[out1] || '?';

        patterns.push({
            type: 'channel_to_grayscale',
            outputNode: combineNode.name,
            sourceNode: sourceNode.name,
            channel: channel,
            isFromExtract: isExtract,
            nodesToRemove: [], // Mark but don't remove - this is a valid pattern
            replacement: null,
            optimizationHint: `Grayscale from channel ${channel}`
        });
    }

    return patterns;
}

/**
 * Detect unused extract outputs (dead code)
 * Extract nodes whose outputs are never used can be removed
 */
function _detectUnusedExtractPattern(nodes, nodeMap, ngOutputs) {
    const patterns = [];

    // Build a map of all node inputs (connections)
    const usedOutputs = new Set();
    for (const node of nodes) {
        if (!node.inputs) continue;
        for (const input of node.inputs) {
            if (input.nodename) {
                usedOutputs.add(`${input.nodename}:${input.output || 'out'}`);
            }
        }
    }

    // Also mark nodes referenced by NodeGraph outputs as used
    if (ngOutputs) {
        for (const output of ngOutputs) {
            if (output.nodename) {
                usedOutputs.add(`${output.nodename}:${output.output || 'out'}`);
            }
        }
    }

    // Check each extract node
    for (const node of nodes) {
        if (node.category !== 'extract_color3' && node.category !== 'extract_vector3' &&
            node.category !== 'separate3_color3' && node.category !== 'separate3_vector3') continue;

        // For separate nodes, check each output
        const outputs = node.category.startsWith('separate') ? ['outx', 'outy', 'outz'] : ['out'];
        const unusedOutputs = outputs.filter(out => !usedOutputs.has(`${node.name}:${out}`));

        if (unusedOutputs.length === outputs.length) {
            // All outputs unused - node is dead code
            patterns.push({
                type: 'unused_extract',
                outputNode: node.name,
                unusedOutputs: unusedOutputs,
                nodesToRemove: [node.name],
                passthrough: null
            });
        } else if (unusedOutputs.length > 0 && node.category.startsWith('separate')) {
            // Some outputs unused - partial dead code (info only)
            patterns.push({
                type: 'partial_unused_extract',
                outputNode: node.name,
                unusedOutputs: unusedOutputs,
                nodesToRemove: [], // Don't remove, just mark
                optimizationHint: `Unused channels: ${unusedOutputs.join(', ')}`
            });
        }
    }

    return patterns;
}

/**
 * Detect single channel modification pattern:
 * extract(R,G,B) -> modify(R) -> combine(R',G,B)
 * This is a masked operation that could be optimized
 */
function _detectSingleChannelModPattern(nodes, nodeMap) {
    const patterns = [];

    for (const combineNode of nodes) {
        if (combineNode.category !== 'combine3_color3' && combineNode.category !== 'combine3_vector3') continue;

        const inputs = [
            _getInputInfo(combineNode, 'in1'),
            _getInputInfo(combineNode, 'in2'),
            _getInputInfo(combineNode, 'in3')
        ];

        if (inputs.some(i => !i?.nodename)) continue;

        // Track which channels come directly from extract and which are modified
        const channelSources = inputs.map((input, idx) => {
            const node = nodeMap.get(input.nodename);
            if (!node) return { modified: true, source: null };

            // Direct from extract/separate?
            if (node.category === 'extract_color3' || node.category === 'extract_vector3' ||
                node.category === 'separate3_color3' || node.category === 'separate3_vector3') {
                const srcInput = _getInputInfo(node, 'in');
                const channelIdx = _getInputInfo(node, 'index')?.value ??
                    (input.output === 'outx' ? 0 : input.output === 'outy' ? 1 : input.output === 'outz' ? 2 : idx);
                return { modified: false, source: srcInput?.nodename, channel: channelIdx, extractNode: node.name };
            }

            // Check if it's a math op on an extracted channel
            const mathOps = ['multiply', 'add', 'subtract', 'divide', 'power'];
            const isMathOp = mathOps.some(op => node.category?.includes(op));
            if (isMathOp) {
                const mathIn = _getInputInfo(node, 'in1') || _getInputInfo(node, 'in');
                if (mathIn?.nodename) {
                    const mathSrc = nodeMap.get(mathIn.nodename);
                    if (mathSrc?.category?.includes('extract') || mathSrc?.category?.includes('separate')) {
                        const srcInput = _getInputInfo(mathSrc, 'in');
                        return { modified: true, source: srcInput?.nodename, modNode: node.name, extractNode: mathSrc.name };
                    }
                }
            }

            return { modified: true, source: null };
        });

        // Check if all unmodified channels come from the same source
        const unmodifiedSources = channelSources.filter(c => !c.modified && c.source);
        const modifiedChannels = channelSources.filter(c => c.modified && c.source);

        if (unmodifiedSources.length >= 2 && modifiedChannels.length === 1) {
            const commonSource = unmodifiedSources[0].source;
            if (unmodifiedSources.every(c => c.source === commonSource) &&
                modifiedChannels[0].source === commonSource) {
                const modIdx = channelSources.findIndex(c => c.modified);
                const channelNames = ['R', 'G', 'B'];

                patterns.push({
                    type: 'single_channel_modification',
                    outputNode: combineNode.name,
                    inputNode: commonSource,
                    modifiedChannel: channelNames[modIdx],
                    modificationNode: modifiedChannels[0].modNode,
                    nodesToRemove: [], // Don't remove, mark for optimization
                    optimizationHint: `Single channel ${channelNames[modIdx]} modification - could be masked operation`
                });
            }
        }
    }

    return patterns;
}

// ============================================================================
// Math Operation Optimization Patterns
// ============================================================================

/**
 * Detect add-then-subtract or subtract-then-add of same value: a + b - b = a
 */
function _detectAddSubtractInversePattern(nodes, nodeMap) {
    const patterns = [];

    for (const node of nodes) {
        if (!node.category?.includes('subtract') && !node.category?.includes('add')) continue;

        const in1 = _getInputInfo(node, 'in1');
        const in2 = _getInputInfo(node, 'in2');

        if (!in1?.nodename || in2?.type !== 'value') continue;

        const prevNode = nodeMap.get(in1.nodename);
        if (!prevNode) continue;

        // Check for inverse operation
        const isAdd = node.category.includes('add');
        const isSubtract = node.category.includes('subtract');
        const prevIsAdd = prevNode.category?.includes('add');
        const prevIsSubtract = prevNode.category?.includes('subtract');

        if ((isAdd && prevIsSubtract) || (isSubtract && prevIsAdd)) {
            const prevIn1 = _getInputInfo(prevNode, 'in1');
            const prevIn2 = _getInputInfo(prevNode, 'in2');

            if (prevIn1?.type === 'connection' && prevIn2?.type === 'value') {
                // Check if values are equal
                const val1 = in2.value;
                const val2 = prevIn2.value;
                const areEqual = (a, b) => {
                    if (typeof a === 'number' && typeof b === 'number') return Math.abs(a - b) < 0.0001;
                    if (Array.isArray(a) && Array.isArray(b) && a.length === b.length) {
                        return a.every((v, i) => Math.abs(v - b[i]) < 0.0001);
                    }
                    return false;
                };

                if (areEqual(val1, val2)) {
                    patterns.push({
                        type: 'add_subtract_inverse',
                        outputNode: node.name,
                        intermediateNode: prevNode.name,
                        inputNode: prevIn1.nodename,
                        value: val1,
                        nodesToRemove: [node.name, prevNode.name],
                        passthrough: { nodename: prevIn1.nodename, output: prevIn1.output || 'out' }
                    });
                }
            }
        }
    }

    return patterns;
}

/**
 * Detect multiply-then-divide or divide-then-multiply of same value: a * b / b = a
 */
function _detectMultiplyDivideInversePattern(nodes, nodeMap) {
    const patterns = [];

    for (const node of nodes) {
        if (!node.category?.includes('divide') && !node.category?.includes('multiply')) continue;

        const in1 = _getInputInfo(node, 'in1');
        const in2 = _getInputInfo(node, 'in2');

        if (!in1?.nodename || in2?.type !== 'value') continue;

        const prevNode = nodeMap.get(in1.nodename);
        if (!prevNode) continue;

        const isMul = node.category.includes('multiply');
        const isDiv = node.category.includes('divide');
        const prevIsMul = prevNode.category?.includes('multiply');
        const prevIsDiv = prevNode.category?.includes('divide');

        if ((isMul && prevIsDiv) || (isDiv && prevIsMul)) {
            const prevIn1 = _getInputInfo(prevNode, 'in1');
            const prevIn2 = _getInputInfo(prevNode, 'in2');

            if (prevIn1?.type === 'connection' && prevIn2?.type === 'value') {
                const val1 = in2.value;
                const val2 = prevIn2.value;
                const areEqual = (a, b) => {
                    if (typeof a === 'number' && typeof b === 'number') return Math.abs(a - b) < 0.0001;
                    if (Array.isArray(a) && Array.isArray(b) && a.length === b.length) {
                        return a.every((v, i) => Math.abs(v - b[i]) < 0.0001);
                    }
                    return false;
                };

                if (areEqual(val1, val2)) {
                    patterns.push({
                        type: 'multiply_divide_inverse',
                        outputNode: node.name,
                        intermediateNode: prevNode.name,
                        inputNode: prevIn1.nodename,
                        value: val1,
                        nodesToRemove: [node.name, prevNode.name],
                        passthrough: { nodename: prevIn1.nodename, output: prevIn1.output || 'out' }
                    });
                }
            }
        }
    }

    return patterns;
}

/**
 * Detect idempotent operation chains: abs(abs(x)) = abs(x), floor(floor(x)) = floor(x), etc.
 */
function _detectIdempotentChainPattern(nodes, nodeMap) {
    const patterns = [];

    // Idempotent operations: applying twice gives same result as applying once
    const idempotentOps = [
        'absval', 'abs',          // |x| = ||x||
        'floor',                   // floor(floor(x)) = floor(x)
        'ceil',                    // ceil(ceil(x)) = ceil(x)
        'sign',                    // sign(sign(x)) = sign(x)
        'normalize',               // normalize(normalize(x)) = normalize(x) (already have separate)
        'saturate',                // saturate(saturate(x)) = saturate(x)
        'clamp'                    // clamp(clamp(x)) = clamp(x) with same bounds
    ];

    for (const node of nodes) {
        const category = node.category?.replace(/_color3|_float|_vector3/g, '');
        if (!idempotentOps.includes(category)) continue;

        const inInput = _getInputInfo(node, 'in') || _getInputInfo(node, 'in1');
        if (!inInput?.nodename) continue;

        const prevNode = nodeMap.get(inInput.nodename);
        if (!prevNode) continue;

        const prevCategory = prevNode.category?.replace(/_color3|_float|_vector3/g, '');

        if (category === prevCategory) {
            // For clamp, verify bounds are the same
            if (category === 'clamp') {
                const low1 = _getInputInfo(node, 'low');
                const high1 = _getInputInfo(node, 'high');
                const low2 = _getInputInfo(prevNode, 'low');
                const high2 = _getInputInfo(prevNode, 'high');

                const sameVal = (a, b) => {
                    if (a?.type !== b?.type) return false;
                    if (a?.type === 'value') return a.value === b.value;
                    return a?.nodename === b?.nodename;
                };

                if (!sameVal(low1, low2) || !sameVal(high1, high2)) continue;
            }

            const prevIn = _getInputInfo(prevNode, 'in') || _getInputInfo(prevNode, 'in1');
            if (prevIn?.type === 'connection') {
                patterns.push({
                    type: 'idempotent_chain',
                    operation: category,
                    outputNode: node.name,
                    redundantNode: prevNode.name,
                    inputNode: prevIn.nodename,
                    nodesToRemove: [prevNode.name],
                    replacement: {
                        ...node,
                        inputs: node.inputs.map(inp =>
                            (inp.name === 'in' || inp.name === 'in1') ?
                            { ...inp, nodename: prevIn.nodename, output: prevIn.output || 'out' } : inp
                        )
                    }
                });
            }
        }
    }

    return patterns;
}

/**
 * Detect mix with same inputs: mix(a, a, factor) = a
 */
function _detectMixSameInputsPattern(nodes, nodeMap) {
    const patterns = [];

    for (const node of nodes) {
        if (!node.category?.includes('mix')) continue;

        const bg = _getInputInfo(node, 'bg');
        const fg = _getInputInfo(node, 'fg');

        // Both must be connections to the same node
        if (bg?.type !== 'connection' || fg?.type !== 'connection') continue;
        if (bg.nodename !== fg.nodename) continue;
        if ((bg.output || 'out') !== (fg.output || 'out')) continue;

        patterns.push({
            type: 'mix_same_inputs',
            outputNode: node.name,
            inputNode: bg.nodename,
            nodesToRemove: [node.name],
            passthrough: { nodename: bg.nodename, output: bg.output || 'out' }
        });
    }

    return patterns;
}

/**
 * Detect colorspace roundtrip: srgb_to_linear -> linear_to_srgb = identity
 */
function _detectColorspaceRoundtripPattern(nodes, nodeMap) {
    const patterns = [];

    const roundtripPairs = [
        ['srgb_to_linear', 'linear_to_srgb'],
        ['linear_to_srgb', 'srgb_to_linear'],
        ['rgb_to_hsv', 'hsv_to_rgb'],
        ['hsv_to_rgb', 'rgb_to_hsv']
    ];

    for (const node of nodes) {
        const category = node.category?.replace(/_color3|_color4/g, '');

        for (const [first, second] of roundtripPairs) {
            if (category !== second) continue;

            const inInput = _getInputInfo(node, 'in');
            if (!inInput?.nodename) continue;

            const prevNode = nodeMap.get(inInput.nodename);
            const prevCategory = prevNode?.category?.replace(/_color3|_color4/g, '');

            if (prevCategory === first) {
                const origInput = _getInputInfo(prevNode, 'in');
                if (origInput?.type === 'connection') {
                    patterns.push({
                        type: 'colorspace_roundtrip',
                        conversion: `${first} -> ${second}`,
                        outputNode: node.name,
                        intermediateNode: prevNode.name,
                        inputNode: origInput.nodename,
                        nodesToRemove: [node.name, prevNode.name],
                        passthrough: { nodename: origInput.nodename, output: origInput.output || 'out' }
                    });
                }
            }
        }
    }

    return patterns;
}

/**
 * Detect chained normalize pattern: normalize -> normalize
 * Only the last normalize is needed
 */
function _detectChainedNormalizePattern(nodes, nodeMap) {
    const patterns = [];
    for (const node of nodes) {
        if (node.category !== 'normalize_vector3' && node.category !== 'normalize_float') continue;
        const inInput = _getInputInfo(node, 'in');
        if (!inInput || inInput.type !== 'connection') continue;

        const prevNode = nodeMap.get(inInput.nodename);
        if (!prevNode) continue;

        // Check if previous node is also a normalize
        if (prevNode.category === node.category) {
            const prevInput = _getInputInfo(prevNode, 'in');
            if (prevInput?.type === 'connection') {
                patterns.push({
                    type: 'chained_normalize',
                    outputNode: node.name,
                    redundantNode: prevNode.name,
                    inputNode: prevInput.nodename,
                    nodesToRemove: [prevNode.name],
                    replacement: {
                        ...node,
                        inputs: [{ name: 'in', nodename: prevInput.nodename, output: prevInput.output || 'out' }]
                    }
                });
            }
        }
    }
    return patterns;
}

/**
 * Detect luminance + extract pattern: luminance_color3 -> extract_color3(index=0)
 * Extract after luminance is redundant since luminance outputs a scalar-like value
 */
function _detectLuminanceExtractPattern(nodes, nodeMap) {
    const patterns = [];
    for (const extractNode of nodes) {
        if (extractNode.category !== 'extract_color3') continue;
        const inInput = _getInputInfo(extractNode, 'in');
        const indexInput = _getInputInfo(extractNode, 'index');
        if (!inInput || inInput.type !== 'connection') continue;

        // Check if index is 0 (red channel, which contains luminance result)
        if (indexInput?.type === 'value' && indexInput.value === 0) {
            const prevNode = nodeMap.get(inInput.nodename);
            if (prevNode?.category === 'luminance_color3') {
                // The luminance node outputs to all channels equally, extract[0] is redundant
                patterns.push({
                    type: 'luminance_extract',
                    outputNode: extractNode.name,
                    luminanceNode: prevNode.name,
                    nodesToRemove: [], // Don't remove luminance, just bypass extract
                    replacement: null  // Will be handled as passthrough
                });
            }
        }
    }
    return patterns;
}

/**
 * Detect convert color3<->vector3 roundtrip: convert_color3_vector3 -> convert_vector3_color3
 * This is a no-op and can be removed
 */
function _detectConvertRoundtripPattern(nodes, nodeMap) {
    const patterns = [];
    for (const node of nodes) {
        if (node.category !== 'convert_vector3' && node.type !== 'ND_convert_vector3_color3') continue;
        const inInput = _getInputInfo(node, 'in');
        if (!inInput || inInput.type !== 'connection') continue;

        const prevNode = nodeMap.get(inInput.nodename);
        if (!prevNode) continue;

        // Check if previous is color3 to vector3 conversion
        if (prevNode.category === 'convert_color3' || prevNode.type === 'ND_convert_color3_vector3') {
            const origInput = _getInputInfo(prevNode, 'in');
            if (origInput?.type === 'connection') {
                patterns.push({
                    type: 'convert_roundtrip',
                    outputNode: node.name,
                    convertNode: prevNode.name,
                    inputNode: origInput.nodename,
                    nodesToRemove: [prevNode.name, node.name],
                    passthrough: { nodename: origInput.nodename, output: origInput.output || 'out' }
                });
            }
        }
    }
    return patterns;
}

/**
 * Detect separate + combine passthrough pattern
 * If extract_color3 feeds directly into combine3_color3 in RGB order, it's a no-op
 */
function _detectSeparateCombinePassthrough(nodes, nodeMap) {
    const patterns = [];
    for (const combineNode of nodes) {
        if (combineNode.category !== 'combine3_color3') continue;

        const in1 = _getInputInfo(combineNode, 'in1');
        const in2 = _getInputInfo(combineNode, 'in2');
        const in3 = _getInputInfo(combineNode, 'in3');

        // All inputs must be connections
        if (!in1?.nodename || !in2?.nodename || !in3?.nodename) continue;

        const extract1 = nodeMap.get(in1.nodename);
        const extract2 = nodeMap.get(in2.nodename);
        const extract3 = nodeMap.get(in3.nodename);

        // All must be extract_color3
        if (extract1?.category !== 'extract_color3' ||
            extract2?.category !== 'extract_color3' ||
            extract3?.category !== 'extract_color3') continue;

        // All must extract from the same source
        const src1 = _getInputInfo(extract1, 'in');
        const src2 = _getInputInfo(extract2, 'in');
        const src3 = _getInputInfo(extract3, 'in');

        if (!src1?.nodename || src1.nodename !== src2?.nodename || src1.nodename !== src3?.nodename) continue;

        // Check indices are 0, 1, 2 (RGB order)
        const idx1 = _getInputInfo(extract1, 'index');
        const idx2 = _getInputInfo(extract2, 'index');
        const idx3 = _getInputInfo(extract3, 'index');

        if (idx1?.value === 0 && idx2?.value === 1 && idx3?.value === 2) {
            patterns.push({
                type: 'separate_combine_passthrough',
                outputNode: combineNode.name,
                extractNodes: [extract1.name, extract2.name, extract3.name],
                inputNode: src1.nodename,
                nodesToRemove: [extract1.name, extract2.name, extract3.name, combineNode.name],
                passthrough: { nodename: src1.nodename, output: src1.output || 'out' }
            });
        }
    }
    return patterns;
}

/**
 * Detect clamp with default values (0, 1) - common saturation pattern
 */
function _detectClampPattern(nodes, nodeMap) {
    const patterns = [];
    for (const node of nodes) {
        if (node.category !== 'clamp_float' && node.category !== 'clamp_color3') continue;

        const inInput = _getInputInfo(node, 'in');
        const lowInput = _getInputInfo(node, 'low');
        const highInput = _getInputInfo(node, 'high');

        if (!inInput || inInput.type !== 'connection') continue;

        // Check for clamp(x, 0, 1) - saturate pattern
        const isLowZero = lowInput?.type === 'value' && (_isZero(lowInput.value) || _isBlack(lowInput.value));
        const isHighOne = highInput?.type === 'value' && (_isOne(highInput.value) || _isWhite(highInput.value));

        if (isLowZero && isHighOne) {
            patterns.push({
                type: 'saturate',
                outputNode: node.name,
                inputNode: inInput.nodename,
                nodesToRemove: [],
                replacement: {
                    name: node.name,
                    category: node.category === 'clamp_color3' ? 'saturate_color3' : 'saturate_float',
                    type: node.category === 'clamp_color3' ? 'ND_saturate_color3' : 'ND_saturate_float',
                    inputs: [{ name: 'in', nodename: inInput.nodename, output: 'out' }]
                }
            });
        }
    }
    return patterns;
}

/**
 * Detect geometry normal/tangent setup chain
 * Blender exports: normal -> normalize -> tangent -> normalize -> rotate3d -> normalize
 * This common pattern can be marked as a geometry setup block
 */
function _detectGeometrySetupPattern(nodes, nodeMap) {
    const patterns = [];

    // Find normal_vector3 nodes as starting points
    for (const normalNode of nodes) {
        if (normalNode.category !== 'normal_vector3') continue;

        // Look for the chain: normal -> normalize
        const normalRefs = nodes.filter(n =>
            n.category === 'normalize_vector3' &&
            _getInputInfo(n, 'in')?.nodename === normalNode.name
        );

        if (normalRefs.length === 0) continue;

        // Found a geometry setup - mark the chain for potential optimization
        // For now, we just detect it but don't modify (it's a useful pattern to preserve)
        patterns.push({
            type: 'geometry_setup',
            normalNode: normalNode.name,
            normalizeNodes: normalRefs.map(n => n.name),
            nodesToRemove: [], // Don't remove, just mark
            replacement: null
        });
    }

    return patterns;
}

/**
 * Detect chained inverse operations that cancel out
 * Examples: invert->invert, gamma(g)->gamma(1/g), negate->negate
 * Can detect chains up to specified depth (default 16)
 */
function _detectInverseChainPattern(nodes, nodeMap, maxDepth = 16) {
    const patterns = [];
    const processed = new Set();

    // Helper to check if two values are multiplicative inverses (a * b ≈ 1)
    const areInverses = (a, b) => {
        if (typeof a === 'number' && typeof b === 'number') {
            return Math.abs(a * b - 1) < 0.0001;
        }
        if (Array.isArray(a) && Array.isArray(b) && a.length === b.length) {
            return a.every((v, i) => Math.abs(v * b[i] - 1) < 0.0001);
        }
        return false;
    };

    // Invert pattern: subtract(1, x) where the pattern chains
    // Blender's invert is: constant(1,1,1) -> subtract(constant, input) -> mix
    // Double invert should cancel out
    for (const node of nodes) {
        if (processed.has(node.name)) continue;

        // Detect invert chain: mix nodes with factor=1 that feed into each other
        if (node.category === 'mix_color3' || node.category === 'mix_float') {
            const mixInput = _getInputInfo(node, 'mix');
            const fgInput = _getInputInfo(node, 'fg');

            // Check if this is an invert (mix factor = 1, fg comes from subtract)
            if (mixInput?.type === 'value' && _isOne(mixInput.value) && fgInput?.type === 'connection') {
                const fgNode = nodeMap.get(fgInput.nodename);
                if (fgNode?.category === 'subtract_color3' || fgNode?.category === 'subtract_float') {
                    // This is an invert pattern, check if input is also an invert
                    const subIn2 = _getInputInfo(fgNode, 'in2');
                    if (subIn2?.type === 'connection') {
                        const inputMix = nodeMap.get(subIn2.nodename);
                        if (inputMix?.category === node.category) {
                            const inputMixFactor = _getInputInfo(inputMix, 'mix');
                            const inputFg = _getInputInfo(inputMix, 'fg');
                            if (inputMixFactor?.type === 'value' && _isOne(inputMixFactor.value) && inputFg?.type === 'connection') {
                                const inputFgNode = nodeMap.get(inputFg.nodename);
                                if (inputFgNode?.category === fgNode.category) {
                                    // Double invert found! Trace back to original input
                                    const innerSubIn2 = _getInputInfo(inputFgNode, 'in2');
                                    if (innerSubIn2?.type === 'connection') {
                                        patterns.push({
                                            type: 'double_invert',
                                            outputNode: node.name,
                                            chainNodes: [fgNode.name, inputMix.name, inputFgNode.name],
                                            inputNode: innerSubIn2.nodename,
                                            nodesToRemove: [node.name, fgNode.name, inputMix.name, inputFgNode.name],
                                            passthrough: { nodename: innerSubIn2.nodename, output: innerSubIn2.output || 'out' }
                                        });
                                        processed.add(node.name);
                                        processed.add(fgNode.name);
                                        processed.add(inputMix.name);
                                        processed.add(inputFgNode.name);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Detect gamma chain: power(x, g) -> power(x, 1/g) = identity
        if (node.category === 'power_color3' || node.category === 'power_float') {
            const in1 = _getInputInfo(node, 'in1');
            const in2 = _getInputInfo(node, 'in2');

            if (in1?.type === 'connection' && in2?.type === 'value') {
                const prevNode = nodeMap.get(in1.nodename);
                if (prevNode?.category === node.category) {
                    const prevIn1 = _getInputInfo(prevNode, 'in1');
                    const prevIn2 = _getInputInfo(prevNode, 'in2');

                    if (prevIn1?.type === 'connection' && prevIn2?.type === 'value') {
                        // Check if gamma values are inverses
                        if (areInverses(in2.value, prevIn2.value)) {
                            patterns.push({
                                type: 'gamma_inverse_chain',
                                outputNode: node.name,
                                chainNodes: [prevNode.name],
                                inputNode: prevIn1.nodename,
                                gamma1: prevIn2.value,
                                gamma2: in2.value,
                                nodesToRemove: [node.name, prevNode.name],
                                passthrough: { nodename: prevIn1.nodename, output: prevIn1.output || 'out' }
                            });
                            processed.add(node.name);
                            processed.add(prevNode.name);
                        }
                    }
                }
            }
        }

        // Detect negate chain: negate -> negate = identity (multiply by -1 twice)
        if (node.category === 'multiply_color3' || node.category === 'multiply_float') {
            const in1 = _getInputInfo(node, 'in1');
            const in2 = _getInputInfo(node, 'in2');

            // Check if this is a negate (multiply by -1)
            const isNegate = (in2?.type === 'value' && (in2.value === -1 || (Array.isArray(in2.value) && in2.value.every(v => v === -1))));

            if (in1?.type === 'connection' && isNegate) {
                const prevNode = nodeMap.get(in1.nodename);
                if (prevNode?.category === node.category) {
                    const prevIn1 = _getInputInfo(prevNode, 'in1');
                    const prevIn2 = _getInputInfo(prevNode, 'in2');
                    const prevIsNegate = (prevIn2?.type === 'value' && (prevIn2.value === -1 || (Array.isArray(prevIn2.value) && prevIn2.value.every(v => v === -1))));

                    if (prevIn1?.type === 'connection' && prevIsNegate) {
                        patterns.push({
                            type: 'double_negate',
                            outputNode: node.name,
                            chainNodes: [prevNode.name],
                            inputNode: prevIn1.nodename,
                            nodesToRemove: [node.name, prevNode.name],
                            passthrough: { nodename: prevIn1.nodename, output: prevIn1.output || 'out' }
                        });
                        processed.add(node.name);
                        processed.add(prevNode.name);
                    }
                }
            }
        }
    }

    // Detect longer chains (up to maxDepth)
    // Walk through nodes and find chains of the same operation type
    const chainableOps = ['multiply', 'add', 'power', 'mix'];

    for (const startNode of nodes) {
        if (processed.has(startNode.name)) continue;

        const category = startNode.category?.replace(/_color3|_float|_vector3/g, '');
        if (!chainableOps.includes(category)) continue;

        // Build chain by following connections
        let chain = [startNode];
        let current = startNode;
        let depth = 0;

        while (depth < maxDepth) {
            const in1 = _getInputInfo(current, 'in1') || _getInputInfo(current, 'in') || _getInputInfo(current, 'fg');
            if (!in1 || in1.type !== 'connection') break;

            const prevNode = nodeMap.get(in1.nodename);
            if (!prevNode) break;

            const prevCategory = prevNode.category?.replace(/_color3|_float|_vector3/g, '');
            if (prevCategory !== category) break;

            chain.push(prevNode);
            current = prevNode;
            depth++;
        }

        // Analyze chain for cancellation patterns
        if (chain.length >= 2) {
            // For multiply chains, check if product of all factors = 1
            if (category === 'multiply') {
                let product = 1;
                let hasNonConnection = false;
                let firstInput = null;

                for (let i = 0; i < chain.length; i++) {
                    const node = chain[i];
                    const in2 = _getInputInfo(node, 'in2');
                    if (in2?.type === 'value') {
                        const val = Array.isArray(in2.value) ? in2.value[0] : in2.value;
                        product *= val;
                    } else {
                        hasNonConnection = true;
                        break;
                    }

                    if (i === chain.length - 1) {
                        const in1 = _getInputInfo(node, 'in1');
                        if (in1?.type === 'connection') {
                            firstInput = in1;
                        }
                    }
                }

                if (!hasNonConnection && Math.abs(product - 1) < 0.0001 && firstInput) {
                    patterns.push({
                        type: 'multiply_chain_identity',
                        outputNode: startNode.name,
                        chainLength: chain.length,
                        chainNodes: chain.map(n => n.name),
                        product: product,
                        nodesToRemove: chain.map(n => n.name),
                        passthrough: { nodename: firstInput.nodename, output: firstInput.output || 'out' }
                    });
                    chain.forEach(n => processed.add(n.name));
                }
            }

            // For add chains, check if sum of all addends = 0
            if (category === 'add') {
                let sum = 0;
                let hasNonConnection = false;
                let firstInput = null;

                for (let i = 0; i < chain.length; i++) {
                    const node = chain[i];
                    const in2 = _getInputInfo(node, 'in2');
                    if (in2?.type === 'value') {
                        const val = Array.isArray(in2.value) ? in2.value[0] : in2.value;
                        sum += val;
                    } else {
                        hasNonConnection = true;
                        break;
                    }

                    if (i === chain.length - 1) {
                        const in1 = _getInputInfo(node, 'in1');
                        if (in1?.type === 'connection') {
                            firstInput = in1;
                        }
                    }
                }

                if (!hasNonConnection && Math.abs(sum) < 0.0001 && firstInput) {
                    patterns.push({
                        type: 'add_chain_identity',
                        outputNode: startNode.name,
                        chainLength: chain.length,
                        chainNodes: chain.map(n => n.name),
                        sum: sum,
                        nodesToRemove: chain.map(n => n.name),
                        passthrough: { nodename: firstInput.nodename, output: firstInput.output || 'out' }
                    });
                    chain.forEach(n => processed.add(n.name));
                }
            }
        }
    }

    return patterns;
}

/**
 * Detect separate->combine passthrough chains at any depth
 * separate_color3 -> ... -> combine_color3 where channels reconnect = identity
 */
function _detectSeparateCombineChain(nodes, nodeMap, maxDepth = 16) {
    const patterns = [];

    for (const combineNode of nodes) {
        if (combineNode.category !== 'combine3_color3' && combineNode.category !== 'combine3_vector3') continue;

        const in1 = _getInputInfo(combineNode, 'in1');
        const in2 = _getInputInfo(combineNode, 'in2');
        const in3 = _getInputInfo(combineNode, 'in3');

        if (!in1 || !in2 || !in3) continue;
        if (in1.type !== 'connection' || in2.type !== 'connection' || in3.type !== 'connection') continue;

        // Trace each input back to find if they all come from the same separate node
        const traceToSeparate = (nodename, output, depth = 0) => {
            if (depth > maxDepth) return null;

            const node = nodeMap.get(nodename);
            if (!node) return null;

            // Check if this is a separate/extract node
            if (node.category === 'separate3_color3' || node.category === 'separate3_vector3' ||
                node.category === 'extract_color3' || node.category === 'extract_vector3') {
                return { node: node, output: output, depth: depth };
            }

            // Follow single-input nodes (passthrough-like)
            const inInput = _getInputInfo(node, 'in') || _getInputInfo(node, 'in1');
            if (inInput?.type === 'connection') {
                return traceToSeparate(inInput.nodename, inInput.output || 'out', depth + 1);
            }

            return null;
        };

        const source1 = traceToSeparate(in1.nodename, in1.output || 'out');
        const source2 = traceToSeparate(in2.nodename, in2.output || 'out');
        const source3 = traceToSeparate(in3.nodename, in3.output || 'out');

        // Check if all three come from the same separate node with correct outputs
        if (source1 && source2 && source3 &&
            source1.node.name === source2.node.name &&
            source2.node.name === source3.node.name) {

            const separateNode = source1.node;
            const sepInput = _getInputInfo(separateNode, 'in');

            // Verify outputs are outx/outy/outz or out (with index)
            const outputs = [source1.output, source2.output, source3.output].sort();
            const expectedOutputs = ['outx', 'outy', 'outz'];
            const isCorrectOrder = outputs[0] === 'outx' && outputs[1] === 'outy' && outputs[2] === 'outz';

            if (sepInput?.type === 'connection' && isCorrectOrder) {
                // Collect all intermediate nodes
                const collectIntermediateNodes = (startNodename, endNodename) => {
                    const nodes = [];
                    let current = startNodename;
                    while (current && current !== endNodename) {
                        const node = nodeMap.get(current);
                        if (!node) break;
                        nodes.push(current);
                        const inInput = _getInputInfo(node, 'in') || _getInputInfo(node, 'in1');
                        if (!inInput?.nodename) break;
                        current = inInput.nodename;
                    }
                    return nodes;
                };

                const intermediateNodes = new Set([
                    ...collectIntermediateNodes(in1.nodename, separateNode.name),
                    ...collectIntermediateNodes(in2.nodename, separateNode.name),
                    ...collectIntermediateNodes(in3.nodename, separateNode.name)
                ]);

                patterns.push({
                    type: 'separate_combine_chain',
                    outputNode: combineNode.name,
                    separateNode: separateNode.name,
                    inputNode: sepInput.nodename,
                    chainDepth: Math.max(source1.depth, source2.depth, source3.depth),
                    intermediateNodes: [...intermediateNodes],
                    nodesToRemove: [combineNode.name, separateNode.name, ...intermediateNodes],
                    passthrough: { nodename: sepInput.nodename, output: sepInput.output || 'out' }
                });
            }
        }
    }

    return patterns;
}

/**
 * Detect identity operations (multiply by 1, add 0, etc.)
 */
function _detectIdentityOps(nodes, nodeMap) {
    const identities = [];
    for (const node of nodes) {
        let isIdentity = false;
        let inputConnection = null;

        if (node.category === 'multiply_color3' || node.category === 'multiply_float') {
            const in1 = _getInputInfo(node, 'in1');
            const in2 = _getInputInfo(node, 'in2');
            if (in1?.type === 'connection' && in2?.type === 'value' && (_isOne(in2.value) || _isWhite(in2.value))) {
                isIdentity = true; inputConnection = in1;
            } else if (in2?.type === 'connection' && in1?.type === 'value' && (_isOne(in1.value) || _isWhite(in1.value))) {
                isIdentity = true; inputConnection = in2;
            }
        }
        if (node.category === 'add_color3' || node.category === 'add_float') {
            const in1 = _getInputInfo(node, 'in1');
            const in2 = _getInputInfo(node, 'in2');
            if (in1?.type === 'connection' && in2?.type === 'value' && (_isZero(in2.value) || _isBlack(in2.value))) {
                isIdentity = true; inputConnection = in1;
            } else if (in2?.type === 'connection' && in1?.type === 'value' && (_isZero(in1.value) || _isBlack(in1.value))) {
                isIdentity = true; inputConnection = in2;
            }
        }
        if (node.category === 'subtract_color3' || node.category === 'subtract_float') {
            const in1 = _getInputInfo(node, 'in1');
            const in2 = _getInputInfo(node, 'in2');
            if (in1?.type === 'connection' && in2?.type === 'value' && (_isZero(in2.value) || _isBlack(in2.value))) {
                isIdentity = true; inputConnection = in1;
            }
        }
        if (node.category === 'power_color3' || node.category === 'power_float') {
            const in1 = _getInputInfo(node, 'in1');
            const in2 = _getInputInfo(node, 'in2');
            if (in1?.type === 'connection' && in2?.type === 'value' && (_isOne(in2.value) || _isWhite(in2.value))) {
                isIdentity = true; inputConnection = in1;
            }
        }
        if (node.category === 'mix_color3' || node.category === 'mix_float') {
            const mixInput = _getInputInfo(node, 'mix');
            const bgInput = _getInputInfo(node, 'bg');
            const fgInput = _getInputInfo(node, 'fg');
            // mix=0: use bg only
            if (mixInput?.type === 'value' && _isZero(mixInput.value) && bgInput?.type === 'connection') {
                isIdentity = true; inputConnection = bgInput;
            }
            // mix=1: use fg only
            else if (mixInput?.type === 'value' && _isOne(mixInput.value) && fgInput?.type === 'connection') {
                isIdentity = true; inputConnection = fgInput;
            }
        }
        // divide by 1
        if (node.category === 'divide_color3' || node.category === 'divide_float') {
            const in1 = _getInputInfo(node, 'in1');
            const in2 = _getInputInfo(node, 'in2');
            if (in1?.type === 'connection' && in2?.type === 'value' && (_isOne(in2.value) || _isWhite(in2.value))) {
                isIdentity = true; inputConnection = in1;
            }
        }
        // clamp where input is already in range (rare but possible)
        // rotate3d with amount=0
        if (node.category === 'rotate3d_vector3') {
            const amount = _getInputInfo(node, 'amount');
            const inInput = _getInputInfo(node, 'in');
            if (amount?.type === 'value' && _isZero(amount.value) && inInput?.type === 'connection') {
                isIdentity = true; inputConnection = inInput;
            }
        }

        if (isIdentity && inputConnection) {
            identities.push({ node: node.name, passthrough: inputConnection.nodename, output: inputConnection.output || 'out' });
        }
    }
    return identities;
}

// ============================================================================
// Constant Folding (AGGRESSIVE optimization level)
// ============================================================================

/**
 * Evaluate a constant expression for the given operation
 * Returns null if the operation cannot be folded
 */
function _evaluateConstantOp(category, inputs) {
    const getVal = (name) => {
        const input = inputs.find(i => i.name === name);
        return input?.value;
    };

    // Helper for color3/vector3 operations
    const vec3Op = (a, b, op) => {
        if (!Array.isArray(a) || !Array.isArray(b) || a.length !== 3 || b.length !== 3) return null;
        return [op(a[0], b[0]), op(a[1], b[1]), op(a[2], b[2])];
    };

    const vec3Unary = (a, op) => {
        if (!Array.isArray(a) || a.length !== 3) return null;
        return [op(a[0]), op(a[1]), op(a[2])];
    };

    // Binary operations
    if (category === 'add_color3' || category === 'add_vector3') {
        return vec3Op(getVal('in1'), getVal('in2'), (a, b) => a + b);
    }
    if (category === 'add_float') {
        const a = getVal('in1'), b = getVal('in2');
        return (typeof a === 'number' && typeof b === 'number') ? a + b : null;
    }
    if (category === 'subtract_color3' || category === 'subtract_vector3') {
        return vec3Op(getVal('in1'), getVal('in2'), (a, b) => a - b);
    }
    if (category === 'subtract_float') {
        const a = getVal('in1'), b = getVal('in2');
        return (typeof a === 'number' && typeof b === 'number') ? a - b : null;
    }
    if (category === 'multiply_color3' || category === 'multiply_vector3') {
        return vec3Op(getVal('in1'), getVal('in2'), (a, b) => a * b);
    }
    if (category === 'multiply_float') {
        const a = getVal('in1'), b = getVal('in2');
        return (typeof a === 'number' && typeof b === 'number') ? a * b : null;
    }
    if (category === 'divide_color3' || category === 'divide_vector3') {
        const b = getVal('in2');
        if (Array.isArray(b) && b.some(v => v === 0)) return null; // Avoid div by zero
        return vec3Op(getVal('in1'), b, (a, b) => a / b);
    }
    if (category === 'divide_float') {
        const a = getVal('in1'), b = getVal('in2');
        if (b === 0) return null;
        return (typeof a === 'number' && typeof b === 'number') ? a / b : null;
    }
    if (category === 'power_color3' || category === 'power_vector3') {
        return vec3Op(getVal('in1'), getVal('in2'), (a, b) => Math.pow(a, b));
    }
    if (category === 'power_float') {
        const a = getVal('in1'), b = getVal('in2');
        return (typeof a === 'number' && typeof b === 'number') ? Math.pow(a, b) : null;
    }
    if (category === 'min_color3' || category === 'min_vector3') {
        return vec3Op(getVal('in1'), getVal('in2'), Math.min);
    }
    if (category === 'min_float') {
        const a = getVal('in1'), b = getVal('in2');
        return (typeof a === 'number' && typeof b === 'number') ? Math.min(a, b) : null;
    }
    if (category === 'max_color3' || category === 'max_vector3') {
        return vec3Op(getVal('in1'), getVal('in2'), Math.max);
    }
    if (category === 'max_float') {
        const a = getVal('in1'), b = getVal('in2');
        return (typeof a === 'number' && typeof b === 'number') ? Math.max(a, b) : null;
    }

    // Unary operations
    if (category === 'sqrt_float') {
        const v = getVal('in');
        return (typeof v === 'number' && v >= 0) ? Math.sqrt(v) : null;
    }
    if (category === 'sqrt_color3' || category === 'sqrt_vector3') {
        const v = getVal('in');
        if (Array.isArray(v) && v.some(x => x < 0)) return null;
        return vec3Unary(v, Math.sqrt);
    }
    if (category === 'absval_float') {
        const v = getVal('in');
        return (typeof v === 'number') ? Math.abs(v) : null;
    }
    if (category === 'absval_color3' || category === 'absval_vector3') {
        return vec3Unary(getVal('in'), Math.abs);
    }
    if (category === 'sin_float') {
        const v = getVal('in');
        return (typeof v === 'number') ? Math.sin(v) : null;
    }
    if (category === 'cos_float') {
        const v = getVal('in');
        return (typeof v === 'number') ? Math.cos(v) : null;
    }
    if (category === 'floor_float') {
        const v = getVal('in');
        return (typeof v === 'number') ? Math.floor(v) : null;
    }
    if (category === 'floor_color3' || category === 'floor_vector3') {
        return vec3Unary(getVal('in'), Math.floor);
    }
    if (category === 'ceil_float') {
        const v = getVal('in');
        return (typeof v === 'number') ? Math.ceil(v) : null;
    }
    if (category === 'ceil_color3' || category === 'ceil_vector3') {
        return vec3Unary(getVal('in'), Math.ceil);
    }
    if (category === 'round_float') {
        const v = getVal('in');
        return (typeof v === 'number') ? Math.round(v) : null;
    }
    if (category === 'round_color3' || category === 'round_vector3') {
        return vec3Unary(getVal('in'), Math.round);
    }

    // Invert: 1 - x
    if (category === 'invert_color3' || category === 'invert_float') {
        const v = getVal('in');
        const amount = getVal('amount') ?? 1;
        if (category === 'invert_float') {
            if (typeof v !== 'number') return null;
            return v + amount * (1 - 2 * v); // lerp(v, 1-v, amount)
        } else {
            if (!Array.isArray(v) || v.length !== 3) return null;
            if (typeof amount === 'number') {
                return v.map(x => x + amount * (1 - 2 * x));
            }
            return null;
        }
    }

    // Clamp
    if (category === 'clamp_float') {
        const v = getVal('in'), low = getVal('low') ?? 0, high = getVal('high') ?? 1;
        if (typeof v !== 'number' || typeof low !== 'number' || typeof high !== 'number') return null;
        return Math.min(Math.max(v, low), high);
    }
    if (category === 'clamp_color3' || category === 'clamp_vector3') {
        const v = getVal('in'), low = getVal('low') ?? [0, 0, 0], high = getVal('high') ?? [1, 1, 1];
        if (!Array.isArray(v) || !Array.isArray(low) || !Array.isArray(high)) return null;
        return v.map((x, i) => Math.min(Math.max(x, low[i] ?? 0), high[i] ?? 1));
    }

    // Mix/lerp
    if (category === 'mix_float') {
        const bg = getVal('bg'), fg = getVal('fg'), mix = getVal('mix');
        if (typeof bg !== 'number' || typeof fg !== 'number' || typeof mix !== 'number') return null;
        return bg + mix * (fg - bg);
    }
    if (category === 'mix_color3') {
        const bg = getVal('bg'), fg = getVal('fg'), mix = getVal('mix');
        if (!Array.isArray(bg) || !Array.isArray(fg)) return null;
        if (typeof mix === 'number') {
            return bg.map((b, i) => b + mix * (fg[i] - b));
        }
        return null;
    }

    // Extract channel
    if (category === 'extract_color3' || category === 'extract_vector3') {
        const v = getVal('in'), index = getVal('index');
        if (!Array.isArray(v) || typeof index !== 'number' || index < 0 || index > 2) return null;
        return v[Math.floor(index)];
    }

    // Combine channels
    if (category === 'combine3_color3' || category === 'combine3_vector3') {
        const in1 = getVal('in1'), in2 = getVal('in2'), in3 = getVal('in3');
        if (typeof in1 !== 'number' || typeof in2 !== 'number' || typeof in3 !== 'number') return null;
        return [in1, in2, in3];
    }

    // Luminance (rec709)
    if (category === 'luminance_color3') {
        const v = getVal('in');
        if (!Array.isArray(v) || v.length !== 3) return null;
        return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
    }

    // Normalize vector
    if (category === 'normalize_vector3') {
        const v = getVal('in');
        if (!Array.isArray(v) || v.length !== 3) return null;
        const len = Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len === 0) return [0, 0, 0];
        return [v[0] / len, v[1] / len, v[2] / len];
    }

    // Magnitude
    if (category === 'magnitude_vector3') {
        const v = getVal('in');
        if (!Array.isArray(v) || v.length !== 3) return null;
        return Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }

    // Dot product
    if (category === 'dotproduct_vector3') {
        const a = getVal('in1'), b = getVal('in2');
        if (!Array.isArray(a) || !Array.isArray(b) || a.length !== 3 || b.length !== 3) return null;
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    // Cross product
    if (category === 'crossproduct_vector3') {
        const a = getVal('in1'), b = getVal('in2');
        if (!Array.isArray(a) || !Array.isArray(b) || a.length !== 3 || b.length !== 3) return null;
        return [
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]
        ];
    }

    // Type conversions (color3 <-> vector3)
    if (category === 'convert_color3_vector3' || category === 'convert_vector3_color3') {
        return getVal('in'); // Same data, different type interpretation
    }

    return null; // Unknown or unsupported operation
}

/**
 * Detect nodes that can be constant folded (all inputs are constant values)
 * Returns an array of {node, value, type} objects
 */
function _detectConstantFoldable(nodes, nodeMap) {
    const foldable = [];
    const foldedValues = new Map(); // Cache folded values for chained folding

    // Keep iterating until no more nodes can be folded
    let changed = true;
    while (changed) {
        changed = false;

        for (const node of nodes) {
            if (foldedValues.has(node.name)) continue; // Already folded
            if (!node.category || !node.inputs) continue;

            // Check if all inputs are constant (either direct value or already-folded node)
            let allConstant = true;
            const resolvedInputs = [];

            for (const input of node.inputs) {
                if (input.nodename) {
                    // It's a connection - check if the source was already folded
                    if (foldedValues.has(input.nodename)) {
                        resolvedInputs.push({ name: input.name, value: foldedValues.get(input.nodename) });
                    } else {
                        allConstant = false;
                        break;
                    }
                } else if (input.value !== undefined) {
                    resolvedInputs.push({ name: input.name, value: input.value });
                } else {
                    // No value and no connection - might have default, skip for safety
                    allConstant = false;
                    break;
                }
            }

            if (!allConstant) continue;

            // Try to evaluate
            const result = _evaluateConstantOp(node.category, resolvedInputs);
            if (result !== null) {
                foldedValues.set(node.name, result);
                const outputType = _getOutputType(node.category);
                foldable.push({
                    node: node.name,
                    category: node.category,
                    value: result,
                    type: outputType
                });
                changed = true;
            }
        }
    }

    return foldable;
}

/**
 * Get the output type for a node category
 */
function _getOutputType(category) {
    if (category.endsWith('_color3')) return 'color3f';
    if (category.endsWith('_vector3')) return 'vector3f';
    if (category.endsWith('_float') || category === 'luminance_color3' ||
        category === 'magnitude_vector3' || category === 'dotproduct_vector3' ||
        category.startsWith('extract_')) {
        return 'float';
    }
    if (category.startsWith('combine3_')) {
        return category.includes('color') ? 'color3f' : 'vector3f';
    }
    return 'float'; // Default
}

/**
 * Apply constant folding optimizations to the node graph
 */
function _applyConstantFolding(nodeGraph, foldedNodes) {
    if (foldedNodes.length === 0) return nodeGraph;

    const ng = nodeGraph.nodegraph;
    const nodes = [...ng.nodes];
    const foldedMap = new Map(foldedNodes.map(f => [f.node, f]));

    // Replace folded nodes with constant nodes, rewire connections
    const newNodes = [];
    const constantReplacements = new Map();

    for (const node of nodes) {
        if (foldedMap.has(node.name)) {
            const folded = foldedMap.get(node.name);
            // Replace with constant node
            const constantNode = {
                name: node.name,
                category: `constant_${folded.type === 'float' ? 'float' : folded.type.replace('f', '')}`,
                type: `ND_constant_${folded.type === 'float' ? 'float' : folded.type.replace('f', '')}`,
                inputs: [{ name: 'value', type: folded.type, value: folded.value }]
            };
            newNodes.push(constantNode);
            constantReplacements.set(node.name, folded.value);
        } else {
            // Keep original node, but update inputs if they reference folded nodes
            const updatedNode = { ...node };
            if (node.inputs) {
                updatedNode.inputs = node.inputs.map(input => {
                    if (input.nodename && foldedMap.has(input.nodename)) {
                        // Replace connection with direct value
                        const folded = foldedMap.get(input.nodename);
                        return { name: input.name, type: folded.type, value: folded.value };
                    }
                    return input;
                });
            }
            newNodes.push(updatedNode);
        }
    }

    return {
        ...nodeGraph,
        nodegraph: { ...ng, nodes: newNodes }
    };
}

/**
 * Apply pattern optimizations to a node graph
 */
function _applyPatternOptimizations(nodeGraph, patterns, identities) {
    if (!nodeGraph?.nodegraph) return nodeGraph;

    const ng = nodeGraph.nodegraph;
    const nodes = [...ng.nodes];

    const nodesToRemove = new Set();
    const replacements = new Map();
    const passthroughMap = new Map();

    // Process patterns
    for (const pattern of patterns) {
        // Add nodes to remove
        if (pattern.nodesToRemove) {
            for (const name of pattern.nodesToRemove) nodesToRemove.add(name);
        }
        // Add replacement if exists
        if (pattern.replacement) {
            replacements.set(pattern.outputNode, pattern.replacement);
        }
        // Handle passthrough patterns (like convert roundtrip, separate+combine)
        if (pattern.passthrough) {
            passthroughMap.set(pattern.outputNode, pattern.passthrough);
            nodesToRemove.add(pattern.outputNode);
        }
    }

    // Process identity operations (skip nodes already handled by pattern replacements)
    for (const identity of identities) {
        if (replacements.has(identity.node)) continue;
        if (nodesToRemove.has(identity.passthrough) && !replacements.has(identity.passthrough)) continue;
        passthroughMap.set(identity.node, { nodename: identity.passthrough, output: identity.output });
        nodesToRemove.add(identity.node);
    }

    // Resolve chained passthroughs (A->B->C becomes A->C)
    let changed = true;
    while (changed) {
        changed = false;
        for (const [node, target] of passthroughMap) {
            if (passthroughMap.has(target.nodename)) {
                const finalTarget = passthroughMap.get(target.nodename);
                passthroughMap.set(node, finalTarget);
                changed = true;
            }
        }
    }

    const newNodes = [];
    for (const node of nodes) {
        if (nodesToRemove.has(node.name) && !replacements.has(node.name)) continue;

        let newNode = replacements.get(node.name) || { ...node };
        if (newNode && newNode.inputs) {
            newNode.inputs = newNode.inputs.map(input => {
                if (input.nodename && passthroughMap.has(input.nodename)) {
                    const pt = passthroughMap.get(input.nodename);
                    return { ...input, nodename: pt.nodename, output: pt.output };
                }
                return input;
            });
        }
        if (newNode) newNodes.push(newNode);
    }

    const newOutputs = (ng.outputs || []).map(output => {
        if (output.nodename && passthroughMap.has(output.nodename)) {
            const pt = passthroughMap.get(output.nodename);
            return { ...output, nodename: pt.nodename, output: pt.output };
        }
        return output;
    });

    return {
        ...nodeGraph,
        nodegraph: { ...ng, nodes: newNodes, outputs: newOutputs },
        optimizationInfo: {
            patternsApplied: patterns.filter(p => p.nodesToRemove?.length > 0 || p.replacement).map(p => p.type),
            identitiesRemoved: identities.length,
            nodesRemoved: nodesToRemove.size,
            originalNodeCount: nodes.length,
            optimizedNodeCount: newNodes.length
        }
    };
}

/**
 * Dead-code elimination: mark nodes as active/inactive based on reachability from outputs.
 * Walks backward from nodegraph outputs that are actually connected to material parameters.
 * Outputs not referenced in the connections array are considered dead.
 * Nodes not reachable from any connected output are marked inactive (dead code).
 *
 * @param {Object} nodeGraph - The nodeGraph JSON object
 * @returns {Object} nodeGraph with _activeNodes Set and nodes annotated with _active flag
 */
function markActiveNodes(nodeGraph) {
    if (!nodeGraph?.nodegraph) return nodeGraph;

    const ng = nodeGraph.nodegraph;
    const nodes = ng.nodes || [];
    const outputs = ng.outputs || [];
    const connections = nodeGraph.connections || [];

    // Build name→node map for fast lookup
    const nodeMap = new Map();
    for (const node of nodes) {
        nodeMap.set(node.name, node);
    }

    // Only seed from outputs that are actually connected to material parameters
    const connectedOutputNames = new Set(connections.map(c => c.output));

    // Walk backward from connected outputs to find all reachable nodes
    const active = new Set();
    const queue = [];

    for (const output of outputs) {
        if (output.nodename && nodeMap.has(output.nodename) && connectedOutputNames.has(output.name)) {
            queue.push(output.nodename);
        }
    }

    while (queue.length > 0) {
        const name = queue.pop();
        if (active.has(name)) continue;
        active.add(name);

        const node = nodeMap.get(name);
        if (!node?.inputs) continue;

        for (const input of node.inputs) {
            if (input.nodename && nodeMap.has(input.nodename) && !active.has(input.nodename)) {
                queue.push(input.nodename);
            }
        }
    }

    // Annotate nodes with _active flag
    const annotatedNodes = nodes.map(node => ({
        ...node,
        _active: active.has(node.name)
    }));

    return {
        ...nodeGraph,
        nodegraph: { ...ng, nodes: annotatedNodes },
        _activeNodes: active
    };
}

/**
 * Remove inactive (dead) nodes from a nodeGraph.
 * Call markActiveNodes first, then this to strip dead code.
 *
 * @param {Object} nodeGraph - nodeGraph with _active annotations from markActiveNodes
 * @returns {Object} nodeGraph with only active nodes
 */
function removeInactiveNodes(nodeGraph) {
    if (!nodeGraph?.nodegraph) return nodeGraph;

    const ng = nodeGraph.nodegraph;
    const nodes = ng.nodes || [];
    const activeNodes = nodes.filter(n => n._active !== false);
    const removedCount = nodes.length - activeNodes.length;

    return {
        ...nodeGraph,
        nodegraph: { ...ng, nodes: activeNodes },
        _dceInfo: { removedCount, originalCount: nodes.length }
    };
}

/**
 * Optimize a MaterialX NodeGraph by detecting and simplifying common patterns
 *
 * @param {Object} nodeGraph - The nodeGraph JSON object (from material.openPBR.nodeGraph)
 * @param {number} level - Optimization level (use NodeGraphOptimizationLevel)
 * @returns {Object} Optimized nodeGraph with optimizationInfo
 */
function optimizeNodeGraph(nodeGraph, level = NodeGraphOptimizationLevel.STANDARD) {
    if (!nodeGraph?.nodegraph || level === NodeGraphOptimizationLevel.NONE) return nodeGraph;

    const nodes = nodeGraph.nodegraph.nodes || [];
    const nodeMap = _buildNodeMap(nodes);

    let patterns = [];
    let identities = [];

    if (level >= NodeGraphOptimizationLevel.STANDARD) {
        patterns = [
            // Blender-specific patterns
            ..._detectInvertPattern(nodes, nodeMap),
            ..._detectBrightnessContrastPattern(nodes, nodeMap),
            ..._detectHSVAdjustPattern(nodes, nodeMap),
            // Normalize and conversion patterns
            ..._detectChainedNormalizePattern(nodes, nodeMap),
            ..._detectLuminanceExtractPattern(nodes, nodeMap),
            ..._detectConvertRoundtripPattern(nodes, nodeMap),
            ..._detectSeparateCombinePassthrough(nodes, nodeMap),
            ..._detectClampPattern(nodes, nodeMap),
            ..._detectGeometrySetupPattern(nodes, nodeMap),
            // Chain patterns (up to 16 depth)
            ..._detectInverseChainPattern(nodes, nodeMap, 16),
            ..._detectSeparateCombineChain(nodes, nodeMap, 16),
            // Extract channel patterns
            ..._detectSwizzlePattern(nodes, nodeMap),
            ..._detectChannelToGrayscalePattern(nodes, nodeMap),
            ..._detectUnusedExtractPattern(nodes, nodeMap, nodeGraph.nodegraph.outputs),
            ..._detectSingleChannelModPattern(nodes, nodeMap),
            // Math inverse patterns
            ..._detectAddSubtractInversePattern(nodes, nodeMap),
            ..._detectMultiplyDivideInversePattern(nodes, nodeMap),
            // Idempotent and misc patterns
            ..._detectIdempotentChainPattern(nodes, nodeMap),
            ..._detectMixSameInputsPattern(nodes, nodeMap),
            ..._detectColorspaceRoundtripPattern(nodes, nodeMap)
        ];
    }

    if (level >= NodeGraphOptimizationLevel.BASIC) {
        identities = _detectIdentityOps(nodes, nodeMap);
    }

    // Apply pattern optimizations first
    let result = _applyPatternOptimizations(nodeGraph, patterns, identities);

    // Then apply constant folding for AGGRESSIVE level
    if (level >= NodeGraphOptimizationLevel.AGGRESSIVE) {
        const updatedNodes = result.nodegraph.nodes || [];
        const updatedNodeMap = _buildNodeMap(updatedNodes);
        const foldedNodes = _detectConstantFoldable(updatedNodes, updatedNodeMap);

        if (foldedNodes.length > 0) {
            result = _applyConstantFolding(result, foldedNodes);
            // Update optimization info
            result.optimizationInfo = {
                ...result.optimizationInfo,
                constantsFolded: foldedNodes.length,
                foldedOperations: foldedNodes.map(f => f.category)
            };
        }
    }

    // Always mark active/inactive nodes (DCE annotation)
    result = markActiveNodes(result);

    return result;
}

/**
 * Analyze a NodeGraph and return optimization opportunities without applying them
 *
 * @param {Object} nodeGraph - The nodeGraph JSON object
 * @returns {Object} Analysis results with patterns and potential reductions
 */
function analyzeNodeGraph(nodeGraph) {
    if (!nodeGraph?.nodegraph) return { error: 'Invalid node graph' };

    const nodes = nodeGraph.nodegraph.nodes || [];
    const nodeMap = _buildNodeMap(nodes);

    // Detect all patterns
    const invertPatterns = _detectInvertPattern(nodes, nodeMap);
    const bcPatterns = _detectBrightnessContrastPattern(nodes, nodeMap);
    const hsvPatterns = _detectHSVAdjustPattern(nodes, nodeMap);
    const chainedNormalizePatterns = _detectChainedNormalizePattern(nodes, nodeMap);
    const luminanceExtractPatterns = _detectLuminanceExtractPattern(nodes, nodeMap);
    const convertRoundtripPatterns = _detectConvertRoundtripPattern(nodes, nodeMap);
    const separateCombinePatterns = _detectSeparateCombinePassthrough(nodes, nodeMap);
    const clampPatterns = _detectClampPattern(nodes, nodeMap);
    const geometrySetupPatterns = _detectGeometrySetupPattern(nodes, nodeMap);
    const inverseChainPatterns = _detectInverseChainPattern(nodes, nodeMap, 16);
    const separateCombineChainPatterns = _detectSeparateCombineChain(nodes, nodeMap, 16);
    // New extract channel patterns
    const swizzlePatterns = _detectSwizzlePattern(nodes, nodeMap);
    const channelToGrayscalePatterns = _detectChannelToGrayscalePattern(nodes, nodeMap);
    const unusedExtractPatterns = _detectUnusedExtractPattern(nodes, nodeMap, nodeGraph?.nodegraph?.outputs);
    const singleChannelModPatterns = _detectSingleChannelModPattern(nodes, nodeMap);
    // New math patterns
    const addSubtractInversePatterns = _detectAddSubtractInversePattern(nodes, nodeMap);
    const multiplyDivideInversePatterns = _detectMultiplyDivideInversePattern(nodes, nodeMap);
    // New misc patterns
    const idempotentChainPatterns = _detectIdempotentChainPattern(nodes, nodeMap);
    const mixSameInputsPatterns = _detectMixSameInputsPattern(nodes, nodeMap);
    const colorspaceRoundtripPatterns = _detectColorspaceRoundtripPattern(nodes, nodeMap);
    const identities = _detectIdentityOps(nodes, nodeMap);

    const allNodesToRemove = new Set([
        ...invertPatterns.flatMap(p => p.nodesToRemove),
        ...bcPatterns.flatMap(p => p.nodesToRemove),
        ...hsvPatterns.flatMap(p => p.nodesToRemove),
        ...chainedNormalizePatterns.flatMap(p => p.nodesToRemove),
        ...luminanceExtractPatterns.flatMap(p => p.nodesToRemove),
        ...convertRoundtripPatterns.flatMap(p => p.nodesToRemove),
        ...separateCombinePatterns.flatMap(p => p.nodesToRemove),
        ...clampPatterns.flatMap(p => p.nodesToRemove),
        ...geometrySetupPatterns.flatMap(p => p.nodesToRemove),
        ...inverseChainPatterns.flatMap(p => p.nodesToRemove),
        ...separateCombineChainPatterns.flatMap(p => p.nodesToRemove),
        ...swizzlePatterns.flatMap(p => p.nodesToRemove),
        ...channelToGrayscalePatterns.flatMap(p => p.nodesToRemove),
        ...unusedExtractPatterns.flatMap(p => p.nodesToRemove),
        ...singleChannelModPatterns.flatMap(p => p.nodesToRemove),
        ...addSubtractInversePatterns.flatMap(p => p.nodesToRemove),
        ...multiplyDivideInversePatterns.flatMap(p => p.nodesToRemove),
        ...idempotentChainPatterns.flatMap(p => p.nodesToRemove),
        ...mixSameInputsPatterns.flatMap(p => p.nodesToRemove),
        ...colorspaceRoundtripPatterns.flatMap(p => p.nodesToRemove),
        ...identities.map(i => i.node)
    ]);

    const totalPatterns = invertPatterns.length + bcPatterns.length + hsvPatterns.length +
        chainedNormalizePatterns.length + luminanceExtractPatterns.length + convertRoundtripPatterns.length +
        separateCombinePatterns.length + clampPatterns.length + geometrySetupPatterns.length +
        inverseChainPatterns.length + separateCombineChainPatterns.length +
        swizzlePatterns.length + channelToGrayscalePatterns.length + unusedExtractPatterns.length +
        singleChannelModPatterns.length + addSubtractInversePatterns.length + multiplyDivideInversePatterns.length +
        idempotentChainPatterns.length + mixSameInputsPatterns.length + colorspaceRoundtripPatterns.length;

    return {
        totalNodes: nodes.length,
        patterns: {
            // Blender patterns
            invert: invertPatterns.map(p => ({ outputNode: p.outputNode, inputNode: p.inputNode, factor: p.factor })),
            brightnessContrast: bcPatterns.map(p => ({ outputNode: p.outputNode, inputNode: p.inputNode, brightness: p.brightness, contrast: p.contrast })),
            hsvAdjust: hsvPatterns.map(p => ({ outputNode: p.outputNode, inputNode: p.inputNode, hue: p.hue, saturation: p.saturation, value: p.value })),
            // Conversion patterns
            chainedNormalize: chainedNormalizePatterns.map(p => ({ outputNode: p.outputNode, chainLength: p.chainLength })),
            luminanceExtract: luminanceExtractPatterns.map(p => ({ outputNode: p.outputNode, inputNode: p.inputNode })),
            convertRoundtrip: convertRoundtripPatterns.map(p => ({ outputNode: p.outputNode, inputNode: p.inputNode })),
            separateCombine: separateCombinePatterns.map(p => ({ outputNode: p.outputNode, inputNode: p.inputNode })),
            clamp: clampPatterns.map(p => ({ outputNode: p.outputNode, isDefaultRange: p.isDefaultRange })),
            geometrySetup: geometrySetupPatterns.map(p => ({ outputNode: p.outputNode, mergedCount: p.mergedCount })),
            // Chain patterns
            inverseChain: inverseChainPatterns.map(p => ({ type: p.type, outputNode: p.outputNode, chainLength: p.chainNodes?.length || 0 })),
            separateCombineChain: separateCombineChainPatterns.map(p => ({ outputNode: p.outputNode, chainDepth: p.chainDepth })),
            // Extract channel patterns
            swizzle: swizzlePatterns.map(p => ({ outputNode: p.outputNode, swizzle: p.swizzle, hint: p.optimizationHint })),
            channelToGrayscale: channelToGrayscalePatterns.map(p => ({ outputNode: p.outputNode, channel: p.channel })),
            unusedExtract: unusedExtractPatterns.map(p => ({ outputNode: p.outputNode, unusedOutputs: p.unusedOutputs })),
            singleChannelMod: singleChannelModPatterns.map(p => ({ outputNode: p.outputNode, channel: p.modifiedChannel, hint: p.optimizationHint })),
            // Math patterns
            addSubtractInverse: addSubtractInversePatterns.map(p => ({ outputNode: p.outputNode, inputNode: p.inputNode })),
            multiplyDivideInverse: multiplyDivideInversePatterns.map(p => ({ outputNode: p.outputNode, inputNode: p.inputNode })),
            // Misc patterns
            idempotentChain: idempotentChainPatterns.map(p => ({ outputNode: p.outputNode, operation: p.operation })),
            mixSameInputs: mixSameInputsPatterns.map(p => ({ outputNode: p.outputNode, inputNode: p.inputNode })),
            colorspaceRoundtrip: colorspaceRoundtripPatterns.map(p => ({ outputNode: p.outputNode, conversion: p.conversion }))
        },
        identityOps: identities,
        potentialReduction: {
            patternsFound: totalPatterns,
            identitiesFound: identities.length,
            nodesRemovable: allNodesToRemove.size
        },
        // Constant folding analysis (AGGRESSIVE level)
        constantFolding: _detectConstantFoldable(nodes, nodeMap).map(f => ({
            node: f.node,
            operation: f.category,
            foldedValue: f.value
        }))
    };
}

/**
 * Get a human-readable summary of optimization results
 *
 * @param {Object} optimizedGraph - Graph returned from optimizeNodeGraph()
 * @returns {string} Human-readable summary
 */
function getOptimizationSummary(optimizedGraph) {
    if (!optimizedGraph?.optimizationInfo) return 'No optimization info available';
    const info = optimizedGraph.optimizationInfo;
    const lines = [
        `Node count: ${info.originalNodeCount} -> ${info.optimizedNodeCount} (${info.nodesRemoved} removed)`,
        `Patterns applied: ${info.patternsApplied?.length > 0 ? info.patternsApplied.join(', ') : 'none'}`,
        `Identity ops removed: ${info.identitiesRemoved}`
    ];
    if (info.constantsFolded) {
        lines.push(`Constants folded: ${info.constantsFolded} (${info.foldedOperations?.join(', ') || 'none'})`);
    }
    return lines.join('\n');
}

// ============================================================================
// LightUSDOpenPBR Class (Legacy compatibility)
// ============================================================================

/**
 * A minimal OpenPBR material representation for compatibility
 */
class LightUSDOpenPBR {
    constructor(opts = {}) {
        this.baseColor = opts.baseColor !== undefined ? new THREE.Color(opts.baseColor) : new THREE.Color(1, 1, 1);
        this.opacity = opts.opacity !== undefined ? opts.opacity : 1.0;
        this.metallic = opts.metallic !== undefined ? opts.metallic : 0.0;
        this.roughness = opts.roughness !== undefined ? opts.roughness : 0.5;
        this.emissive = opts.emissive !== undefined ? new THREE.Color(opts.emissive) : new THREE.Color(0, 0, 0);
        this.emissiveIntensity = opts.emissiveIntensity !== undefined ? opts.emissiveIntensity : 0.0;

        // Texture map placeholders
        this.baseColorMap = opts.baseColorMap || null;
        this.metallicRoughnessMap = opts.metallicRoughnessMap || null;
        this.normalMap = opts.normalMap || null;
        this.aoMap = opts.aoMap || null;
        this.emissiveMap = opts.emissiveMap || null;

        // UV transform
        this.uvScale = opts.uvScale || new THREE.Vector2(1, 1);
        this.uvOffset = opts.uvOffset || new THREE.Vector2(0, 0);

        this.name = opts.name || '';
    }

    /**
     * Convert to MeshPhysicalMaterial
     */
    toMeshPhysicalMaterial() {
        const material = new THREE.MeshPhysicalMaterial({
            color: this.baseColor,
            metalness: this.metallic,
            roughness: this.roughness,
            emissive: this.emissive,
            emissiveIntensity: this.emissiveIntensity,
            opacity: this.opacity,
            transparent: this.opacity < 1.0,
            map: this.baseColorMap,
            metalnessMap: this.metallicRoughnessMap,
            roughnessMap: this.metallicRoughnessMap,
            normalMap: this.normalMap,
            aoMap: this.aoMap,
            emissiveMap: this.emissiveMap
        });

        material.name = this.name;
        material.userData.openPBR = this;

        return material;
    }
}

// ============================================================================
// Exports
// ============================================================================

export {
    // Material conversion
    LightUSDOpenPBR,
    convertOpenPBRToMeshPhysicalMaterial,
    convertOpenPBRToMeshPhysicalMaterialLoaded,
    loadTextureFromUSD,
    extractValue,
    hasTexture,
    getTextureId,
    createColor,
    extractNormalMapScale,
    extractNormalParam,
    extractNormalMapScaleFromGetter,
    extractNormalParamFromGetter,
    applyOpenPBRNormalMapFromGetter,
    OPENPBR_TO_THREEJS_MAP,
    OPENPBR_TEXTURE_MAP,
    // NodeGraph optimization
    NodeGraphOptimizationLevel,
    optimizeNodeGraph,
    analyzeNodeGraph,
    getOptimizationSummary,
    // DCE (dead-code elimination)
    markActiveNodes,
    removeInactiveNodes
};
