// SPDX-License-Identifier: Apache-2.0
// Core no longer implements the render API itself. Resolve it once, at module
// creation, from the independently linked Tydra feature. The classes and every
// object they own live in the feature's heap; only JS handles cross here.
{
  const tydra = await Module.loadFeature('tydra');
  Module.RenderStream = tydra.RenderStream;
  Module.FlattenSession = tydra.FlattenSession;
  // AssetStore and LayerDocument must share the RenderStream heap: objects are
  // passed between them (beginFromLayerDocument / importAssetStore).
  Module.AssetStore = tydra.AssetStore;
  Module.LayerDocument = tydra.LayerDocument;

  // Explicit C-hook forwarding. Instrumentation and tests patch these symbols
  // on the core module; the calls now happen in the feature module, so get/set
  // accessors route both reads and patches to the module that owns the objects.
  for (const key of Object.keys(tydra)) {
    if (!/^_lightusd_wasm_(render|asset_store|flatten)/.test(key)) continue;
    Object.defineProperty(Module, key, {
      configurable: true,
      get: () => tydra[key],
      set: (value) => { tydra[key] = value; }
    });
  }

  // RenderStream's EXR path uses the feature's own bridge. Loading EXR via
  // core must therefore install it in both modules (idempotent per module).
  const coreLoadFeature = Module.loadFeature;
  const mirrored = new Map();
  Module.loadFeature = (name) => {
    if (name !== 'exr' && name !== 'validator') return coreLoadFeature(name);
    // Keep promise identity: callers dedupe on the very same promise object.
    let pending = mirrored.get(name);
    if (!pending) {
      pending = (async () => {
        const feature = await coreLoadFeature(name);
        await tydra.loadFeature(name);
        return feature;
      })();
      mirrored.set(name, pending);
      pending.catch(() => { if (mirrored.get(name) === pending) mirrored.delete(name); });
    }
    return pending;
  };
}
