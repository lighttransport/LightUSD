#!/usr/bin/env node
// bench-flatten.js — peak-RSS A/B harness for the USDZ flatten pipelines.
//
// Spawns the usdzconvert CLI once per (model × pipeline) under `/usr/bin/time -v`
// so each run's TRUE peak resident set size is captured in a fresh process
// (the only reliable way — an in-process measurement misses the emscripten heap
// high-water mark and can't recover from a wasm `abort()`). Emits a markdown
// table comparing `legacy` vs `core`, and — when a run overflows the wasm32
// 2 GB ceiling and the wasm64 glue is present — re-runs it under wasm64 and
// records the result as `<pipeline>-wasm64`.
//
// Usage:
//   node web/js/cli/bench-flatten.js [model.usdz ...] [--pipelines legacy,core]
//                                    [--max-usdc-mb N] [--max-mem-mb N]
//                                    [--stream-textures] [--no-wasm64]
// With no model args, uses the default corpus under ../../../models/.

import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const CLI = path.join(__dirname, 'usdzconvert.js');
const WASM64_GLUE = path.join(__dirname, '../src/lightusd/lightusd_64.js');

function parseArgs() {
  const a = process.argv.slice(2);
  const o = {
    models: [], pipelines: ['legacy', 'core'], maxUsdcMb: 4096, maxMemMb: 4096,
    streamTextures: false, wasm64: true,
  };
  for (let i = 0; i < a.length; i++) {
    if (a[i] === '--pipelines') o.pipelines = a[++i].split(',');
    else if (a[i] === '--max-usdc-mb') o.maxUsdcMb = parseInt(a[++i], 10) || 0;
    else if (a[i] === '--max-mem-mb') o.maxMemMb = parseInt(a[++i], 10) || 0;
    else if (a[i] === '--stream-textures') o.streamTextures = true;
    else if (a[i] === '--no-wasm64') o.wasm64 = false;
    else o.models.push(a[i]);
  }
  if (o.models.length === 0) {
    // No models given: benchmark every .usdz found in the local models/ dir.
    const dir = path.join(__dirname, '../../../models');
    if (fs.existsSync(dir)) {
      for (const m of fs.readdirSync(dir).sort()) {
        if (m.toLowerCase().endsWith('.usdz')) o.models.push(path.join(dir, m));
      }
    }
  }
  return o;
}

// One CLI run: returns { rssKB, ok, aborted, ms, err }.
function runOne(model, pipeline, opts, wasm64) {
  const out = path.join(os.tmpdir(), `bench_${path.basename(model, '.usdz')}_${pipeline}${wasm64 ? '_64' : ''}.usdz`);
  try { fs.rmSync(out, { force: true }); } catch {}
  const args = ['-v', 'node', CLI, model, '--pipeline', pipeline, '-o', out];
  if (opts.maxUsdcMb) args.push('--max-usdc-mb', String(opts.maxUsdcMb));
  if (opts.maxMemMb) args.push('--max-mem-mb', String(opts.maxMemMb));
  if (opts.streamTextures) args.push('--stream-textures');
  const env = { ...process.env };
  if (wasm64) env.LIGHTUSD_WASM64 = '1';
  const t0 = Date.now();
  const r = spawnSync('/usr/bin/time', args, {
    env, encoding: 'utf8', maxBuffer: 64 * 1024 * 1024,
  });
  const ms = Date.now() - t0;
  const stderr = r.stderr || '';
  const combined = stderr + (r.stdout || '');
  let rssKB = 0;
  const m = stderr.match(/Maximum resident set size \(kbytes\):\s*(\d+)/);
  if (m) rssKB = parseInt(m[1], 10);
  const aborted = /Cannot enlarge memory|out of memory|RuntimeError|abort\(|OOM/i.test(combined);
  const ok = r.status === 0 && fs.existsSync(out) && !aborted;
  return { rssKB, ok, aborted, ms, status: r.status };
}

function fmtMB(kb) { return kb ? (kb / 1024).toFixed(0) + ' MB' : '—'; }

function main() {
  const opts = parseArgs();
  const have64 = opts.wasm64 && fs.existsSync(WASM64_GLUE);
  console.log(`bench-flatten: ${opts.models.length} model(s) × [${opts.pipelines.join(', ')}]` +
              `  (wasm64 fallback: ${have64 ? 'available' : 'off'})\n`);

  const rows = [];
  for (const model of opts.models) {
    const inMB = (fs.statSync(model).size / 1048576).toFixed(0);
    const row = { model: path.basename(model), inMB, results: {} };
    for (const p of opts.pipelines) {
      process.stdout.write(`  ${row.model}  ${p} ... `);
      let res = runOne(model, p, opts, false);
      let tag = p;
      if (res.aborted && have64) {
        process.stdout.write(`wasm32 overflow → wasm64 ... `);
        const res64 = runOne(model, p, opts, true);
        row.results[p + '-wasm64'] = res64;
        tag = p + '-wasm64';
        res = res64;
      }
      row.results[p] = row.results[p] || res;
      console.log(res.ok ? `${fmtMB(res.rssKB)} (${(res.ms / 1000).toFixed(1)}s)`
                         : `FAILED (${res.aborted ? 'overflow' : 'status ' + res.status})`);
    }
    rows.push(row);
  }

  // Markdown table.
  const cols = [];
  for (const p of opts.pipelines) { cols.push(p); if (have64) cols.push(p + '-wasm64'); }
  console.log('\n| Model | Input | ' + cols.join(' | ') + ' | core/legacy |');
  console.log('|---|---|' + cols.map(() => '---').join('|') + '|---|');
  for (const row of rows) {
    const cells = cols.map((c) => {
      const r = row.results[c];
      if (!r) return '—';
      return r.ok ? fmtMB(r.rssKB) : (r.aborted ? '**overflow**' : 'fail');
    });
    let ratio = '—';
    const lg = row.results['legacy'], nx = row.results['core'];
    if (lg && nx && lg.ok && nx.ok && nx.rssKB) {
      ratio = (lg.rssKB / nx.rssKB).toFixed(2) + '×';
    }
    console.log(`| ${row.model} | ${row.inMB} MB | ${cells.join(' | ')} | ${ratio} |`);
  }
}

main();
