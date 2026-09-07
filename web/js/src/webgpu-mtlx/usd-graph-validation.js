// SPDX-License-Identifier: Apache-2.0
// Native property snapshot checks; these do not assert rendering support.
export async function validateUSDGraphSnapshot(moduleOptions = {}) {
  const { default: createModule } = await import('../lightusd/lightusd_combined.js');
  const native = await createModule(moduleOptions), layer = new native.LightUSDLoaderNative();
  const source = `#usda 1.0
def Material "Mat" (
    prepend apiSchemas = ["ColorSpaceAPI"]
) {
    uniform token colorSpace:name = "lin_ap1_scene"
    token outputs:mtlx:surface.connect = </Mat/Surface.outputs:out>
    def Shader "Surface" {
        uniform token info:id = "ND_standard_surface_surfaceshader"
        color3f inputs:base_color = (0.25, 0.5, 0.75) (
            colorSpace = "srgb_rec709_scene"
        )
        float inputs:roughness = 0.3
        float inputs:roughness.connect = </Mat/Graph.outputs:result>
        token outputs:out
    }
    def NodeGraph "Graph" {
        float inputs:amount = 0.4
        float outputs:result.connect = </Mat/Graph/Value.outputs:out>
        def Shader "Value" {
            uniform token info:id = "ND_constant_float"
            float inputs:value.connect = </Mat/Graph.inputs:amount>
            float outputs:out
        }
    }
}
over "AssetOverride" {
    asset inputs:file = @../maps/override.exr@
}`;
  try {
    if (!layer.loadAsLayerFromBinary(new TextEncoder().encode(source), 'shading-snapshot.usda')) throw new Error(layer.error());
    const text = layer.getShadingGraphJSON();
    if (!text) throw new Error(layer.error());
    const graph = JSON.parse(text);
    const property = (path, name) => {
      const result = graph.prims.find(p => p.path === path)?.properties[name];
      if (!result) throw new Error(`Missing USD graph property ${path}.${name}: ${text}`);
      return result;
    };
    const check = (actual, expected) => {
      if (JSON.stringify(actual) !== JSON.stringify(expected)) throw new Error(`USD graph snapshot mismatch: ${JSON.stringify(actual)} != ${JSON.stringify(expected)}`);
    };
    check(graph.version, 1);
    check(graph.colorMetadataVersion, 1);
    check(graph.colorSpaces['/Mat'].value, 'lin_ap1_scene');
    check(graph.assetPaths.length, 1);
    check(graph.assetPaths[0].primPath, '/AssetOverride');
    check(graph.assetPaths[0].propertyPath, '/AssetOverride.inputs:file');
    check(graph.assetPaths[0].authored, '../maps/override.exr');
    check(graph.prims.length, 4);
    check(property('/Mat', 'outputs:mtlx:surface').connections, ['/Mat/Surface.outputs:out']);
    check(property('/Mat/Surface', 'info:id').value, 'ND_standard_surface_surfaceshader');
    check(property('/Mat/Surface', 'inputs:base_color').value, [.25, .5, .75]);
    check(property('/Mat/Surface', 'inputs:base_color').type, 'color3f');
    check(property('/Mat/Surface', 'inputs:base_color').colorSpace, 'srgb_rec709_scene');
    check(property('/Mat/Surface', 'inputs:roughness').connections, ['/Mat/Graph.outputs:result']);
    if (Math.abs(property('/Mat/Surface', 'inputs:roughness').value - .3) > 1e-6) throw new Error('Connected default was lost');
    check(property('/Mat/Graph', 'outputs:result').connections, ['/Mat/Graph/Value.outputs:out']);
    check(property('/Mat/Graph/Value', 'inputs:value').connections, ['/Mat/Graph.inputs:amount']);
    return { prims: graph.prims.length, connections: 4, connectedDefault: true };
  } finally { layer.delete(); }
}

export async function validateUSDMaterialTranslation(renderer) {
  const { parseMaterialX } = await import('./graph.js');
  const { materialXFromUSD } = await import('./usd-graph.js');
  const { fetchResource } = await import('./resources.js');
  const library = parseMaterialX(new TextDecoder().decode(await fetchResource('/__mtlx/libraries/pbrlib/pbrlib_defs.mtlx')));
  const { default: createModule } = await import('../lightusd/lightusd_combined.js');
  const native = await createModule(), layer = new native.LightUSDLoaderNative();
  const source = `#usda 1.0
def Material "M" (
    prepend apiSchemas = ["ColorSpaceAPI"]
) {
    uniform token colorSpace:name = "lin_ap1_scene"
    token outputs:mtlx:surface.connect = </M/Graph.outputs:surface>
    def NodeGraph "Graph" {
        color3f inputs:color = (0.25, 0.5, 0.75) (
            colorSpace = "srgb_rec709_scene"
        )
        token outputs:surface.connect = </M/Surface.outputs:out>
    }
    def Shader "Emission" (
        prepend apiSchemas = ["ColorSpaceAPI"]
    ) {
        uniform token colorSpace:name = "data"
        uniform token info:id = "ND_uniform_edf"
        color3f inputs:color.connect = </M/Graph.inputs:color>
        token outputs:out
    }
    def Shader "Surface" {
        uniform token info:id = "ND_surface"
        token inputs:edf.connect = </M/Emission.outputs:out>
        token outputs:out
    }
}`;
  try {
    if (!layer.loadAsLayerFromBinary(new TextEncoder().encode(source), 'material-translation.usda')) throw new Error(layer.error());
    const doc = materialXFromUSD(JSON.parse(layer.getShadingGraphJSON()), '/M', { library });
    const scene = { positions: [-10,-10,0,10,-10,0,10,10,0,-10,10,0], indices: [0,1,2,0,2,3], materials: [doc],
      camera: { origin: [0,0,1], target: [0,0,0], fov: 45 }, lighting: { environment: [0,0,0], directional: { radiance: [0,0,0] } } };
    await renderer.loadScene(scene); renderer.setMode('path-physical');
    for (let i = 0; i < 20 && renderer.samples < 2; i++) await renderer.renderStep();
    if (renderer.samples < 2) throw new Error('USD translated emission paths did not complete');
    const capture = await renderer.capture({ format: 'float32' });
    const expected = [.25,.5,.75].map(c => ((c + .055) / 1.055) ** 2.4);
    for (let i = 0; i < capture.pixels.length; i += 4) {
      for (let c = 0; c < 3; c++) if (!Number.isFinite(capture.pixels[i+c]) || Math.abs(capture.pixels[i+c] - expected[c]) > 1e-6) throw new Error('USD to MaterialX emission color-space/radiance mismatch');
    }
    return { nodes: doc.nodes.length, expected, samples: renderer.samples };
  } finally { layer.delete(); }
}
