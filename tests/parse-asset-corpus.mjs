#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-present Light Transport Entertainment, Inc.
//
// parse-asset-corpus.mjs — run the native `lusdcat` tool over a tree of USD
// assets (e.g. a checkout of usd-wg/assets) and classify each file as
// PASS / WARN / FAIL (plus TIMEOUT / CRASH / SKIP), then categorize the
// warnings and errors by source-location signature.
//
// Status model (lusdcat prints `WARN : …` / `ERR : …` to stderr; exit 0 = the
// file loaded, non-zero = load failed):
//   PASS    exit 0, no `WARN :` in stderr
//   WARN    exit 0, has `WARN :`
//   FAIL    non-zero exit, not killed by a signal
//   TIMEOUT killed after --timeout
//   CRASH   terminated by a signal (SIGSEGV/SIGABRT/…)
//   SKIP    .mtlx (MaterialX is XML; lusdcat parses USD only) unless --include-mtlx
//
// Usage:
//   node tests/parse-asset-corpus.mjs [--assets DIR] [--optional-assets] [--lusdcat PATH]
//        [--mode load|flatten] [--timeout MS] [--jobs N] [--include-mtlx]
//        [--out DIR]
// Env: LUSDCAT_PATH overrides the default binary path.
//
// Outputs (under --out, default tests/asset-parse-results/):
//   results.tsv   one row per file: relpath, status, exit, signal, ms, headline
//   summary.md    status totals + warning/error category tables + FAIL lists
//   summary.json  machine-readable (diff runs over time)

import { spawn } from 'node:child_process';
import { promises as fs } from 'node:fs';
import os from 'node:os';
import path from 'node:path';

// ---------------------------------------------------------------------------
// args
// ---------------------------------------------------------------------------

function parseArgs(argv) {
  const a = {
    assets: process.env.USD_WG_ASSETS_DIR || '',
    lusdcat: process.env.LUSDCAT_PATH || './build/lusdcat',
    mode: 'load',
    timeout: 30000,
    jobs: Math.max(1, (os.cpus()?.length || 4) - 1),
    includeMtlx: false,
    // Let an automated CTest registration skip cleanly on developer machines
    // that do not have the optional usd-wg/assets checkout. Explicit CLI use
    // remains strict unless this flag is passed.
    optionalAssets: false,
    // Regression gate: exit non-zero if (FAIL + CRASH) exceeds this. Default
    // off (report only); CI passes e.g. `--max-fail 0`.
    maxFail: Infinity,
    // Compare-mode ratchet: exit non-zero if DIFFER+DIFFERR exceeds this.
    maxDiffer: Infinity,
    out: 'tests/asset-parse-results',
    // Compare mode: cross-check each asset against the OpenUSD reference
    // (`usdcat`) and diff lightusd's re-serialization against it (`lusddiff`).
    compare: false,
    usdcat: process.env.USDCAT_PATH || '',
    lusddiff: process.env.LUSDDIFF_PATH || './build/lusddiff',
    // Extra args prepended when invoking --lusdcat for the compare-mode layer
    // serialization (legacy lusdcat prints the layer bare; the next CLI (now lusdcat) needs
    // `--rewrite-layer`).
    serializeArgs: [],
  };
  for (let i = 2; i < argv.length; i++) {
    const k = argv[i];
    const val = () => argv[++i];
    switch (k) {
      case '--assets': a.assets = val(); break;
      case '--lusdcat': a.lusdcat = val(); break;
      case '--mode': a.mode = val(); break;
      case '--timeout': a.timeout = parseInt(val(), 10); break;
      case '--jobs': a.jobs = parseInt(val(), 10); break;
      case '--include-mtlx': a.includeMtlx = true; break;
      case '--optional-assets': a.optionalAssets = true; break;
      case '--max-fail': a.maxFail = parseInt(val(), 10); break;
      case '--max-differ': a.maxDiffer = parseInt(val(), 10); break;
      case '--out': a.out = val(); break;
      case '--compare': a.compare = true; break;
      case '--usdcat': a.usdcat = val(); break;
      case '--lusddiff': a.lusddiff = val(); break;
      case '--serialize-args': a.serializeArgs = val().split(/\s+/).filter(Boolean); break;
      case '-h': case '--help':
        console.log('Usage: node tests/parse-asset-corpus.mjs [--assets DIR] [--optional-assets] [--lusdcat PATH] ' +
          '[--mode load|flatten] [--timeout MS] [--jobs N] [--include-mtlx] [--out DIR]\n' +
          '       [--compare [--usdcat PATH] [--lusddiff PATH]]');
        process.exit(0);
        break;
      default:
        console.error('Unknown arg: ' + k); process.exit(2);
    }
  }
  if (a.mode !== 'load' && a.mode !== 'flatten') {
    console.error(`--mode must be load|flatten (got ${a.mode})`); process.exit(2);
  }
  return a;
}

function validateInputs(a) {
  if (!a.assets) {
    if (a.optionalAssets) {
      console.log('SKIPPED: no asset corpus configured (set USD_WG_ASSETS_DIR or pass --assets).');
      process.exit(77);
    }
    console.error('No asset corpus configured. Pass --assets DIR or set USD_WG_ASSETS_DIR.');
    process.exit(2);
  }
  if (!a.lusdcat) {
    console.error('No lusdcat configured. Pass --lusdcat PATH or set LUSDCAT_PATH.');
    process.exit(2);
  }
}

// ---------------------------------------------------------------------------
// fs helpers
// ---------------------------------------------------------------------------

const USD_EXTS = new Set(['.usd', '.usda', '.usdc', '.usdz']);

async function walk(dir, includeMtlx, out = []) {
  let entries;
  try { entries = await fs.readdir(dir, { withFileTypes: true }); } catch { return out; }
  for (const e of entries) {
    if (e.name === '.git') continue;
    const p = path.join(dir, e.name);
    if (e.isDirectory()) {
      await walk(p, includeMtlx, out);
    } else if (e.isFile()) {
      const ext = path.extname(e.name).toLowerCase();
      if (USD_EXTS.has(ext)) out.push({ file: p, ext });
      else if (ext === '.mtlx' && includeMtlx) out.push({ file: p, ext });
    }
  }
  return out;
}

async function newestMtime(dir, depth = 3) {
  let newest = 0;
  async function rec(d, lvl) {
    if (lvl < 0) return;
    let entries;
    try { entries = await fs.readdir(d, { withFileTypes: true }); } catch { return; }
    for (const e of entries) {
      const p = path.join(d, e.name);
      if (e.isDirectory()) await rec(p, lvl - 1);
      else { try { const s = await fs.stat(p); if (s.mtimeMs > newest) newest = s.mtimeMs; } catch {} }
    }
  }
  await rec(dir, depth);
  return newest;
}

// ---------------------------------------------------------------------------
// run one file
// ---------------------------------------------------------------------------

// Run a process with a timeout. Uses spawn (not execFile) so we can DISCARD
// stdout via opts.discardStdout — `lusdcat -f` prints the whole flattened layer,
// which for large scenes is hundreds of MB and would blow execFile's maxBuffer
// (killing the process and producing a spurious failure). The pass/fail check
// only needs the exit status + stderr, so the main run discards stdout.
function run(bin, args, timeout, opts = {}) {
  return new Promise((resolve) => {
    const t0 = Date.now();
    const capture = !opts.discardStdout;
    let child;
    try {
      child = spawn(bin, args, {
        stdio: ['ignore', capture ? 'pipe' : 'ignore', 'pipe'],
        env: opts.env ? { ...process.env, ...opts.env } : process.env,
      });
    } catch (e) {
      return resolve({ code: null, signal: null, killed: false, stdout: '', stderr: String(e), ms: Date.now() - t0 });
    }
    let stdout = '', stderr = '', killed = false;
    const STDOUT_CAP = 96 * 1024 * 1024, STDERR_CAP = 4 * 1024 * 1024;
    if (capture && child.stdout) child.stdout.on('data', (d) => { if (stdout.length < STDOUT_CAP) stdout += d; });
    child.stderr.on('data', (d) => { if (stderr.length < STDERR_CAP) stderr += d; });
    const timer = setTimeout(() => { killed = true; child.kill('SIGKILL'); }, timeout);
    child.on('error', (e) => { clearTimeout(timer); resolve({ code: null, signal: null, killed, stdout, stderr: stderr || String(e), ms: Date.now() - t0 }); });
    child.on('close', (code, signal) => {
      clearTimeout(timer);
      resolve({ code, signal, killed, stdout, stderr, ms: Date.now() - t0 });
    });
  });
}

// Cap the USDA-text size for the flatten pass/fail run: heavy composed scenes
// (e.g. baked vertex-animation timeSamples) would otherwise serialize to many GB
// of USDA and hang/OOM. With the cap, lusdcat keeps timeSamples compact by
// falling back to in-memory USDC and exits 0 — so the harness measures
// "did composition succeed", not "can we hold the giant USDA text".
const runOne = (bin, flag, file, timeout) =>
  run(bin, [flag, file], timeout,
      { discardStdout: true, env: { LUSDCAT_MAX_USDA_MB: '1024' } });

// Cross-check one asset against the OpenUSD reference. Returns
// { ref: REF_PASS|REF_WARN|REF_FAIL, diff: MATCH|DIFFER|DIFFERR|NA }.
//   ref  — does OpenUSD `usdcat` parse it (and is it warning-clean)?
//   diff — `lusddiff` between usdcat's re-serialization and lusdcat's, i.e. do
//          the two implementations agree on the parsed layer? (layer-level, not
//          composed — `--mode flatten` covers composition separately.)
async function compareOne(a, file, tmpdir, idx) {
  const refOut = path.join(tmpdir, `${idx}.ref.usda`);
  const oursOut = path.join(tmpdir, `${idx}.ours.usda`);

  const u = await run(a.usdcat, [file], a.timeout);
  let ref;
  if (u.killed || u.signal) ref = 'REF_FAIL';
  else if (u.code !== 0) ref = 'REF_FAIL';
  else ref = /\b(error|warning|coding error)\b/i.test(u.stderr) ? 'REF_WARN' : 'REF_PASS';

  let diff = 'NA';
  if (ref !== 'REF_FAIL') {
    // ref serialization via usdcat -o; ours via lusdcat to stdout (-o is buggy).
    const ru = await run(a.usdcat, [file, '-o', refOut], a.timeout);
    const ro = await run(a.lusdcat, [...a.serializeArgs, file], a.timeout);
    if (ru.code === 0 && ro.code === 0 && ro.stdout) {
      try {
        await fs.writeFile(oursOut, ro.stdout);
        const d = await run(a.lusddiff, ['--quiet', refOut, oursOut], a.timeout);
        diff = d.code === 0 ? 'MATCH' : d.code === 1 ? 'DIFFER' : 'DIFFERR';
      } catch { diff = 'DIFFERR'; }
      finally { await fs.rm(refOut, { force: true }); await fs.rm(oursOut, { force: true }); }
    }
  }
  return { ref, diff };
}

function classify(ext, r) {
  if (ext === '.mtlx') return 'SKIP';
  if (r.killed) return 'TIMEOUT';
  if (r.signal) return 'CRASH';
  if (r.code === 0) return /\bWARN : /.test(r.stderr) ? 'WARN' : 'PASS';
  return 'FAIL';
}

// ---------------------------------------------------------------------------
// message categorization
// ---------------------------------------------------------------------------

// A `src/<file>.<ext>:<Func>():<line>` stack frame (lightusd logs these).
const FRAME_RE = /src\/[\w./-]+\.(?:cc|hh|h|cpp|inc):[A-Za-z_]\w*\(\):\d+/;
const FRAME_RE_G = new RegExp(FRAME_RE.source, 'g');
const frameKey = (f) => f.replace(/:\d+$/, ''); // drop the volatile line number

function maskMsg(s) {
  return s
    .replace(/`[^`]*`|'[^']*'|"[^"]*"/g, '‹id›')         // quoted identifiers
    .replace(/0x[0-9a-fA-F][0-9a-fA-F ]*/g, '0x…')        // hex / byte dumps
    .replace(/\b\d+\b/g, 'N')                              // numbers
    .replace(/\/[^\s:]+/g, '‹path›')                       // path-like tokens
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, 200);
}

// Category signature for one `WARN :`/`ERR :` message line, using the deepest
// source frame available (in the message, else anywhere in the file's stderr).
function signatureOf(msg, stderr) {
  const inMsg = msg.match(FRAME_RE);
  if (inMsg) return frameKey(inMsg[0]);
  const all = stderr.match(FRAME_RE_G);
  if (all && all.length) return frameKey(all[all.length - 1]);
  return maskMsg(msg);
}

function* tagged(stderr) {
  for (const line of stderr.split(/\r?\n/)) {
    const m = line.match(/\b(WARN|ERR) : (.*\S)\s*$/);
    if (m) yield { kind: m[1] === 'ERR' ? 'error' : 'warning', text: m[2] };
  }
}

function headline(stderr) {
  let firstWarn = '';
  for (const { kind, text } of tagged(stderr)) {
    if (kind === 'error') return text.replace(/\s+/g, ' ').slice(0, 200);
    if (!firstWarn) firstWarn = text;
  }
  return firstWarn.replace(/\s+/g, ' ').slice(0, 200);
}

// Add to a category map: sig -> {count, files:Set, samples:Set}
function addCat(map, sig, rel, rawMsg) {
  let e = map.get(sig);
  if (!e) { e = { count: 0, files: new Set(), samples: new Set() }; map.set(sig, e); }
  e.count++;
  if (e.files.size < 50) e.files.add(rel);
  if (e.samples.size < 4) e.samples.add(rawMsg.replace(/\s+/g, ' ').slice(0, 160));
}

// ---------------------------------------------------------------------------
// concurrency pool
// ---------------------------------------------------------------------------

async function pool(items, n, fn) {
  const results = new Array(items.length);
  let idx = 0;
  let done = 0;
  const total = items.length;
  async function worker() {
    for (;;) {
      const i = idx++;
      if (i >= total) return;
      results[i] = await fn(items[i], i);
      done++;
      if (done % 25 === 0 || done === total) {
        process.stderr.write(`\r  parsed ${done}/${total}…   `);
      }
    }
  }
  await Promise.all(Array.from({ length: Math.min(n, total) }, worker));
  process.stderr.write('\n');
  return results;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

const ORDER = ['PASS', 'WARN', 'FAIL', 'TIMEOUT', 'CRASH', 'SKIP'];

async function main() {
  const a = parseArgs(process.argv);
  validateInputs(a);
  const flag = a.mode === 'flatten' ? '-f' : '-l';

  // Graceful skip when the asset corpus is not present (e.g. CI without the
  // usd-wg/assets checkout). Exit 0 so it does not fail a regression run.
  try {
    const st = await fs.stat(a.assets);
    if (!st.isDirectory()) throw new Error('not a directory');
  } catch {
    console.log(`SKIPPED: usd-wg asset corpus not found at ${a.assets} ` +
      `(set USD_WG_ASSETS_DIR or pass --assets).`);
    process.exit(0);
  }

  // binary present?
  try { await fs.access(a.lusdcat); }
  catch { console.error(`lusdcat not found at ${a.lusdcat}. Build it (ninja lusdcat) or pass --lusdcat.`); process.exit(2); }

  // staleness hint: binary older than the newest src/ file?
  try {
    const binM = (await fs.stat(a.lusdcat)).mtimeMs;
    const srcM = await newestMtime(path.join(path.dirname(path.dirname(path.resolve(a.lusdcat))), 'src')).catch(() => 0);
    const srcM2 = await newestMtime('src').catch(() => 0);
    const newestSrc = Math.max(srcM, srcM2);
    if (newestSrc && binM < newestSrc) {
      console.warn(`! ${a.lusdcat} (${new Date(binM).toISOString()}) is older than src/ ` +
        `(${new Date(newestSrc).toISOString()}). Rebuild for current results: ninja lusdcat`);
    }
  } catch {}

  // compare-mode setup
  let tmpdir = null;
  if (a.compare) {
    for (const [name, p] of [['usdcat', a.usdcat], ['lusddiff', a.lusddiff]]) {
      try { await fs.access(p); }
      catch { console.error(`--compare: ${name} not found at ${p} (pass --${name}).`); process.exit(2); }
    }
    tmpdir = await fs.mkdtemp(path.join(os.tmpdir(), 'asset-cmp-'));
  }

  const files = await walk(a.assets, a.includeMtlx);
  files.sort((x, y) => x.file.localeCompare(y.file));
  if (!files.length) { console.error(`No USD files under ${a.assets}`); process.exit(2); }

  console.error(`Running ${a.lusdcat} ${flag} over ${files.length} files from ${a.assets} ` +
    `(jobs=${a.jobs}, timeout=${a.timeout}ms)${a.compare ? ` + compare vs ${a.usdcat}` : ''}…`);

  const t0 = Date.now();
  const rows = await pool(files, a.jobs, async ({ file, ext }, i) => {
    const r = await runOne(a.lusdcat, flag, file, a.timeout);
    const status = classify(ext, r);
    const row = { rel: path.relative(a.assets, file), ext, status, code: r.code, signal: r.signal, ms: r.ms, stderr: r.stderr };
    if (a.compare && ext !== '.mtlx') {
      const c = await compareOne(a, file, tmpdir, i);
      row.ref = c.ref; row.diff = c.diff;
    }
    return row;
  });
  const elapsed = ((Date.now() - t0) / 1000).toFixed(1);
  if (tmpdir) await fs.rm(tmpdir, { recursive: true, force: true });

  // tally + categorize
  const totals = Object.fromEntries(ORDER.map((s) => [s, 0]));
  const warnCats = new Map();
  const errCats = new Map();
  for (const row of rows) {
    totals[row.status]++;
    if (row.status === 'SKIP') continue;
    if (row.status === 'WARN') {
      for (const { kind, text } of tagged(row.stderr)) {
        if (kind === 'warning') addCat(warnCats, signatureOf(text, row.stderr), row.rel, text);
      }
    } else if (row.status === 'FAIL') {
      const errs = [...tagged(row.stderr)].filter((t) => t.kind === 'error');
      const primary = errs[0]?.text || `(no ERR message; exit ${row.code})`;
      addCat(errCats, signatureOf(primary, row.stderr), row.rel, primary);
    } else if (row.status === 'CRASH') {
      addCat(errCats, `CRASH: ${row.signal}`, row.rel, `terminated by ${row.signal}`);
    } else if (row.status === 'TIMEOUT') {
      addCat(errCats, `TIMEOUT > ${a.timeout}ms`, row.rel, 'killed after timeout');
    }
  }

  // compare-mode aggregates
  const REF = ['REF_PASS', 'REF_WARN', 'REF_FAIL'];
  let crosstab = null, diffTally = null, bugList = null;
  if (a.compare) {
    crosstab = {};
    for (const s of ORDER) crosstab[s] = Object.fromEntries(REF.map((r) => [r, 0]));
    diffTally = { MATCH: 0, DIFFER: 0, DIFFERR: 0, NA: 0 };
    for (const row of rows) {
      if (row.status === 'SKIP') continue;
      if (row.ref) crosstab[row.status][row.ref]++;
      if (row.diff) diffTally[row.diff]++;
    }
    // lightusd worse than the reference: lightusd could not parse it but OpenUSD could.
    bugList = rows.filter((r) => ['FAIL', 'TIMEOUT', 'CRASH'].includes(r.status) && r.ref && r.ref !== 'REF_FAIL');
  }

  await fs.mkdir(a.out, { recursive: true });
  await fs.writeFile(path.join(a.out, '.gitignore'), '*\n!.gitignore\n');

  // results.tsv
  const tsvHead = a.compare ? 'relpath\tstatus\tref\tdiff\texit\tsignal\tms\theadline'
                            : 'relpath\tstatus\texit\tsignal\tms\theadline';
  const tsv = [tsvHead, ...rows.map((r) => {
    const lead = a.compare ? [r.rel, r.status, r.ref ?? '', r.diff ?? ''] : [r.rel, r.status];
    return [...lead, r.code ?? '', r.signal ?? '', r.ms, headline(r.stderr)].join('\t');
  })].join('\n') + '\n';
  await fs.writeFile(path.join(a.out, 'results.tsv'), tsv);

  // summary.md
  const sortCats = (m) => [...m.entries()].sort((x, y) => y[1].count - x[1].count);
  const catTable = (m) => {
    const rowsS = sortCats(m).map(([sig, e]) =>
      `| ${e.count} | \`${sig}\` | ${[...e.files].slice(0, 3).map((f) => `\`${f}\``).join('<br>')} |`);
    return rowsS.length ? `| Count | Signature | Example files |\n|---:|---|---|\n${rowsS.join('\n')}\n` : '_none_\n';
  };
  const failList = rows.filter((r) => ['FAIL', 'TIMEOUT', 'CRASH'].includes(r.status))
    .map((r) => `- **${r.status}** \`${r.rel}\` (exit ${r.code ?? '-'}${r.signal ? `, ${r.signal}` : ''}) — ${headline(r.stderr) || '(no message)'}`);

  // compare-mode markdown block
  let compareMd = [];
  if (a.compare) {
    compareMd = [
      `## Compare vs OpenUSD reference (\`${a.usdcat}\`)`,
      '',
      '**lightusd status × OpenUSD `usdcat` status** (which side accepts each asset):',
      '',
      `| lightusd \\ ref | ${REF.join(' | ')} |`,
      `|---|${REF.map(() => '---:').join('|')}|`,
      ...ORDER.filter((s) => s !== 'SKIP' && REF.some((r) => crosstab[s][r]))
        .map((s) => `| ${s} | ${REF.map((r) => crosstab[s][r]).join(' | ')} |`),
      '',
      '**`lusddiff` (lightusd re-serialization vs OpenUSD `usdcat` output, layer-level):** ' +
        Object.entries(diffTally).map(([k, v]) => `${k} ${v}`).join(' · '),
      '',
      `**lightusd can't parse but OpenUSD can (parser bugs) — ${bugList.length}:**`,
      '',
      bugList.length ? bugList.map((r) => `- \`${r.rel}\` (${r.status}, ref ${r.ref}) — ${headline(r.stderr) || ''}`).join('\n') : '_none_',
      '',
    ];
  }

  const md = [
    `# Asset parse report — \`lusdcat ${flag}\``,
    '',
    `- Corpus: \`${a.assets}\` — ${files.length} files`,
    `- Tool: \`${a.lusdcat}\` (mode: ${a.mode})`,
    `- Elapsed: ${elapsed}s, jobs ${a.jobs}, timeout ${a.timeout}ms`,
    '',
    '## Status totals',
    '',
    '| Status | Count |',
    '|---|---:|',
    ...ORDER.filter((s) => totals[s] || s !== 'SKIP' || a.includeMtlx).map((s) => `| ${s} | ${totals[s]} |`),
    '',
    ...compareMd,
    `## Warning categories (${[...warnCats.values()].reduce((n, e) => n + e.count, 0)} occurrences)`,
    '',
    catTable(warnCats),
    `## Error categories (per failing file)`,
    '',
    catTable(errCats),
    `## FAIL / TIMEOUT / CRASH files (${failList.length})`,
    '',
    failList.length ? failList.join('\n') : '_none_',
    '',
  ].join('\n');
  await fs.writeFile(path.join(a.out, 'summary.md'), md);

  // summary.json
  const catJson = (m) => sortCats(m).map(([sig, e]) => ({ signature: sig, count: e.count, files: [...e.files], samples: [...e.samples] }));
  await fs.writeFile(path.join(a.out, 'summary.json'), JSON.stringify({
    assets: a.assets, lusdcat: a.lusdcat, mode: a.mode, files: files.length,
    elapsedSec: Number(elapsed), totals,
    ...(a.compare ? { compare: { usdcat: a.usdcat, crosstab, diffTally,
      lightusdOnlyFailures: bugList.map((r) => ({ rel: r.rel, status: r.status, ref: r.ref, headline: headline(r.stderr) })) } } : {}),
    warningCategories: catJson(warnCats), errorCategories: catJson(errCats),
  }, null, 2) + '\n');

  // console
  console.log('\n== Status totals ==');
  for (const s of ORDER) if (totals[s] || (s === 'SKIP' && a.includeMtlx)) console.log(`  ${s.padEnd(8)} ${totals[s]}`);
  if (a.compare) {
    console.log('\n== Compare vs OpenUSD usdcat ==');
    console.log('  diff: ' + Object.entries(diffTally).map(([k, v]) => `${k} ${v}`).join('  '));
    console.log(`  lightusd-only failures (OpenUSD parses, lightusd doesn't): ${bugList.length}`);
    for (const r of bugList) console.log(`    ${r.status}/${r.ref}  ${r.rel}`);
  }
  const top = (label, m) => {
    const cats = sortCats(m).slice(0, 8);
    if (!cats.length) return;
    console.log(`\n== Top ${label} ==`);
    for (const [sig, e] of cats) console.log(`  ${String(e.count).padStart(4)}  ${sig}`);
  };
  top('warning categories', warnCats);
  top('error categories', errCats);
  console.log(`\nWrote ${path.join(a.out, 'summary.md')}, results.tsv, summary.json`);

  // Regression gate.
  if (a.compare && !rows.some((row) => row.ref === 'REF_PASS' || row.ref === 'REF_WARN')) {
    console.error('REGRESSION: OpenUSD could not read any corpus input; no reference comparisons ran.');
    process.exit(1);
  }
  if (Number.isFinite(a.maxFail)) {
    const hard = (totals.FAIL || 0) + (totals.CRASH || 0);
    if (hard > a.maxFail) {
      console.error(`\nREGRESSION: ${hard} FAIL+CRASH > --max-fail ${a.maxFail} ` +
        `(mode ${a.mode}). See ${path.join(a.out, 'summary.md')}.`);
      process.exit(1);
    }
    console.log(`Regression gate OK: ${hard} FAIL+CRASH <= --max-fail ${a.maxFail}.`);
  }
  if (a.compare && Number.isFinite(a.maxDiffer)) {
    const differ = (diffTally.DIFFER || 0) + (diffTally.DIFFERR || 0);
    if (differ > a.maxDiffer) {
      console.error(`\nREGRESSION: ${differ} DIFFER+DIFFERR > --max-differ ${a.maxDiffer} ` +
        `(mode ${a.mode}). See ${path.join(a.out, 'summary.md')}.`);
      process.exit(1);
    }
    console.log(`Compare gate OK: ${differ} DIFFER+DIFFERR <= --max-differ ${a.maxDiffer}.`);
  }
}

main().catch((e) => { console.error(e); process.exit(1); });
