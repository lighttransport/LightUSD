import { defineConfig } from 'vite'
import path from 'path'
import { compression } from 'vite-plugin-compression2'

// Do not minify(we want to make demo website simple)
// base: "./" => make asset path relative(required for static hosting of lightusd demo page at github pages)
export default defineConfig(({ command, mode }) => {
  const useLocalLightUSD = command === 'serve' && mode === 'development';
  const lightusdRoot = useLocalLightUSD
    ? path.resolve(__dirname, '../js/src/lightusd')
    : path.resolve(__dirname, 'node_modules/lightusd');
  const nextUtils = useLocalLightUSD
    ? path.resolve(__dirname, '../js/src/lightusd/NextRenderSceneUtils.js')
    : path.resolve(__dirname, 'src/next-backend-production-shim.js');

  return {
    base: "./",
    define: {
        __LIGHTUSD_LOCAL_DEV__: JSON.stringify(useLocalLightUSD),
    },
    server: {
        fs: {
            allow: [__dirname, path.resolve(__dirname, '../js')],
        },
        headers: {
            'Cross-Origin-Opener-Policy': 'same-origin',
            'Cross-Origin-Embedder-Policy': 'require-corp',
        },
    },
    resolve: {
        alias: [
            { find: 'lightusd-next-demo-utils', replacement: nextUtils },
            { find: 'lightusd', replacement: lightusdRoot },
            { find: 'fzstd', replacement: path.resolve(__dirname, 'node_modules/fzstd') },
            { find: 'three', replacement: path.resolve(__dirname, 'node_modules/three') },
            { find: 'lightusd-js', replacement: path.resolve(__dirname, '../js') },
        ],
    },
    build: {
        rollupOptions: {
          input: {
            main: path.resolve(__dirname, 'index.html'),
            hair_fur: path.resolve(__dirname, 'hair-fur.html'),
            materialx_openchess: path.resolve(__dirname, 'materialx-openchess.html'),
            materialx_node: path.resolve(__dirname, 'materialx-node.html'),
            materialx_physical: path.resolve(__dirname, 'materialx-physical.html'),
            usdlux: path.resolve(__dirname, 'usdlux.html'),
            skinning: path.resolve(__dirname, 'skinning.html'),
            metahuman: path.resolve(__dirname, 'metahuman.html'),
            xform_skinning_animation: path.resolve(__dirname, 'xform-skinning-animation.html'),
            physics: path.resolve(__dirname, 'physics.html'),
            asset_resolver: path.resolve(__dirname, 'asset-resolver.html'),
            composition: path.resolve(__dirname, 'composition.html'),
            export_demo: path.resolve(__dirname, 'export.html'),
            usd_assets: path.resolve(__dirname, 'usd-assets.html'),
            usd_physics: path.resolve(__dirname, 'usd-physics.html'),
            material_editor: path.resolve(__dirname, 'material-editor.html'),
            animation_timeline: path.resolve(__dirname, 'animation-timeline.html'),
            usd_inspector: path.resolve(__dirname, 'usd-inspector.html'),
            composition_viz: path.resolve(__dirname, 'composition-viz.html'),
            streaming_viz: path.resolve(__dirname, 'streaming-viz.html'),
            viewer_toolkit: path.resolve(__dirname, 'viewer-toolkit.html'),
            usd_viewer: path.resolve(__dirname, 'usd-viewer.html'),
            animation_blending: path.resolve(__dirname, 'animation-blending.html'),
            procedural_usd: path.resolve(__dirname, 'procedural-usd.html'),
            usdz_packager: path.resolve(__dirname, 'usdz-packager.html'),
            usd_diff: path.resolve(__dirname, 'usd-diff.html'),
            backend_compare: path.resolve(__dirname, 'backend-compare.html'),
          },
        },
        minify: true,
        cssMinify: true,
    },
    optimizeDeps: {
        exclude: ['lightusd', '@lighttransport/mujoco-wasm'],
    },
    // Use only gzip here. Vite will emit the WASM referenced by the npm package.
    plugins: [
      compression({algorithms: ['gzip']}),
    ],
  };
});
