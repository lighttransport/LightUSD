/**
 * @file lightusd_c.cpp
 * @brief Implementation of C99 API for LightUSD
 *
 * This file implements the C API by wrapping the LightUSD C++ implementation.
 * It compiles as C++ internally but exposes a pure C interface.
 */

#include "lightusd_c.h"

// Include LightUSD headers - adjust paths as needed
#include "../../src/legacy/lightusd.hh"
#include "../../src/legacy/stage.hh"
#include "../../src/legacy/core/prim.hh"
#include "../../src/legacy/value-types.hh"
#include "../../src/legacy/usdGeom.hh"
#include "../../src/legacy/usdShade.hh"

#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

// Use C linkage for all exported functions
extern "C" {

// ============================================================================
// Internal Implementation Structures
// ============================================================================

/**
 * Internal structure for stage handle
 */
struct lightusd_stage_impl {
    lightusd::legacy::Stage stage;
    std::string last_error;
    std::vector<std::unique_ptr<lightusd_prim_impl>> prim_cache;
    std::vector<std::unique_ptr<lightusd_value_impl>> value_cache;
};

/**
 * Internal structure for prim handle
 */
struct lightusd_prim_impl {
    const lightusd::legacy::Prim* prim;        // Borrowed pointer from stage
    lightusd_stage_impl* parent_stage;    // Owning stage
    std::string cached_path;            // Cached path string
    std::string cached_name;            // Cached name string
};

/**
 * Internal structure for value handle
 */
struct lightusd_value_impl {
    lightusd::legacy::value::Value value;      // Owned value
    lightusd_value_type cached_type;      // Cached type for fast access
    std::string string_cache;          // For string returns
};

// ============================================================================
// Helper Functions
// ============================================================================

static void copy_error_message(const std::string& error, char* error_buf, size_t error_buf_size) {
    if (error_buf && error_buf_size > 0) {
        size_t copy_len = std::min(error.length(), error_buf_size - 1);
        std::memcpy(error_buf, error.c_str(), copy_len);
        error_buf[copy_len] = '\0';
    }
}

static lightusd_prim_type get_prim_type(const lightusd::legacy::Prim& prim) {
    // Check concrete prim types
    if (prim.is<lightusd::legacy::Xform>()) return LIGHTUSD_PRIM_XFORM;
    if (prim.is<lightusd::legacy::GeomMesh>()) return LIGHTUSD_PRIM_MESH;
    if (prim.is<lightusd::legacy::Material>()) return LIGHTUSD_PRIM_MATERIAL;
    if (prim.is<lightusd::legacy::Shader>()) return LIGHTUSD_PRIM_SHADER;
    if (prim.is<lightusd::legacy::GeomCamera>()) return LIGHTUSD_PRIM_CAMERA;
    if (prim.is<lightusd::legacy::SphereLight>()) return LIGHTUSD_PRIM_SPHERE_LIGHT;
    if (prim.is<lightusd::legacy::DistantLight>()) return LIGHTUSD_PRIM_DISTANT_LIGHT;
    if (prim.is<lightusd::legacy::DiskLight>()) return LIGHTUSD_PRIM_DISK_LIGHT;
    if (prim.is<lightusd::legacy::RectLight>()) return LIGHTUSD_PRIM_RECT_LIGHT;
    if (prim.is<lightusd::legacy::CylinderLight>()) return LIGHTUSD_PRIM_CYLINDER_LIGHT;
    if (prim.is<lightusd::legacy::DomeLight>()) return LIGHTUSD_PRIM_DOME_LIGHT;
    if (prim.is<lightusd::legacy::Skeleton>()) return LIGHTUSD_PRIM_SKELETON;
    if (prim.is<lightusd::legacy::SkelRoot>()) return LIGHTUSD_PRIM_SKELROOT;
    if (prim.is<lightusd::legacy::SkelAnimation>()) return LIGHTUSD_PRIM_SKELANIMATION;
    if (prim.is<lightusd::legacy::Scope>()) return LIGHTUSD_PRIM_SCOPE;
    if (prim.is<lightusd::legacy::GeomSubset>()) return LIGHTUSD_PRIM_GEOMSUBSET;
    if (prim.is<lightusd::legacy::GeomSphere>()) return LIGHTUSD_PRIM_SPHERE;
    if (prim.is<lightusd::legacy::GeomCube>()) return LIGHTUSD_PRIM_CUBE;
    if (prim.is<lightusd::legacy::GeomCylinder>()) return LIGHTUSD_PRIM_CYLINDER;
    if (prim.is<lightusd::legacy::GeomCapsule>()) return LIGHTUSD_PRIM_CAPSULE;
    if (prim.is<lightusd::legacy::GeomCone>()) return LIGHTUSD_PRIM_CONE;
    if (prim.is<lightusd::legacy::GeomBasisCurves>()) return LIGHTUSD_PRIM_BASIS_CURVES;
    if (prim.is<lightusd::legacy::GeomNurbsCurves>()) return LIGHTUSD_PRIM_NURBS_CURVE;
    if (prim.is<lightusd::legacy::GeomNurbsPatch>()) return LIGHTUSD_PRIM_NURBS_PATCH;
    if (prim.is<lightusd::legacy::GeomPointInstancer>()) return LIGHTUSD_PRIM_POINT_INSTANCER;

    return LIGHTUSD_PRIM_UNKNOWN;
}

static lightusd_value_type get_value_type(const lightusd::legacy::value::Value& value) {
    // Map LightUSD value types to our C enum
    // This is simplified - real implementation would check actual type
    if (value.is_bool()) return LIGHTUSD_VALUE_BOOL;
    if (value.is_int()) return LIGHTUSD_VALUE_INT;
    if (value.is_uint()) return LIGHTUSD_VALUE_UINT;
    if (value.is_int64()) return LIGHTUSD_VALUE_INT64;
    if (value.is_uint64()) return LIGHTUSD_VALUE_UINT64;
    if (value.is_float()) return LIGHTUSD_VALUE_FLOAT;
    if (value.is_double()) return LIGHTUSD_VALUE_DOUBLE;
    if (value.is_string()) return LIGHTUSD_VALUE_STRING;
    if (value.is_token()) return LIGHTUSD_VALUE_TOKEN;
    if (value.is_asset_path()) return LIGHTUSD_VALUE_ASSET_PATH;

    // Check vector types
    if (value.is_float2()) return LIGHTUSD_VALUE_FLOAT2;
    if (value.is_float3()) return LIGHTUSD_VALUE_FLOAT3;
    if (value.is_float4()) return LIGHTUSD_VALUE_FLOAT4;
    if (value.is_double2()) return LIGHTUSD_VALUE_DOUBLE2;
    if (value.is_double3()) return LIGHTUSD_VALUE_DOUBLE3;
    if (value.is_double4()) return LIGHTUSD_VALUE_DOUBLE4;

    // Check matrix types
    if (value.is_matrix3d()) return LIGHTUSD_VALUE_MATRIX3D;
    if (value.is_matrix4d()) return LIGHTUSD_VALUE_MATRIX4D;

    // Check quaternion types
    if (value.is_quatf()) return LIGHTUSD_VALUE_QUATF;
    if (value.is_quatd()) return LIGHTUSD_VALUE_QUATD;

    // Check array - simplified, would need more logic for array element type
    if (value.is_array()) return LIGHTUSD_VALUE_ARRAY;

    return LIGHTUSD_VALUE_NONE;
}

// ============================================================================
// Global State
// ============================================================================

static bool g_initialized = false;
static bool g_debug_logging = false;

// ============================================================================
// Core API Implementation
// ============================================================================

lightusd_result lightusd_init(void) {
    if (g_initialized) {
        return LIGHTUSD_SUCCESS;
    }

    // Initialize any LightUSD global state if needed
    g_initialized = true;
    return LIGHTUSD_SUCCESS;
}

void lightusd_shutdown(void) {
    g_initialized = false;
    g_debug_logging = false;
}

const char* lightusd_get_version(void) {
    static char version_str[32];
    snprintf(version_str, sizeof(version_str), "%d.%d.%d",
             LIGHTUSD_C_VERSION_MAJOR,
             LIGHTUSD_C_VERSION_MINOR,
             LIGHTUSD_C_VERSION_PATCH);
    return version_str;
}

lightusd_result lightusd_load_from_file(
    const char* filepath,
    const lightusd_load_options* options,
    lightusd_stage* out_stage,
    char* error_buf,
    size_t error_buf_size)
{
    if (!filepath || !out_stage) {
        copy_error_message("Invalid arguments", error_buf, error_buf_size);
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    // Create stage wrapper
    auto stage_impl = std::make_unique<lightusd_stage_impl>();

    // Setup load options
    lightusd::legacy::USDLoadOptions load_opts;
    if (options) {
        if (options->max_memory_limit_mb > 0) {
            load_opts.max_memory_limit_in_mb = static_cast<int32_t>(options->max_memory_limit_mb);
        }
        if (options->max_depth > 0) {
            load_opts.max_depth = options->max_depth;
        }
        // Note: composition control would need additional implementation
    }

    // Load the file
    std::string warn, err;
    bool ret = lightusd::legacy::LoadUSDFromFile(
        filepath,
        &stage_impl->stage,
        &warn,
        &err,
        load_opts);

    if (!ret) {
        std::string full_error = "Failed to load USD: " + err;
        if (!warn.empty()) {
            full_error += " (warnings: " + warn + ")";
        }
        copy_error_message(full_error, error_buf, error_buf_size);
        return LIGHTUSD_ERROR_PARSE_FAILED;
    }

    // Store warnings if in strict mode
    if (options && options->strict_mode && !warn.empty()) {
        copy_error_message("Warnings in strict mode: " + warn, error_buf, error_buf_size);
        return LIGHTUSD_ERROR_PARSE_FAILED;
    }

    *out_stage = stage_impl.release();
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_load_from_memory(
    const void* data,
    size_t size,
    lightusd_format format,
    const lightusd_load_options* options,
    lightusd_stage* out_stage,
    char* error_buf,
    size_t error_buf_size)
{
    if (!data || size == 0 || !out_stage) {
        copy_error_message("Invalid arguments", error_buf, error_buf_size);
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    // Create stage wrapper
    auto stage_impl = std::make_unique<lightusd_stage_impl>();

    // Setup load options
    lightusd::legacy::USDLoadOptions load_opts;
    if (options) {
        if (options->max_memory_limit_mb > 0) {
            load_opts.max_memory_limit_in_mb = static_cast<int32_t>(options->max_memory_limit_mb);
        }
    }

    // Detect format if auto
    std::string detected_ext = ".usda";  // default
    if (format == LIGHTUSD_FORMAT_USDC) {
        detected_ext = ".usdc";
    } else if (format == LIGHTUSD_FORMAT_USDZ) {
        detected_ext = ".usdz";
    } else if (format == LIGHTUSD_FORMAT_AUTO) {
        // Try to detect from content
        if (size >= 4) {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            if (bytes[0] == 'P' && bytes[1] == 'K') {
                detected_ext = ".usdz";  // ZIP format
            } else if (bytes[0] == 0x07 && bytes[1] == 0x53) {
                detected_ext = ".usdc";  // Crate format
            }
        }
    }

    // Load from memory
    std::string warn, err;
    bool ret = lightusd::legacy::LoadUSDFromMemory(
        static_cast<const uint8_t*>(data),
        size,
        "memory" + detected_ext,  // Filename with extension for format detection
        &stage_impl->stage,
        &warn,
        &err,
        load_opts);

    if (!ret) {
        copy_error_message("Failed to load USD: " + err, error_buf, error_buf_size);
        return LIGHTUSD_ERROR_PARSE_FAILED;
    }

    *out_stage = stage_impl.release();
    return LIGHTUSD_SUCCESS;
}

void lightusd_stage_free(lightusd_stage stage) {
    if (stage) {
        delete stage;
    }
}

lightusd_prim lightusd_stage_get_root_prim(lightusd_stage stage) {
    if (!stage) {
        return nullptr;
    }

    // Get the pseudo root
    const lightusd::legacy::Prim* root = &stage->stage.root_prims();
    if (!root) {
        return nullptr;
    }

    // Create prim wrapper
    auto prim_impl = std::make_unique<lightusd_prim_impl>();
    prim_impl->prim = root;
    prim_impl->parent_stage = stage;

    // Cache it and return
    lightusd_prim_impl* result = prim_impl.get();
    stage->prim_cache.push_back(std::move(prim_impl));
    return result;
}

size_t lightusd_prim_get_child_count(lightusd_prim prim) {
    if (!prim || !prim->prim) {
        return 0;
    }
    return prim->prim->children().size();
}

lightusd_prim lightusd_prim_get_child_at(lightusd_prim prim, size_t index) {
    if (!prim || !prim->prim || index >= prim->prim->children().size()) {
        return nullptr;
    }

    const lightusd::legacy::Prim& child = prim->prim->children()[index];

    // Create wrapper for child
    auto child_impl = std::make_unique<lightusd_prim_impl>();
    child_impl->prim = &child;
    child_impl->parent_stage = prim->parent_stage;

    // Cache and return
    lightusd_prim_impl* result = child_impl.get();
    prim->parent_stage->prim_cache.push_back(std::move(child_impl));
    return result;
}

const char* lightusd_prim_get_name(lightusd_prim prim) {
    if (!prim || !prim->prim) {
        return "";
    }

    // Cache the name string
    prim->cached_name = prim->prim->element_name();
    return prim->cached_name.c_str();
}

lightusd_prim_type lightusd_prim_get_type(lightusd_prim prim) {
    if (!prim || !prim->prim) {
        return LIGHTUSD_PRIM_UNKNOWN;
    }
    return get_prim_type(*prim->prim);
}

// ============================================================================
// Extended Core API Implementation
// ============================================================================

const char* lightusd_prim_get_path(lightusd_prim prim) {
    if (!prim || !prim->prim) {
        return "";
    }

    // Build and cache the path
    // Note: Real implementation would use prim->prim_path() or similar
    prim->cached_path = prim->prim->absolute_path();
    return prim->cached_path.c_str();
}

lightusd_prim lightusd_stage_get_prim_at_path(lightusd_stage stage, const char* path) {
    if (!stage || !path) {
        return nullptr;
    }

    // Find prim by path
    // Note: This is simplified - real implementation would traverse the hierarchy
    lightusd::legacy::Path usd_path(path);
    const lightusd::legacy::Prim* found = stage->stage.GetPrimAtPath(usd_path);

    if (!found) {
        return nullptr;
    }

    // Create wrapper
    auto prim_impl = std::make_unique<lightusd_prim_impl>();
    prim_impl->prim = found;
    prim_impl->parent_stage = stage;

    lightusd_prim_impl* result = prim_impl.get();
    stage->prim_cache.push_back(std::move(prim_impl));
    return result;
}

int lightusd_prim_is_type(lightusd_prim prim, lightusd_prim_type type) {
    if (!prim || !prim->prim) {
        return 0;
    }
    return get_prim_type(*prim->prim) == type ? 1 : 0;
}

const char* lightusd_prim_get_type_name(lightusd_prim prim) {
    if (!prim || !prim->prim) {
        return "Unknown";
    }
    return prim->prim->type_name().c_str();
}

size_t lightusd_prim_get_property_count(lightusd_prim prim) {
    if (!prim || !prim->prim) {
        return 0;
    }
    return prim->prim->properties().size();
}

const char* lightusd_prim_get_property_name_at(lightusd_prim prim, size_t index) {
    if (!prim || !prim->prim || index >= prim->prim->properties().size()) {
        return "";
    }

    // Get property name at index
    auto it = prim->prim->properties().begin();
    std::advance(it, index);
    return it->first.c_str();
}

lightusd_value lightusd_prim_get_property(lightusd_prim prim, const char* name) {
    if (!prim || !prim->prim || !name) {
        return nullptr;
    }

    // Find property by name
    auto it = prim->prim->properties().find(name);
    if (it == prim->prim->properties().end()) {
        return nullptr;
    }

    // Create value wrapper
    auto value_impl = std::make_unique<lightusd_value_impl>();
    value_impl->value = it->second.value;
    value_impl->cached_type = get_value_type(value_impl->value);

    // Cache and return
    lightusd_value_impl* result = value_impl.get();
    prim->parent_stage->value_cache.push_back(std::move(value_impl));
    return result;
}

void lightusd_value_free(lightusd_value value) {
    // Values are owned by the stage cache, so we don't delete here
    // In a production implementation, we might want reference counting
}

lightusd_value_type lightusd_value_get_type(lightusd_value value) {
    if (!value) {
        return LIGHTUSD_VALUE_NONE;
    }
    return value->cached_type;
}

int lightusd_value_is_array(lightusd_value value) {
    if (!value) {
        return 0;
    }
    return value->value.is_array() ? 1 : 0;
}

size_t lightusd_value_get_array_size(lightusd_value value) {
    if (!value || !value->value.is_array()) {
        return 0;
    }
    return value->value.array_size();
}

// ============================================================================
// Value Extraction Implementation
// ============================================================================

lightusd_result lightusd_value_get_bool(lightusd_value value, int* out) {
    if (!value || !out) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_bool()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    *out = value->value.as_bool() ? 1 : 0;
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_int(lightusd_value value, int* out) {
    if (!value || !out) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_int()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    *out = value->value.as_int();
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_float(lightusd_value value, float* out) {
    if (!value || !out) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_float()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    *out = value->value.as_float();
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_double(lightusd_value value, double* out) {
    if (!value || !out) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_double()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    *out = value->value.as_double();
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_string(lightusd_value value, const char** out) {
    if (!value || !out) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_string()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    value->string_cache = value->value.as_string();
    *out = value->string_cache.c_str();
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_float3(lightusd_value value, float* out_xyz) {
    if (!value || !out_xyz) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_float3()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    auto vec3 = value->value.as_float3();
    out_xyz[0] = vec3[0];
    out_xyz[1] = vec3[1];
    out_xyz[2] = vec3[2];
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_matrix4d(lightusd_value value, double* out_mat4x4) {
    if (!value || !out_mat4x4) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_matrix4d()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    auto mat = value->value.as_matrix4d();
    // Copy as column-major (OpenGL convention)
    for (int i = 0; i < 16; ++i) {
        out_mat4x4[i] = mat.m[i / 4][i % 4];
    }
    return LIGHTUSD_SUCCESS;
}

// ============================================================================
// Mesh API Implementation
// ============================================================================

lightusd_result lightusd_mesh_get_points(
    lightusd_prim mesh,
    const float** out_points,
    size_t* out_count)
{
    if (!mesh || !mesh->prim || !out_points || !out_count) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    // Check if this is a mesh
    const lightusd::legacy::GeomMesh* mesh_prim = mesh->prim->as<lightusd::legacy::GeomMesh>();
    if (!mesh_prim) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    // Get points attribute
    const auto& points = mesh_prim->points;
    if (points.value.empty()) {
        *out_points = nullptr;
        *out_count = 0;
        return LIGHTUSD_SUCCESS;
    }

    // Return pointer to data
    // Note: Simplified - real implementation would handle time samples
    *out_points = reinterpret_cast<const float*>(points.value.data());
    *out_count = points.value.size();
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_mesh_get_face_counts(
    lightusd_prim mesh,
    const int** out_counts,
    size_t* out_count)
{
    if (!mesh || !mesh->prim || !out_counts || !out_count) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    const lightusd::legacy::GeomMesh* mesh_prim = mesh->prim->as<lightusd::legacy::GeomMesh>();
    if (!mesh_prim) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    const auto& counts = mesh_prim->faceVertexCounts;
    if (counts.value.empty()) {
        *out_counts = nullptr;
        *out_count = 0;
        return LIGHTUSD_SUCCESS;
    }

    *out_counts = counts.value.data();
    *out_count = counts.value.size();
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_mesh_get_indices(
    lightusd_prim mesh,
    const int** out_indices,
    size_t* out_count)
{
    if (!mesh || !mesh->prim || !out_indices || !out_count) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    const lightusd::legacy::GeomMesh* mesh_prim = mesh->prim->as<lightusd::legacy::GeomMesh>();
    if (!mesh_prim) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    const auto& indices = mesh_prim->faceVertexIndices;
    if (indices.value.empty()) {
        *out_indices = nullptr;
        *out_count = 0;
        return LIGHTUSD_SUCCESS;
    }

    *out_indices = indices.value.data();
    *out_count = indices.value.size();
    return LIGHTUSD_SUCCESS;
}

// ============================================================================
// Utility Functions
// ============================================================================

void lightusd_free(void* ptr) {
    if (ptr) {
        free(ptr);
    }
}

const char* lightusd_result_to_string(lightusd_result result) {
    switch (result) {
        case LIGHTUSD_SUCCESS: return "Success";
        case LIGHTUSD_ERROR_FILE_NOT_FOUND: return "File not found";
        case LIGHTUSD_ERROR_PARSE_FAILED: return "Parse failed";
        case LIGHTUSD_ERROR_OUT_OF_MEMORY: return "Out of memory";
        case LIGHTUSD_ERROR_INVALID_ARGUMENT: return "Invalid argument";
        case LIGHTUSD_ERROR_NOT_SUPPORTED: return "Not supported";
        case LIGHTUSD_ERROR_COMPOSITION_FAILED: return "Composition failed";
        case LIGHTUSD_ERROR_INVALID_FORMAT: return "Invalid format";
        case LIGHTUSD_ERROR_IO_ERROR: return "I/O error";
        case LIGHTUSD_ERROR_INTERNAL: return "Internal error";
        default: return "Unknown error";
    }
}

const char* lightusd_prim_type_to_string(lightusd_prim_type type) {
    switch (type) {
        case LIGHTUSD_PRIM_UNKNOWN: return "Unknown";
        case LIGHTUSD_PRIM_XFORM: return "Xform";
        case LIGHTUSD_PRIM_MESH: return "Mesh";
        case LIGHTUSD_PRIM_MATERIAL: return "Material";
        case LIGHTUSD_PRIM_SHADER: return "Shader";
        case LIGHTUSD_PRIM_CAMERA: return "Camera";
        case LIGHTUSD_PRIM_DISTANT_LIGHT: return "DistantLight";
        case LIGHTUSD_PRIM_SPHERE_LIGHT: return "SphereLight";
        case LIGHTUSD_PRIM_RECT_LIGHT: return "RectLight";
        case LIGHTUSD_PRIM_DISK_LIGHT: return "DiskLight";
        case LIGHTUSD_PRIM_CYLINDER_LIGHT: return "CylinderLight";
        case LIGHTUSD_PRIM_DOME_LIGHT: return "DomeLight";
        case LIGHTUSD_PRIM_SKELETON: return "Skeleton";
        case LIGHTUSD_PRIM_SKELROOT: return "SkelRoot";
        case LIGHTUSD_PRIM_SKELANIMATION: return "SkelAnimation";
        case LIGHTUSD_PRIM_SCOPE: return "Scope";
        case LIGHTUSD_PRIM_GEOMSUBSET: return "GeomSubset";
        case LIGHTUSD_PRIM_SPHERE: return "Sphere";
        case LIGHTUSD_PRIM_CUBE: return "Cube";
        case LIGHTUSD_PRIM_CYLINDER: return "Cylinder";
        case LIGHTUSD_PRIM_CAPSULE: return "Capsule";
        case LIGHTUSD_PRIM_CONE: return "Cone";
        case LIGHTUSD_PRIM_NURBS_PATCH: return "NurbsPatch";
        case LIGHTUSD_PRIM_NURBS_CURVE: return "NurbsCurve";
        case LIGHTUSD_PRIM_BASIS_CURVES: return "BasisCurves";
        case LIGHTUSD_PRIM_POINT_INSTANCER: return "PointInstancer";
        case LIGHTUSD_PRIM_VOLUME: return "Volume";
        default: return "Unknown";
    }
}

lightusd_format lightusd_detect_format(const char* filepath) {
    if (!filepath) {
        return LIGHTUSD_FORMAT_AUTO;
    }

    std::string path(filepath);

    // Check extension
    if (path.ends_with(".usda")) {
        return LIGHTUSD_FORMAT_USDA;
    } else if (path.ends_with(".usdc")) {
        return LIGHTUSD_FORMAT_USDC;
    } else if (path.ends_with(".usdz")) {
        return LIGHTUSD_FORMAT_USDZ;
    }

    return LIGHTUSD_FORMAT_AUTO;
}

void lightusd_stage_print_hierarchy(lightusd_stage stage, int max_depth) {
    if (!stage) {
        return;
    }

    // Simple hierarchy printer
    std::function<void(const lightusd::legacy::Prim*, int)> print_prim;
    print_prim = [&](const lightusd::legacy::Prim* prim, int depth) {
        if (max_depth > 0 && depth >= max_depth) {
            return;
        }

        // Indent
        for (int i = 0; i < depth; ++i) {
            printf("  ");
        }

        // Print prim info
        printf("- %s [%s]\n", prim->element_name().c_str(), prim->type_name().c_str());

        // Print children
        for (const auto& child : prim->children()) {
            print_prim(&child, depth + 1);
        }
    };

    printf("Stage Hierarchy:\n");
    print_prim(&stage->stage.root_prims(), 0);
}

void lightusd_set_debug_logging(int enable) {
    g_debug_logging = enable != 0;
}

// ============================================================================
// Additional Value Extraction Functions
// ============================================================================

lightusd_result lightusd_value_get_uint(lightusd_value value, unsigned int* out) {
    if (!value || !out) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_uint()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    *out = value->value.as_uint();
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_int64(lightusd_value value, int64_t* out) {
    if (!value || !out) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_int64()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    *out = value->value.as_int64();
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_uint64(lightusd_value value, uint64_t* out) {
    if (!value || !out) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_uint64()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    *out = value->value.as_uint64();
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_token(lightusd_value value, const char** out) {
    if (!value || !out) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_token()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    value->string_cache = value->value.as_token();
    *out = value->string_cache.c_str();
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_asset_path(lightusd_value value, const char** out) {
    if (!value || !out) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_asset_path()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    value->string_cache = value->value.as_asset_path();
    *out = value->string_cache.c_str();
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_float2(lightusd_value value, float* out_xy) {
    if (!value || !out_xy) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_float2()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    auto vec = value->value.as_float2();
    out_xy[0] = vec[0];
    out_xy[1] = vec[1];
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_float4(lightusd_value value, float* out_xyzw) {
    if (!value || !out_xyzw) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_float4()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    auto vec = value->value.as_float4();
    out_xyzw[0] = vec[0];
    out_xyzw[1] = vec[1];
    out_xyzw[2] = vec[2];
    out_xyzw[3] = vec[3];
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_double2(lightusd_value value, double* out_xy) {
    if (!value || !out_xy) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_double2()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    auto vec = value->value.as_double2();
    out_xy[0] = vec[0];
    out_xy[1] = vec[1];
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_double3(lightusd_value value, double* out_xyz) {
    if (!value || !out_xyz) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_double3()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    auto vec = value->value.as_double3();
    out_xyz[0] = vec[0];
    out_xyz[1] = vec[1];
    out_xyz[2] = vec[2];
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_double4(lightusd_value value, double* out_xyzw) {
    if (!value || !out_xyzw) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_double4()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    auto vec = value->value.as_double4();
    out_xyzw[0] = vec[0];
    out_xyzw[1] = vec[1];
    out_xyzw[2] = vec[2];
    out_xyzw[3] = vec[3];
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_matrix3d(lightusd_value value, double* out_mat3x3) {
    if (!value || !out_mat3x3) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_matrix3d()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    auto mat = value->value.as_matrix3d();
    for (int i = 0; i < 9; ++i) {
        out_mat3x3[i] = mat.m[i / 3][i % 3];
    }
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_value_get_float_array(lightusd_value value, const float** out_data, size_t* out_count) {
    if (!value || !out_data || !out_count) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_array() || !value->value.is_float_array()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    // Get array data - implementation depends on LightUSD array interface
    *out_data = nullptr;
    *out_count = 0;
    return LIGHTUSD_ERROR_NOT_SUPPORTED;  // TODO: Implement
}

lightusd_result lightusd_value_get_int_array(lightusd_value value, const int** out_data, size_t* out_count) {
    if (!value || !out_data || !out_count) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_array() || !value->value.is_int_array()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    *out_data = nullptr;
    *out_count = 0;
    return LIGHTUSD_ERROR_NOT_SUPPORTED;  // TODO: Implement
}

lightusd_result lightusd_value_get_float3_array(lightusd_value value, const float** out_data, size_t* out_count) {
    if (!value || !out_data || !out_count) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!value->value.is_array() || !value->value.is_float3_array()) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    *out_data = nullptr;
    *out_count = 0;
    return LIGHTUSD_ERROR_NOT_SUPPORTED;  // TODO: Implement
}

// ============================================================================
// Additional Mesh Functions
// ============================================================================

lightusd_result lightusd_mesh_get_normals(
    lightusd_prim mesh,
    const float** out_normals,
    size_t* out_count)
{
    if (!mesh || !mesh->prim || !out_normals || !out_count) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    const lightusd::legacy::GeomMesh* mesh_prim = mesh->prim->as<lightusd::legacy::GeomMesh>();
    if (!mesh_prim) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    const auto& normals = mesh_prim->normals;
    if (normals.value.empty()) {
        *out_normals = nullptr;
        *out_count = 0;
        return LIGHTUSD_SUCCESS;
    }

    *out_normals = reinterpret_cast<const float*>(normals.value.data());
    *out_count = normals.value.size();
    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_mesh_get_uvs(
    lightusd_prim mesh,
    const float** out_uvs,
    size_t* out_count,
    int primvar_index)
{
    if (!mesh || !mesh->prim || !out_uvs || !out_count) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    const lightusd::legacy::GeomMesh* mesh_prim = mesh->prim->as<lightusd::legacy::GeomMesh>();
    if (!mesh_prim) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    // Find the primary UV attribute (usually "st" or "uv")
    // This is simplified - real implementation would search primvars
    const auto& st = mesh_prim->st;
    if (st.value.empty()) {
        *out_uvs = nullptr;
        *out_count = 0;
        return LIGHTUSD_SUCCESS;
    }

    *out_uvs = reinterpret_cast<const float*>(st.value.data());
    *out_count = st.value.size();
    return LIGHTUSD_SUCCESS;
}

const char* lightusd_mesh_get_subdivision_scheme(lightusd_prim mesh) {
    if (!mesh || !mesh->prim) {
        return "none";
    }

    const lightusd::legacy::GeomMesh* mesh_prim = mesh->prim->as<lightusd::legacy::GeomMesh>();
    if (!mesh_prim) {
        return "none";
    }

    if (mesh_prim->subdivisionScheme == lightusd::Sdf_Scheme::SdfSchemeCatmullClark) {
        return "catmullClark";
    } else if (mesh_prim->subdivisionScheme == lightusd::Sdf_Scheme::SdfSchemeLoop) {
        return "loop";
    } else if (mesh_prim->subdivisionScheme == lightusd::Sdf_Scheme::SdfSchemeBilinear) {
        return "bilinear";
    }

    return "none";
}

// ============================================================================
// Transform and Matrix Functions
// ============================================================================

lightusd_result lightusd_xform_get_local_matrix(
    lightusd_prim xform,
    double time,
    double* out_matrix)
{
    if (!xform || !xform->prim || !out_matrix) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    const lightusd::legacy::Xform* xform_prim = xform->prim->as<lightusd::legacy::Xform>();
    if (!xform_prim) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    // Evaluate transform at time
    lightusd::legacy::value::matrix4d mat;
    bool ret = xform_prim->getLocalMatrix(time, &mat);
    if (!ret) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    // Copy matrix (column-major)
    for (int i = 0; i < 16; ++i) {
        out_matrix[i] = mat.m[i / 4][i % 4];
    }

    return LIGHTUSD_SUCCESS;
}

lightusd_result lightusd_prim_get_world_matrix(
    lightusd_prim prim,
    double time,
    double* out_matrix)
{
    if (!prim || !prim->prim || !out_matrix) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    // Build world matrix by accumulating parent transforms
    // This is a simplified version - real implementation would traverse up
    lightusd::legacy::value::matrix4d world_mat = lightusd::legacy::value::matrix4d::identity();

    const lightusd::legacy::Prim* current = prim->prim;
    while (current) {
        const lightusd::legacy::Xform* xform = current->as<lightusd::legacy::Xform>();
        if (xform) {
            lightusd::legacy::value::matrix4d local_mat;
            if (xform->getLocalMatrix(time, &local_mat)) {
                world_mat = local_mat * world_mat;
            }
        }

        // Get parent - implementation depends on LightUSD structure
        current = nullptr;  // TODO: Get parent prim
    }

    // Copy result
    for (int i = 0; i < 16; ++i) {
        out_matrix[i] = world_mat.m[i / 4][i % 4];
    }

    return LIGHTUSD_SUCCESS;
}

// ============================================================================
// Material and Shader Functions
// ============================================================================

lightusd_prim lightusd_prim_get_bound_material(lightusd_prim prim) {
    if (!prim || !prim->prim) {
        return nullptr;
    }

    // Look for materialBinding relationship
    const auto& rels = prim->prim->relationships();
    for (const auto& rel_pair : rels) {
        if (rel_pair.first == "material:binding") {
            // Get target of relationship
            if (!rel_pair.second.targets().empty()) {
                const auto& target = rel_pair.second.targets()[0];
                lightusd_prim mat = lightusd_stage_get_prim_at_path(
                    prim->parent_stage, target.GetAsString().c_str());
                return mat;
            }
        }
    }

    return nullptr;
}

lightusd_prim lightusd_material_get_surface_shader(lightusd_prim material) {
    if (!material || !material->prim) {
        return nullptr;
    }

    const lightusd::legacy::Material* mat = material->prim->as<lightusd::legacy::Material>();
    if (!mat) {
        return nullptr;
    }

    // Look for surface shader
    if (!mat->surfaceShader.empty()) {
        lightusd_prim shader = lightusd_stage_get_prim_at_path(
            material->parent_stage, mat->surfaceShader.c_str());
        return shader;
    }

    return nullptr;
}

lightusd_value lightusd_shader_get_input(lightusd_prim shader, const char* input_name) {
    if (!shader || !shader->prim || !input_name) {
        return nullptr;
    }

    const lightusd::legacy::Shader* shdr = shader->prim->as<lightusd::legacy::Shader>();
    if (!shdr) {
        return nullptr;
    }

    // Get input value
    auto it = shdr->inputs.find(input_name);
    if (it == shdr->inputs.end()) {
        return nullptr;
    }

    // Create value wrapper
    auto value_impl = std::make_unique<lightusd_value_impl>();
    value_impl->value = it->second.value;
    value_impl->cached_type = get_value_type(value_impl->value);

    lightusd_value_impl* result = value_impl.get();
    shader->parent_stage->value_cache.push_back(std::move(value_impl));
    return result;
}

const char* lightusd_shader_get_type_id(lightusd_prim shader) {
    if (!shader || !shader->prim) {
        return "Unknown";
    }

    const lightusd::legacy::Shader* shdr = shader->prim->as<lightusd::legacy::Shader>();
    if (!shdr) {
        return "Unknown";
    }

    return shdr->info.shader_type.c_str();
}

// ============================================================================
// Animation and Time Sampling Functions
// ============================================================================

int lightusd_stage_has_animation(lightusd_stage stage) {
    if (!stage) {
        return 0;
    }

    // Check if stage has any time codes
    return stage->stage.timeCodesPerSecond > 0 ? 1 : 0;
}

lightusd_result lightusd_stage_get_time_range(
    lightusd_stage stage,
    double* out_start_time,
    double* out_end_time,
    double* out_fps)
{
    if (!stage || !out_start_time || !out_end_time || !out_fps) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    if (!lightusd_stage_has_animation(stage)) {
        *out_start_time = 0.0;
        *out_end_time = 0.0;
        *out_fps = 24.0;
        return LIGHTUSD_SUCCESS;
    }

    // Get time range from stage metadata
    *out_start_time = stage->stage.startTimeCode;
    *out_end_time = stage->stage.endTimeCode;
    *out_fps = stage->stage.timeCodesPerSecond;

    return LIGHTUSD_SUCCESS;
}

int lightusd_value_is_animated(lightusd_value value) {
    if (!value) {
        return 0;
    }

    // Check if value has time samples
    // Implementation depends on LightUSD value structure
    return 0;  // TODO: Implement
}

lightusd_result lightusd_value_get_time_samples(
    lightusd_value value,
    const double** out_times,
    size_t* out_count)
{
    if (!value || !out_times || !out_count) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    *out_times = nullptr;
    *out_count = 0;
    return LIGHTUSD_ERROR_NOT_SUPPORTED;  // TODO: Implement
}

lightusd_value lightusd_value_eval_at_time(lightusd_value value, double time) {
    if (!value) {
        return nullptr;
    }

    // Create new value at evaluated time
    auto eval_value = std::make_unique<lightusd_value_impl>();
    eval_value->value = value->value;  // Simplified - would interpolate
    eval_value->cached_type = value->cached_type;

    // TODO: Actually evaluate at time

    lightusd_value_impl* result = eval_value.get();
    // Note: We need access to stage to cache this
    return result;
}

// ============================================================================
// Metadata Functions
// ============================================================================

lightusd_value lightusd_prim_get_metadata(lightusd_prim prim, const char* key) {
    if (!prim || !prim->prim || !key) {
        return nullptr;
    }

    // Get metadata from prim
    const auto& meta = prim->prim->meta;

    // This is simplified - real implementation would access metadata properly
    return nullptr;  // TODO: Implement
}

lightusd_result lightusd_prim_get_metadata_keys(
    lightusd_prim prim,
    const char*** out_keys,
    size_t* out_count)
{
    if (!prim || !prim->prim || !out_keys || !out_count) {
        return LIGHTUSD_ERROR_INVALID_ARGUMENT;
    }

    *out_keys = nullptr;
    *out_count = 0;
    return LIGHTUSD_ERROR_NOT_SUPPORTED;  // TODO: Implement
}

// ============================================================================
// Debug and Utility Functions
// ============================================================================

void lightusd_get_memory_stats(
    lightusd_stage stage,
    size_t* out_bytes_used,
    size_t* out_bytes_peak)
{
    if (!out_bytes_used || !out_bytes_peak) {
        return;
    }

    if (!stage) {
        // Return global stats
        *out_bytes_used = 0;
        *out_bytes_peak = 0;
        return;
    }

    // Calculate approximate memory usage
    // This is a simplified estimate
    size_t total = sizeof(lightusd_stage_impl);

    // Add size of cached prims and values
    total += stage->prim_cache.size() * sizeof(lightusd_prim_impl);
    total += stage->value_cache.size() * sizeof(lightusd_value_impl);

    // Add stage data estimate
    total += 1024 * 1024;  // Rough estimate for stage data

    *out_bytes_used = total;
    *out_bytes_peak = total;  // Simplified - would track peak
}

const char* lightusd_value_type_to_string(lightusd_value_type type) {
    switch (type) {
        case LIGHTUSD_VALUE_NONE: return "None";
        case LIGHTUSD_VALUE_BOOL: return "Bool";
        case LIGHTUSD_VALUE_INT: return "Int";
        case LIGHTUSD_VALUE_UINT: return "UInt";
        case LIGHTUSD_VALUE_INT64: return "Int64";
        case LIGHTUSD_VALUE_UINT64: return "UInt64";
        case LIGHTUSD_VALUE_HALF: return "Half";
        case LIGHTUSD_VALUE_FLOAT: return "Float";
        case LIGHTUSD_VALUE_DOUBLE: return "Double";
        case LIGHTUSD_VALUE_STRING: return "String";
        case LIGHTUSD_VALUE_TOKEN: return "Token";
        case LIGHTUSD_VALUE_ASSET_PATH: return "AssetPath";
        case LIGHTUSD_VALUE_INT2: return "Int2";
        case LIGHTUSD_VALUE_INT3: return "Int3";
        case LIGHTUSD_VALUE_INT4: return "Int4";
        case LIGHTUSD_VALUE_HALF2: return "Half2";
        case LIGHTUSD_VALUE_HALF3: return "Half3";
        case LIGHTUSD_VALUE_HALF4: return "Half4";
        case LIGHTUSD_VALUE_FLOAT2: return "Float2";
        case LIGHTUSD_VALUE_FLOAT3: return "Float3";
        case LIGHTUSD_VALUE_FLOAT4: return "Float4";
        case LIGHTUSD_VALUE_DOUBLE2: return "Double2";
        case LIGHTUSD_VALUE_DOUBLE3: return "Double3";
        case LIGHTUSD_VALUE_DOUBLE4: return "Double4";
        case LIGHTUSD_VALUE_MATRIX2D: return "Matrix2D";
        case LIGHTUSD_VALUE_MATRIX3D: return "Matrix3D";
        case LIGHTUSD_VALUE_MATRIX4D: return "Matrix4D";
        case LIGHTUSD_VALUE_QUATH: return "QuatH";
        case LIGHTUSD_VALUE_QUATF: return "QuatF";
        case LIGHTUSD_VALUE_QUATD: return "QuatD";
        case LIGHTUSD_VALUE_COLOR3F: return "Color3F";
        case LIGHTUSD_VALUE_COLOR3D: return "Color3D";
        case LIGHTUSD_VALUE_COLOR4F: return "Color4F";
        case LIGHTUSD_VALUE_COLOR4D: return "Color4D";
        case LIGHTUSD_VALUE_NORMAL3F: return "Normal3F";
        case LIGHTUSD_VALUE_NORMAL3D: return "Normal3D";
        case LIGHTUSD_VALUE_POINT3F: return "Point3F";
        case LIGHTUSD_VALUE_POINT3D: return "Point3D";
        case LIGHTUSD_VALUE_TEXCOORD2F: return "TexCoord2F";
        case LIGHTUSD_VALUE_TEXCOORD2D: return "TexCoord2D";
        case LIGHTUSD_VALUE_TEXCOORD3F: return "TexCoord3F";
        case LIGHTUSD_VALUE_TEXCOORD3D: return "TexCoord3D";
        case LIGHTUSD_VALUE_ARRAY: return "Array";
        case LIGHTUSD_VALUE_DICTIONARY: return "Dictionary";
        case LIGHTUSD_VALUE_TIME_SAMPLES: return "TimeSamples";
        case LIGHTUSD_VALUE_RELATIONSHIP: return "Relationship";
        default: return "Unknown";
    }
}

} // extern "C"