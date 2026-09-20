# HDRGen - Synthetic HDR/EXR Environment Map Generator

A pure Node.js tool for generating synthetic HDR environment maps for testing, debugging, and prototyping PBR materials and IBL (Image-Based Lighting) setups.

## Features

- **Pure JavaScript/Node.js** - No external dependencies, no native bindings
- **Multiple Presets** - White furnace, sun & sky, sunset, overcast, studio lighting
- **Flexible Output** - HDR (Radiance RGBE) and EXR (OpenEXR float scanline) formats
- **Dual Projections** - Equirectangular (lat-long) and cubemap
- **Physically-Based** - Linear color space, HDR values, proper intensity scaling
- **Customizable** - Extensive options for each preset
- **CLI & API** - Use from command line or import as library

## Installation

```bash
cd tools/hdrgen
npm install
```

Make CLI globally available:
```bash
npm link
```

## Quick Start

### Generate White Furnace (Energy Conservation Test)

```bash
node src/cli.js --preset white-furnace -o output/furnace.hdr
```

### Generate Sun & Sky Environment

```bash
node src/cli.js --preset sun-sky --sun-elevation 45 -o output/sky.hdr
```

### Generate Studio Lighting

```bash
node src/cli.js --preset studio -o output/studio.hdr
```

### Generate Cubemap

```bash
node src/cli.js --preset studio --projection cubemap --width 512 -o output/studio_cube
```

### Generate All Examples

```bash
npm run example
```

## CLI Usage

```bash
hdrgen [OPTIONS]
```

### General Options

| Option | Description | Default |
|--------|-------------|---------|
| `-h, --help` | Show help message | - |
| `-p, --preset <name>` | Preset name (white-furnace, sun-sky, sunset, overcast, studio) | `white-furnace` |
| `-i, --input <path>` | Read an existing lat-long HDR or float EXR panorama | disabled |
| `-w, --width <px>` | Width in pixels | `2048` |
| `--height <px>` | Height in pixels | `1024` |
| `--projection <type>` | Projection type (latlong, cubemap) | `latlong` |
| `-f, --format <fmt>` | Output format (hdr, exr) | `hdr` |
| `-o, --output <path>` | Output file path | `output/<preset>_<proj>.<fmt>` |
| `--importance-map <path>` | Write luminance/solid-angle sampling CDFs as JSON | disabled |
| `--exr-compression <mode>` | OpenEXR compression (`zips` or `none`) | `zips` |
| `--prefilter-dir <path>` | Write diffuse/GGX IBL maps and a manifest | disabled |
| `--prefilter-size <px>` | Specular level-0 width | `64` |
| `--prefilter-levels <n>` | GGX roughness mip count | `6` |
| `--prefilter-samples <n>` | Deterministic samples per output texel | `64` |
| `--time-sequence <a:b:s>` | Inclusive solar-hour range (`start:end:step`) | disabled |

The importance-map JSON contains a normalized `rowCdf` with `height + 1`
entries and row-major `conditionalCdf` data with
`height * (width + 1)` entries. Weights use Rec. 709 luminance multiplied by
`sin(theta)` at each texel center, so a renderer can select a row, then a
column, without over-sampling the poles of a lat-long map. The file also records
its schema `version`, projection, dimensions, and unnormalized `totalWeight`.

```bash
hdrgen -p sun-sky -w 1024 --height 512 \
  -o output/sky.hdr --importance-map output/sky.importance.json
```

Existing Radiance HDR and full-resolution float OpenEXR panoramas can use the
same rotation, intensity, importance-map, cubemap, and prefilter pipeline:

```bash
hdrgen -i input/studio.exr --rotation 90 --intensity-scale 0.5 \
  -f exr -o output/studio-rotated.exr
```

Generate a deterministic daylight sequence with numbered frames and a JSON
manifest. Runs use a simple 06:00 sunrise, 12:00 zenith, 18:00 sunset arc:

```bash
hdrgen -p sun-sky --time-sequence 6:18:0.5 -w 1024 --height 512 \
  -f exr -o output/day/sky-####.exr
```

IBL prefiltering writes cosine-convolved diffuse irradiance, GGX specular
roughness levels, and `manifest.json`. All maps use lat-long projection and
float EXR so they can be consumed without RGBE quantization.

```bash
hdrgen -p studio -o output/studio.exr -f exr \
  --prefilter-dir output/studio-ibl --prefilter-size 128 \
  --prefilter-levels 7 --prefilter-samples 128
```

### White Furnace Options

| Option | Description | Default |
|--------|-------------|---------|
| `--intensity <val>` | Furnace intensity | `1.0` |

**Purpose:** Uniform white environment for testing energy conservation. A perfect diffuse BRDF should integrate to exactly the furnace intensity.

### Sun & Sky Options

| Option | Description | Default |
|--------|-------------|---------|
| `--sun-elevation <deg>` | Sun elevation angle (0=horizon, 90=zenith) | `45` |
| `--sun-azimuth <deg>` | Sun azimuth angle (0=north, 90=east) | `135` |
| `--sun-intensity <val>` | Sun disk intensity | `100.0` |
| `--sky-intensity <val>` | Base sky intensity | `0.5` |

**Purpose:** Procedural outdoor environment with directional sun and atmospheric sky gradient.

### Studio Lighting Options

| Option | Description | Default |
|--------|-------------|---------|
| `--key-intensity <val>` | Key light intensity (main light) | `50.0` |
| `--fill-intensity <val>` | Fill light intensity (shadow fill) | `10.0` |
| `--rim-intensity <val>` | Rim light intensity (back highlight) | `20.0` |
| `--ambient-intensity <val>` | Ambient light intensity | `0.5` |

**Purpose:** Professional 3-point lighting setup for product visualization and character lighting.

### Sunset and Overcast

`sunset` supplies warm low-sun defaults while accepting the same sun and sky
options as `sun-sky`. `overcast` implements a smooth CIE-style diffuse sky and
accepts `--sky-intensity` plus `--ground-intensity` for its lower-hemisphere
bounce.

```bash
hdrgen -p sunset --sun-azimuth 240 -o output/sunset.hdr
hdrgen -p overcast --sky-intensity 1.5 --ground-intensity 0.1 \
  -o output/overcast.hdr
```

## Presets

### 1. White Furnace

Uniform white environment at specified intensity. Essential for validating PBR material energy conservation.

**Use Cases:**
- Energy conservation testing
- BRDF validation
- Exposure calibration
- Albedo verification

**Example:**
```bash
# Standard furnace test
hdrgen -p white-furnace --intensity 1.0 -o output/furnace.hdr

# High intensity test
hdrgen -p white-furnace --intensity 10.0 -o output/furnace_10x.hdr
```

**Expected Results:**
- Perfectly diffuse white material should appear with albedo = intensity
- Metals should appear dark (no diffuse reflection)
- Specular reflections should be uniform in all directions

### 2. Sun & Sky

Procedural sky with sun disk and atmospheric gradient. Simplified Hosek-Wilkie sky model approximation.

**Use Cases:**
- Outdoor scene lighting
- Architecture visualization
- Product photography (outdoor)
- Time-of-day studies

**Parameters:**
- **Sun Elevation:** Height of sun above horizon (0°-90°)
  - `0°-10°`: Sunrise/sunset (warm, long shadows)
  - `30°-60°`: Mid-day (balanced, natural)
  - `80°-90°`: Noon (overhead, harsh)
- **Sun Azimuth:** Compass direction of sun (0°-360°)
  - `0°`: North
  - `90°`: East
  - `180°`: South
  - `270°`: West

**Examples:**
```bash
# Afternoon sun (front-right)
hdrgen -p sun-sky --sun-elevation 45 --sun-azimuth 135 -o output/afternoon.hdr

# Sunset (low sun, west)
hdrgen -p sun-sky --sun-elevation 5 --sun-azimuth 270 --sun-intensity 150 -o output/sunset.hdr

# Noon (overhead)
hdrgen -p sun-sky --sun-elevation 85 --sun-azimuth 0 --sun-intensity 200 -o output/noon.hdr

# Sunrise (low sun, east)
hdrgen -p sun-sky --sun-elevation 10 --sun-azimuth 90 --sun-intensity 120 -o output/sunrise.hdr
```

### 3. Studio Lighting

3-point lighting setup with key, fill, and rim lights. Classic photography and cinematography lighting pattern.

**Use Cases:**
- Product rendering
- Character lighting
- Controlled lighting studies
- Material comparison

**Light Configuration:**
- **Key Light:** Main light source (front-right, elevated)
  - Position: 45° right, 30° up
  - Color: Warm (slightly yellow)
- **Fill Light:** Shadow fill (front-left, lower)
  - Position: 30° left, 20° up
  - Color: Cool (slightly blue)
- **Rim Light:** Edge/separation light (back, elevated)
  - Position: Behind subject, 25° up
  - Color: Neutral white

**Examples:**
```bash
# Standard studio setup
hdrgen -p studio -o output/studio.hdr

# High-key lighting (bright, low contrast)
hdrgen -p studio --key-intensity 80 --fill-intensity 30 --ambient-intensity 1.0 -o output/highkey.hdr

# Low-key lighting (dramatic, high contrast)
hdrgen -p studio --key-intensity 30 --fill-intensity 5 --rim-intensity 40 --ambient-intensity 0.1 -o output/lowkey.hdr

# Product lighting (strong rim for edge definition)
hdrgen -p studio --key-intensity 60 --fill-intensity 15 --rim-intensity 50 -o output/product.hdr
```

## Projection Types

### Equirectangular (Lat-Long)

Single image with 2:1 aspect ratio (e.g., 2048x1024). Standard format for environment maps.

**Characteristics:**
- Full 360° horizontal, 180° vertical coverage
- Distortion at poles (top and bottom stretched)
- Most compact format (single file)
- Widely supported by renderers and DCCs

**Output:** Single `.hdr` or `.exr` file

```bash
hdrgen -p sun-sky --projection latlong -w 2048 --height 1024 -o output/sky.hdr
```

### Cubemap

Six square images representing faces of a cube. No distortion, but requires 6 separate files.

**Face Order (OpenGL Convention):**
- `+X`: Right
- `-X`: Left
- `+Y`: Top (up)
- `-Y`: Bottom (down)
- `+Z`: Front
- `-Z`: Back

**Characteristics:**
- No polar distortion
- Uniform sampling across all directions
- Larger total file size (6 files)
- Preferred for real-time rendering and importance sampling

**Output:** Six files with face suffixes: `_+X.hdr`, `_-X.hdr`, etc.

```bash
hdrgen -p studio --projection cubemap --width 512 -o output/studio_cube
# Creates: studio_cube_+X.hdr, studio_cube_-X.hdr, ..., studio_cube_-Z.hdr
```

## File Formats

### HDR (Radiance RGBE)

Standard format for HDR images. Widely supported, compact, 8-bit per channel with shared exponent.

**Specifications:**
- Format: RGBE (RGB + 8-bit shared exponent)
- Precision: ~7 decimal digits
- Dynamic range: 10^-38 to 10^38
- File size: 4 bytes per pixel
- Extension: `.hdr`

**Advantages:**
- Small file size
- Fast to write
- Universal support
- Good for most IBL use cases

**Limitations:**
- Limited precision (8-bit mantissa)
- No alpha channel
- No arbitrary metadata

### EXR (OpenEXR)

High-precision format from ILM. HDRGen writes OpenEXR v2 scanlines with 32-bit
floating-point B, G, and R channels. Per-scanline ZIPS compression is enabled
by default; use `--exr-compression none` for uncompressed compatibility output.

**Specifications:**
- Format: Float (32-bit) per channel
- Precision: 24-bit mantissa
- File size: 12 bytes per pixel plus scanline/header overhead
- Extension: `.exr`

## API Usage

### As ES6 Module

```javascript
import { HDRGenerator } from './src/hdrgen.js';

// Generate lat-long sun & sky
const result = HDRGenerator.generate({
  preset: 'sun-sky',
  width: 2048,
  height: 1024,
  projection: 'latlong',
  format: 'hdr',
  output: 'output/sky.hdr',
  presetOptions: {
    sunElevation: 45,
    sunAzimuth: 135,
    sunIntensity: 100.0
  }
});

// Access raw image data
const { latLongImage } = result;
console.log(`Generated ${latLongImage.width}x${latLongImage.height} HDR image`);
```

### Programmatic Generation

```javascript
import { HDRImage, EnvMapPresets, HDRWriter } from './src/hdrgen.js';

// Create custom environment
const image = new HDRImage(2048, 1024);

// Apply preset
EnvMapPresets.sunSky(image, {
  sunElevation: 30,
  sunIntensity: 150.0
});

// Write to file
HDRWriter.writeRGBE(image, 'output/custom.hdr');
```

### Custom Procedural Environments

```javascript
import { HDRImage, Vec3 } from './src/hdrgen.js';

const image = new HDRImage(1024, 512);

for (let y = 0; y < image.height; y++) {
  for (let x = 0; x < image.width; x++) {
    const u = x / image.width;
    const v = y / image.height;

    // Custom procedural function
    const r = Math.sin(u * Math.PI * 4) * 0.5 + 0.5;
    const g = Math.sin(v * Math.PI * 4) * 0.5 + 0.5;
    const b = 0.5;

    image.setPixel(x, y, r * 10.0, g * 10.0, b * 10.0);
  }
}

HDRWriter.writeRGBE(image, 'output/procedural.hdr');
```

## Resolution Guidelines

### Lat-Long Environments

| Use Case | Resolution | Aspect | File Size (HDR) |
|----------|-----------|--------|-----------------|
| Quick preview | 512x256 | 2:1 | ~0.5 MB |
| Development | 1024x512 | 2:1 | ~2 MB |
| Production (low) | 2048x1024 | 2:1 | ~8 MB |
| Production (standard) | 4096x2048 | 2:1 | ~32 MB |
| Production (high) | 8192x4096 | 2:1 | ~128 MB |

### Cubemap Environments

| Use Case | Face Size | Total Resolution | File Size (HDR x6) |
|----------|----------|------------------|-------------------|
| Preview | 128x128 | 128 | ~0.4 MB |
| Low | 256x256 | 256 | ~1.5 MB |
| Medium | 512x512 | 512 | ~6 MB |
| High | 1024x1024 | 1K | ~24 MB |
| Ultra | 2048x2048 | 2K | ~96 MB |

**Recommendation:** Start with 1024x512 lat-long for development, use 2048x1024 or higher for final renders.

## Testing & Validation

### Energy Conservation Test

White furnace with diffuse white material should reflect exactly the furnace intensity:

```bash
# Generate test environment
hdrgen -p white-furnace --intensity 1.0 -o output/furnace_test.hdr

# In your renderer:
# 1. Load furnace_test.hdr
# 2. Create material: diffuse white (albedo = 1.0)
# 3. Render sphere
# 4. Expected result: sphere appears with color = (1.0, 1.0, 1.0)
```

### Sun Direction Validation

```bash
# Generate known sun position (45° elevation, 90° east)
hdrgen -p sun-sky --sun-elevation 45 --sun-azimuth 90 -o output/sun_test.hdr

# Visual check: Bright spot should be in upper-right quadrant of lat-long image
# Azimuth 90° = right side of image (east)
# Elevation 45° = mid-height of image
```

### Studio Lighting Validation

```bash
# Generate studio environment
hdrgen -p studio -o output/studio_test.hdr

# Expected light positions:
# - Key light: Front-right (bright warm spot)
# - Fill light: Front-left (dimmer cool glow)
# - Rim light: Back (sharp highlight)
```

## Importing in DCCs

### Blender

1. Switch to **Shading** workspace
2. Select **World** shader
3. Add **Environment Texture** node
4. Open generated `.hdr` file
5. Connect to **Background** shader

### Houdini

1. Create **Environment Light**
2. Set **Environment Map** to generated `.hdr` file
3. Adjust **Intensity** if needed

### Maya

1. Create **Skydome Light**
2. Load generated `.hdr` in **Color** attribute
3. Set **Exposure** if needed

### Unreal Engine

1. Import `.hdr` as **Texture Cube** (for cubemaps) or **Texture 2D** (for lat-long)
2. Create **Skylight** actor
3. Set **Source Type** to **SLS Specified Cubemap**
4. Assign texture

### Unity

1. Import `.hdr` file
2. Set **Texture Shape** to **Cube** (for cubemap) or **2D** (for lat-long)
3. In **Lighting** window, assign to **Environment Skybox**

## Technical Details

### Color Space

All generated environments are in **linear color space** (no gamma encoding). This is correct for PBR rendering and HDR textures.

- Input values: Linear RGB
- Output values: Linear RGB (HDR, values can exceed 1.0)
- No sRGB transfer function applied

### HDR Range

Values are unbounded and can significantly exceed 1.0:
- Sun disk: 50-200+ (direct sunlight simulation)
- Sky: 0.1-2.0 (indirect ambient)
- Studio lights: 10-100 (controlled lighting)
- White furnace: 0.1-10.0 (testing range)

### Coordinate System

**Lat-Long (Equirectangular):**
- U (horizontal): 0 = -180° (west), 0.5 = 0° (north), 1.0 = 180° (east)
- V (vertical): 0 = -90° (down), 0.5 = 0° (horizon), 1.0 = 90° (up)
- Direction: +X right, +Y up, +Z forward

**Cubemap (OpenGL Convention):**
- Faces: +X, -X, +Y, -Y, +Z, -Z
- +Y is up (top face)
- Right-handed coordinate system

## Troubleshooting

### Generated files appear too dark/bright

Adjust intensity parameters:
```bash
# Increase sun intensity
hdrgen -p sun-sky --sun-intensity 200 -o output/bright_sky.hdr

# Decrease studio key light
hdrgen -p studio --key-intensity 25 -o output/dim_studio.hdr
```

### Sun position is incorrect

Check azimuth convention:
- 0° = North (top of lat-long image, middle)
- 90° = East (right side of lat-long image)
- 180° = South (bottom, middle)
- 270° = West (left side)

### Cubemap faces don't align properly

Ensure DCC is using OpenGL convention (+Y up). Some DCCs (DirectX convention) may require face reordering.

## Limitations

- **EXR Compression:** ZIPS and uncompressed scanlines are supported; ZIP/PIZ are not
- **Sky Model:** Simplified procedural sky (not full Hosek-Wilkie or Nishita)
- **Compression:** HDR files are uncompressed (no RLE compression)
- **Metadata:** Minimal file metadata (no custom tags)

## Roadmap

- [x] Proper float OpenEXR writing with ZIPS compression
- [ ] Multi-scanline ZIP and wavelet-based PIZ EXR compression
- [x] Sunset and overcast presets
- [x] Importance sampling map generation (`--importance-map` JSON row/column CDFs)
- [x] Deterministic diffuse irradiance and GGX specular pre-filtering
- [x] HDR/EXR panorama loading, rotation, and intensity/exposure scaling
- [x] Deterministic time-of-day sequence with numbered outputs and manifest
- [ ] Web-based visualizer

## License

Apache License 2.0

Copyright 2024 - Present, Light Transport Entertainment Inc.

## References

- [HDR Image Formats](https://www.pauldebevec.com/Research/HDR/)
- [Radiance RGBE Format](https://floyd.lbl.gov/radiance/refer/Notes/picture_format.html)
- [OpenEXR Specification](https://www.openexr.com/)
- [PBR Theory](https://www.pbrt.org/)
- [Hosek-Wilkie Sky Model](https://cgg.mff.cuni.cz/projects/SkylightModelling/)

## Contributing

Contributions welcome! Please ensure:
- Code follows existing style
- Tests pass: `npm test`
- Examples work: `npm run example`
- Documentation is updated

## Support

For issues or questions, see: https://github.com/syoyo/lightusd
