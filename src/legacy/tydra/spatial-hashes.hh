// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Spatial hashing algorithms for efficient vertex similarity search
// Based on sorted hash grid implementation with Morton code ordering
//
#pragma once

#include <vector>
#include <algorithm>
#include <unordered_map>
#include <memory>
#include <cmath>
#include <functional>
#include <array>
#include <limits>
#include <cstdint>

#include "value-types.hh"

namespace lightusd {
namespace tydra {
namespace spatial {

// Morton code utilities for 3D spatial hashing
namespace morton {

inline uint32_t expandBits(uint32_t v) {
    v = (v | (v << 16)) & 0x030000FF;
    v = (v | (v <<  8)) & 0x0300F00F;
    v = (v | (v <<  4)) & 0x030C30C3;
    v = (v | (v <<  2)) & 0x09249249;
    return v;
}

inline uint32_t compactBits(uint32_t v) {
    v &= 0x09249249;
    v = (v | (v >> 2)) & 0x030C30C3;
    v = (v | (v >> 4)) & 0x0300F00F;
    v = (v | (v >> 8)) & 0x030000FF;
    v = (v | (v >> 16)) & 0x000003FF;
    return v;
}

inline uint32_t morton3D(uint32_t x, uint32_t y, uint32_t z) {
    x = std::min(x, uint32_t(0x3FF));
    y = std::min(y, uint32_t(0x3FF));
    z = std::min(z, uint32_t(0x3FF));

    uint32_t xx = expandBits(x);
    uint32_t yy = expandBits(y);
    uint32_t zz = expandBits(z);

    return (xx << 2) | (yy << 1) | zz;
}

inline uint64_t expandBits64(uint32_t v) {
    uint64_t x = v;
    x = (x | (x << 32)) & 0x1F00000000FFFFull;
    x = (x | (x << 16)) & 0x1F0000FF0000FFull;
    x = (x | (x <<  8)) & 0x100F00F00F00F00Full;
    x = (x | (x <<  4)) & 0x10C30C30C30C30C3ull;
    x = (x | (x <<  2)) & 0x1249249249249249ull;
    return x;
}

inline uint32_t compactBits64(uint64_t v) {
    v &= 0x1249249249249249ull;
    v = (v | (v >> 2)) & 0x10C30C30C30C30C3ull;
    v = (v | (v >> 4)) & 0x100F00F00F00F00Full;
    v = (v | (v >> 8)) & 0x1F0000FF0000FFull;
    v = (v | (v >> 16)) & 0x1F00000000FFFFull;
    v = (v | (v >> 32)) & 0x00000000001FFFFFull;
    return static_cast<uint32_t>(v);
}

inline uint64_t morton3D64(uint32_t x, uint32_t y, uint32_t z) {
    x = std::min(x, uint32_t(0x1FFFFF));
    y = std::min(y, uint32_t(0x1FFFFF));
    z = std::min(z, uint32_t(0x1FFFFF));

    uint64_t xx = expandBits64(x);
    uint64_t yy = expandBits64(y);
    uint64_t zz = expandBits64(z);

    return (xx << 2) | (yy << 1) | zz;
}

inline void decodeMorton3D64(uint64_t code, uint32_t& x, uint32_t& y, uint32_t& z) {
    x = compactBits64(code >> 2);
    y = compactBits64(code >> 1);
    z = compactBits64(code);
}

} // namespace morton

// Helper function to compute grid coordinates
template <typename T>
inline void computeGridCoords(const T pos[3], const T origin[3], T cellSize,
                              uint32_t& gx, uint32_t& gy, uint32_t& gz) {
    T dx = pos[0] - origin[0];
    T dy = pos[1] - origin[1];
    T dz = pos[2] - origin[2];

    gx = static_cast<uint32_t>(std::max(T(0), dx / cellSize));
    gy = static_cast<uint32_t>(std::max(T(0), dy / cellSize));
    gz = static_cast<uint32_t>(std::max(T(0), dz / cellSize));
}

// Get 27 neighbor offsets for 3x3x3 neighborhood search
inline std::array<std::array<int, 3>, 27> getNeighborOffsets() {
    std::array<std::array<int, 3>, 27> offsets;
    size_t idx = 0;
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -1; dz <= 1; dz++) {
                offsets[idx++] = {dx, dy, dz};
            }
        }
    }
    return offsets;
}

///
/// VertexSpatialHashGrid - Optimized spatial hash grid for vertex similarity search
/// Uses Morton code ordering for cache-friendly traversal
///
template <typename T = float>
class VertexSpatialHashGrid {
public:
    struct Vertex {
        value::float3 position;
        value::float3 normal;
        value::float2 uv0;
        value::float2 uv1;
        value::float3 tangent;
        value::float3 binormal;
        value::float3 color;
        float opacity;
        uint32_t id;

        Vertex() : id(0), opacity(0.0f) {
            position = {0, 0, 0};
            normal = {0, 0, 0};
            uv0 = {0, 0};
            uv1 = {0, 0};
            tangent = {0, 0, 0};
            binormal = {0, 0, 0};
            color = {0, 0, 0};
        }
    };

    struct Cell {
        std::vector<uint32_t> indices;
        std::unique_ptr<VertexSpatialHashGrid> subcell;
        uint32_t level = 0;
    };

    struct SearchResult {
        uint32_t vertexId;
        T distanceSquared;
    };

    // Function type for custom similarity checking
    using SimilarityFunc = std::function<bool(const Vertex&, const Vertex&, T)>;

protected:
    std::vector<Vertex> vertices_;
    std::unordered_map<uint64_t, Cell> grid_;
    T cellSize_;
    T positionEpsilon_;
    T attributeEpsilon_;
    T origin_[3];
    T bounds_[3];
    uint32_t maxItemsPerCell_;
    uint32_t currentLevel_;
    uint32_t maxLevel_;

    std::vector<std::pair<uint64_t, uint32_t>> sortedEntries_;
    bool needsRebuild_ = true;

public:
    VertexSpatialHashGrid(T cellSize = 0.01f,
                         T positionEps = 1e-6f,
                         T attributeEps = 1e-3f,
                         uint32_t maxItemsPerCell = 128,
                         uint32_t level = 0,
                         uint32_t maxLevel = 5)
        : cellSize_(cellSize),
          positionEpsilon_(positionEps),
          attributeEpsilon_(attributeEps),
          maxItemsPerCell_(maxItemsPerCell),
          currentLevel_(level),
          maxLevel_(maxLevel) {
        origin_[0] = origin_[1] = origin_[2] = (std::numeric_limits<T>::max)();
        bounds_[0] = bounds_[1] = bounds_[2] = std::numeric_limits<T>::lowest();
    }

    void addVertex(const Vertex& vertex) {
        vertices_.push_back(vertex);

        origin_[0] = std::min(origin_[0], static_cast<T>(vertex.position[0]));
        origin_[1] = std::min(origin_[1], static_cast<T>(vertex.position[1]));
        origin_[2] = std::min(origin_[2], static_cast<T>(vertex.position[2]));

        bounds_[0] = std::max(bounds_[0], static_cast<T>(vertex.position[0]));
        bounds_[1] = std::max(bounds_[1], static_cast<T>(vertex.position[1]));
        bounds_[2] = std::max(bounds_[2], static_cast<T>(vertex.position[2]));

        needsRebuild_ = true;
    }

    void reserveVertices(size_t count) {
        vertices_.reserve(count);
    }

    void build() {
        if (!needsRebuild_ || vertices_.empty()) return;

        grid_.clear();
        sortedEntries_.clear();
        sortedEntries_.reserve(vertices_.size());

        // Extend origin slightly to handle boundary cases
        T extendedOrigin[3] = {
            origin_[0] - cellSize_,
            origin_[1] - cellSize_,
            origin_[2] - cellSize_
        };

        // Compute Morton codes for all vertices
        for (size_t i = 0; i < vertices_.size(); ++i) {
            const auto& v = vertices_[i];
            uint32_t gx, gy, gz;
            T pos[3] = {
                static_cast<T>(v.position[0]),
                static_cast<T>(v.position[1]),
                static_cast<T>(v.position[2])
            };
            computeGridCoords(pos, extendedOrigin, cellSize_, gx, gy, gz);

            uint64_t mortonCode = morton::morton3D64(gx, gy, gz);
            sortedEntries_.emplace_back(mortonCode, static_cast<uint32_t>(i));
        }

        // Sort by Morton code for cache-friendly access
        std::sort(sortedEntries_.begin(), sortedEntries_.end());

        // Build grid cells
        for (const auto& entry : sortedEntries_) {
            grid_[entry.first].indices.push_back(entry.second);
        }

        // Subdivide large cells if needed
        if (currentLevel_ < maxLevel_) {
            for (auto& gridEntry : grid_) {
                if (gridEntry.second.indices.size() > maxItemsPerCell_) {
                    subdivideCell(gridEntry.first, gridEntry.second);
                }
            }
        }

        needsRebuild_ = false;
    }

    // Find similar vertices using position and attribute comparison
    std::vector<SearchResult> findSimilarVertices(const Vertex& queryVertex,
                                                  T searchRadius = -1.0f) {
        if (needsRebuild_) build();

        std::vector<SearchResult> results;

        // Use position epsilon as search radius if not specified
        if (searchRadius < 0) {
            searchRadius = positionEpsilon_;
        }

        T searchRadiusSq = searchRadius * searchRadius;

        T extendedOrigin[3] = {
            origin_[0] - cellSize_,
            origin_[1] - cellSize_,
            origin_[2] - cellSize_
        };

        uint32_t gx, gy, gz;
        T pos[3] = {
            static_cast<T>(queryVertex.position[0]),
            static_cast<T>(queryVertex.position[1]),
            static_cast<T>(queryVertex.position[2])
        };
        computeGridCoords(pos, extendedOrigin, cellSize_, gx, gy, gz);

        // Search in 27-neighborhood
        static const auto neighborOffsets = getNeighborOffsets();

        for (const auto& offset : neighborOffsets) {
            int nx = static_cast<int>(gx) + offset[0];
            int ny = static_cast<int>(gy) + offset[1];
            int nz = static_cast<int>(gz) + offset[2];

            if (nx < 0 || ny < 0 || nz < 0) continue;

            uint64_t neighborCode = morton::morton3D64(
                static_cast<uint32_t>(nx),
                static_cast<uint32_t>(ny),
                static_cast<uint32_t>(nz)
            );

            auto it = grid_.find(neighborCode);
            if (it != grid_.end()) {
                searchInCell(it->second, queryVertex, searchRadiusSq, results);
            }
        }

        // Sort results by distance
        std::sort(results.begin(), results.end(),
                 [](const SearchResult& a, const SearchResult& b) {
                     return a.distanceSquared < b.distanceSquared;
                 });

        return results;
    }

    // Find exact match considering epsilon for all attributes
    bool findExactVertex(const Vertex& queryVertex, uint32_t& outId) {
        auto results = findSimilarVertices(queryVertex, positionEpsilon_);

        for (const auto& r : results) {
            const auto& v = vertices_[r.vertexId];

            // Check position with position epsilon
            if (!float3_equal(v.position, queryVertex.position, positionEpsilon_)) {
                continue;
            }

            // Check other attributes with attribute epsilon
            if (!float3_equal(v.normal, queryVertex.normal, attributeEpsilon_)) continue;
            if (!float2_equal(v.uv0, queryVertex.uv0, attributeEpsilon_)) continue;
            if (!float2_equal(v.uv1, queryVertex.uv1, attributeEpsilon_)) continue;
            if (!float3_equal(v.tangent, queryVertex.tangent, attributeEpsilon_)) continue;
            if (!float3_equal(v.binormal, queryVertex.binormal, attributeEpsilon_)) continue;
            if (!float3_equal(v.color, queryVertex.color, attributeEpsilon_)) continue;
            if (!float_equal(v.opacity, queryVertex.opacity, attributeEpsilon_)) continue;

            outId = v.id;
            return true;
        }

        return false;
    }

    void clear() {
        vertices_.clear();
        grid_.clear();
        sortedEntries_.clear();
        origin_[0] = origin_[1] = origin_[2] = (std::numeric_limits<T>::max)();
        bounds_[0] = bounds_[1] = bounds_[2] = std::numeric_limits<T>::lowest();
        needsRebuild_ = true;
    }

    size_t getVertexCount() const { return vertices_.size(); }
    size_t getCellCount() const { return grid_.size(); }
    T getCellSize() const { return cellSize_; }

    const Vertex& getVertex(uint32_t idx) const { return vertices_[idx]; }

    void getStatistics(size_t& totalCells, size_t& maxCellSize,
                      size_t& avgCellSize, size_t& subdivisionCount) const {
        totalCells = grid_.size();
        maxCellSize = 0;
        size_t totalItems = 0;
        subdivisionCount = 0;

        for (const auto& gridEntry : grid_) {
            const auto& cell = gridEntry.second;
            size_t cellItemCount = cell.indices.size();
            if (cell.subcell) {
                cellItemCount = 0;
                size_t subCells, subMax, subAvg, subSubdiv;
                cell.subcell->getStatistics(subCells, subMax, subAvg, subSubdiv);
                subdivisionCount += 1 + subSubdiv;
            }
            maxCellSize = std::max(maxCellSize, cellItemCount);
            totalItems += cellItemCount;
        }

        avgCellSize = totalCells > 0 ? totalItems / totalCells : 0;
    }

private:
    void subdivideCell(uint64_t mortonCode, Cell& cell) {
        uint32_t x, y, z;
        morton::decodeMorton3D64(mortonCode, x, y, z);

        T subcellSize = cellSize_ / 2.0f;
        cell.subcell = std::make_unique<VertexSpatialHashGrid>(
            subcellSize, positionEpsilon_, attributeEpsilon_,
            maxItemsPerCell_, currentLevel_ + 1, maxLevel_
        );

        T extendedOrigin[3] = {
            origin_[0] - cellSize_,
            origin_[1] - cellSize_,
            origin_[2] - cellSize_
        };

        T cellOrigin[3] = {
            extendedOrigin[0] + x * cellSize_,
            extendedOrigin[1] + y * cellSize_,
            extendedOrigin[2] + z * cellSize_
        };

        cell.subcell->origin_[0] = cellOrigin[0];
        cell.subcell->origin_[1] = cellOrigin[1];
        cell.subcell->origin_[2] = cellOrigin[2];

        cell.subcell->bounds_[0] = cellOrigin[0] + cellSize_;
        cell.subcell->bounds_[1] = cellOrigin[1] + cellSize_;
        cell.subcell->bounds_[2] = cellOrigin[2] + cellSize_;

        // Transfer vertices to subcell
        for (uint32_t idx : cell.indices) {
            cell.subcell->vertices_.push_back(vertices_[idx]);
        }

        cell.subcell->build();

        cell.indices.clear();
        cell.level = currentLevel_ + 1;
    }

    void searchInCell(const Cell& cell, const Vertex& queryVertex, T searchRadiusSq,
                     std::vector<SearchResult>& results) {
        if (cell.subcell) {
            auto subResults = cell.subcell->findSimilarVertices(
                queryVertex, std::sqrt(searchRadiusSq)
            );
            results.insert(results.end(), subResults.begin(), subResults.end());
        } else {
            for (uint32_t idx : cell.indices) {
                const auto& v = vertices_[idx];
                T dx = v.position[0] - queryVertex.position[0];
                T dy = v.position[1] - queryVertex.position[1];
                T dz = v.position[2] - queryVertex.position[2];
                T distSq = dx*dx + dy*dy + dz*dz;

                if (distSq <= searchRadiusSq) {
                    results.push_back({v.id, distSq});
                }
            }
        }
    }

    // Helper comparison functions (same as in render-data.hh)
    bool float_equal(float a, float b, float eps) const {
        return std::abs(a - b) <= eps;
    }

    bool float2_equal(const value::float2& a, const value::float2& b, float eps) const {
        return float_equal(a[0], b[0], eps) && float_equal(a[1], b[1], eps);
    }

    bool float3_equal(const value::float3& a, const value::float3& b, float eps) const {
        return float_equal(a[0], b[0], eps) &&
               float_equal(a[1], b[1], eps) &&
               float_equal(a[2], b[2], eps);
    }
};

} // namespace spatial
} // namespace tydra
} // namespace lightusd

