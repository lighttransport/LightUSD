#!/usr/bin/env node
// USD Export CLI Tool — LightUSD WASM
// Load a USD file, export as USDA / USDC / USDZ.
// Usage: node cli/usd-export-cli.js <input-file> [options]

import { LightUSDLoader } from '../src/lightusd/LightUSDLoader.js';
import fs from 'node:fs';
import path from 'node:path';

// ---- Argument Parsing ----

function printHelp() {
  console.log(`
USD Export CLI — LightUSD WASM

Usage:
  node cli/usd-export-cli.js <input-file> [options]

Options:
  --format <fmt>   Export format: usda, usdc, usdz, all (default: usda)
  -o, --output <path>  Output file path (default: derived from input + format)
  --sample         Ignore input file; generate & export a sample scene instead
  -v, --verbose    Verbose output
  -h, --help       Show this help

Examples:
  node cli/usd-export-cli.js model.usdc --format usda
  node cli/usd-export-cli.js model.usdc --format all -o exported
  node cli/usd-export-cli.js --sample --format usdz -o sample.usdz
`);
}

function parseArgs() {
  const args = process.argv.slice(2);
  const options = {
    inputFile: null,
    format: 'usda',
    outputFile: null,
    sample: false,
    verbose: false,
  };

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (arg === '-h' || arg === '--help') {
      printHelp();
      process.exit(0);
    } else if (arg === '--format') {
      options.format = args[++i];
      if (!['usda', 'usdc', 'usdz', 'all'].includes(options.format)) {
        console.error('Error: --format must be usda, usdc, usdz, or all');
        process.exit(1);
      }
    } else if (arg === '-o' || arg === '--output') {
      options.outputFile = args[++i];
    } else if (arg === '--sample') {
      options.sample = true;
    } else if (arg === '-v' || arg === '--verbose') {
      options.verbose = true;
    } else if (!options.inputFile) {
      options.inputFile = arg;
    } else {
      console.error(`Unknown argument: ${arg}`);
      process.exit(1);
    }
  }

  if (!options.inputFile && !options.sample) {
    console.error('Error: Input file is required (or use --sample)');
    printHelp();
    process.exit(1);
  }

  return options;
}

// ---- Helpers ----

function resolveOutputPath(inputFile, format, outputFile) {
  if (outputFile) {
    // If --format all, outputFile is treated as a base name (no extension)
    if (format === 'all') {
      const ext = path.extname(outputFile);
      const base = ext ? outputFile.slice(0, -ext.length) : outputFile;
      return base; // caller appends .usda/.usdc/.usdz
    }
    // If outputFile has no extension, append the format
    if (!path.extname(outputFile)) {
      return `${outputFile}.${format}`;
    }
    return outputFile;
  }
  // Derive from input
  const base = inputFile
    ? inputFile.replace(/\.(usd|usda|usdc|usdz)$/i, '')
    : 'sample';
  return format === 'all' ? base : `${base}.${format}`;
}

// ---- Export Functions ----

function exportUSDA(usd, outPath, verbose) {
  const usda = usd.exportAsUSDA();
  if (!usda || usda.length === 0) {
    console.error('USDA export failed:', usd.error());
    return false;
  }
  fs.writeFileSync(outPath, usda, 'utf-8');
  if (verbose) console.log(`  USDA: ${usda.length} chars`);
  console.log(`Wrote ${outPath}`);
  return true;
}

function exportUSDC(usd, outPath, verbose) {
  const data = usd.exportAsUSDC();
  if (!data) {
    console.error('USDC export failed:', usd.error());
    return false;
  }
  const bytes = new Uint8Array(data);
  fs.writeFileSync(outPath, bytes);
  if (verbose) console.log(`  USDC: ${bytes.length} bytes`);
  console.log(`Wrote ${outPath}`);
  return true;
}

function exportUSDZ(usd, outPath, verbose) {
  const data = usd.exportAsUSDZ();
  if (!data) {
    console.error('USDZ export failed:', usd.error());
    return false;
  }
  const bytes = new Uint8Array(data);
  fs.writeFileSync(outPath, bytes);
  if (verbose) console.log(`  USDZ: ${bytes.length} bytes`);
  console.log(`Wrote ${outPath}`);
  return true;
}

// ---- Main ----

async function main() {
  const opts = parseArgs();

  // Init WASM
  const loader = new LightUSDLoader();
  await loader.init({ useMemory64: false });
  if (opts.verbose) console.log('WASM module loaded.');

  const usd = new loader.native_.NextUSDZConverterNative();

  if (opts.sample) {
    // Generate sample scene (no texture in CLI mode — just geometry)
    const ok = usd.createSampleScene();
    if (!ok) {
      console.error('createSampleScene failed:', usd.error());
      process.exit(1);
    }
    if (opts.verbose) console.log('Sample scene created.');
  } else {
    // Load input file
    const inputPath = opts.inputFile;
    if (!fs.existsSync(inputPath)) {
      console.error(`File not found: ${inputPath}`);
      process.exit(1);
    }

    const fileData = new Uint8Array(fs.readFileSync(inputPath));
    const filename = path.basename(inputPath);

    if (opts.verbose) console.log(`Loading ${filename} (${(fileData.length / 1024).toFixed(1)} KB)...`);

    const ok = usd.loadFromBinary(fileData, filename);
    if (!ok) {
      console.error('Load failed:', usd.error());
      process.exit(1);
    }

    if (usd.warn()) {
      console.warn('Warning:', usd.warn());
    }

    if (opts.verbose) console.log('File loaded.');
  }

  // Export
  const formats = opts.format === 'all' ? ['usda', 'usdc', 'usdz'] : [opts.format];
  const basePath = resolveOutputPath(opts.inputFile, opts.format, opts.outputFile);

  let ok = true;
  for (const fmt of formats) {
    const outPath = opts.format === 'all' ? `${basePath}.${fmt}` : basePath;
    switch (fmt) {
      case 'usda': ok = exportUSDA(usd, outPath, opts.verbose) && ok; break;
      case 'usdc': ok = exportUSDC(usd, outPath, opts.verbose) && ok; break;
      case 'usdz': ok = exportUSDZ(usd, outPath, opts.verbose) && ok; break;
    }
  }

  usd.delete();

  if (!ok) process.exit(1);
}

main().catch(err => {
  console.error('Error:', err);
  process.exit(1);
});
