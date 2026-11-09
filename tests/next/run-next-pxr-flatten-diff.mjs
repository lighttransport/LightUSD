#!/usr/bin/env node
// next-vs-pxr flatten differential gate.
//
// Flattens each input with `next_usdcat -f` and with the pinned OpenUSD
// `usdcat --flatten`, then compares the two USDA outputs semantically using
// the parser/comparator from tests/compare-usda.js. Divergences that are
// understood-and-accepted live in tests/next/next-pxr-flatten-xfail.txt
// (one `<relpath><TAB or spaces><reason>` per line, # comments); any diff not
// on that list fails the gate.
//
// Inputs: tests/usda/*.usda, tests/usdc/*.usdc, and (when --suite-root is
// given) <suite-root>/composition/tests/assets/*/root.usd.
//
// Classification:
//   pass       outputs semantically equivalent (tool banners ignored)
//   diff       semantic difference (fails unless xfail-listed)
//   next-error next_usdcat failed (fails unless xfail-listed)
//   pxr-error  the ORACLE cannot read the input (skipped: many legacy
//              fixtures are intentionally invalid; not a next defect)
//   skip       fixture opts out via a `# XFAIL:`/`# XDIFF:` header (legacy
//              roundtrip conventions; those gates own such cases)
//
// Usage:
//   node tests/next/run-next-pxr-flatten-diff.mjs \
//     --next-usdcat build-next/next_usdcat \
//     [--usdcat ref/dist/bin/usdcat] \
//     [--suite-root ~/.cache/lightusd/.../releases/1.0.1] \
//     [--xfail tests/next/next-pxr-flatten-xfail.txt] \
//     [--report-only] [--jobs N] [--float-tolerance 1e-5] [--verbose]

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { execFile } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, '..', '..');
const require = createRequire(import.meta.url);
const { parseUsda, compareUsda } = require(path.join(repoRoot, 'tests', 'compare-usda.js'));

function parseArgs(argv) {
  const a = {
    nextUsdcat: null,
    usdcat: process.env.USDCAT_PATH || path.join(repoRoot, 'ref', 'dist', 'bin', 'usdcat'),
    suiteRoot: process.env.AOUSD_CORE_SUPPLEMENTAL_ROOT || null,
    xfail: path.join(__dirname, 'next-pxr-flatten-xfail.txt'),
    reportOnly: false,
    jobs: Math.max(1, (os.cpus()?.length || 4) - 1),
    floatTolerance: 1e-5,
    timeout: 60000,
    verbose: false,
  };
  for (let i = 2; i < argv.length; i++) {
    const k = argv[i];
    const val = () => argv[++i];
    switch (k) {
      case '--next-usdcat': a.nextUsdcat = val(); break;
      case '--lusdcat': a.nextUsdcat = val(); break;  // unified name
      case '--usdcat': a.usdcat = val(); break;
      case '--suite-root': a.suiteRoot = val(); break;
      case '--xfail': a.xfail = val(); break;
      case '--report-only': a.reportOnly = true; break;
      case '--jobs': a.jobs = parseInt(val(), 10); break;
      case '--float-tolerance': a.floatTolerance = parseFloat(val()); break;
      case '--timeout': a.timeout = parseInt(val(), 10); break;
      case '--verbose': a.verbose = true; break;
      case '-h': case '--help':
        console.log('Usage: node tests/next/run-next-pxr-flatten-diff.mjs --lusdcat PATH ' +
          '[--usdcat PATH] [--suite-root DIR] [--xfail FILE] [--report-only] [--jobs N] ' +
          '[--float-tolerance F] [--timeout MS] [--verbose]');
        process.exit(0);
        break;
      default:
        console.error('Unknown arg: ' + k);
        process.exit(2);
    }
  }
  if (!a.nextUsdcat) {
    console.error('--lusdcat is required (legacy alias: --next-usdcat)');
    process.exit(2);
  }
  return a;
}

function loadXfails(file) {
  const map = new Map(); // relpath -> reason
  if (!fs.existsSync(file)) return map;
  for (const raw of fs.readFileSync(file, 'utf8').split('\n')) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const m = line.match(/^(\S+)\s+(.*)$/);
    if (m) map.set(m[1], m[2]);
    else map.set(line, '(no reason recorded)');
  }
  return map;
}

function collectInputs(a) {
  const inputs = []; // { file, rel }
  for (const dir of ['tests/usda', 'tests/usdc']) {
    const abs = path.join(repoRoot, dir);
    if (!fs.existsSync(abs)) continue;
    for (const e of fs.readdirSync(abs).sort()) {
      if (!/\.(usda|usdc)$/.test(e)) continue;
      inputs.push({ file: path.join(abs, e), rel: `${dir}/${e}` });
    }
  }
  if (a.suiteRoot) {
    const assets = path.join(a.suiteRoot, 'composition', 'tests', 'assets');
    if (fs.existsSync(assets)) {
      for (const e of fs.readdirSync(assets).sort()) {
        const root = path.join(assets, e, 'root.usd');
        if (fs.existsSync(root)) {
          inputs.push({ file: root, rel: `supplemental/${e}` });
        }
      }
    } else {
      console.error(`warning: no composition assets under ${a.suiteRoot}`);
    }
  }
  return inputs;
}

function run(cmd, args, timeout) {
  return new Promise((resolve) => {
    execFile(cmd, args, { timeout, maxBuffer: 256 * 1024 * 1024 }, (error, stdout, stderr) => {
      resolve({ error, stdout, stderr });
    });
  });
}

// Tool-added banner metadata that legitimately differs between the two
// writers (next emits a comment; pxr flatten adds a machine-generated doc).
function stripBannerMeta(usda) {
  if (usda && usda.metadata) {
    delete usda.metadata.comment;
    delete usda.metadata.doc;
    delete usda.metadata.documentation;
  }
  return usda;
}

function fixtureOptOut(file) {
  if (!/\.usda$/.test(file)) return null;
  try {
    const head = fs.readFileSync(file, 'utf8').split('\n', 32);
    for (const line of head) {
      const t = line.trim();
      if (t.startsWith('# XFAIL:')) return 'XFAIL: ' + t.slice(8).trim();
      if (t.startsWith('# XDIFF:')) return 'XDIFF: ' + t.slice(8).trim();
    }
  } catch { /* binary or unreadable; no opt-out */ }
  return null;
}

async function processOne(a, input, tmpdir, idx) {
  const optOut = fixtureOptOut(input.file);
  if (optOut) return { ...input, status: 'skip', detail: optOut };

  const nextOut = path.join(tmpdir, `next-${idx}.usda`);
  const pxrOut = path.join(tmpdir, `pxr-${idx}.usda`);

  const p = await run(a.usdcat, ['--flatten', '-o', pxrOut, input.file], a.timeout);
  if (p.error) {
    return { ...input, status: 'pxr-error', detail: (p.stderr || p.error.message).split('\n')[0] };
  }
  const n = await run(a.nextUsdcat,
    ['-f', '--instance-mode', 'prototypes', '--prototype-numbering', 'usdcat',
     '-o', nextOut, input.file], a.timeout);
  if (n.error) {
    return { ...input, status: 'next-error', detail: (n.stderr || n.error.message).split('\n')[0] };
  }

  let nextUsda, pxrUsda;
  try {
    nextUsda = stripBannerMeta(parseUsda(fs.readFileSync(nextOut, 'utf8')));
  } catch (e) {
    return { ...input, status: 'next-error', detail: `unparseable next output: ${e.message}` };
  }
  try {
    pxrUsda = stripBannerMeta(parseUsda(fs.readFileSync(pxrOut, 'utf8')));
  } catch (e) {
    return { ...input, status: 'pxr-error', detail: `unparseable pxr output: ${e.message}` };
  }

  // Both flattened outputs describe the same source layer. OpenUSD rewrites
  // asset identifiers to absolute paths while next intentionally keeps them
  // authored-relative, so compare their lexically-resolved targets using the
  // original layer directory as the anchor (never the temporary output dir).
  const assetPathBase = path.dirname(input.file);
  const diffs = compareUsda(nextUsda, pxrUsda, {
    floatTolerance: a.floatTolerance,
    resolveAssetPaths: true,
    assetPathBase1: assetPathBase,
    assetPathBase2: assetPathBase,
  });
  if (diffs.length === 0) return { ...input, status: 'pass' };
  return {
    ...input,
    status: 'diff',
    detail: `${diffs.length} difference(s)`,
    diffs: diffs.slice(0, 8).map((d) =>
      `${d.type || ''} ${d.path || ''} ${(d.message || d.description || '').slice(0, 160)}`.trim()),
  };
}

async function main() {
  const a = parseArgs(process.argv);
  const xfails = loadXfails(a.xfail);
  const inputs = collectInputs(a);
  if (inputs.length === 0) {
    console.error('no inputs found');
    process.exit(2);
  }
  const tmpdir = fs.mkdtempSync(path.join(os.tmpdir(), 'next-pxr-flatten-'));

  const results = [];
  let cursor = 0;
  async function worker() {
    while (cursor < inputs.length) {
      const idx = cursor++;
      results[idx] = await processOne(a, inputs[idx], tmpdir, idx);
    }
  }
  await Promise.all(Array.from({ length: Math.min(a.jobs, inputs.length) }, worker));
  fs.rmSync(tmpdir, { recursive: true, force: true });

  const counts = { pass: 0, diff: 0, 'next-error': 0, 'pxr-error': 0, skip: 0, xfail: 0, xpass: 0, intentional: 0 };
  // Entries whose reason is tagged INTENTIONAL: (deliberate lightusd behavior,
  // e.g. lossless unknown-metadata preservation, portable relative asset
  // paths) or ORACLE- (a pxr bug/nondeterminism/limitation) are PERMANENT:
  // counted separately and never expected to prune. Burn-down completion =
  // zero xfail entries outside this bucket.
  const isPermanent = (rel) => {
    const reason = xfails.get(rel) || '';
    return reason.startsWith('INTENTIONAL:') || reason.startsWith('ORACLE-');
  };
  const failures = [];
  for (const r of results) {
    const listed = xfails.has(r.rel);
    if (r.status === 'pass' || r.status === 'diff' || r.status === 'next-error') {
      if (listed && r.status === 'pass') {
        if (isPermanent(r.rel)) { counts.pass++; continue; }  // oracle flaps
        counts.xpass++; counts.pass++; continue;
      }
      if (listed) {
        if (isPermanent(r.rel)) counts.intentional++;
        else counts.xfail++;
        continue;
      }
    }
    counts[r.status]++;
    if (r.status === 'diff' || r.status === 'next-error') failures.push(r);
  }

  for (const r of results) {
    const listed = xfails.has(r.rel);
    const tag = listed && r.status !== 'pass' ? 'XFAIL' : r.status.toUpperCase();
    if (a.verbose || (r.status !== 'pass' && r.status !== 'skip')) {
      console.log(`[${tag}] ${r.rel}${r.detail ? ' — ' + r.detail : ''}`);
      if (r.diffs && (a.verbose || !listed)) for (const d of r.diffs) console.log(`    ${d}`);
    }
  }

  console.log(`\nnext-vs-pxr flatten diff: ${inputs.length} inputs — ` +
    `${counts.pass} pass (${counts.xpass} xpass), ${counts.xfail} xfail, ` +
    `${counts.intentional} intentional, ` +
    `${failures.length} FAIL (${counts.diff} diff + ${counts['next-error']} next-error), ` +
    `${counts['pxr-error']} pxr-skip, ${counts.skip} fixture-skip`);
  if (counts.xpass > 0) {
    console.log(`note: ${counts.xpass} xfail-listed input(s) now pass — prune them from ${path.relative(repoRoot, a.xfail)}`);
  }

  if (counts['pxr-error'] + counts.skip === inputs.length) {
    console.error('OpenUSD could not read any selected input; no reference comparisons ran.');
    process.exit(2);
  }
  if (failures.length > 0 && !a.reportOnly) process.exit(1);
}

await main();
