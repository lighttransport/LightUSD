# Raytracing Data Structure Design for Tydra

## Document Overview

This document outlines the design for raytracing-optimized data structures in the Tydra framework. The goal is to create structures that complement the existing rasterization-focused `RenderScene` while maintaining architectural consistency and minimizing code duplication.

**Author:** Design Document
**Date:** 2025-11-28
**Status:** Archived implementation sketch. Do not extend this parallel scene
model for production rendering; the active path converts Tydra `RenderScene`
into the shared LightRT host scene used by `lusdview` and `lusdrender`.

The data declarations in `raytracing-data.hh/.cc` exist, including scene bounds
and validation, but `RaytracingSceneConverter` and its BVH builder remain
placeholders. Completing them would duplicate the maintained RenderScene,
`rt_scene_build`, and LightRT paths. They remain source-compatible experimental
types until a consumer demonstrates a requirement that the active pipeline
cannot satisfy.

---

## 1. Background and Motivation

### 1.1 Current State

The existing `RenderScene` class and associated structures (`RenderMesh`, `RenderMaterial`, `RenderCamera`, etc.) are optimized for rasterization-based rendering pipelines (OpenGL, Vulkan, WebGL). Key characteristics include:

- Vertex buffer and index buffer layout
- GPU-friendly attribute packing
- Face-varying and vertex-varying attribute handling
- Material structure focused on UsdPreviewSurface and OpenPBR

### 1.2 Raytracing Requirements

Raytracing engines have different performance characteristics and data access patterns:

1. **Random access patterns**: Rays can hit any part of geometry in any order
2. **Acceleration structures**: Need BVH or similar spatial data structures
3. **Material evaluation**: Path tracing requires more complete BSDF information
4. **Light sampling**: Direct light sampling is critical for performance
5. **Instancing**: Heavy use of instancing to reduce memory
6. **Texture sampling**: More random access, less cache coherent
7. **Triangle-centric**: Everything converted to triangles with explicit data

### 1.3 Design Goals

- **Compatibility**: Reuse existing data structures where possible
- **Minimal duplication**: Share vertex data, texture data, and material parameters
- **Flexibility**: Support both CPU and GPU raytracers (Embree, OptiX, custom)
- **Performance**: Optimize for raytracing access patterns
- **USD-faithful**: Maintain USD semantics and coordinate systems

---

## 2. High-Level Architecture

### 2.1 Dual-Scene Approach

```
Stage (USD)
    ↓
    ↓ RenderSceneConverter
    ↓
    ├──→ RenderScene (Rasterization)
    │      ├─ RenderMesh (vertex buffers)
    │      ├─ RenderMaterial (preview surface)
    │      └─ RenderCamera
    │
    └──→ RaytracingScene (Raytracing)
           ├─ RTGeometry (triangle soup)
           ├─ RTMaterial (BSDF properties)
           ├─ RTLight (sampling data)
           └─ RTAccelerationStructure
```

### 2.2 Shared Resources

To avoid duplication, the following resources are **shared** between RenderScene and RaytracingScene:

- `TextureImage` - Raw texture data
- `BufferData` - Generic binary buffers
- Geometry vertex positions (via indices or shared buffers)
- Node hierarchy and transforms

---

## 3. Core Data Structures

### 3.1 RTGeometry - Raytracing Geometry

```cpp
/// Raytracing-optimized geometry representation
/// Always represents triangulated, flattened geometry suitable for intersection testing
struct RTGeometry {
  std::string prim_name;
  std::string abs_path;
  std::string display_name;

  // === Triangle Data (Flattened and Triangulated) ===

  // Option 1: Indexed triangles (memory efficient)
  std::vector<vec3> vertices;         // Unique vertex positions
  std::vector<uint32_t> indices;      // 3 indices per triangle

  // Option 2: Direct triangle soup (cache friendly for RT)
  // std::vector<Triangle> triangles;  // 3 vertices per triangle (no indexing)

  // === Per-Vertex Attributes (Indexed, matches vertices) ===
  std::vector<vec3> normals;          // Shading normals (optional, can be computed)
  std::vector<vec2> texcoords0;       // Primary UV coordinates
  std::vector<vec2> texcoords1;       // Secondary UV coordinates (optional)
  std::vector<vec4> colors;           // Vertex colors (optional)
  std::vector<vec4> tangents;         // Tangent space (optional, for normal mapping)

  // === Per-Triangle Attributes ===
  std::vector<uint32_t> material_ids; // Material ID per triangle
  std::vector<vec3> face_normals;     // Geometric normals (optional, can be computed)

  // === Skinning/Animation Data (Optional) ===
  std::vector<vec4> joint_indices;    // Up to 4 joint indices per vertex
  std::vector<vec4> joint_weights;    // Corresponding weights
  std::vector<BlendShapeTarget> blendshapes; // Morph targets

  // === Bounding Volume ===
  AABB bounds;                        // Axis-aligned bounding box

  // === Optimization Hints ===
  bool is_double_sided{false};        // Disable backface culling
  bool cast_shadows{true};
  bool receive_shadows{true};

  // === Source Reference ===
  int32_t source_mesh_id{-1};         // Index to RenderScene::meshes (if available)
};
```

**Design rationale:**
- Always flattened triangles (no n-gons)
- Simple indexed or triangle soup representation
- Per-triangle material assignment for heterogeneous meshes
- Bounding box for BVH construction
- Optional reference back to RenderMesh to avoid data duplication

### 3.2 RTMaterial - Raytracing Material

```cpp
/// Material optimized for path tracing / BSDF evaluation
struct RTMaterial {
  std::string name;
  std::string abs_path;
  std::string display_name;

  // === Base Material Model (PBR Metallic-Roughness or Specular-Glossiness) ===

  // Base color / albedo
  vec3 base_color{1.0f, 1.0f, 1.0f};
  int32_t base_color_texture{-1};     // Index to textures array

  // Metallic-roughness workflow
  float metallic{0.0f};
  float roughness{0.5f};
  int32_t metallic_roughness_texture{-1};

  // Specular workflow (alternative)
  vec3 specular_color{1.0f, 1.0f, 1.0f};
  float specular_factor{0.5f};
  int32_t specular_texture{-1};

  // === Extended Material Properties ===

  // Emission
  vec3 emission{0.0f, 0.0f, 0.0f};
  float emission_strength{1.0f};
  int32_t emission_texture{-1};

  // Normal mapping
  int32_t normal_texture{-1};
  float normal_scale{1.0f};

  // Occlusion
  int32_t occlusion_texture{-1};
  float occlusion_strength{1.0f};

  // Alpha / Opacity
  float opacity{1.0f};
  int32_t opacity_texture{-1};
  OpacityMode opacity_mode{OpacityMode::Opaque}; // Opaque, Mask, Blend
  float opacity_threshold{0.5f};

  // === Advanced Properties ===

  // Transmission (for glass, translucent materials)
  float transmission{0.0f};
  int32_t transmission_texture{-1};

  // Index of refraction
  float ior{1.5f};

  // Clearcoat (for car paint, etc.)
  float clearcoat{0.0f};
  float clearcoat_roughness{0.0f};
  int32_t clearcoat_texture{-1};
  int32_t clearcoat_roughness_texture{-1};
  int32_t clearcoat_normal_texture{-1};

  // Sheen (for cloth)
  float sheen{0.0f};
  vec3 sheen_color{1.0f, 1.0f, 1.0f};
  float sheen_roughness{0.5f};

  // Subsurface scattering
  float subsurface{0.0f};
  vec3 subsurface_color{1.0f, 1.0f, 1.0f};
  float subsurface_radius{1.0f};

  // Anisotropic reflection
  float anisotropic{0.0f};
  float anisotropic_rotation{0.0f};
  int32_t anisotropic_texture{-1};

  // === Material Behavior Flags ===
  bool is_double_sided{false};
  bool is_thin_walled{false};         // Thin surface approximation
  bool cast_shadows{true};
  bool receive_shadows{true};
  bool visible_to_camera{true};
  bool visible_in_reflections{true};
  bool visible_in_refractions{true};

  // === Source Reference ===
  int32_t source_material_id{-1};     // Index to RenderScene::materials

  // === Helper Methods ===
  bool is_emissive() const {
    return emission.x > 0.0f || emission.y > 0.0f || emission.z > 0.0f;
  }

  bool is_transmissive() const {
    return transmission > 0.0f || opacity < 1.0f;
  }

  bool has_textures() const {
    return base_color_texture >= 0 || normal_texture >= 0 ||
           emission_texture >= 0 || metallic_roughness_texture >= 0;
  }
};

enum class OpacityMode {
  Opaque,   // Fully opaque, ignore alpha
  Mask,     // Binary alpha test (cutout)
  Blend,    // Full alpha blending / transparency
};
```

**Design rationale:**
- Comprehensive BSDF parameter set for path tracing
- Support for both metallic-roughness and specular workflows
- Extended properties (clearcoat, sheen, transmission, SSS)
- Visibility flags for render layer control
- Reference to source RenderMaterial for texture lookup

### 3.3 RTLight - Raytracing Light

```cpp
/// Light source optimized for importance sampling in path tracing
struct RTLight {
  std::string name;
  std::string abs_path;
  std::string display_name;

  enum class Type {
    Point,          // Omnidirectional point light
    Directional,    // Distant directional light (sun)
    Spot,           // Spotlight with cone
    Area,           // Area light (rectangle, disk, sphere)
    Environment,    // Environment map / IBL
    Mesh,           // Emissive mesh light
  };

  Type type{Type::Point};

  // === Common Properties ===
  vec3 color{1.0f, 1.0f, 1.0f};      // Light color
  float intensity{1.0f};              // Light intensity (multiplier)

  // === Position/Orientation (in world space) ===
  vec3 position{0.0f, 0.0f, 0.0f};
  vec3 direction{0.0f, -1.0f, 0.0f}; // For directional/spot lights
  mat4 transform;                     // Full transformation matrix

  // === Type-Specific Parameters ===

  // Point/Spot light
  float radius{0.0f};                 // Physical size (for soft shadows)

  // Spot light
  float cone_angle{45.0f};            // Outer cone angle (degrees)
  float cone_angle_softness{5.0f};    // Inner-to-outer transition

  // Area light
  vec2 area_size{1.0f, 1.0f};         // Width and height
  AreaShape area_shape{AreaShape::Rectangle};

  // Environment light
  int32_t envmap_texture{-1};         // Index to textures (lat-long or cubemap)
  float envmap_rotation{0.0f};        // Rotation around Y axis

  // Mesh light
  int32_t emissive_mesh_id{-1};       // Index to RTGeometry

  // === Sampling Data ===
  float total_power{0.0f};            // Precomputed total power (for MIS)
  float inv_area{0.0f};               // 1/area (for area lights)

  // Environment map importance sampling (optional)
  struct EnvmapSamplingData {
    std::vector<float> cdf;           // Cumulative distribution function
    std::vector<float> pdf;           // Probability density function
    int32_t width{0};
    int32_t height{0};
  };
  nonstd::optional<EnvmapSamplingData> envmap_sampling;

  // === Visibility Flags ===
  bool cast_shadows{true};
  bool visible_to_camera{true};
  bool visible_in_reflections{true};

  // === Source Reference ===
  int32_t source_light_id{-1};        // Index to RenderScene::lights
};

enum class AreaShape {
  Rectangle,
  Disk,
  Sphere,
};
```

**Design rationale:**
- Unified light representation for all types
- Precomputed sampling data for importance sampling
- Support for physically-based units and soft shadows
- Environment map with optional importance sampling data

### 3.4 RTInstance - Instancing Support

```cpp
/// Instance of geometry with unique transform and material override
struct RTInstance {
  uint32_t geometry_id;               // Index to RTGeometry
  mat4 transform;                     // Instance transformation matrix
  mat4 inverse_transform;             // Precomputed inverse (for ray transform)

  // Material override (optional)
  std::vector<uint32_t> material_overrides; // Per-primitive material override
                                             // Empty = use geometry defaults

  // Visibility flags (per-instance)
  bool visible{true};
  bool cast_shadows{true};
  bool receive_shadows{true};

  // User data
  uint32_t user_id{0};                // Application-specific ID
};
```

**Design rationale:**
- Explicit instancing for memory efficiency
- Per-instance material overrides
- Precomputed inverse transform for ray intersection

### 3.5 RTAccelerationStructure - Abstract BVH Interface

```cpp
/// Abstract interface for acceleration structure
/// Implementations can use Embree, OptiX, or custom BVH
struct RTAccelerationStructure {

  enum class Type {
    None,
    BVH2,          // Binary BVH
    BVH4,          // 4-wide BVH (SIMD friendly)
    BVH8,          // 8-wide BVH (AVX-512)
    QBVH,          // Quantized BVH
    Embree,        // Intel Embree
    OptiX,         // NVIDIA OptiX
    Custom,        // User-provided
  };

  Type type{Type::None};

  // Opaque handle to native acceleration structure
  void* native_handle{nullptr};

  // Bounding box of entire scene
  AABB scene_bounds;

  // Build statistics
  struct BuildStats {
    size_t num_nodes{0};
    size_t num_leaves{0};
    size_t max_depth{0};
    double build_time_ms{0.0};
    size_t memory_bytes{0};
  };
  BuildStats stats;

  // Build configuration
  struct BuildConfig {
    int max_leaf_size{4};             // Max triangles per leaf
    int max_depth{64};                // Max tree depth
    float traversal_cost{1.0f};       // SAH traversal cost
    float intersection_cost{1.0f};    // SAH intersection cost
    bool use_spatial_splits{false};   // Higher quality, slower build
  };
};
```

**Design rationale:**
- Abstraction over different BVH implementations
- Opaque handle for native libraries (Embree, OptiX)
- Build statistics for profiling

### 3.6 RaytracingScene - Top-Level Container

```cpp
/// Top-level raytracing scene container
class RaytracingScene {
public:
  std::string usd_filename;

  // === Shared Resources (Reference to RenderScene) ===
  // Note: These can be shared pointers or indices into RenderScene

  std::vector<TextureImage>* shared_images{nullptr};  // Shared with RenderScene
  std::vector<BufferData>* shared_buffers{nullptr};   // Shared with RenderScene

  // === Raytracing-Specific Data ===

  std::vector<RTGeometry> geometries;
  std::vector<RTMaterial> materials;
  std::vector<RTLight> lights;
  std::vector<RTInstance> instances;
  std::vector<RTCamera> cameras;

  // Acceleration structure
  RTAccelerationStructure accel_structure;

  // === Scene Metadata ===
  SceneMetadata meta;  // Shared with RenderScene

  // Background / environment
  int32_t environment_light_id{-1};   // Index to lights
  vec3 background_color{0.0f, 0.0f, 0.0f};

  // === Helper Methods ===

  /// Build or rebuild acceleration structure
  bool build_acceleration_structure(
    const RTAccelerationStructure::BuildConfig& config
  );

  /// Estimate memory usage
  size_t estimate_memory_usage() const;

  /// Validate scene consistency
  bool validate(std::string* warn, std::string* err) const;

  /// Get emissive lights (including mesh lights)
  std::vector<uint32_t> get_emissive_geometry_ids() const;
};
```

**Design rationale:**
- Similar structure to RenderScene for consistency
- Explicit shared resource references to avoid duplication
- Acceleration structure as first-class member
- Helper methods for common operations

---

## 4. Conversion Pipeline

### 4.1 Converter Class

```cpp
/// Convert USD Stage to RaytracingScene
class RaytracingSceneConverter {
public:

  struct Config {
    bool share_with_render_scene{true};   // Share textures/buffers with RenderScene
    bool build_acceleration_structure{true}; // Build BVH automatically
    bool merge_static_instances{false};   // Merge non-animated instances
    bool convert_to_triangle_soup{false}; // No indexing (cache friendly)

    RTAccelerationStructure::BuildConfig accel_config;
  };

  /// Convert from Stage
  bool ConvertToRaytracingScene(
    const Stage& stage,
    RaytracingScene* rt_scene,
    const Config& config = Config()
  );

  /// Convert from existing RenderScene (reuse data)
  bool ConvertFromRenderScene(
    const RenderScene& render_scene,
    RaytracingScene* rt_scene,
    const Config& config = Config()
  );

  const std::string& GetError() const { return err_; }
  const std::string& GetWarning() const { return warn_; }

private:
  std::string err_;
  std::string warn_;

  bool convert_mesh(const RenderMesh& mesh, RTGeometry* rt_geom);
  bool convert_material(const RenderMaterial& mat, RTMaterial* rt_mat);
  bool convert_light(const RenderLight& light, RTLight* rt_light);
  bool extract_emissive_geometry(const RaytracingScene& scene);
};
```

### 4.2 Conversion Flow

```
Stage → RenderSceneConverter → RenderScene
                                      ↓
                                      ↓ (optional, share resources)
                                      ↓
                       RaytracingSceneConverter → RaytracingScene
                                                        ↓
                                          Build Acceleration Structure
```

**Two conversion paths:**

1. **Direct:** `Stage → RaytracingScene`
2. **Shared:** `Stage → RenderScene → RaytracingScene` (share textures/buffers)

---

## 5. Historical Implementation Plan

### Phase 1: Core Data Structures
- [x] Implement `RTGeometry`, `RTMaterial`, `RTLight` structs
- [x] Implement `RTInstance` and `RTCamera` structs
- [x] Implement `RaytracingScene` class
- [x] Add to `src/tydra/raytracing-data.hh`

### Phase 2: Converter

Superseded by `RenderSceneConverter` plus the shared LightRT host-scene build.
The placeholder `RaytracingSceneConverter` is retained for source compatibility;
new mesh, material, and light conversion work belongs in the active shared path.

### Phase 3: Acceleration Structure

- [x] Retain the experimental `RTAccelerationStructure` carrier and scene bounds.
- [x] Build production CPU/GPU acceleration structures through LightRT.

### Phase 4: Testing & Optimization

Production conversion, validation, rendering, and performance coverage lives in
the `lusdview`, `lusdrender`, and Tydra `RenderScene` tests. A separate test and
example matrix for the placeholder converter would create a second behavioral
contract and is intentionally not planned.

---

## 6. File Organization

```
src/tydra/
├── render-data.hh/cc              (existing, rasterization)
├── raytracing-data.hh/cc          (new, raytracing structures)
├── raytracing-accel.hh/cc         (new, BVH and accel structure)
├── raytracing-converter.hh/cc     (new, Stage → RaytracingScene)
└── RAYTRACING_DATA_DESIGN.md      (this document)
```

---

## 7. Design Decisions & Trade-offs

### 7.1 Indexed vs Triangle Soup

**Decision:** Support both via compile-time or runtime option

- **Indexed:** Lower memory, better for instancing
- **Triangle Soup:** Better cache locality for ray intersection

### 7.2 Shared Resources

**Decision:** Share textures and buffers with RenderScene when possible

- **Pro:** Significant memory savings
- **Con:** Lifetime management complexity

### 7.3 Material Model

**Decision:** Extended PBR with transmission, clearcoat, sheen, SSS

- **Pro:** Supports modern material models (OpenPBR, MaterialX)
- **Con:** More complex than basic UsdPreviewSurface

### 7.4 Acceleration Structure

**Decision:** Abstract interface with pluggable backends

- **Pro:** Flexibility (Embree, OptiX, custom)
- **Con:** Need to handle different APIs and capabilities

---

## 8. Open Questions

1. **Memory management:** Should we use `shared_ptr` for shared resources?
2. **Animation:** How to handle time-varying geometry/materials?
3. **UDIM textures:** Need special handling for UDIM in raytracing?
4. **GPU raytracing:** Do we need separate data layout for OptiX/DXR/Vulkan RT?
5. **Volumes:** Support for volume rendering (VDB, OpenVDB)?

---

## 9. References

- USD Preview Surface: https://openusd.org/release/spec_usdpreviewsurface.html
- MaterialX OpenPBR: https://academysoftwarefoundation.github.io/OpenPBR/
- Intel Embree: https://www.embree.org/
- NVIDIA OptiX: https://developer.nvidia.com/optix
- PBR Texture Mapping: https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html

---

## Appendix A: Comparison Table

| Feature | RenderScene (Rasterizer) | RaytracingScene |
|---------|-------------------------|-----------------|
| Geometry | Vertex buffers + indices | Triangle soup or indexed |
| Material | Preview surface, shader graph | Extended PBR, BSDF params |
| Lights | Simple light sources | Importance sampling data |
| Instances | Implicit (scene graph) | Explicit RTInstance array |
| Accel Structure | None (GPU handles) | BVH required |
| Memory | Optimized for GPU upload | Optimized for random access |

---

## Appendix B: Example Usage

```cpp
// Load USD
Stage stage;
LoadUSDFromFile("model.usd", &stage);

// Option 1: Direct conversion
RaytracingSceneConverter rt_converter;
RaytracingScene rt_scene;
rt_converter.ConvertToRaytracingScene(stage, &rt_scene);

// Option 2: Share with RenderScene
RenderSceneConverter render_converter;
RenderScene render_scene;
render_converter.ConvertToRenderScene(stage, &render_scene);

RaytracingSceneConverter rt_converter;
RaytracingScene rt_scene;
rt_converter.ConvertFromRenderScene(render_scene, &rt_scene);

// Build acceleration structure
RTAccelerationStructure::BuildConfig config;
config.max_leaf_size = 4;
rt_scene.build_acceleration_structure(config);

// Use for rendering
MyPathTracer tracer;
tracer.set_scene(&rt_scene);
tracer.render();
```
