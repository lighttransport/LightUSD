import { defineConfig } from 'vite';
import { execFileSync } from 'child_process';
import path from 'path';

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
  build: {
    rollupOptions: {
      input: {
        main: path.resolve(__dirname, 'index.html'),
        lucia: path.resolve(__dirname, 'lucia-code/index.html'),
      },
    },
  },
  };
});
