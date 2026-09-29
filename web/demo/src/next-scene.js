import { LightUSDLoaderUtils } from 'lightusd/LightUSDLoaderUtils.js';
import { buildNextThreeNode, isNextScene } from 'lightusd-next-demo-utils';
import { applyUSDMaterialFeatures } from './usd-material-features.js';

async function hydrateExternalTextures(usd, sourceUrl) {
  if (!sourceUrl || !usd?.archiveEntries) return;
  const missing = [...new Set((usd.textures || [])
    .map((record) => record?.assetPath || record?.uri)
    .filter((path) => path && !/^(data:|blob:)/i.test(path)))]
    .filter((path) => !usd.archiveEntries.has(path.replace(/^\.\//, '')));
  let next = 0;
  const worker = async () => {
    while (next < missing.length) {
      const path = missing[next++];
      try {
        const response = await fetch(new URL(path, new URL(sourceUrl, document.baseURI)));
        if (!response.ok || /text\/html|application\/json/i.test(response.headers.get('content-type') || '')) continue;
        usd.archiveEntries.set(path.replace(/^\.\//, ''), new Uint8Array(await response.arrayBuffer()));
      } catch {
        // A missing optional texture does not prevent geometry from loading.
      }
    }
  };
  await Promise.all(Array.from({ length: Math.min(4, missing.length) }, worker));
}

// Standalone demos share the same next scene path as demo-foundation.
export async function buildNextDemoScene(usd, {
  onProgress = null,
  envMap = null,
  envMapIntensity = 1,
  releaseBuildData = false,
  awaitTextures = true,
  sourceUrl = null,
} = {}) {
  if (!isNextScene(usd)) {
    throw new Error('The demo requires a next RenderStream scene.');
  }
  const hydration = hydrateExternalTextures(usd, sourceUrl);
  const built = buildNextThreeNode(usd, {
    skipTextures: false,
    lazyTextures: true,
    releaseBuildData,
    onProgress,
  });
  if (built.textureManager) {
    const textureLoad = hydration.then(() => {
      if (built.textureManager.aborted) return null;
      return built.textureManager.startLoading({
        concurrency: LightUSDLoaderUtils.defaultTextureConcurrency(),
        onTextureLoaded: (material, _texture, task) => {
          for (const binding of task?.bindings || []) {
            if (binding.material === material) {
              applyUSDMaterialFeatures(material, binding.mapProperty);
            }
          }
        },
        onProgress,
      });
    });
    if (awaitTextures) await textureLoad;
    else textureLoad.catch((error) => console.warn('Texture loading failed:', error));
  } else if (awaitTextures) await hydration;
  else hydration.catch((error) => console.warn('Texture hydration failed:', error));
  built.node.traverse((object) => {
    const materials = object.material
      ? (Array.isArray(object.material) ? object.material : [object.material])
      : [];
    for (const material of materials) {
      applyUSDMaterialFeatures(material);
      if (envMap && 'envMap' in material) material.envMap = envMap;
      if ('envMapIntensity' in material) material.envMapIntensity = envMapIntensity;
      material.needsUpdate = true;
    }
  });
  return built;
}
