// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// Internal post-JS bridge for next's C dispatch. Object IDs and emval IDs are
// uint32 on both wasm32 and memory64; no native pointer is exposed to JS.
{
  const live = new WeakMap();
  function invoke(handle, method, args) {
    const input = Emval.toHandle(args);
    try {
      const output = Module['_lightusd_next_call'](handle, method, input) >>> 0;
      if (!output) throw new TypeError('Invalid or stale LightUSD handle/method');
      try { return Emval.toValue(output); }
      finally { Module['_lightusd_next_val_release'](output); }
    } finally {
      Module['_lightusd_next_val_release'](input);
    }
  }
  function defineClass(name, kind, methods) {
    const Native = class {
      constructor() {
        const handle = Module['_lightusd_next_create'](kind) >>> 0;
        if (!handle) throw new Error('Unable to allocate LightUSD object');
        live.set(this, {handle, busy: 0, kind});
      }
      delete() {
        const state = live.get(this);
        if (!state || !state.handle) throw new TypeError('LightUSD object already deleted');
        if (state.busy) throw new TypeError('Cannot delete an active LightUSD object');
        Module['_lightusd_next_destroy'](state.handle);
        state.handle = 0;
      }
      isDeleted() { return !live.get(this)?.handle; }
    };
    Object.defineProperty(Native, 'name', {value: name});
    for (const [method, id, types] of methods) {
      Object.defineProperty(Native.prototype, method, {value: function(...args) {
        const state = live.get(this);
        if (!state?.handle || state.kind !== kind) throw new TypeError('Invalid LightUSD receiver');
        if (args.length !== types.length) throw new TypeError(method + ': wrong argument count');
        for (let i = 0; i < types.length; ++i) {
          if (types[i] === 's' && typeof args[i] !== 'string') throw new TypeError(method + ': expected string');
          if (types[i] === 'n' && typeof args[i] !== 'number') throw new TypeError(method + ': expected number');
          if (types[i] === 'b') args[i] = !!args[i];
        }
        ++state.busy;
        try { return invoke(state.handle, id, args); }
        finally { --state.busy; }
      }});
    }
    Module[name] = Native;
  }
  Module['usddiff'] = (options) => invoke(0, 1, [options]);
  Module['validateFromBinary'] = (bytes, filename, options) =>
      invoke(0, 2, [bytes, filename, options]);
  defineClass('NextUSDZConverterNative', 1, [
    ['rewriteRoot', 3, 'vsv'],
    ['clearURDFMeshBuffers', 4, ''],
    ['setVisualMesh', 5, 'svvvv'],
    ['setCollisionMesh', 6, 'svvvv'],
    ['createURDFPhysicsScene', 7, 's'],
    ['loadFromBinary', 8, 'vs'],
    ['extractPhysicsSceneJSON', 9, ''],
    ['setAsset', 10, 'sv'],
    ['setUSDCExportLimitMB', 11, 'nn'],
    ['exportAsUSDA', 12, ''],
    ['exportAsUSDC', 13, ''],
    ['exportAsUSDZ', 14, ''],
    ['error', 15, ''],
    ['warn', 16, ''],
  ]);
  defineClass('SubdivStreamer', 2, [
    ['refineStream', 17, 'vvvvvnnnnnnnbv'],
    ['heapBytes', 18, ''],
  ]);
  defineClass('NextFlattenSession', 3, [
    ['begin', 19, 'vsb'],
    ['setVariantOverride', 20, 'ss'],
    ['provideLayer', 21, 'sv'],
    ['step', 22, 'v'],
    ['end', 23, ''],
  ]);
  defineClass('RenderStream', 4, [
    ['setMaterialDedup', 24, 'b'],
    ['setMeshMerge', 25, 'b'],
    ['setMeshMergeBakeTransform', 26, 'b'],
    ['setFlattenRenderTree', 27, 'b'],
    ['setMeshOnly', 28, 'b'],
    ['setComputeTangents', 29, 'b'],
    ['setRenderSettingsPath', 30, 's'],
    ['setBuildVertexIndices', 31, 'b'],
    ['setTangentMethod', 32, 's'],
    ['provideAsset', 33, 'sv'],
    ['clearAssets', 34, ''],
    ['setVariantOverride', 35, 'ss'],
    ['clearVariantOverrides', 36, ''],
    ['listVariants', 37, ''],
    ['begin', 38, 'v'],
    ['beginOwned', 39, 's'],
    ['meshCount', 40, ''],
    ['numMeshes', 40, ''],
    ['nodeCount', 41, ''],
    ['numNodes', 41, ''],
    ['lightCount', 42, ''],
    ['numLights', 42, ''],
    ['pointsCount', 43, ''],
    ['numPoints', 43, ''],
    ['curvesCount', 44, ''],
    ['numCurves', 44, ''],
    ['cameraCount', 45, ''],
    ['numCameras', 45, ''],
    ['pointInstancerCount', 46, ''],
    ['numPointInstancers', 46, ''],
    ['pointInstanceDrawCount', 47, ''],
    ['numPointInstanceDraws', 47, ''],
    ['skeletonCount', 48, ''],
    ['numSkeletons', 48, ''],
    ['unsupportedRenderableCount', 49, ''],
    ['numUnsupportedRenderables', 49, ''],
    ['numAnimations', 50, ''],
    ['getAnimation', 51, 'n'],
    ['getAnimationView', 52, 'n'],
    ['getAllAnimations', 53, ''],
    ['getAnimationInfo', 54, 'n'],
    ['getAllAnimationInfos', 55, ''],
    ['getNode', 56, 'n'],
    ['getLight', 57, 'n'],
    ['getPoints', 58, 'n'],
    ['getCurves', 59, 'n'],
    ['getCamera', 60, 'n'],
    ['getPointInstancer', 61, 'n'],
    ['getPointInstanceDraw', 62, 'n'],
    ['getSkeleton', 63, 'n'],
    ['getUnsupportedRenderables', 64, ''],
    ['getSceneMetadata', 65, ''],
    ['getStats', 66, ''],
    ['getMesh', 67, 'n'],
    ['error', 68, ''],
    ['end', 69, ''],
  ]);
}
