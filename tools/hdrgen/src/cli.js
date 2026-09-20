#!/usr/bin/env node
/**
 * HDRGen CLI - Command line interface for HDR environment map generation
 *
 * Copyright 2024 - Present, Light Transport Entertainment Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

import { HDRGenerator, Vec3 } from './hdrgen.js';
import * as path from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

// ============================================================================
// CLI Argument Parser
// ============================================================================

function parseArgs(argv) {
  const args = {
    preset: 'white-furnace',
    width: 2048,
    height: 1024,
    projection: 'latlong',
    format: 'hdr',
    output: null,
    importanceMap: null,
    exrCompression: 'zips',
    prefilter: null,
    presetOptions: {},
    rotation: 0,
    intensityScale: 1.0,
    tonemapOptions: {
      exposure: 0.0,
      gamma: 2.2,
      method: 'reinhard'
    },
    help: false
  };

  for (let i = 2; i < argv.length; i++) {
    const arg = argv[i];

    if (arg === '-h' || arg === '--help') {
      args.help = true;
    } else if (arg === '-p' || arg === '--preset') {
      args.preset = argv[++i];
    } else if (arg === '-w' || arg === '--width') {
      args.width = parseInt(argv[++i]);
    } else if (arg === '--height') {
      args.height = parseInt(argv[++i]);
    } else if (arg === '--projection') {
      args.projection = argv[++i];
    } else if (arg === '-f' || arg === '--format') {
      args.format = argv[++i];
    } else if (arg === '-o' || arg === '--output') {
      args.output = argv[++i];
    } else if (arg === '--importance-map') {
      args.importanceMap = argv[++i];
    } else if (arg === '--exr-compression') {
      args.exrCompression = argv[++i];
    } else if (arg === '--prefilter-dir') {
      args.prefilter = args.prefilter || {};
      args.prefilter.directory = argv[++i];
    } else if (arg === '--prefilter-size') {
      args.prefilter = args.prefilter || {};
      args.prefilter.width = parseInt(argv[++i]);
      args.prefilter.height = Math.max(1, Math.floor(args.prefilter.width / 2));
    } else if (arg === '--prefilter-levels') {
      args.prefilter = args.prefilter || {};
      args.prefilter.levels = parseInt(argv[++i]);
    } else if (arg === '--prefilter-samples') {
      args.prefilter = args.prefilter || {};
      args.prefilter.samples = parseInt(argv[++i]);
    }
    // Transform options
    else if (arg === '--rotation' || arg === '--rotate') {
      args.rotation = parseFloat(argv[++i]);
    } else if (arg === '--intensity-scale' || arg === '--scale') {
      args.intensityScale = parseFloat(argv[++i]);
    }
    // Tone mapping options (for LDR output)
    else if (arg === '--exposure') {
      args.tonemapOptions.exposure = parseFloat(argv[++i]);
    } else if (arg === '--gamma') {
      args.tonemapOptions.gamma = parseFloat(argv[++i]);
    } else if (arg === '--tonemap-method') {
      args.tonemapOptions.method = argv[++i];
    }
    // Sun/Sky options
    else if (arg === '--sun-elevation') {
      args.presetOptions.sunElevation = parseFloat(argv[++i]);
    } else if (arg === '--sun-azimuth') {
      args.presetOptions.sunAzimuth = parseFloat(argv[++i]);
    } else if (arg === '--sun-intensity') {
      args.presetOptions.sunIntensity = parseFloat(argv[++i]);
    } else if (arg === '--sky-intensity') {
      args.presetOptions.skyIntensity = parseFloat(argv[++i]);
    } else if (arg === '--ground-intensity') {
      args.presetOptions.groundIntensity = parseFloat(argv[++i]);
    }
    // Studio options
    else if (arg === '--key-intensity') {
      args.presetOptions.keyIntensity = parseFloat(argv[++i]);
    } else if (arg === '--fill-intensity') {
      args.presetOptions.fillIntensity = parseFloat(argv[++i]);
    } else if (arg === '--rim-intensity') {
      args.presetOptions.rimIntensity = parseFloat(argv[++i]);
    } else if (arg === '--ambient-intensity') {
      args.presetOptions.ambientIntensity = parseFloat(argv[++i]);
    }
    // White furnace options
    else if (arg === '--intensity') {
      args.presetOptions.intensity = parseFloat(argv[++i]);
    }
  }

  return args;
}

function printHelp() {
  console.log(`
HDRGen - Synthetic HDR/EXR Environment Map Generator

USAGE:
  hdrgen [OPTIONS]

OPTIONS:
  -h, --help              Show this help message
  -p, --preset <name>     Preset: white-furnace, sun-sky, sunset, overcast, studio
  -w, --width <px>        Width in pixels [default: 2048]
  --height <px>           Height in pixels [default: 1024]
  --projection <type>     Projection type (latlong, cubemap) [default: latlong]
  -f, --format <fmt>      Output format (hdr, exr, png, bmp, jpg) [default: hdr]
  -o, --output <path>     Output file path [default: output/<preset>_<proj>.<fmt>]
  --importance-map <path> Write lat-long luminance/solid-angle CDFs as JSON
  --exr-compression <mode> OpenEXR compression: zips or none [default: zips]
  --prefilter-dir <path>   Write diffuse/GGX IBL maps and manifest to directory
  --prefilter-size <px>    Specular level-0 width [default: 64]
  --prefilter-levels <n>   Number of GGX roughness levels [default: 6]
  --prefilter-samples <n>  Samples per output texel [default: 64]

TRANSFORM OPTIONS:
  --rotation <deg>        Rotate environment map (degrees, +CCW) [default: 0]
  --intensity-scale <val> Global intensity multiplier [default: 1.0]

LDR/TONE MAPPING OPTIONS (for PNG/BMP/JPG output):
  --exposure <ev>         Exposure adjustment in EV [default: 0.0]
  --gamma <val>           Gamma correction [default: 2.2]
  --tonemap-method <m>    Method: simple, reinhard, aces [default: reinhard]

WHITE FURNACE OPTIONS:
  --intensity <val>       Furnace intensity [default: 1.0]

SUN & SKY OPTIONS:
  --sun-elevation <deg>   Sun elevation angle in degrees [default: 45]
  --sun-azimuth <deg>     Sun azimuth angle in degrees [default: 135]
  --sun-intensity <val>   Sun disk intensity [default: 100.0]
  --sky-intensity <val>   Base sky intensity [default: 0.5]
  --ground-intensity <v>  Overcast lower-hemisphere bounce [default: 0.08]

STUDIO LIGHTING OPTIONS:
  --key-intensity <val>   Key light intensity [default: 50.0]
  --fill-intensity <val>  Fill light intensity [default: 10.0]
  --rim-intensity <val>   Rim light intensity [default: 20.0]
  --ambient-intensity <val> Ambient light intensity [default: 0.5]

EXAMPLES:
  # Generate white furnace for testing
  hdrgen --preset white-furnace -o output/furnace.hdr

  # Generate sun & sky with low sun
  hdrgen --preset sun-sky --sun-elevation 15 --sun-azimuth 90 -o output/sunset.hdr

  # Generate studio lighting as cubemap
  hdrgen --preset studio --projection cubemap --width 512 -o output/studio

  # High-resolution sky with intense sun
  hdrgen -p sun-sky -w 4096 --height 2048 --sun-intensity 200 -o output/sky_4k.hdr

  # Rotate environment 90 degrees
  hdrgen -p sun-sky --rotation 90 -o output/sky_rotated.hdr

  # Scale intensity 2x and output as PNG
  hdrgen -p studio --intensity-scale 2.0 -f png -o output/studio.png

  # Generate LDR preview with custom exposure
  hdrgen -p sun-sky -f png --exposure 1.0 --gamma 2.2 -o output/sky_preview.png

  # Generate BMP with ACES tone mapping
  hdrgen -p studio -f bmp --tonemap-method aces --exposure -0.5 -o output/studio.bmp

PRESETS:
  white-furnace  - Uniform white environment for energy conservation testing
  sun-sky        - Procedural sky with sun disk and atmospheric gradient
  sunset         - Warm low sun with a dusk sky gradient
  overcast       - CIE-style diffuse sky with neutral ground bounce
  studio         - 3-point lighting setup (key, fill, rim lights)

FORMATS:
  HDR Formats:
    hdr          - Radiance RGBE format (.hdr)
    exr          - OpenEXR v2 float scanlines (.exr; ZIPS by default)

  LDR Formats (with automatic tone mapping):
    png          - PNG format (.png) [uncompressed]
    bmp          - BMP format (.bmp) [24-bit RGB]
    jpg/jpeg     - JPEG format [converts to BMP, requires jpeg-js for true JPEG]

PROJECTIONS:
  latlong        - Equirectangular lat-long projection (single image)
  cubemap        - Cubemap projection (6 faces: +X, -X, +Y, -Y, +Z, -Z)

OUTPUT:
  - For latlong: Single file at specified path
  - For cubemap: Six files with face suffixes (_+X, _-X, _+Y, _-Y, _+Z, _-Z)
  - Default output directory: tools/hdrgen/output/

NOTES:
  - All outputs are in linear color space (no gamma encoding)
  - HDR values can exceed 1.0 (high dynamic range)
  - Cubemap faces use OpenGL convention (+Y is up)
  - Generated maps are suitable for IBL (Image-Based Lighting) in renderers

For more information, see: tools/hdrgen/README.md
`);
}

// ============================================================================
// Main CLI Entry Point
// ============================================================================

async function main() {
  const args = parseArgs(process.argv);

  if (args.help) {
    printHelp();
    process.exit(0);
  }

  // Generate default output path if not specified
  if (!args.output) {
    const outputDir = path.join(__dirname, '../output');
    const filename = `${args.preset}_${args.projection}.${args.format}`;
    args.output = path.join(outputDir, filename);
  }

  // Ensure output directory exists
  const outputDir = path.dirname(args.output);
  try {
    const fs = await import('fs');
    if (!fs.existsSync(outputDir)) {
      fs.mkdirSync(outputDir, { recursive: true });
    }
    if (args.importanceMap) {
      fs.mkdirSync(path.dirname(args.importanceMap), { recursive: true });
    }
  } catch (err) {
    console.error(`Error creating output directory: ${err.message}`);
    process.exit(1);
  }

  try {
    // Generate environment map
    HDRGenerator.generate({
      preset: args.preset,
      width: args.width,
      height: args.height,
      projection: args.projection,
      format: args.format,
      output: args.output,
      presetOptions: args.presetOptions,
      rotation: args.rotation,
      intensityScale: args.intensityScale,
      tonemapOptions: args.tonemapOptions,
      importanceMap: args.importanceMap,
      exrCompression: args.exrCompression,
      prefilter: args.prefilter
    });

    console.log('\n✓ Generation complete!');
    console.log(`Output: ${args.output}\n`);

  } catch (err) {
    console.error(`\n✗ Error: ${err.message}`);
    console.error(err.stack);
    process.exit(1);
  }
}

// Run CLI
main();
