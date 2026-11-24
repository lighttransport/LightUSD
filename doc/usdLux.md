# UsdLux Lighting Support (C++ Core / Tydra)

LightUSD supports the full USD lighting schema (UsdLux) including parsing, reconstruction, pretty-printing, and conversion to a rendering-friendly representation via Tydra.

## Source Files

| File | Description |
|------|-------------|
| `src/usdLux.hh` | Light prim structs, API schemas, enums |
| `src/usdLux.cc` | Utility functions, enum conversion, light computation helpers |
| `src/prim-property-tables.hh` | Property table macros for light attribute binding |
| `src/prim-reconstruct-lightprim.cc` | Deserialization of light prims from USD |
| `src/tydra/render-data.hh` | `RenderLight` struct for rendering pipelines |
| `src/tydra/render-data-anim.cc` | `Convert*Light()` functions (Tydra conversion) |

---

## Supported Light Types

| USD Prim | Base Class | Key Properties |
|----------|-----------|----------------|
| `SphereLight` | BoundableLight | `radius` (0.5), optional ShapingAPI |
| `CylinderLight` | BoundableLight | `length` (1.0), `radius` (0.5) |
| `RectLight` | BoundableLight | `width` (1.0), `height` (1.0), optional `file`, optional ShapingAPI |
| `DiskLight` | BoundableLight | `radius` (0.5) |
| `DistantLight` | NonboundableLight | `angle` (0.53 degrees) |
| `DomeLight` | NonboundableLight | `file`, `textureFormat`, `guideRadius` (1e5), `portals` |
| `GeometryLight` | NonboundableLight | `geometry` relationship (deprecated in USD) |
| `PortalLight` | NonboundableLight | `geometry` relationship |
| `PluginLight` | Xformable + Collection | `shaderId` (plugin-defined via shader registry) |

### Boundable vs Non-boundable

- **BoundableLight** (SphereLight, CylinderLight, RectLight, DiskLight): Has spatial `extent` attribute for bounding boxes. Inherits from `Xformable`.
- **NonboundableLight** (DistantLight, DomeLight, GeometryLight, PortalLight): No spatial bounds. Inherits from `Xformable`.

---

## Common Light Properties (LightAPI)

All light types share these properties (defined on BoundableLight/NonboundableLight base classes):

| USD Attribute | C++ Field | Type | Default |
|--------------|-----------|------|---------|
| `inputs:color` | `color` | color3f | {1,1,1} |
| `inputs:intensity` | `intensity` | float | 1.0 |
| `inputs:exposure` | `exposure` | float | 0.0 |
| `inputs:diffuse` | `diffuse` | float | 1.0 |
| `inputs:specular` | `specular` | float | 1.0 |
| `inputs:normalize` | `normalize` | bool | false |
| `inputs:enableColorTemperature` | `enableColorTemperature` | bool | false |
| `inputs:colorTemperature` | `colorTemperature` | float | 6500.0 |

All numeric properties are `Animatable<T>`, supporting time-sampled values.

---

## API Schemas

### ShapingAPI

Applied to SphereLight, RectLight, DiskLight, CylinderLight. Controls cone emission and IES profiles.

| USD Attribute | C++ Field | Type | Default |
|--------------|-----------|------|---------|
| `inputs:shaping:focus` | `shapingFocus` | float | 0.0 |
| `inputs:shaping:focusTint` | `shapingFocusTint` | color3f | {0,0,0} |
| `inputs:shaping:cone:angle` | `shapingConeAngle` | float | 90.0 |
| `inputs:shaping:cone:softness` | `shapingConeSoftness` | float | 0.0 |
| `inputs:shaping:ies:file` | `shapingIesFile` | AssetPath | |
| `inputs:shaping:ies:angleScale` | `shapingIesAngleScale` | float | 0.0 |
| `inputs:shaping:ies:normalize` | `shapingIesNormalize` | bool | false |

### ShadowAPI

Applied to all light types.

| USD Attribute | C++ Field | Type | Default |
|--------------|-----------|------|---------|
| `inputs:shadow:enable` | `shadowEnable` | bool | true |
| `inputs:shadow:color` | `shadowColor` | color3f | {0,0,0} |
| `inputs:shadow:distance` | `shadowDistance` | float | -1.0 (infinite) |
| `inputs:shadow:falloff` | `shadowFalloff` | float | -1.0 (none) |
| `inputs:shadow:falloffGamma` | `shadowFalloffGamma` | float | 1.0 |

### MeshLightAPI / VolumeLightAPI

Applied to Mesh/Volume prims to make them emit light. Field: `light:materialSyncMode` (token, default `"materialGlowTintsLight"`).

---

## DomeLight Texture Formats

`DomeLight::TextureFormat` (and `DomeLight_1::TextureFormat`) in `src/usdLux.hh`;
Tydra mirrors this as `RenderLight::DomeTextureFormat`:

```cpp
enum class TextureFormat {
  Automatic,   // Auto-detect from file
  Latlong,     // Equirectangular (lat/long)
  MirroredBall,// Mirrored ball projection
  Angular,     // Angular map projection
};
```

---

## Utility Functions (`src/usdLux.cc`)

```cpp
// Compute effective light color with optional color temperature (Kelvin)
value::color3f ComputeEffectiveLightColor(
    const value::color3f &baseColor,
    bool enableColorTemperature,
    float colorTemperature);

// Compute intensity with exposure: baseIntensity * 2^exposure
float ComputeFinalLightIntensity(float baseIntensity, float exposure);

// Query helpers
bool HasLightShaping(const ShapingAPI &shaping);
bool AreShadowsEnabled(const ShadowAPI &shadow);
value::color3f GetEffectiveShadowColor(const ShadowAPI &shadow);

// Prim classification
bool IsBoundableLight(const Prim &prim);
bool IsNonboundableLight(const Prim &prim);
bool IsLightFilterPrim(const Prim &prim);
```

---

## Tydra Rendering Pipeline

### RenderLight Structure (`src/tydra/render-data.hh`)

Tydra converts USD light prims into a flat, rendering-friendly `RenderLight` struct:

```cpp
struct RenderLight {
  enum class Type {
    Point, Sphere, Disk, Rect, Cylinder, Distant, Dome, Geometry, Portal
  };

  std::string name, abs_path, display_name;
  Type type;

  // LightAPI
  vec3 color;
  float intensity, exposure;
  float diffuse, specular;
  bool normalize;
  bool enableColorTemperature;
  float colorTemperature;

  // Transform
  mat4 transform;              // world transform
  vec3 position;               // world position
  vec3 direction;              // light direction (-Z in local space)

  // Type-specific
  float radius;                // Sphere/Disk
  float width, height;         // Rect
  float length;                // Cylinder
  float angle;                 // Distant (degrees)
  std::string textureFile;     // Dome/Rect texture path
  DomeTextureFormat domeTextureFormat;  // embedded enum
  float guideRadius;           // Dome visualization
  int32_t envmap_texture_id;   // Index into RenderScene textures (-1 = none)

  // ShapingAPI
  float shapingConeAngle, shapingConeSoftness;
  float shapingFocus;
  vec3 shapingFocusTint;
  std::string shapingIesFile;
  float shapingIesAngleScale;
  bool shapingIesNormalize;

  // ShadowAPI
  bool shadowEnable;
  vec3 shadowColor;
  float shadowDistance, shadowFalloff, shadowFalloffGamma;

  // GeometryLight
  int32_t geometry_mesh_id;
  std::string material_sync_mode;

  // LTE SpectralAPI (optional)
  nonstd::optional<SpectralEmission> spd_emission;
};
```

### Conversion Functions

Each light type has a dedicated `RenderSceneConverter` method (declared in
`src/tydra/render-data-converter.hh`, defined in `src/tydra/render-data-anim.cc`):

```cpp
bool ConvertSphereLight(env, abs_path, light, rlight_out);
bool ConvertDistantLight(env, abs_path, light, rlight_out);
bool ConvertDomeLight(env, abs_path, light, rlight_out);
bool ConvertRectLight(env, abs_path, light, rlight_out);
bool ConvertDiskLight(env, abs_path, light, rlight_out);
bool ConvertCylinderLight(env, abs_path, light, rlight_out);
bool ConvertGeometryLight(env, abs_path, light, rlight_out);
```

Internal helpers:
- `ExtractCommonLightProperties()` - Extracts LightAPI + ShadowAPI fields
- `ExtractShapingProperties()` - Extracts ShapingAPI fields (for lights with optional shaping)

### DomeLight Texture Loading

`ConvertDomeLight()` handles environment map loading:
1. Resolves `inputs:texture:file` asset path
2. Loads image via the asset resolver (supports EXR, HDR)
3. Falls back to raw asset storage if image decode fails
4. Stores texture ID in `envmap_texture_id` for downstream consumers

### Transform Extraction

World transformation is extracted from the Tydra scene graph's `global_matrix`:
- `position` = translation column of the matrix
- `direction` = negated Z column (lights face -Z in local space)

---

## Reconstruction (USD Parsing)

Light prims are reconstructed in `src/prim-reconstruct-lightprim.cc` via `ReconstructPrim<T>` template specializations. Each light type uses property table macros from `src/prim-property-tables.hh` to bind USD attributes to C++ struct fields.

Property tables are organized as:
- `LIGHT_COMMON_ATTRS` - color, intensity, exposure, diffuse, specular, normalize, color temperature
- `LIGHT_SHADOW_ATTRS` - shadow:enable, shadow:color, shadow:distance, etc.
- `LIGHT_SHAPING_ATTRS` - shaping:focus, shaping:cone:angle, etc.
- `{TYPE}_LIGHT_TYPED_ATTRS` - Type-specific attributes (radius, width, height, angle, etc.)

---

## LTE Spectral Emission (Extension)

LightUSD includes an experimental SpectralAPI for wavelength-dependent light emission. The `SpectralEmission` struct now lives in `src/tydra/render-data-shader.hh` (shared by the shader/light pipeline) and is carried through to `RenderLight::spd_emission` (`src/tydra/render-data.hh`):

```cpp
struct SpectralEmission {
  std::vector<std::pair<float, float>> samples;  // (wavelength_nm, power)
  std::string interpolation;                       // "linear", "step", etc.
  std::string unit;                                // "watts", "lumens"
  std::string preset;                              // "D65", "A", etc.
};
```

This is applied to the base light classes and carried through to `RenderLight`.

---

## Type IDs

Runtime type identification for light prims (`src/value-types.hh`):

```
TYPE_ID_LUX_SPHERE, TYPE_ID_LUX_DOME, TYPE_ID_LUX_CYLINDER,
TYPE_ID_LUX_DISK, TYPE_ID_LUX_RECT, TYPE_ID_LUX_DISTANT,
TYPE_ID_LUX_GEOMETRY, TYPE_ID_LUX_PORTAL, TYPE_ID_LUX_PLUGIN,
TYPE_ID_LUX_DOME_1, TYPE_ID_LUX_LIGHT_FILTER, TYPE_ID_LUX_PLUGIN_LIGHT_FILTER
```

(`DomeLight_1` for the older DomeLight variant; `LightFilter` /
`PluginLightFilter` are the light-filter prims.)

---

## Implementation Status

### Fully Supported
- All 9 light prim types (parse, reconstruct, pretty-print, Tydra convert)
- LightAPI, ShadowAPI, ShapingAPI
- DomeLight texture loading (EXR, HDR)
- Transform and world-space position/direction extraction
- Color temperature computation
- Intensity + exposure computation
- Light linking (`light:lightLink` / `light:shadowLink` collections resolved
  against scene geometry via `ResolveLightLinking()` —
  `src/tydra/render-data-anim.cc`, mirrored in `src/tydra/render-converter.cc`)
- MeshLightAPI area lights (mesh marked as area light during `ConvertMesh`,
  `src/tydra/render-data-mesh.cc`)

### Partial
- VolumeLightAPI (struct defined, limited Tydra support)
- LightFilter (struct defined, no evaluation)
- LTE SpectralAPI (carried through pipeline, no physical rendering)

### Not Implemented
- IES profile loading and evaluation
- Procedural light filter evaluation
