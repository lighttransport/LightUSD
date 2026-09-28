import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';
import { constants, zstdCompressSync } from 'node:zlib';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const packageRoot = path.resolve(__dirname, '..');
const repoRoot = path.resolve(packageRoot, '..', '..');
const sourceDir = path.resolve(packageRoot, '..', 'js', 'src', 'lightusd');
// Dependency-free modules that live one directory above sourceDir but are
// imported by the packaged loaders (e.g. LightUSDLoader.js pulls
// parseUSDZEntries from ../usdzconvert.js). They are staged flat into dist and
// the staged importers are rewritten from '../<name>' to './<name>'.
const PARENT_DIR_MODULES = ['usdzconvert.js', 'texture-memory-budget.mjs'];
const distDir = path.resolve(packageRoot, 'dist');
const rootEntrypoint = path.resolve(packageRoot, 'index.js');
const readmePath = path.resolve(packageRoot, 'README.md');
const licensePath = path.resolve(repoRoot, 'LICENSE');
const manifestPath = path.resolve(packageRoot, 'package.json');

const REQUIRED_SOURCE_FILES = [
  'lightusd.js',
  'lightusd.wasm',
  'lightusd_64.js',
  'lightusd_64.wasm',
  'lightusd_next.js',
  'lightusd_next.wasm',
  'lightusd_next_64.js',
  'lightusd_next_64.wasm',
  'lightusd_mcp.js',
  'lightusd_mcp.wasm',
  'lightusd_mcp_64.js',
  'lightusd_mcp_64.wasm'
];

const SEMVER_RE = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z-.]+)?(?:\+[0-9A-Za-z-.]+)?$/;
const REQUIRED_NODE_MAJOR = 24;

function parseArgs(argv) {
  const args = {
    releaseVersion: null
  };

  for (const arg of argv) {
    if (arg.startsWith('--release-version=')) {
      args.releaseVersion = arg.slice('--release-version='.length);
    } else if (arg === '--help' || arg === '-h') {
      console.log('Usage: node ./scripts/stage-package.mjs [--release-version=x.y.z]');
      process.exit(0);
    } else {
      throw new Error(`Unknown argument: ${arg}`);
    }
  }

  return args;
}

function ensureFileExists(filePath) {
  if (!fs.existsSync(filePath)) {
    throw new Error(`Required file not found: ${filePath}`);
  }
}

function listSourceFiles(dir, extension) {
  return fs.readdirSync(dir)
    .filter((entry) => entry.endsWith(extension))
    .sort();
}

function copyFiles(files, fromDir, toDir) {
  for (const file of files) {
    fs.copyFileSync(path.join(fromDir, file), path.join(toDir, file));
  }
}

function ensureNodeSupportsZstd() {
  const major = Number(process.versions.node.split('.')[0]);
  if (!Number.isFinite(major) || major < REQUIRED_NODE_MAJOR) {
    throw new Error(`Node.js v${REQUIRED_NODE_MAJOR}+ is required for built-in Zstd compression. Current: ${process.version}`);
  }

  if (typeof zstdCompressSync !== 'function') {
    throw new Error(`This Node.js build does not expose zstdCompressSync. Current: ${process.version}`);
  }
}

function compressWasm(filePath) {
  const compressed = zstdCompressSync(fs.readFileSync(filePath), {
    params: {
      [constants.ZSTD_c_compressionLevel]: 19
    }
  });
  fs.writeFileSync(`${filePath}.zst`, compressed);
}

function buildPublishManifest(sourceManifest, releaseVersion) {
  const manifest = {
    ...sourceManifest,
    version: releaseVersion || sourceManifest.version
  };

  delete manifest.private;
  delete manifest.scripts;

  return manifest;
}

function main() {
  const { releaseVersion } = parseArgs(process.argv.slice(2));

  if (releaseVersion && !SEMVER_RE.test(releaseVersion)) {
    throw new Error(`Invalid semver release version: ${releaseVersion}`);
  }

  ensureNodeSupportsZstd();

  ensureFileExists(rootEntrypoint);
  ensureFileExists(readmePath);
  ensureFileExists(licensePath);
  ensureFileExists(manifestPath);

  for (const file of REQUIRED_SOURCE_FILES) {
    ensureFileExists(path.join(sourceDir, file));
  }

  fs.rmSync(distDir, { recursive: true, force: true });
  fs.mkdirSync(distDir, { recursive: true });

  const jsFiles = listSourceFiles(sourceDir, '.js');
  const wasmFiles = listSourceFiles(sourceDir, '.wasm');

  copyFiles(jsFiles, sourceDir, distDir);
  copyFiles(wasmFiles, sourceDir, distDir);

  const parentSourceDir = path.resolve(sourceDir, '..');
  for (const module of PARENT_DIR_MODULES) {
    ensureFileExists(path.join(parentSourceDir, module));
    fs.copyFileSync(path.join(parentSourceDir, module), path.join(distDir, module));
  }
  for (const jsFile of jsFiles) {
    const stagedPath = path.join(distDir, jsFile);
    let content = fs.readFileSync(stagedPath, 'utf8');
    let rewritten = content;
    for (const module of PARENT_DIR_MODULES) {
      rewritten = rewritten.split(`'../${module}'`).join(`'./${module}'`);
      rewritten = rewritten.split(`"../${module}"`).join(`"./${module}"`);
    }
    if (rewritten !== content) {
      fs.writeFileSync(stagedPath, rewritten, 'utf8');
    }
  }
  fs.copyFileSync(rootEntrypoint, path.join(distDir, 'index.js'));
  fs.copyFileSync(readmePath, path.join(distDir, 'README.md'));
  fs.copyFileSync(licensePath, path.join(distDir, 'LICENSE'));

  const sourceManifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  const publishManifest = buildPublishManifest(sourceManifest, releaseVersion);
  fs.writeFileSync(
    path.join(distDir, 'package.json'),
    `${JSON.stringify(publishManifest, null, 2)}\n`,
    'utf8'
  );

  for (const wasmFile of wasmFiles) {
    compressWasm(path.join(distDir, wasmFile));
  }

  console.log(`Staged ${jsFiles.length} JS files and ${wasmFiles.length} WASM files into ${distDir}`);
}

try {
  main();
} catch (error) {
  console.error(`[stage-package] ${error.message}`);
  process.exit(1);
}
