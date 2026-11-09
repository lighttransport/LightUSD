#!/usr/bin/env node
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const PIN = '1b91f3c464891af259d51d9ee9ee9e6c357f7079';
const REPOSITORY = 'https://github.com/usd-wg/assets.git';
const RELATIVE_SOURCE = 'full_assets/OpenChessSet';
const EXPECTED_FILES = ['chess_set.usda', 'README.md', 'assets/Chessboard/Chessboard_mat.mtlx'];
const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const demoDir = path.resolve(scriptDir, '..');
const repositoryRoot = path.resolve(demoDir, '..', '..');
const localAssetsLink = path.join(repositoryRoot, 'usd-assets');
const outputDir = path.join(demoDir, 'public', 'assets', 'openchess');
const markerPath = path.join(outputDir, '.lightusd-openchess.json');
const previousMarkerPath = path.join(outputDir, '.tinyusdz-openchess.json');
const indexPath = path.join(outputDir, 'asset-index.json');
const checkOnly = process.argv.includes('--check');
const force = process.argv.includes('--force');

function validTree(root) {
  return EXPECTED_FILES.every((file) => fs.existsSync(path.join(root, file)));
}

function checkPrepared() {
  if (!validTree(outputDir) || !fs.existsSync(indexPath)) return false;
  try {
    const marker = fs.existsSync(markerPath) ? markerPath : previousMarkerPath;
    return JSON.parse(fs.readFileSync(marker, 'utf8')).revision === PIN;
  } catch {
    return false;
  }
}

function git(args, options = {}) {
  const result = spawnSync('git', args, { encoding: 'utf8', stdio: options.inherit ? 'inherit' : 'pipe' });
  // Some managed sandboxes annotate an otherwise successful subprocess with
  // EPERM. The exit status and output remain authoritative in that case.
  if (result.status !== 0) throw result.error || new Error(result.stderr || `git ${args.join(' ')} failed`);
  return result.stdout || '';
}

if (!force && checkPrepared()) {
  console.log(`OpenChessSet is prepared at ${outputDir}`);
  process.exit(0);
}

if (checkOnly) {
  console.error('OpenChessSet is not prepared. Run: npm run prepare:openchess');
  process.exit(1);
}

let sourceRoot = process.env.USD_ASSETS_DIR || process.env.USD_WG_ASSETS_DIR || '';
let temporaryRoot = '';
if (!sourceRoot && validTree(path.join(localAssetsLink, RELATIVE_SOURCE))) {
  // `usd-assets` is commonly a developer-owned symlink to an existing
  // usd-wg/assets checkout. Prefer it to avoid downloading the 4K OpenChess
  // textures again; fs.cpSync below dereferences it into Vite's public tree.
  sourceRoot = localAssetsLink;
  console.log(`Using local USD assets checkout: ${fs.realpathSync(sourceRoot)}`);
}
if (sourceRoot) {
  sourceRoot = path.resolve(sourceRoot);
  const revision = git(['-C', sourceRoot, 'rev-parse', 'HEAD']).trim();
  if (revision !== PIN && process.env.LIGHTUSD_ALLOW_UNPINNED_OPENCHESS !== '1') {
    throw new Error(`USD assets checkout is ${revision}; expected ${PIN}. Set LIGHTUSD_ALLOW_UNPINNED_OPENCHESS=1 to override.`);
  }
} else {
  temporaryRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'lightusd-openchess-'));
  sourceRoot = path.join(temporaryRoot, 'assets');
  git(['clone', '--filter=blob:none', '--no-checkout', REPOSITORY, sourceRoot], { inherit: true });
  git(['-C', sourceRoot, 'sparse-checkout', 'set', RELATIVE_SOURCE], { inherit: true });
  git(['-C', sourceRoot, 'checkout', '--detach', PIN], { inherit: true });
}

const sourceDir = path.join(sourceRoot, RELATIVE_SOURCE);
if (!validTree(sourceDir)) throw new Error(`Incomplete OpenChessSet source: ${sourceDir}`);
fs.rmSync(outputDir, { recursive: true, force: true });
fs.mkdirSync(path.dirname(outputDir), { recursive: true });
fs.cpSync(sourceDir, outputDir, { recursive: true, dereference: true });
const textureExtensions = new Set(['.png', '.jpg', '.jpeg', '.webp', '.hdr', '.exr']);
const texturePaths = [];
const visit = (directory) => {
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const absolute = path.join(directory, entry.name);
    if (entry.isDirectory()) visit(absolute);
    else if (textureExtensions.has(path.extname(entry.name).toLowerCase())) {
      texturePaths.push(path.relative(outputDir, absolute).split(path.sep).join('/'));
    }
  }
};
visit(outputDir);
texturePaths.sort();
fs.writeFileSync(indexPath, `${JSON.stringify({ revision: PIN, textures: texturePaths }, null, 2)}\n`);
fs.writeFileSync(markerPath, `${JSON.stringify({ revision: PIN, source: REPOSITORY }, null, 2)}\n`);
if (temporaryRoot) fs.rmSync(temporaryRoot, { recursive: true, force: true });
console.log(`Prepared OpenChessSet at ${outputDir}`);
