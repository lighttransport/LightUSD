// SPDX-License-Identifier: Apache 2.0
// Copyright 2025, Light Transport Entertainment Inc.

///
/// @file raytracing-data.hh
/// @brief Raytracing-optimized data structures for Tydra
///
/// This header defines data structures optimized for raytracing rendering,
/// complementing the rasterization-focused RenderScene structures.
///
/// Key features:
/// - Triangle-centric geometry representation
/// - Explicit instancing support
/// - BVH/acceleration structure abstraction
/// - Extended PBR material model (transmission, clearcoat, SSS, etc.)
/// - Importance-sampled light sources
/// - Resource sharing with RenderScene to minimize duplication
///
/// Main classes:
/// - RaytracingScene: Top-level raytracing scene container
/// - RTGeometry: Flattened, triangulated geometry for ray intersection
/// - RTMaterial: Extended BSDF material for path tracing
/// - RTLight: Light sources with importance sampling data
/// - RTInstance: Explicit geometry instancing with transforms
/// - RTAccelerationStructure: Abstract BVH interface
///
/// Usage:
/// ```cpp
/// lightusd::tydra::RaytracingSceneConverter converter;
/// lightusd::tydra::RaytracingScene rt_scene;
/// converter.ConvertToRaytracingScene(stage, &rt_scene);
/// rt_scene.build_acceleration_structure(config);
/// ```
///
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nonstd/expected.hpp"
#include "nonstd/optional.hpp"
#include "value-types.hh"

// Forward declare spectral types from render-data.hh
// Full definitions available when including render-data.hh
namespace lightusd {
namespace tydra {
  struct SpectralData;
  struct SpectralIOR;
  struct SpectralEmission;
  enum class SpectralInterpolation;
  enum class IlluminantPreset;
  enum class WavelengthUnit;
}
}

namespace lightusd {

// Forward declarations
class Stage;

namespace tydra {

// Forward declarations
class RenderScene;
struct RenderMesh;
struct RenderMaterial;
struct TextureImage;
struct BufferData;

// Type aliases for convenience
using vec2 = value::float2;
using vec3 = value::float3;
using vec4 = value::float4;
using mat4 = value::matrix4f;

///
/// Axis-Aligned Bounding Box
///
struct AABB {
  vec3 min{(std::numeric_limits<float>::max)(),
           (std::numeric_limits<float>::max)(),
           (std::numeric_limits<float>::max)()};
  vec3 max{std::numeric_limits<float>::lowest(),
           std::numeric_limits<float>::lowest(),
           std::numeric_limits<float>::lowest()};

  /// Test if AABB is valid (min <= max)
  bool is_valid() const {
    return min[0] <= max[0] && min[1] <= max[1] && min[2] <= max[2];
  }

  /// Get center of AABB
  vec3 center() const {
    vec3 result;
    result[0] = (min[0] + max[0]) * 0.5f;
    result[1] = (min[1] + max[1]) * 0.5f;
    result[2] = (min[2] + max[2]) * 0.5f;
    return result;
  }

  /// Get extents (size) of AABB
  vec3 extents() const {
    vec3 result;
    result[0] = max[0] - min[0];
    result[1] = max[1] - min[1];
    result[2] = max[2] - min[2];
    return result;
  }

  /// Get surface area of AABB
  float surface_area() const {
    vec3 e = extents();
    return 2.0f * (e[0] * e[1] + e[1] * e[2] + e[2] * e[0]);
  }

  /// Expand AABB to include point
  void expand(const vec3& p) {
    min[0] = std::min(min[0], p[0]);
    min[1] = std::min(min[1], p[1]);
    min[2] = std::min(min[2], p[2]);
    max[0] = std::max(max[0], p[0]);
    max[1] = std::max(max[1], p[1]);
    max[2] = std::max(max[2], p[2]);
  }

  /// Expand AABB to include another AABB
  void expand(const AABB& other) {
    min[0] = std::min(min[0], other.min[0]);
    min[1] = std::min(min[1], other.min[1]);
    min[2] = std::min(min[2], other.min[2]);
    max[0] = std::max(max[0], other.max[0]);
    max[1] = std::max(max[1], other.max[1]);
    max[2] = std::max(max[2], other.max[2]);
  }
};

///
/// Geometry type for raytracing
///
enum class RTGeometryType {
  TriangleMesh,  ///< Polygon mesh with triangles (indices.size() % 3 == 0)
  QuadMesh,      ///< Polygon mesh with quads (indices.size() % 4 == 0)
  MixedMesh,     ///< Mixed triangle/quad mesh (uses face_vertex_counts)
  Sphere,        ///< Analytic sphere (use native ray-sphere intersection)
  Cylinder,      ///< Analytic cylinder (use native ray-cylinder intersection)
  Capsule,       ///< Analytic capsule (use native ray-capsule intersection)
  Cone,          ///< Analytic cone (use native ray-cone intersection)
};

///
/// Raytracing-optimized geometry representation.
/// Supports both polygon meshes (triangles/quads) and analytic primitives.
/// For primitives, use native ray intersection to avoid tessellation cost.
///
struct RTGeometry {
  std::string prim_name;     ///< Prim name (element name)
  std::string abs_path;      ///< Absolute prim path
  std::string display_name;  ///< displayName prim metadata

  RTGeometryType geom_type{RTGeometryType::TriangleMesh};  ///< Geometry type

  // === Mesh Data (for TriangleMesh, QuadMesh, MixedMesh) ===

  std::vector<vec3> vertices;       ///< Unique vertex positions
  std::vector<uint32_t> indices;    ///< Face indices (3 per tri, 4 per quad, or variable)
  std::vector<uint32_t> face_vertex_counts;  ///< For MixedMesh: vertices per face

  // === Per-Vertex Attributes (Indexed, matches vertices) ===

  std::vector<vec3> normals;        ///< Shading normals (optional)
  std::vector<vec2> texcoords0;     ///< Primary UV coordinates (optional)
  std::vector<vec2> texcoords1;     ///< Secondary UV coordinates (optional)
  std::vector<vec4> colors;         ///< Vertex colors (optional)
  std::vector<vec4> tangents;       ///< Tangent space (optional, xyz=tangent, w=handedness)

  // === Per-Face Attributes ===

  std::vector<uint32_t> material_ids;  ///< Material ID per face
  std::vector<vec3> face_normals;      ///< Geometric normals (optional)

  // === Skinning/Animation Data (Optional) ===

  std::vector<vec4> joint_indices;  ///< Up to 4 joint indices per vertex
  std::vector<vec4> joint_weights;  ///< Corresponding weights

  // === Analytic Primitive Data (for Sphere, Cylinder, Capsule, Cone) ===

  vec3 prim_center{0.0f, 0.0f, 0.0f};  ///< Primitive center/origin
  float prim_radius{1.0f};              ///< Sphere/Cylinder/Capsule radius
  float prim_height{2.0f};              ///< Cylinder/Capsule height
  vec3 prim_axis{0.0f, 1.0f, 0.0f};    ///< Cylinder/Capsule/Cone axis direction
  mat4 prim_transform;                  ///< Primitive local-to-world transform

  // === Bounding Volume ===

  AABB bounds;  ///< Axis-aligned bounding box

  // === Optimization Hints ===

  bool is_double_sided{false};  ///< Disable backface culling
  bool cast_shadows{true};      ///< Cast shadows in raytracing
  bool receive_shadows{true};   ///< Receive shadows

  // === Source Reference ===

  int32_t source_mesh_id{-1};  ///< Index to RenderScene::meshes (if available)

  /// Get number of faces (triangles, quads, or mixed)
  size_t num_faces() const {
    if (geom_type == RTGeometryType::TriangleMesh) {
      return indices.size() / 3;
    } else if (geom_type == RTGeometryType::QuadMesh) {
      return indices.size() / 4;
    } else if (geom_type == RTGeometryType::MixedMesh) {
      return face_vertex_counts.size();
    } else {
      return 1;  // Primitive has 1 "face"
    }
  }

  /// Get number of vertices
  size_t num_vertices() const { return vertices.size(); }

  /// Check if this is a mesh (vs analytic primitive)
  bool is_mesh() const {
    return geom_type == RTGeometryType::TriangleMesh ||
           geom_type == RTGeometryType::QuadMesh ||
           geom_type == RTGeometryType::MixedMesh;
  }

  /// Check if this is an analytic primitive
  bool is_primitive() const {
    return geom_type == RTGeometryType::Sphere ||
           geom_type == RTGeometryType::Cylinder ||
           geom_type == RTGeometryType::Capsule ||
           geom_type == RTGeometryType::Cone;
  }

  /// Compute bounding box (from vertices for mesh, or from primitive params)
  void compute_bounds();
};

///
/// Opacity/alpha blending mode
///
enum class OpacityMode {
  Opaque,  ///< Fully opaque, ignore alpha
  Mask,    ///< Binary alpha test (cutout)
  Blend,   ///< Full alpha blending / transparency
};

///
/// Material optimized for path tracing / BSDF evaluation.
/// Supports extended PBR material model with transmission, clearcoat, sheen, SSS, etc.
///
struct RTMaterial {
  std::string name;          ///< Material name (element name)
  std::string abs_path;      ///< Absolute material path
  std::string display_name;  ///< displayName metadata

  // === Base Material Model (PBR Metallic-Roughness or Specular-Glossiness) ===

  vec3 base_color{1.0f, 1.0f, 1.0f};  ///< Base color / albedo
  int32_t base_color_texture{-1};     ///< Index to textures array

  // Metallic-roughness workflow
  float metallic{0.0f};                    ///< Metallic factor
  float roughness{0.5f};                   ///< Roughness factor
  int32_t metallic_roughness_texture{-1};  ///< Combined metallic-roughness texture

  // Specular workflow (alternative)
  vec3 specular_color{1.0f, 1.0f, 1.0f};  ///< Specular color
  float specular_factor{0.5f};            ///< Specular intensity
  int32_t specular_texture{-1};           ///< Specular texture

  // === Extended Material Properties ===

  // Emission
  vec3 emission{0.0f, 0.0f, 0.0f};  ///< Emission color
  float emission_strength{1.0f};    ///< Emission multiplier
  int32_t emission_texture{-1};     ///< Emission texture

  // Normal mapping
  int32_t normal_texture{-1};  ///< Normal map texture
  float normal_scale{1.0f};    ///< Normal map intensity

  // Occlusion
  int32_t occlusion_texture{-1};  ///< Ambient occlusion texture
  float occlusion_strength{1.0f}; ///< AO strength

  // Alpha / Opacity
  float opacity{1.0f};              ///< Opacity value
  int32_t opacity_texture{-1};      ///< Opacity/alpha texture
  OpacityMode opacity_mode{OpacityMode::Opaque};  ///< Alpha blending mode
  float opacity_threshold{0.5f};    ///< Alpha test threshold for Mask mode

  // === Advanced Properties ===

  // Transmission (for glass, translucent materials)
  float transmission{0.0f};          ///< Transmission factor
  int32_t transmission_texture{-1};  ///< Transmission texture

  // Index of refraction
  float ior{1.5f};  ///< Index of refraction (1.0 = no refraction)

  // Clearcoat (for car paint, lacquer, etc.)
  float clearcoat{0.0f};                      ///< Clearcoat intensity
  float clearcoat_roughness{0.0f};            ///< Clearcoat roughness
  int32_t clearcoat_texture{-1};              ///< Clearcoat texture
  int32_t clearcoat_roughness_texture{-1};    ///< Clearcoat roughness texture
  int32_t clearcoat_normal_texture{-1};       ///< Clearcoat normal map

  // Sheen (for cloth, velvet)
  float sheen{0.0f};                          ///< Sheen intensity
  vec3 sheen_color{1.0f, 1.0f, 1.0f};         ///< Sheen color tint
  float sheen_roughness{0.5f};                ///< Sheen roughness

  // Subsurface scattering
  float subsurface{0.0f};                     ///< SSS intensity
  vec3 subsurface_color{1.0f, 1.0f, 1.0f};    ///< SSS color
  float subsurface_radius{1.0f};              ///< SSS radius

  // Anisotropic reflection
  float anisotropic{0.0f};            ///< Anisotropic factor
  float anisotropic_rotation{0.0f};   ///< Anisotropic rotation (0-1)
  int32_t anisotropic_texture{-1};    ///< Anisotropic texture

  // === LTE SpectralAPI: Spectral Material Properties ===
  // Optional wavelength-dependent material data for spectral rendering
  // See doc/lte_spectral_api.md for specification

  /// Spectral reflectance: (wavelength, reflectance) pairs
  /// Use for wavelength-dependent diffuse/specular reflectance
  std::vector<vec2> spd_reflectance;

  /// Spectral IOR: (wavelength, IOR) pairs
  /// Use for wavelength-dependent index of refraction (dispersion)
  std::vector<vec2> spd_ior;

  /// Spectral emission: (wavelength, irradiance) pairs
  /// Use for wavelength-dependent emission (spectral light sources)
  std::vector<vec2> spd_emission;

  /// Interpolation method for spectral reflectance
  int32_t spd_reflectance_interp{0};  ///< 0=Linear, 1=Held, 2=Cubic

  /// Interpolation method for spectral IOR (0=Linear, 1=Held, 2=Cubic, 3=Sellmeier)
  int32_t spd_ior_interp{0};

  /// Sellmeier coefficients for IOR (when spd_ior_interp == 3)
  /// B1, B2, B3, C1, C2, C3 (C values in um^2)
  float sellmeier_B[3]{0.0f, 0.0f, 0.0f};
  float sellmeier_C[3]{0.0f, 0.0f, 0.0f};

  /// Wavelength unit: 0=nanometers (default), 1=micrometers
  int32_t wavelength_unit{0};

  // === Material Behavior Flags ===

  bool is_double_sided{false};        ///< Two-sided material
  bool is_thin_walled{false};         ///< Thin surface approximation
  bool cast_shadows{true};            ///< Cast shadows
  bool receive_shadows{true};         ///< Receive shadows
  bool visible_to_camera{true};       ///< Visible in camera rays
  bool visible_in_reflections{true};  ///< Visible in reflection rays
  bool visible_in_refractions{true};  ///< Visible in refraction rays

  // === Source Reference ===

  int32_t source_material_id{-1};  ///< Index to RenderScene::materials

  // === Helper Methods ===

  /// Check if material is emissive
  bool is_emissive() const {
    return emission[0] > 0.0f || emission[1] > 0.0f || emission[2] > 0.0f;
  }

  /// Check if material is transmissive
  bool is_transmissive() const {
    return transmission > 0.0f || opacity < 1.0f;
  }

  /// Check if material has any textures
  bool has_textures() const {
    return base_color_texture >= 0 || normal_texture >= 0 ||
           emission_texture >= 0 || metallic_roughness_texture >= 0;
  }

  /// Check if material has spectral reflectance data
  bool has_spectral_reflectance() const { return !spd_reflectance.empty(); }

  /// Check if material has spectral IOR data
  bool has_spectral_ior() const {
    return !spd_ior.empty() || spd_ior_interp == 3; // 3 = Sellmeier
  }

  /// Check if material has spectral emission data
  bool has_spectral_emission() const { return !spd_emission.empty(); }

  /// Check if material has any spectral data
  bool has_any_spectral_data() const {
    return has_spectral_reflectance() || has_spectral_ior() || has_spectral_emission();
  }
};

///
/// Area light shape
///
enum class AreaShape {
  Rectangle,  ///< Rectangular area light
  Disk,       ///< Circular disk area light
  Sphere,     ///< Spherical area light
};

///
/// Light source optimized for importance sampling in path tracing
///
struct RTLight {
  std::string name;          ///< Light name
  std::string abs_path;      ///< Absolute light path
  std::string display_name;  ///< displayName metadata

  enum class Type {
    Point,         ///< Omnidirectional point light
    Directional,   ///< Distant directional light (sun)
    Spot,          ///< Spotlight with cone
    Area,          ///< Area light (rectangle, disk, sphere)
    Environment,   ///< Environment map / IBL
    Mesh,          ///< Emissive mesh light
  };

  Type type{Type::Point};  ///< Light type

  // === Common Properties ===

  vec3 color{1.0f, 1.0f, 1.0f};  ///< Light color
  float intensity{1.0f};         ///< Light intensity (multiplier)

  // === Position/Orientation (in world space) ===

  vec3 position{0.0f, 0.0f, 0.0f};       ///< Light position
  vec3 direction{0.0f, -1.0f, 0.0f};     ///< Light direction (for directional/spot)
  mat4 transform;                         ///< Full transformation matrix

  // === Type-Specific Parameters ===

  // Point/Spot light
  float radius{0.0f};  ///< Physical size (for soft shadows)

  // Spot light
  float cone_angle{45.0f};           ///< Outer cone angle (degrees)
  float cone_angle_softness{5.0f};   ///< Inner-to-outer transition (degrees)

  // Area light
  vec2 area_size{1.0f, 1.0f};       ///< Width and height
  AreaShape area_shape{AreaShape::Rectangle};  ///< Area light shape

  // Environment light
  int32_t envmap_texture{-1};       ///< Index to textures (lat-long or cubemap)
  float envmap_rotation{0.0f};      ///< Rotation around Y axis (radians)

  // Mesh light
  int32_t emissive_mesh_id{-1};     ///< Index to RTGeometry

  // === Sampling Data ===

  float total_power{0.0f};  ///< Precomputed total power (for MIS)
  float inv_area{0.0f};     ///< 1/area (for area lights)

  // Environment map importance sampling (optional)
  struct EnvmapSamplingData {
    std::vector<float> cdf;  ///< Cumulative distribution function
    std::vector<float> pdf;  ///< Probability density function
    int32_t width{0};
    int32_t height{0};
  };
  nonstd::optional<EnvmapSamplingData> envmap_sampling;

  // === LTE SpectralAPI: Spectral Light Emission ===

  /// Spectral emission: (wavelength, irradiance) pairs
  /// Unit: W m^-2 nm^-1 for nanometers, W m^-2 um^-1 for micrometers
  std::vector<vec2> spd_emission;

  /// Interpolation method for spectral emission (0=Linear, 1=Held, 2=Cubic)
  int32_t spd_emission_interp{0};

  /// Wavelength unit: 0=nanometers (default), 1=micrometers
  int32_t wavelength_unit{0};

  /// Standard illuminant preset: 0=None, 1=A, 2=D50, 3=D65, 4=E, 5=F1, 6=F2, 7=F7, 8=F11
  int32_t illuminant_preset{0};

  // === Visibility Flags ===

  bool cast_shadows{true};            ///< Cast shadows
  bool visible_to_camera{true};       ///< Visible to camera (for area lights)
  bool visible_in_reflections{true};  ///< Visible in reflections

  // === Source Reference ===

  int32_t source_light_id{-1};  ///< Index to RenderScene::lights

  /// Check if light has spectral emission data
  bool has_spectral_emission() const {
    return !spd_emission.empty() || illuminant_preset != 0;
  }
};

///
/// Instance of geometry with unique transform and material override
///
struct RTInstance {
  uint32_t geometry_id;       ///< Index to RTGeometry
  mat4 transform;             ///< Instance transformation matrix
  mat4 inverse_transform;     ///< Precomputed inverse (for ray transform)

  // Material override (optional)
  std::vector<uint32_t> material_overrides;  ///< Per-primitive material override
                                              ///< Empty = use geometry defaults

  // Visibility flags (per-instance)
  bool visible{true};           ///< Instance visibility
  bool cast_shadows{true};      ///< Cast shadows
  bool receive_shadows{true};   ///< Receive shadows

  // User data
  uint32_t user_id{0};  ///< Application-specific ID
};

///
/// Abstract interface for acceleration structure (BVH, etc.)
///
struct RTAccelerationStructure {
  enum class Type {
    None,      ///< No acceleration structure
    BVH2,      ///< Binary BVH
    BVH4,      ///< 4-wide BVH (SIMD friendly)
    BVH8,      ///< 8-wide BVH (AVX-512)
    QBVH,      ///< Quantized BVH
    Embree,    ///< Intel Embree
    OptiX,     ///< NVIDIA OptiX
    Custom,    ///< User-provided
  };

  Type type{Type::None};  ///< Acceleration structure type

  // Opaque handle to native acceleration structure
  void* native_handle{nullptr};

  // Bounding box of entire scene
  AABB scene_bounds;

  // Build statistics
  struct BuildStats {
    size_t num_nodes{0};       ///< Number of BVH nodes
    size_t num_leaves{0};      ///< Number of leaf nodes
    size_t max_depth{0};       ///< Maximum tree depth
    double build_time_ms{0.0}; ///< Build time in milliseconds
    size_t memory_bytes{0};    ///< Memory usage in bytes
  };
  BuildStats stats;

  // Build configuration
  struct BuildConfig {
    int max_leaf_size{4};              ///< Max triangles per leaf
    int max_depth{64};                 ///< Max tree depth
    float traversal_cost{1.0f};        ///< SAH traversal cost
    float intersection_cost{1.0f};     ///< SAH intersection cost
    bool use_spatial_splits{false};    ///< Higher quality, slower build
  };
};

///
/// Camera for raytracing (similar to RenderCamera)
///
struct RTCamera {
  std::string name;
  std::string abs_path;
  std::string display_name;

  // Camera parameters (matching RenderCamera)
  float znear{0.1f};
  float zfar{1000000.0f};
  float focalLength{50.0f};        // [mm]
  float verticalAperture{15.2908f};  // [mm]
  float horizontalAperture{20.965f}; // [mm]

  // Transform
  mat4 view_matrix;       ///< View matrix (world to camera)
  mat4 projection_matrix; ///< Projection matrix

  // Depth of field
  float aperture_radius{0.0f};  ///< Aperture radius for DOF (0 = pinhole)
  float focus_distance{10.0f};  ///< Focus distance

  int32_t source_camera_id{-1};  ///< Index to RenderScene::cameras
};

///
/// Top-level raytracing scene container
///
class RaytracingScene {
 public:
  std::string usd_filename;

  // === Shared Resources (Can reference RenderScene) ===
  // Note: These pointers can point to RenderScene resources to avoid duplication

  std::vector<TextureImage>* shared_images{nullptr};   ///< Shared with RenderScene
  std::vector<BufferData>* shared_buffers{nullptr};    ///< Shared with RenderScene

  // === Raytracing-Specific Data ===

  std::vector<RTGeometry> geometries;     ///< Raytracing geometry
  std::vector<RTMaterial> materials;      ///< Raytracing materials
  std::vector<RTLight> lights;            ///< Raytracing lights
  std::vector<RTInstance> instances;      ///< Geometry instances
  std::vector<RTCamera> cameras;          ///< Raytracing cameras

  // Acceleration structure
  RTAccelerationStructure accel_structure;

  // === Scene Metadata ===

  vec3 background_color{0.0f, 0.0f, 0.0f};  ///< Background color
  int32_t environment_light_id{-1};         ///< Index to lights (environment)

  // === Methods ===

  /// Build or rebuild acceleration structure
  /// @param config Build configuration
  /// @return true on success
  bool build_acceleration_structure(
      const RTAccelerationStructure::BuildConfig& config);

  /// Estimate memory usage in bytes
  size_t estimate_memory_usage() const;

  /// Validate scene consistency
  /// @param warn Warning messages output
  /// @param err Error messages output
  /// @return true if valid
  bool validate(std::string* warn, std::string* err) const;

  /// Get list of emissive geometry IDs
  std::vector<uint32_t> get_emissive_geometry_ids() const;

  /// Compute scene bounding box
  AABB compute_scene_bounds() const;
};

}  // namespace tydra
}  // namespace lightusd
