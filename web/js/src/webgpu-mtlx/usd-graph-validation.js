// SPDX-License-Identifier: Apache-2.0
// Native property snapshot checks; these do not assert rendering support.
export async function validateUSDGraphSnapshot(moduleOptions = {}) {
  const { default: createModule } = await import('../lightusd/lightusd_combined.js');
  const native = await createModule(moduleOptions), layer = new native.LightUSDLoaderNative();
  const source = `#usda 1.0
def Material "Mat" {
    token outputs:mtlx:surface.connect = </Mat/Surface.outputs:out>
    def Shader "Surface" {
        uniform token info:id = "ND_standard_surface_surfaceshader"
        color3f inputs:base_color = (0.25, 0.5, 0.75)
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
    check(graph.prims.length, 4);
    check(property('/Mat', 'outputs:mtlx:surface').connections, ['/Mat/Surface.outputs:out']);
    check(property('/Mat/Surface', 'info:id').value, 'ND_standard_surface_surfaceshader');
    check(property('/Mat/Surface', 'inputs:base_color').value, [.25, .5, .75]);
    check(property('/Mat/Surface', 'inputs:base_color').type, 'color3f');
    check(property('/Mat/Surface', 'inputs:roughness').connections, ['/Mat/Graph.outputs:result']);
    if (Math.abs(property('/Mat/Surface', 'inputs:roughness').value - .3) > 1e-6) throw new Error('Connected default was lost');
    check(property('/Mat/Graph', 'outputs:result').connections, ['/Mat/Graph/Value.outputs:out']);
    check(property('/Mat/Graph/Value', 'inputs:value').connections, ['/Mat/Graph.inputs:amount']);
    return { prims: graph.prims.length, connections: 4, connectedDefault: true };
  } finally { layer.delete(); }
}
