// SPDX-License-Identifier: Apache-2.0
// Canonical texture/material cache keys.
//
// Three JS paths used to carry their own key builders (loader utils, the
// MaterialX texture loader and the three.js RenderScene builder). Keeping the
// signatures here means a texture authored with the same image/sampler/transform
// gets the same identity no matter which path loads it.

/** Color role of a three.js map slot. Color textures need sRGB decoding. */
export function textureColorRole(mapProperty) {
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

/**
 * Stable identity of a texture's authored image, sampler and transform.
 * `usdScene` is a RenderStream-like object exposing getTexture(); a plain
 * texture record is also accepted for callers that already fetched it.
 */
export function textureSignature(textureId, usdScene, textureSignatureCache = null) {
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
  if (textureSignatureCache) textureSignatureCache.set(textureId, signature);
  return signature;
}

/** Loader-side key: color role plus the authored texture signature. */
export function textureCacheKey(
  textureId, usdScene, mapProperty = '', textureSignatureCache = null) {
  return `${textureColorRole(mapProperty)}:${JSON.stringify(
    textureSignature(textureId, usdScene, textureSignatureCache))}`;
}

/** MaterialX-side key for an already-fetched texture record. */
export function textureDataCacheKey(textureId, texData = null) {
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

/** Material identity used by the three.js RenderScene builder. */
export function materialCacheKey(entry) {
  if (Number.isFinite(entry?.materialId) && entry.materialId >= 0) {
    return `id:${entry.materialId}`;
  }
  if (entry?.materialKey) return `key:${entry.materialKey}`;
  return `fallback:${JSON.stringify(entry?.material || {})}:${JSON.stringify(entry?.texturePaths || {})}`;
}
