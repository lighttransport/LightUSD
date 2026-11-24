#!/usr/bin/env node
// MaterialX RenderMaterial Dump CLI Tool
// Usage: node dump-materialx-cli.js <usd-file> [options]

import { LightUSDLoader } from 'lightusd/LightUSDLoader.js';
import fs from 'node:fs';
import path from 'node:path';
import YAML from 'yaml';

// Parse command line arguments
function parseArgs() {
  const args = process.argv.slice(2);
  const options = {
    inputFile: null,
    format: 'json', // 'json', 'yaml', or 'xml'
    outputFile: null,
    materialId: null, // null means all materials
    meshId: null, // null means no mesh dump, number means dump specific mesh
    dumpMesh: false, // dump mesh vertex data
    pretty: true,
    verbose: false,
    summary: false,
    backend: 'core',
    maxMemoryMB: 500
  };

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];

    if (arg === '-h' || arg === '--help') {
      printHelp();
      process.exit(0);
    } else if (arg === '-f' || arg === '--format') {
      options.format = args[++i];
      if (options.format !== 'json' && options.format !== 'xml' && options.format !== 'yaml') {
        console.error('Error: Format must be "json", "yaml", or "xml"');
        process.exit(1);
      }
    } else if (arg === '-o' || arg === '--output') {
      options.outputFile = args[++i];
    } else if (arg === '-m' || arg === '--material') {
      options.materialId = parseInt(args[++i], 10);
    } else if (arg === '--mesh') {
      options.dumpMesh = true;
      const nextArg = args[i + 1];
      if (nextArg && !nextArg.startsWith('-')) {
        options.meshId = parseInt(args[++i], 10);
      }
    } else if (arg === '--no-pretty') {
      options.pretty = false;
    } else if (arg === '--summary') {
      options.summary = true;
    } else if (arg === '--backend') {
      options.backend = args[++i];
      if (!['legacy', 'core', 'auto'].includes(options.backend)) {
        console.error('Error: Backend must be "legacy", "core", or "auto"');
        process.exit(1);
      }
    } else if (arg === '--max-memory-mb') {
      options.maxMemoryMB = Number(args[++i]);
      if (!Number.isFinite(options.maxMemoryMB) || options.maxMemoryMB < 0) {
        console.error('Error: --max-memory-mb must be a non-negative number');
        process.exit(1);
      }
    } else if (arg === '-v' || arg === '--verbose') {
      options.verbose = true;
    } else if (!options.inputFile) {
      options.inputFile = arg;
    }
  }

  if (!options.inputFile) {
    console.error('Error: Input USD file is required');
    printHelp();
    process.exit(1);
  }

  return options;
}

function printHelp() {
  console.log(`
MaterialX RenderMaterial Dump CLI Tool

Usage: node dump-materialx-cli.js <usd-file> [options]

Arguments:
  <usd-file>              USD/USDA/USDC/USDZ file to load

Options:
  -f, --format <format>   Output format: 'json', 'yaml', or 'xml' (default: json)
  -o, --output <file>     Write output to file instead of stdout
  -m, --material <id>     Dump only specific material by ID (default: all)
  --mesh [id]             Dump mesh vertex data (all meshes or specific mesh by ID)
  --no-pretty             Disable pretty-printing for JSON/YAML
  --summary               Print counts/timing only; do not dump material payloads
  --backend <name>        Loader backend: legacy, core, or auto (default: core)
  --max-memory-mb <n>     Native loader memory limit, 0 disables (default: 500)
  -v, --verbose           Enable verbose logging
  -h, --help              Show this help message

Examples:
  # Dump all materials as JSON
  node dump-materialx-cli.js models/polysphere-materialx-001.usda

  # Dump as human-readable YAML
  node dump-materialx-cli.js models/suzanne-pbr.usda -f yaml

  # Dump specific material as MaterialX XML
  node dump-materialx-cli.js models/suzanne-pbr.usda -f xml -m 0

  # Save output to file
  node dump-materialx-cli.js models/teapot-pbr.usdc -o materials.json

  # Verbose mode with YAML output
  node dump-materialx-cli.js models/texturedcube.usdc -f yaml -v
`);
}

function loadFile(filename) {
  try {
    const data = fs.readFileSync(filename);
    const mimeType = 'application/octet-stream';
    const blob = new Blob([data], { type: mimeType });
    const f = new File([blob], path.basename(filename), { type: blob.type });
    return f;
  } catch (err) {
    console.error(`Error loading file: ${err.message}`);
    return null;
  }
}

function formatMaterialOutput(materialData, format, pretty) {
  if (format === 'json') {
    return pretty ? JSON.stringify(materialData, null, 2) : JSON.stringify(materialData);
  } else {
    // XML format - already a string
    return materialData;
  }
}

// Dump mesh vertex data for verification
function dumpMeshData(mesh, meshId) {
  const result = {
    meshId: meshId,
    name: mesh.name || mesh.primName || `Mesh_${meshId}`,
    numPoints: 0,
    numIndices: 0,
    points: [],
    faceVertexIndices: [],
    normals: [],
    texcoords: []
  };

  // Dump points (positions)
  if (mesh.points) {
    result.numPoints = mesh.points.length / 3;
    // Show first few points
    for (let i = 0; i < Math.min(result.numPoints, 10); i++) {
      result.points.push([
        mesh.points[i * 3],
        mesh.points[i * 3 + 1],
        mesh.points[i * 3 + 2]
      ]);
    }
    if (result.numPoints > 10) {
      result.points.push(`... and ${result.numPoints - 10} more`);
    }
  }

  // Dump face vertex indices
  if (mesh.faceVertexIndices) {
    result.numIndices = mesh.faceVertexIndices.length;
    // Show all indices for small meshes, first 30 for larger
    const maxIndices = Math.min(result.numIndices, 30);
    for (let i = 0; i < maxIndices; i++) {
      result.faceVertexIndices.push(mesh.faceVertexIndices[i]);
    }
    if (result.numIndices > 30) {
      result.faceVertexIndices.push(`... and ${result.numIndices - 30} more`);
    }

    // Also show triangles for clarity
    result.triangles = [];
    for (let i = 0; i < Math.min(result.numIndices, 30); i += 3) {
      if (i + 2 < result.numIndices) {
        result.triangles.push([
          mesh.faceVertexIndices[i],
          mesh.faceVertexIndices[i + 1],
          mesh.faceVertexIndices[i + 2]
        ]);
      }
    }
  }

  // Dump normals
  if (mesh.normals) {
    result.numNormals = mesh.normals.length / 3;
    // Show first few normals
    for (let i = 0; i < Math.min(result.numNormals, 10); i++) {
      result.normals.push([
        mesh.normals[i * 3],
        mesh.normals[i * 3 + 1],
        mesh.normals[i * 3 + 2]
      ]);
    }
    if (result.numNormals > 10) {
      result.normals.push(`... and ${result.numNormals - 10} more`);
    }
  }

  // Dump texcoords
  if (mesh.texcoords) {
    result.numTexcoords = mesh.texcoords.length / 2;
    // Show first few texcoords
    for (let i = 0; i < Math.min(result.numTexcoords, 10); i++) {
      result.texcoords.push([
        mesh.texcoords[i * 2],
        mesh.texcoords[i * 2 + 1]
      ]);
    }
    if (result.numTexcoords > 10) {
      result.texcoords.push(`... and ${result.numTexcoords - 10} more`);
    }
  }

  // Compute face normal from first triangle winding to verify
  if (mesh.points && mesh.faceVertexIndices && mesh.faceVertexIndices.length >= 3) {
    const i0 = mesh.faceVertexIndices[0];
    const i1 = mesh.faceVertexIndices[1];
    const i2 = mesh.faceVertexIndices[2];

    const v0 = [mesh.points[i0 * 3], mesh.points[i0 * 3 + 1], mesh.points[i0 * 3 + 2]];
    const v1 = [mesh.points[i1 * 3], mesh.points[i1 * 3 + 1], mesh.points[i1 * 3 + 2]];
    const v2 = [mesh.points[i2 * 3], mesh.points[i2 * 3 + 1], mesh.points[i2 * 3 + 2]];

    // edge1 = v1 - v0, edge2 = v2 - v0
    const edge1 = [v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2]];
    const edge2 = [v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2]];

    // cross product
    const cross = [
      edge1[1] * edge2[2] - edge1[2] * edge2[1],
      edge1[2] * edge2[0] - edge1[0] * edge2[2],
      edge1[0] * edge2[1] - edge1[1] * edge2[0]
    ];

    // normalize
    const len = Math.sqrt(cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
    const faceNormal = len > 0 ? [cross[0] / len, cross[1] / len, cross[2] / len] : [0, 0, 0];

    result.windingAnalysis = {
      firstTriangle: {
        indices: [i0, i1, i2],
        v0: v0,
        v1: v1,
        v2: v2
      },
      computedFaceNormal: faceNormal.map(n => parseFloat(n.toFixed(4))),
      vertexNormal: mesh.normals ? [
        mesh.normals[i0 * 3],
        mesh.normals[i0 * 3 + 1],
        mesh.normals[i0 * 3 + 2]
      ] : null
    };

    // Check if face normal matches vertex normal
    if (mesh.normals) {
      const vn = result.windingAnalysis.vertexNormal;
      const fn = result.windingAnalysis.computedFaceNormal;
      const dot = fn[0] * vn[0] + fn[1] * vn[1] + fn[2] * vn[2];
      result.windingAnalysis.dotProduct = parseFloat(dot.toFixed(4));
      result.windingAnalysis.windingCorrect = dot > 0;
    }
  }

  return result;
}

async function dumpMaterials(options) {
  const timings = {
    start: performance.now(),
    initMs: 0,
    readMs: 0,
    parseMs: 0,
    countMs: 0,
    materialMs: 0,
    totalMs: 0
  };

  if (options.verbose) {
    console.error(`Loading USD file: ${options.inputFile}`);
  }

  // Check if file exists
  if (!fs.existsSync(options.inputFile)) {
    console.error(`Error: File not found: ${options.inputFile}`);
    process.exit(1);
  }

  // Initialize loader
  const initStart = performance.now();
  const loader = new LightUSDLoader();
  await loader.init({ useMemory64: false });
  if (options.maxMemoryMB > 0) {
    loader.setMaxMemoryLimitMB(options.maxMemoryMB);
  }
  timings.initMs = performance.now() - initStart;

  if (options.verbose) {
    console.error('Loader initialized');
  }

  // Load file
  const readStart = performance.now();

  // Read file as ArrayBuffer
  const data = fs.readFileSync(options.inputFile);
  const arrayBuffer = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
  timings.readMs = performance.now() - readStart;

  // Parse USD file with proper conversion to RenderScene
  const parseStart = performance.now();
  const usd = await new Promise((resolve, reject) => {
    loader.parse(arrayBuffer, options.inputFile, resolve, reject, {
      backend: options.backend,
      materialDedup: false,
      mergeMeshes: false,
      mergeMeshesBakeTransform: false,
      flattenRenderTree: false
    });
  });
  timings.parseMs = performance.now() - parseStart;

  if (!usd) {
    console.error('Error: Failed to load USD file');
    process.exit(1);
  }

  if (options.verbose) {
    console.error('USD file loaded successfully');
  }

  // Get number of materials and meshes
  const countStart = performance.now();
  const numMaterials = usd.numMaterials ? usd.numMaterials() : 0;
  const numMeshes = usd.numMeshes ? usd.numMeshes() : 0;
  const numTextures = usd.numTextures ? usd.numTextures() : 0;
  const actualBackend = usd.__backend || options.backend;
  timings.countMs = performance.now() - countStart;

  if (options.summary) {
    timings.totalMs = performance.now() - timings.start;
    const summary = {
      file: options.inputFile,
      fileSizeBytes: data.byteLength,
      backend: actualBackend,
      requestedBackend: options.backend,
      fallbackReason: usd.__backendFallbackReason || null,
      numMeshes,
      numMaterials,
      numTextures,
      timings
    };
    console.log(JSON.stringify(summary, null, 2));
    return;
  }

  if (options.verbose) {
    console.error(`Found ${numMaterials} material(s), ${numMeshes} mesh(es)`);

    // Check for meshes with GeomSubsets
    let subsetsFound = 0;
    for (let i = 0; i < numMeshes; i++) {
      const mesh = usd.getMeshCopy(i);
      if (mesh.materialSubsets && mesh.materialSubsets.length > 0) {
        subsetsFound++;
        console.error(`  Mesh ${i} "${mesh.primName}" has ${mesh.materialSubsets.length} material subset(s)`);
      }
    }
    if (subsetsFound > 0) {
      console.error(`Total: ${subsetsFound} mesh(es) with GeomSubsets (per-face materials)`);
    }
  }

  // Dump mesh data if requested
  if (options.dumpMesh) {
    const meshResults = [];
    const meshIds = options.meshId !== null
      ? [options.meshId]
      : Array.from({ length: numMeshes }, (_, i) => i);

    for (const meshId of meshIds) {
      if (meshId >= numMeshes) {
        console.error(`Warning: Mesh ID ${meshId} out of range (0-${numMeshes - 1})`);
        continue;
      }

      const mesh = usd.getMeshCopy(meshId);
      if (!mesh) {
        console.error(`Warning: Could not get mesh ${meshId}`);
        continue;
      }

      const meshData = dumpMeshData(mesh, meshId);
      meshResults.push(meshData);
    }

    // Output mesh data
    let output;
    if (options.format === 'yaml') {
      output = YAML.stringify(meshResults, {
        indent: 2,
        lineWidth: 0,
        minContentWidth: 0
      });
    } else {
      output = options.pretty
        ? JSON.stringify(meshResults, null, 2)
        : JSON.stringify(meshResults);
    }

    if (options.outputFile) {
      fs.writeFileSync(options.outputFile, output, 'utf8');
      if (options.verbose) {
        console.error(`\nMesh data written to: ${options.outputFile}`);
      }
    } else {
      console.log(output);
    }
    return;
  }

  if (numMaterials === 0) {
    console.error('Warning: No materials found in USD file');
    if (!options.outputFile) {
      console.log(options.format === 'json' ? '[]' : '<?xml version="1.0"?>\n<materials/>\n');
    }
    return;
  }

  const results = [];

  // Determine which materials to dump
  const materialIds = options.materialId !== null
    ? [options.materialId]
    : Array.from({ length: numMaterials }, (_, i) => i);

  // Dump materials
  const materialStart = performance.now();
  for (const matId of materialIds) {
    if (matId >= numMaterials) {
      console.error(`Warning: Material ID ${matId} out of range (0-${numMaterials - 1})`);
      continue;
    }

    if (options.verbose) {
      console.error(`\nProcessing material ${matId}...`);
    }

    try {
      // For YAML, we get JSON from the native module and convert it
      const nativeFormat = (options.format === 'yaml') ? 'json' : options.format;
      const result = usd.getMaterialWithFormat(matId, nativeFormat);

      if (result.error) {
        console.error(`Error getting material ${matId}: ${result.error}`);
        continue;
      }

      if (options.format === 'json' || options.format === 'yaml') {
        const materialData = JSON.parse(result.data);

        if (options.verbose) {
          console.error(`  Format: ${options.format}`);
          console.error(`  Has OpenPBR: ${materialData.hasOpenPBR || false}`);
          console.error(`  Has UsdPreviewSurface: ${materialData.hasUsdPreviewSurface || false}`);
          if (materialData.name) {
            console.error(`  Name: ${materialData.name}`);
          }
        }

        results.push(materialData);
      } else {
        // XML format
        if (options.verbose) {
          console.error(`  Format: ${result.format}`);
          console.error(`  XML length: ${result.data.length} bytes`);
        }

        results.push({
          materialId: matId,
          format: result.format,
          xml: result.data
        });
      }
    } catch (err) {
      console.error(`Error processing material ${matId}: ${err.message}`);
    }
  }
  timings.materialMs = performance.now() - materialStart;

  // Output results
  let output;

  if (options.format === 'json') {
    output = options.pretty
      ? JSON.stringify(results, null, 2)
      : JSON.stringify(results);
  } else if (options.format === 'yaml') {
    // YAML format - human-readable
    output = YAML.stringify(results, {
      indent: 2,
      lineWidth: 0,  // Don't wrap lines
      minContentWidth: 0,
      defaultKeyType: 'PLAIN',
      defaultStringType: 'QUOTE_DOUBLE'
    });
  } else {
    // XML format - combine multiple materials
    if (results.length === 1) {
      output = results[0].xml;
    } else {
      // Wrap multiple materials in a container
      const xmlParts = ['<?xml version="1.0"?>\n<materials>\n'];
      for (const result of results) {
        xmlParts.push(`  <!-- Material ${result.materialId} -->\n`);
        // Indent each line of the XML
        const indented = result.xml
          .split('\n')
          .map(line => line.trim() ? '  ' + line : line)
          .join('\n');
        xmlParts.push(indented);
        xmlParts.push('\n\n');
      }
      xmlParts.push('</materials>\n');
      output = xmlParts.join('');
    }
  }

  // Write to file or stdout
  if (options.outputFile) {
    try {
      fs.writeFileSync(options.outputFile, output, 'utf8');
      if (options.verbose) {
        console.error(`\nOutput written to: ${options.outputFile}`);
      }
    } catch (err) {
      console.error(`Error writing output file: ${err.message}`);
      process.exit(1);
    }
  } else {
    console.log(output);
  }

  if (options.verbose) {
    timings.totalMs = performance.now() - timings.start;
    console.error(`Timing: init=${timings.initMs.toFixed(1)} ms read=${timings.readMs.toFixed(1)} ms parse=${timings.parseMs.toFixed(1)} ms materials=${timings.materialMs.toFixed(1)} ms total=${timings.totalMs.toFixed(1)} ms`);
    console.error('\nDone!');
  }
}

// Main execution
async function main() {
  try {
    const options = parseArgs();
    await dumpMaterials(options);
  } catch (err) {
    console.error(`Fatal error: ${err.message}`);
    if (process.env.DEBUG) {
      console.error(err.stack);
    }
    process.exit(1);
  }
}

main();
