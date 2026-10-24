// SPDX-License-Identifier: Apache-2.0
// Signed distance fields and volumetric meshes through the vendored tinyvdb:
// narrow-band level sets of triangle meshes, OpenVDB (.vdb) level-set I/O,
// level set -> surface mesh, and level set -> tetrahedral mesh (isosurface
// stuffing) for volume deformables.
//
// Devices: "cpu" (the default), "cuda", "hip", or "auto" (CUDA, then HIP,
// then the CPU). CUDA and HIP need a LIGHTUSD_TINYVDB_GPU build and a device
// at runtime (drivers are loaded on first use); an explicit request that
// cannot be met fails. GPU results follow the CPU's rules (tinyvdb
// tinyvdb_gpu_compute.h): same band, distances to rounding, same stuffing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "import.hh"

struct tvdb_grid;
struct tvdb_file;

namespace lightusd {
namespace tydra {
namespace import_asset {

// A float level-set grid: negative inside, background = exterior width.
class LevelSet {
 public:
  LevelSet();
  ~LevelSet();
  LevelSet(LevelSet&&) noexcept;
  LevelSet& operator=(LevelSet&&) noexcept;
  LevelSet(const LevelSet&) = delete;
  LevelSet& operator=(const LevelSet&) = delete;
  bool empty() const { return grid() == nullptr; }
  const tvdb_grid* grid() const;
  double voxel_size() const;       // first axis of the grid transform
  float background() const;
  size_t active_voxels() const;

 private:
  friend struct LevelSetAccess;
  tvdb_grid* owned_ = nullptr;     // built here (libc allocations)
  tvdb_file* file_ = nullptr;      // or read from VDB bytes
  size_t file_grid_ = 0;
};

// Whether `device` can run here; *resolved receives the device it maps to
// ("cpu", "cuda" or "hip") and *why the reason when it cannot.
bool VolumetricDeviceAvailable(const std::string& device, std::string* resolved,
                               std::string* why);

struct LevelSetOptions {
  double voxel_size = 0;      // > 0, mesh units
  double exterior_width = 0;  // > 0; 0 selects 3 voxels
  double interior_width = 0;  // > 0; 0 selects 3 voxels
  size_t max_voxels = 0;      // candidate budget (~128 B each); 0 selects 8,000,000
  int threads = 0;            // 0 automatic (up to 8), 1 serial
  bool exact_winding = false; // exact (O(faces)) instead of fast signs
  std::string name;           // grid name; empty selects "sdf"
  std::string device = "cpu"; // cpu | cuda | hip | auto
};
// Triangulated mesh (positions + indices, or polygon counts). *device_used
// (if given) receives the device that ran.
bool MeshToLevelSet(const Mesh& mesh, const LevelSetOptions& options,
                    LevelSet* out, std::string* err, std::string* device_used = nullptr);

// VDB bytes of one level set (ZIP + active-mask compression).
bool WriteLevelSetVDB(const LevelSet& level_set, std::vector<uint8_t>* bytes,
                      std::string* err);
// The first float grid of class "level set" (or, if none is classed, the
// first float grid with a positive background) in VDB bytes.
bool ReadLevelSetVDB(const uint8_t* data, size_t size, LevelSet* out,
                     std::string* err);

// Trilinear world-space samples of the stored values (inactive tiles
// included, background outside), for `count` points (3 doubles each).
bool SampleLevelSet(const LevelSet& level_set, const double* points, size_t count,
                    float* values, std::string* err);

// Surface at the zero isovalue (sparse marching cubes, outward winding).
bool LevelSetToMesh(const LevelSet& level_set, size_t max_cells, Mesh* out,
                    std::string* err);

struct TetMeshData {
  std::vector<float> points;      // 3 per vertex
  std::vector<int32_t> tets;      // 4 per tetrahedron, positive orientation
  std::vector<int32_t> surface;   // 3 per boundary triangle, outward
};
struct StuffingOptions {
  double spacing = 0;   // lattice spacing (> 0)
  size_t max_tets = 0;  // 0 selects 4,000,000
  int threads = 0;
  std::string device = "cpu"; // cpu | cuda | hip | auto (lattice samples)
};
bool LevelSetToTetMesh(const LevelSet& level_set, const StuffingOptions& options,
                       TetMeshData* out, std::string* err, std::string* device_used = nullptr);

}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
