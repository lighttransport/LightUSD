// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Syoyo Fujita.
// Copyright 2024 - Present, Light Transport Entertainment Inc.

///
/// @file typed-array-render-data.hh
/// @brief TypedArray-enhanced render-data structures for memory optimization
///
/// This header provides memory-optimized versions of Tydra render-data structures
/// that leverage TypedArray for reduced buffer copies and in-place transformations.
///

#pragma once

#include "render-data.hh"
#include "../typed-array.hh"
#include <memory>
#include <type_traits>

namespace lightusd {
namespace tydra {
namespace typed_array_opt {

///
/// Enhanced BufferData that uses TypedArray for memory optimization
///
template<typename T = uint8_t>
struct TypedBufferData {
    ComponentType componentType{ComponentType::UInt8};
    TypedArray<T> data;

    // Constructor from existing BufferData
    TypedBufferData(const BufferData& buffer) {
        componentType = buffer.componentType;
        if (!buffer.data.empty()) {
            data = TypedArray<T>(reinterpret_cast<const T*>(buffer.data.data()),
                                 buffer.data.size() / sizeof(T));
        }
    }

    // Constructor with TypedArray
    TypedBufferData(ComponentType type, TypedArray<T>&& typed_data)
        : componentType(type), data(std::move(typed_data)) {}

    // In-place type conversion methods
    template<typename N>
    TypedBufferData<N> transform_to(std::function<void(const T&, N&)> converter) && {
        static_assert(sizeof(T) >= sizeof(N), "Can only transform to smaller or equal size types");

        auto new_data = data.template transform_1to1<N>(converter);
        return TypedBufferData<N>(componentType, std::move(new_data));
    }

    template<typename N>
    TypedBufferData<N> expand_to(std::function<void(const T&, N&)> converter) && {
        auto new_data = data.template transform_expand<N>(converter);
        return TypedBufferData<N>(componentType, std::move(new_data));
    }

    // Get span view for efficient access
    nonstd::span<T> span() { return data.span(); }
    nonstd::span<const T> span() const { return data.span(); }

    // Memory usage estimation
    size_t estimate_memory_usage() const {
        return sizeof(TypedBufferData) + data.storage().capacity();
    }
};

///
/// Enhanced VertexAttribute using TypedArray for optimized vertex processing
///
struct TypedVertexAttribute {
    std::string name;
    VertexAttributeFormat format{VertexAttributeFormat::Vec3};
    uint32_t elementSize{1};
    uint32_t stride{0};
    VertexVariability variability{VertexVariability::Vertex};
    uint64_t handle{0};

    // Type-erased storage using variant of common vertex data types
    struct Storage {
        enum Type {
            FLOAT, VEC2, VEC3, VEC4,
            INT, IVEC2, IVEC3, IVEC4,
            UINT8, UINT16, UINT32
        } type;

        // Storage for different types
        std::unique_ptr<TypedArray<float>> float_data;
        std::unique_ptr<TypedArray<value::float2>> vec2_data;
        std::unique_ptr<TypedArray<value::float3>> vec3_data;
        std::unique_ptr<TypedArray<value::float4>> vec4_data;
        std::unique_ptr<TypedArray<int32_t>> int_data;
        std::unique_ptr<TypedArray<uint8_t>> uint8_data;
        std::unique_ptr<TypedArray<uint16_t>> uint16_data;
        std::unique_ptr<TypedArray<uint32_t>> uint32_data;

        template<typename T>
        void set_data(TypedArray<T>&& data) {
            if constexpr (std::is_same_v<T, float>) {
                type = FLOAT;
                float_data = std::make_unique<TypedArray<float>>(std::move(data));
            } else if constexpr (std::is_same_v<T, value::float2>) {
                type = VEC2;
                vec2_data = std::make_unique<TypedArray<value::float2>>(std::move(data));
            } else if constexpr (std::is_same_v<T, value::float3>) {
                type = VEC3;
                vec3_data = std::make_unique<TypedArray<value::float3>>(std::move(data));
            } else if constexpr (std::is_same_v<T, value::float4>) {
                type = VEC4;
                vec4_data = std::make_unique<TypedArray<value::float4>>(std::move(data));
            } else if constexpr (std::is_same_v<T, int32_t>) {
                type = INT;
                int_data = std::make_unique<TypedArray<int32_t>>(std::move(data));
            } else if constexpr (std::is_same_v<T, uint8_t>) {
                type = UINT8;
                uint8_data = std::make_unique<TypedArray<uint8_t>>(std::move(data));
            } else if constexpr (std::is_same_v<T, uint16_t>) {
                type = UINT16;
                uint16_data = std::make_unique<TypedArray<uint16_t>>(std::move(data));
            } else if constexpr (std::is_same_v<T, uint32_t>) {
                type = UINT32;
                uint32_data = std::make_unique<TypedArray<uint32_t>>(std::move(data));
            }
        }

        template<typename T>
        TypedArray<T>* get_data() {
            if constexpr (std::is_same_v<T, float>) {
                return (type == FLOAT) ? float_data.get() : nullptr;
            } else if constexpr (std::is_same_v<T, value::float3>) {
                return (type == VEC3) ? vec3_data.get() : nullptr;
            } // ... add other types as needed
            return nullptr;
        }

        size_t vertex_count() const {
            switch(type) {
                case FLOAT: return float_data ? float_data->size() : 0;
                case VEC2: return vec2_data ? vec2_data->size() : 0;
                case VEC3: return vec3_data ? vec3_data->size() : 0;
                case VEC4: return vec4_data ? vec4_data->size() : 0;
                case INT: return int_data ? int_data->size() : 0;
                case UINT8: return uint8_data ? uint8_data->size() : 0;
                case UINT16: return uint16_data ? uint16_data->size() : 0;
                case UINT32: return uint32_data ? uint32_data->size() : 0;
            }
            return 0;
        }

        size_t estimate_memory_usage() const {
            switch(type) {
                case FLOAT: return float_data ? float_data->storage().capacity() : 0;
                case VEC2: return vec2_data ? vec2_data->storage().capacity() : 0;
                case VEC3: return vec3_data ? vec3_data->storage().capacity() : 0;
                case VEC4: return vec4_data ? vec4_data->storage().capacity() : 0;
                case INT: return int_data ? int_data->storage().capacity() : 0;
                case UINT8: return uint8_data ? uint8_data->storage().capacity() : 0;
                case UINT16: return uint16_data ? uint16_data->storage().capacity() : 0;
                case UINT32: return uint32_data ? uint32_data->storage().capacity() : 0;
            }
            return 0;
        }
    } storage;

    std::vector<uint32_t> indices; // Dedicated index buffer

    // Constructor from existing VertexAttribute
    TypedVertexAttribute(const VertexAttribute& vattr) {
        name = vattr.name;
        format = vattr.format;
        elementSize = vattr.elementSize;
        stride = vattr.stride;
        variability = vattr.variability;
        handle = vattr.handle;
        indices = vattr.indices;

        // Convert raw data to appropriate TypedArray based on format
        convert_from_raw_data(vattr);
    }

    // Accessors
    size_t vertex_count() const { return storage.vertex_count(); }
    bool empty() const { return vertex_count() == 0; }
    size_t num_bytes() const { return storage.estimate_memory_usage(); }

    // Type conversion utilities
    template<typename SrcT, typename DstT>
    void transform_data_inplace(std::function<void(const SrcT&, DstT&)> converter) {
        auto* src_data = storage.template get_data<SrcT>();
        if (!src_data) return;

        if constexpr (sizeof(SrcT) >= sizeof(DstT)) {
            // Shrinking transform - can do in-place
            auto dst_data = src_data->template transform_1to1<DstT>(converter);
            storage.template set_data<DstT>(std::move(dst_data));
        } else {
            // Expanding transform - need buffer growth
            auto dst_data = src_data->template transform_expand<DstT>(converter);
            storage.template set_data<DstT>(std::move(dst_data));
        }
    }

    // Precision reduction for memory optimization
    void reduce_to_half_precision() {
        if (storage.type == Storage::VEC3) {
            // Convert float3 to half3 (uint16_t x3)
            auto* vec3_data = storage.get_data<value::float3>();
            if (vec3_data) {
                auto half_data = vec3_data->template transform_1to1<uint16_t>([](const value::float3& src, uint16_t& dst) {
                    // Simple float to half conversion (would need proper half-float library)
                    dst = static_cast<uint16_t>(src[0] * 65535.0f); // Simplified
                });
                storage.set_data(std::move(half_data));
                format = VertexAttributeFormat::Half3;
            }
        }
    }

    // Quantize floating point vertex colors to 8-bit
    void quantize_vertex_colors() {
        if (storage.type == Storage::VEC3) {
            auto* vec3_data = storage.get_data<value::float3>();
            if (vec3_data) {
                auto uint8_data = vec3_data->template transform_1to1<uint8_t>([](const value::float3& color, uint8_t& quantized) {
                    quantized = static_cast<uint8_t>(std::clamp(color[0] * 255.0f, 0.0f, 255.0f));
                });
                storage.set_data(std::move(uint8_data));
                format = VertexAttributeFormat::Byte3;
            }
        }
    }

    size_t estimate_memory_usage() const {
        return sizeof(TypedVertexAttribute) + storage.estimate_memory_usage() +
               indices.capacity() * sizeof(uint32_t);
    }

private:
    void convert_from_raw_data(const VertexAttribute& vattr) {
        switch(vattr.format) {
            case VertexAttributeFormat::Float: {
                auto float_array = TypedArray<float>(
                    reinterpret_cast<const float*>(vattr.data.data()),
                    vattr.data.size() / sizeof(float)
                );
                storage.set_data(std::move(float_array));
                break;
            }
            case VertexAttributeFormat::Vec3: {
                auto vec3_array = TypedArray<value::float3>(
                    reinterpret_cast<const value::float3*>(vattr.data.data()),
                    vattr.data.size() / sizeof(value::float3)
                );
                storage.set_data(std::move(vec3_array));
                break;
            }
            case VertexAttributeFormat::Vec2: {
                auto vec2_array = TypedArray<value::float2>(
                    reinterpret_cast<const value::float2*>(vattr.data.data()),
                    vattr.data.size() / sizeof(value::float2)
                );
                storage.set_data(std::move(vec2_array));
                break;
            }
            // Add other format conversions as needed
            default: {
                // Fallback to uint8_t for unknown formats
                auto uint8_array = TypedArray<uint8_t>(vattr.data.data(), vattr.data.size());
                storage.set_data(std::move(uint8_array));
                break;
            }
        }
    }
};

///
/// Memory-optimized RenderMesh using TypedArray
///
struct TypedRenderMesh {
    std::string prim_name;
    std::string abs_path;
    std::string display_name;

    bool is_single_indexable{false};
    bool doubleSided{false};
    bool is_rightHanded{true};

    // Vertex data using TypedArray for efficient storage and transformation
    TypedArray<value::float3> points;

    // Vertex attributes using TypedVertexAttribute
    TypedVertexAttribute normals;
    std::unordered_map<uint32_t, TypedVertexAttribute> texcoords;
    TypedVertexAttribute tangents;
    TypedVertexAttribute binormals;
    TypedVertexAttribute vertex_colors;
    TypedVertexAttribute vertex_opacities;

    // Index buffers
    TypedArray<uint32_t> usd_face_vertex_indices;
    TypedArray<uint32_t> usd_face_vertex_counts;
    TypedArray<uint32_t> triangulated_face_vertex_indices;
    TypedArray<uint32_t> triangulated_face_vertex_counts;

    // Material and display properties
    int material_id{-1};
    int backface_material_id{-1};
    value::color3f displayColor{0.18f, 0.18f, 0.18f};
    float displayOpacity{1.0f};
    uint64_t handle{0};

    // Constructor from existing RenderMesh
    TypedRenderMesh(const RenderMesh& mesh)
        : prim_name(mesh.prim_name)
        , abs_path(mesh.abs_path)
        , display_name(mesh.display_name)
        , is_single_indexable(mesh.is_single_indexable)
        , doubleSided(mesh.doubleSided)
        , is_rightHanded(mesh.is_rightHanded)
        , normals(mesh.normals)
        , tangents(mesh.tangents)
        , binormals(mesh.binormals)
        , vertex_colors(mesh.vertex_colors)
        , vertex_opacities(mesh.vertex_opacities)
        , material_id(mesh.material_id)
        , backface_material_id(mesh.backface_material_id)
        , displayColor(mesh.displayColor)
        , displayOpacity(mesh.displayOpacity)
        , handle(mesh.handle) {

        // Convert vertex data
        if (!mesh.points.empty()) {
            points = TypedArray<value::float3>(mesh.points.data(), mesh.points.size());
        }

        // Convert texcoords
        for (const auto& [slot_id, texcoord] : mesh.texcoords) {
            texcoords[slot_id] = TypedVertexAttribute(texcoord);
        }

        // Convert index data
        if (!mesh.usdFaceVertexIndices.empty()) {
            usd_face_vertex_indices = TypedArray<uint32_t>(
                mesh.usdFaceVertexIndices.data(), mesh.usdFaceVertexIndices.size());
        }
        if (!mesh.usdFaceVertexCounts.empty()) {
            usd_face_vertex_counts = TypedArray<uint32_t>(
                mesh.usdFaceVertexCounts.data(), mesh.usdFaceVertexCounts.size());
        }
        if (!mesh.triangulatedFaceVertexIndices.empty()) {
            triangulated_face_vertex_indices = TypedArray<uint32_t>(
                mesh.triangulatedFaceVertexIndices.data(), mesh.triangulatedFaceVertexIndices.size());
        }
        if (!mesh.triangulatedFaceVertexCounts.empty()) {
            triangulated_face_vertex_counts = TypedArray<uint32_t>(
                mesh.triangulatedFaceVertexCounts.data(), mesh.triangulatedFaceVertexCounts.size());
        }
    }

    // Memory optimization methods
    void optimize_memory() {
        points.shrink_to_fit();
        usd_face_vertex_indices.shrink_to_fit();
        usd_face_vertex_counts.shrink_to_fit();
        triangulated_face_vertex_indices.shrink_to_fit();
        triangulated_face_vertex_counts.shrink_to_fit();
    }

    // Precision reduction for memory savings
    void reduce_precision() {
        normals.reduce_to_half_precision();
        tangents.reduce_to_half_precision();
        binormals.reduce_to_half_precision();
        vertex_colors.quantize_vertex_colors();

        for (auto& [slot_id, texcoord] : texcoords) {
            texcoord.reduce_to_half_precision();
        }
    }

    // Convert vertex data types for different rendering pipelines
    void convert_indices_to_16bit() {
        // Convert 32-bit indices to 16-bit if vertex count allows
        if (points.size() < 65536) {
            if (!usd_face_vertex_indices.empty()) {
                auto indices_16 = usd_face_vertex_indices.transform_1to1<uint16_t>(
                    [](const uint32_t& src, uint16_t& dst) {
                        dst = static_cast<uint16_t>(src);
                    });
                // Would need to store as uint16 array...
            }
        }
    }

    // Estimate memory usage
    size_t estimate_memory_usage() const {
        size_t total = sizeof(TypedRenderMesh);
        total += points.storage().capacity();
        total += normals.estimate_memory_usage();
        total += tangents.estimate_memory_usage();
        total += binormals.estimate_memory_usage();
        total += vertex_colors.estimate_memory_usage();
        total += vertex_opacities.estimate_memory_usage();
        total += usd_face_vertex_indices.storage().capacity();
        total += usd_face_vertex_counts.storage().capacity();
        total += triangulated_face_vertex_indices.storage().capacity();
        total += triangulated_face_vertex_counts.storage().capacity();

        for (const auto& [slot_id, texcoord] : texcoords) {
            total += texcoord.estimate_memory_usage();
        }

        return total;
    }

    // In-place coordinate system conversion
    void transform_coordinate_system(const value::matrix4f& transform_matrix) {
        // Transform positions in-place
        points.span().for_each([&](value::float3& pos) {
            // Apply matrix transformation to position
            value::float4 pos4{pos[0], pos[1], pos[2], 1.0f};
            // Apply transform (simplified)
            pos[0] = pos4[0]; pos[1] = pos4[1]; pos[2] = pos4[2];
        });

        // Transform normals (inverse transpose)
        auto* normal_data = normals.storage.get_data<value::float3>();
        if (normal_data) {
            normal_data->span().for_each([&](value::float3& normal) {
                // Apply inverse transpose transformation to normal
                // (simplified - would need proper matrix operations)
            });
        }
    }
};

///
/// Conversion utilities for TypedArray integration
///
class TypedArrayRenderConverter {
public:
    // Convert existing RenderMesh to TypedRenderMesh with optimizations
    static TypedRenderMesh convert_mesh(const RenderMesh& mesh, bool optimize_memory = true) {
        TypedRenderMesh typed_mesh(mesh);

        if (optimize_memory) {
            typed_mesh.optimize_memory();
            typed_mesh.reduce_precision();
        }

        return typed_mesh;
    }

    // Batch conversion of multiple meshes with shared optimization settings
    static std::vector<TypedRenderMesh> convert_meshes(
        const std::vector<RenderMesh>& meshes,
        bool optimize_memory = true,
        bool reduce_precision = false) {

        std::vector<TypedRenderMesh> typed_meshes;
        typed_meshes.reserve(meshes.size());

        for (const auto& mesh : meshes) {
            typed_meshes.emplace_back(mesh);
            auto& typed_mesh = typed_meshes.back();

            if (optimize_memory) {
                typed_mesh.optimize_memory();
            }
            if (reduce_precision) {
                typed_mesh.reduce_precision();
            }
        }

        return typed_meshes;
    }

    // Estimate memory savings from conversion
    static size_t estimate_memory_savings(const RenderMesh& original_mesh,
                                        const TypedRenderMesh& typed_mesh) {
        size_t original_size = original_mesh.estimate_memory_usage();
        size_t typed_size = typed_mesh.estimate_memory_usage();
        return (original_size > typed_size) ? (original_size - typed_size) : 0;
    }
};

} // namespace typed_array_opt
} // namespace tydra
} // namespace lightusd