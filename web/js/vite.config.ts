import { defineConfig } from 'vite';
import { execFileSync } from 'child_process';
import path from 'path';
import {copyLocalWasmProducts} from '../build-utils.mjs';

const usdAssetsDir = process.env.USD_WG_ASSETS_DIR || '';
const mujocoDir = process.env.MUJOCO_WASM_DIR || '';
const publicDir = process.env.LIGHTUSD_VITE_PUBLIC_DIR;

// https://vitejs.dev/config/
export default defineConfig(({ command }) => {
  // The browser loaders consume generated Emscripten glue from
  // src/lightusd. Keep every `vite`-based demo command self-contained instead
  // of requiring developers to know which bootstrap command produces it.
  // Browser regression runners can prepare the modules explicitly before
  // starting Vite. Skipping the hook in that case avoids consuming the
  // runner's short server-start timeout (and prevents symlinked worktree paths
  // from making CMake reconfigure the same build under a different spelling).
  if (command === 'serve' && process.env.LIGHTUSD_SKIP_WASM_PREPARE !== '1') {
    execFileSync('bash', [
      path.resolve(__dirname, '../demo/scripts/prepare-local-lightusd.sh'),
    ], { stdio: 'inherit' });
  }

  return {
  ...(publicDir ? { publicDir: path.resolve(publicDir) } : {}),
  // Multi-page app: serve each .html directly and return 404 for unmatched
  // routes instead of falling back to index.html (the default SPA behavior).
  appType: 'mpa',
  server: {
    fs: {
        allow: [
          path.resolve(__dirname),
          ...(usdAssetsDir ? [path.resolve(usdAssetsDir)] : []),
          ...(mujocoDir ? [path.resolve(mujocoDir)] : []),
        ],
    },
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
    watch: {
      ignored: ['**/node_modules/**', '**/assets/**']
    },
  },
  resolve: {
    alias: [
      { find: 'lightusd', replacement: path.resolve(__dirname, './src/lightusd') },
    ],
  },
  optimizeDeps: {
    exclude: ['lightusd'],
  },
  // Lucia's xatlas worker imports the generated ESM WASM factory. IIFE
  // workers cannot participate in Rollup code-splitting.
  worker: { format: 'es' },
  plugins:[copyLocalWasmProducts(path.resolve(__dirname,'src/lightusd'),{includeMCP:true})],
  build: {
    // Demo modules use top-level await (including optional bridge loading).
    target: 'es2022',
    rollupOptions: {
      input: {
        'anim-clips': path.resolve(__dirname, 'anim-clips.html'),
        'animation': path.resolve(__dirname, 'animation.html'),
        'bench-usdzconvert': path.resolve(__dirname, 'bench-usdzconvert.html'),
        'colorspace-regression': path.resolve(__dirname, 'colorspace-regression.html'),
        'http-asset-resolver': path.resolve(__dirname, 'http-asset-resolver.html'),
        'index': path.resolve(__dirname, 'index.html'),
        'material-dedup': path.resolve(__dirname, 'material-dedup.html'),
        'materialx-webgl2': path.resolve(__dirname, 'materialx-webgl2.html'),
        'materialx-webgpu': path.resolve(__dirname, 'materialx-webgpu.html'),
        'materialx': path.resolve(__dirname, 'materialx.html'),
        'mtlx-debug': path.resolve(__dirname, 'mtlx-debug.html'),
        'mtlx-node-tester': path.resolve(__dirname, 'mtlx-node-tester.html'),
        'mtlx-webgpu': path.resolve(__dirname, 'mtlx-webgpu.html'),
        'offscreengl': path.resolve(__dirname, 'offscreengl.html'),
        'openpbr-nodegraph-demo': path.resolve(__dirname, 'openpbr-nodegraph-demo.html'),
        'phys-sim': path.resolve(__dirname, 'phys-sim.html'),
        'progress-demo': path.resolve(__dirname, 'progress-demo.html'),
        'progress-offscreenwebgl': path.resolve(__dirname, 'progress-offscreenwebgl.html'),
        'skin-anim': path.resolve(__dirname, 'skin-anim.html'),
        'streaming': path.resolve(__dirname, 'streaming.html'),
        'subdiv': path.resolve(__dirname, 'subdiv.html'),
        'tangent': path.resolve(__dirname, 'tangent.html'),
        'texcomp': path.resolve(__dirname, 'texcomp.html'),
        'udim': path.resolve(__dirname, 'udim.html'),
        'urdf': path.resolve(__dirname, 'urdf.html'),
        'usd-assets-view': path.resolve(__dirname, 'usd-assets-view.html'),
        'usd-export': path.resolve(__dirname, 'usd-export.html'),
        'usddiff': path.resolve(__dirname, 'usddiff.html'),
        'usdlux': path.resolve(__dirname, 'usdlux.html'),
        'usdzconvert': path.resolve(__dirname, 'usdzconvert.html'),
        'validation': path.resolve(__dirname, 'validation.html'),
        'webgpu-mtlx': path.resolve(__dirname, 'webgpu-mtlx.html'),
        lucia: path.resolve(__dirname, 'lucia-code/index.html'),
      },
    },
  },
  };
});
