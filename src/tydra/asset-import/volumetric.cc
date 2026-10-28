// SPDX-License-Identifier: Apache-2.0
#include "volumetric.hh"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

#include "tinyvdb_io.h"
#include "tinyvdb_sparse_tools.h"
#include "tinyvdb_sparse_tree.h"
#include "tinyvdb_tetmesh.h"
#include "tinyvdb_tet_graded.h"
#include "tinyvdb_tree.h"
#include "tinyvdb_tree_internal.h"
#if defined(LIGHTUSD_HAVE_TINYVDB_GPU)
#include <mutex>

#include "tinyvdb_gpu_compute.h"
#endif

namespace lightusd {
namespace tydra {
namespace import_asset {

struct LevelSetBudget {
  union Header { size_t bytes; std::max_align_t alignment; };
  size_t used = 0, peak = 0, limit = 0;
  bool exhausted = false;
  static void* Allocate(size_t size, void* context) {
    auto* self = static_cast<LevelSetBudget*>(context);
    if (size > SIZE_MAX - sizeof(Header)) { self->exhausted = true; return nullptr; }
    const size_t total = size + sizeof(Header);
    if (total > self->limit - std::min(self->used, self->limit)) {
      self->exhausted = true; return nullptr;
    }
    auto* header = static_cast<Header*>(std::malloc(total));
    if (!header) return nullptr;
    header->bytes = total;
    self->used += total;
    self->peak = std::max(self->peak, self->used);
    return header + 1;
  }
  static void Free(void* ptr, size_t, void* context) {
    if (!ptr) return;
    auto* self = static_cast<LevelSetBudget*>(context);
    auto* header = static_cast<Header*>(ptr) - 1;
    self->used -= header->bytes;
    std::free(header);
  }
  static void* Reallocate(void* ptr, size_t, size_t size, void* context) {
    if (!ptr) return Allocate(size, context);
    auto* header = static_cast<Header*>(ptr) - 1;
    const size_t old = header->bytes - sizeof(Header);
    void* replacement = Allocate(size, context);
    if (!replacement) return nullptr;
    std::memcpy(replacement, ptr, std::min(old, size));
    Free(ptr, old, context);
    return replacement;
  }
};

struct LevelSetAccess {
  static void Reset(LevelSet* s) {
    if (s->owned_) {
      tvdb_grid_destroy_owned(s->owned_);
      std::free(s->owned_);
      s->owned_ = nullptr;
    }
    if (s->file_) {
      tvdb_file_close(s->file_);
      std::free(s->file_);
      s->file_ = nullptr;
    }
    delete s->budget_;
    s->budget_ = nullptr;
    s->file_grid_ = 0;
  }
  static void Adopt(LevelSet* s, tvdb_grid* grid) {
    Reset(s);
    s->owned_ = grid;
  }
  static void AdoptFile(LevelSet* s, tvdb_file* file, size_t index, LevelSetBudget* budget) {
    Reset(s);
    s->file_ = file;
    s->file_grid_ = index;
    s->budget_ = budget;
  }
};

LevelSet::LevelSet() = default;
LevelSet::~LevelSet() { LevelSetAccess::Reset(this); }
LevelSet::LevelSet(LevelSet&& o) noexcept
    : owned_(o.owned_), file_(o.file_), file_grid_(o.file_grid_), budget_(o.budget_) {
  o.owned_ = nullptr;
  o.file_ = nullptr;
  o.file_grid_ = 0;
  o.budget_ = nullptr;
}
LevelSet& LevelSet::operator=(LevelSet&& o) noexcept {
  if (this != &o) {
    LevelSetAccess::Reset(this);
    owned_ = o.owned_;
    file_ = o.file_;
    file_grid_ = o.file_grid_;
    budget_ = o.budget_;
    o.owned_ = nullptr;
    o.file_ = nullptr;
    o.file_grid_ = 0;
    o.budget_ = nullptr;
  }
  return *this;
}
size_t LevelSet::decode_peak_bytes() const {
  return budget_ ? budget_->peak + sizeof(LevelSetBudget) + sizeof(tvdb_file) : 0;
}
const tvdb_grid* LevelSet::grid() const {
  if (owned_) return owned_;
  if (file_ && file_grid_ < file_->num_grids) return file_->grids + file_grid_;
  return nullptr;
}
double LevelSet::voxel_size() const {
  const tvdb_grid* g = grid();
  return g ? g->transform.voxel_size[0] : 0.0;
}
float LevelSet::background() const {
  const tvdb_grid* g = grid();
  return g && g->tree.num_nodes ? g->tree.nodes[0].u.root.background.u.f : 0.0f;
}
size_t LevelSet::active_voxels() const {
  const tvdb_grid* g = grid();
  if (!g) return 0;
  tvdb_tree_diagnostics_t d;
  tvdb_error_t err;
  std::memset(&d, 0, sizeof(d));
  if (tvdb_grid_diagnose(g, TVDB_TREE_DIAG_GENERIC, 0, 0, &d, &err) != TVDB_OK) return 0;
  return size_t(d.active_voxels);
}

namespace {

// Thread pool for a call: NULL selects tinyvdb's automatic sizing.
struct Pool {
  tvdb_thread_pool_t* pool = nullptr;
  ~Pool() { tvdb_thread_pool_destroy(pool); }
  bool Make(int threads, std::string* err) {
    if (threads <= 0) return true;
    tvdb_error_t e;
    if (tvdb_thread_pool_create(size_t(threads), &pool, &e) != TVDB_OK) {
      if (err) *err = std::string("thread pool: ") + e.message;
      return false;
    }
    return true;
  }
};

// Exact winding evaluates every face for every candidate voxel; past this
// many face-voxel evaluations the call is refused (use the fast signs).
constexpr double kMaxExactWindingWork = 1e11;

bool Fail(std::string* err, const char* what, const tvdb_error_t& e) {
  if (err) *err = std::string(what) + ": " + (e.message[0] ? e.message : tvdb_status_string(e.status));
  return false;
}

// The device a call runs on: "cpu", or a CUDA/HIP device locked for the call.
struct DeviceLease {
  std::string name = "cpu";
#if defined(LIGHTUSD_HAVE_TINYVDB_GPU)
  tvdb_gpu_compute_t* device = nullptr;
  std::unique_lock<std::mutex> lock;
#endif
};

#if defined(LIGHTUSD_HAVE_TINYVDB_GPU)
// One device per runtime, created on first use and kept for the process
// (ROCm cannot be unloaded safely); its mutex serializes calls, as
// tinyvdb_gpu_compute requires.
struct GpuSlot {
  std::mutex mutex;
  tvdb_gpu_compute_t* device = nullptr;
  bool tried = false;
  std::string why;
};
GpuSlot& Slot(bool hip) {
  static GpuSlot slots[2];
  return slots[hip ? 1 : 0];
}
bool LeaseGpu(bool hip, DeviceLease* lease, std::string* why) {
  GpuSlot& s = Slot(hip);
  std::unique_lock<std::mutex> lock(s.mutex);
  if (!s.tried) {
    s.tried = true;
    tvdb_error_t e;
    std::memset(&e, 0, sizeof(e));
    if (tvdb_gpu_compute_create(hip ? TVDB_GPU_BACKEND_HIP : TVDB_GPU_BACKEND_CUDA, 0, &s.device,
                                &e) != TVDB_OK)
      s.why = e.message[0] ? e.message : tvdb_status_string(e.status);
  }
  if (!s.device) {
    if (why) *why = (hip ? "HIP: " : "CUDA: ") + s.why;
    return false;
  }
  lease->name = hip ? "hip" : "cuda";
  lease->device = s.device;
  lease->lock = std::move(lock);
  return true;
}
#endif

bool LeaseDevice(const std::string& want, DeviceLease* lease, std::string* err) {
  if (want.empty() || want == "cpu") return true;
  if (want != "cuda" && want != "hip" && want != "auto") {
    if (err) *err = "volumetric device must be cpu, cuda, hip or auto: " + want;
    return false;
  }
#if defined(LIGHTUSD_HAVE_TINYVDB_GPU)
  std::string why_cuda, why_hip;
  if (want != "hip" && LeaseGpu(false, lease, &why_cuda)) return true;
  if (want != "cuda" && LeaseGpu(true, lease, &why_hip)) return true;
  if (want == "auto") return true;  // the CPU
  if (err) *err = "volumetric device unavailable: " + (want == "cuda" ? why_cuda : why_hip);
  return false;
#else
  if (want == "auto") return true;
  if (err) *err = "volumetric device " + want + " needs a LIGHTUSD_TINYVDB_GPU build";
  return false;
#endif
}

const char* GridClass(const tvdb_grid* g) {
  for (size_t i = 0; i < g->metadata.count; ++i) {
    const tvdb_meta_entry_t& e = g->metadata.entries[i];
    if (e.name && !std::strcmp(e.name, "class") && e.value.type == TVDB_VALUE_STRING)
      return e.value.u.s.str;
  }
  return "";
}

bool LevelSetMeshSizes(const Mesh& mesh, size_t* vertex_count,
                       size_t* triangle_count, size_t* converted_bytes,
                       std::string* err) {
  if (mesh.positions.size() % 3) {
    if (err) *err = "level set: mesh positions are not xyz triples";
    return false;
  }
  const size_t nv = mesh.positions.size() / 3;
  if (!nv) {
    if (err) *err = "level set: mesh has no vertices";
    return false;
  }
  size_t nf = 0;
  if (mesh.counts.empty()) {
    if (mesh.indices.size() % 3) {
      if (err) *err = "level set: indices are not triangles";
      return false;
    }
    nf = mesh.indices.size() / 3;
  } else {
    size_t at = 0;
    for (int32_t count : mesh.counts) {
      if (count < 3 || at > mesh.indices.size() ||
          size_t(count) > mesh.indices.size() - at) {
        if (err) *err = "level set: invalid polygon counts";
        return false;
      }
      const size_t added = size_t(count) - 2;
      if (added > SIZE_MAX - nf) {
        if (err) *err = "level set: triangle count overflow";
        return false;
      }
      nf += added;
      at += size_t(count);
    }
  }
  if (nv > SIZE_MAX / sizeof(tvdb_vec3f) ||
      nf > SIZE_MAX / sizeof(tvdb_triangle)) {
    if (err) *err = "level set: converted mesh size overflow";
    return false;
  }
  const size_t vertex_bytes = nv * sizeof(tvdb_vec3f);
  const size_t triangle_bytes = nf * sizeof(tvdb_triangle);
  if (triangle_bytes > SIZE_MAX - vertex_bytes) {
    if (err) *err = "level set: converted mesh size overflow";
    return false;
  }
  if (vertex_count) *vertex_count = nv;
  if (triangle_count) *triangle_count = nf;
  if (converted_bytes) *converted_bytes = vertex_bytes + triangle_bytes;
  return true;
}

}  // namespace

bool VolumetricDeviceAvailable(const std::string& device, std::string* resolved,
                               std::string* why) {
  DeviceLease lease;
  const bool ok = LeaseDevice(device, &lease, why);
  if (resolved) *resolved = ok ? lease.name : "cpu";
  return ok;
}

bool MeshToLevelSet(const Mesh& mesh, const LevelSetOptions& options, LevelSet* out,
                    std::string* err, std::string* device_used) {
  if (!out || !(options.voxel_size > 0) || !std::isfinite(options.voxel_size)) {
    if (err) *err = "level set: voxel size must be positive";
    return false;
  }
  size_t nv = 0, triangle_count = 0, converted_bytes = 0;
  if (!LevelSetMeshSizes(mesh, &nv, &triangle_count, &converted_bytes, err))
    return false;
  size_t max_voxels = options.max_voxels ? options.max_voxels : 8000000;
  if (options.max_working_memory_bytes) {
    if (converted_bytes >= options.max_working_memory_bytes) {
      if (err) *err = "level set: mesh conversion exceeds working memory budget";
      return false;
    }
    const size_t voxel_budget =
        (options.max_working_memory_bytes - converted_bytes) / 128;
    if (!voxel_budget) {
      if (err) *err = "level set: mesh conversion leaves no voxel budget";
      return false;
    }
    max_voxels = std::min(max_voxels, voxel_budget);
  }
  std::vector<tvdb_vec3f> vertices(nv);
  for (size_t i = 0; i < nv; ++i)
    vertices[i] = {mesh.positions[3 * i], mesh.positions[3 * i + 1], mesh.positions[3 * i + 2]};
  std::vector<tvdb_triangle> faces;
  if (options.max_working_memory_bytes) faces.reserve(triangle_count);
  auto index = [&](size_t i, uint32_t* v) {
    const int32_t x = mesh.indices[i];
    if (x < 0 || size_t(x) >= nv) return false;
    *v = uint32_t(x);
    return true;
  };
  if (!mesh.counts.empty()) {
    size_t at = 0;
    for (int32_t c : mesh.counts) {
      if (c < 3 || at > mesh.indices.size() ||
          size_t(c) > mesh.indices.size() - at) {
        if (err) *err = "level set: invalid polygon counts";
        return false;
      }
      uint32_t a = 0, b = 0, d = 0;
      for (int32_t k = 1; k + 1 < c; ++k) {  // fan
        if (!index(at, &a) || !index(at + size_t(k), &b) || !index(at + size_t(k) + 1, &d)) {
          if (err) *err = "level set: face index out of range";
          return false;
        }
        faces.push_back({a, b, d});
      }
      at += size_t(c);
    }
  } else {
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
      uint32_t a = 0, b = 0, d = 0;
      if (!index(i, &a) || !index(i + 1, &b) || !index(i + 2, &d)) {
        if (err) *err = "level set: face index out of range";
        return false;
      }
      faces.push_back({a, b, d});
    }
  }
  if (faces.empty()) {
    if (err) *err = "level set: mesh has no faces";
    return false;
  }
  if (options.exact_winding && double(faces.size()) * double(max_voxels) > kMaxExactWindingWork) {
    if (err)
      *err = "level set: exact winding work exceeds the limit (" + std::to_string(faces.size()) +
             " faces); use the fast signs or fewer voxels";
    return false;
  }
  tvdb_triangle_mesh tm;
  std::memset(&tm, 0, sizeof(tm));
  tm.vertices = vertices.data();
  tm.vertex_count = tm.vertex_capacity = vertices.size();
  tm.faces = faces.data();
  tm.face_count = tm.face_capacity = faces.size();
  tvdb_mesh_level_set_options_t o;
  std::memset(&o, 0, sizeof(o));
  o.voxel_size = float(options.voxel_size);
  o.exterior_width = float(options.exterior_width > 0 ? options.exterior_width
                                                      : 3 * options.voxel_size);
  o.interior_width = float(options.interior_width > 0 ? options.interior_width
                                                      : 3 * options.voxel_size);
  o.sign = options.exact_winding ? TVDB_LEVEL_SET_SIGN_WINDING_EXACT
                                 : TVDB_LEVEL_SET_SIGN_WINDING_FAST;
  o.max_voxels = max_voxels;
  o.grid_name = options.name.empty() ? "sdf" : options.name.c_str();
  Pool pool;
  if (!pool.Make(options.threads, err)) return false;
  DeviceLease lease;
  if (!LeaseDevice(options.device, &lease, err)) return false;
  auto* grid = static_cast<tvdb_grid*>(std::calloc(1, sizeof(tvdb_grid)));
  if (!grid) {
    if (err) *err = "level set: out of memory";
    return false;
  }
  tvdb_error_t e;
  std::memset(&e, 0, sizeof(e));
  tvdb_status_t st;
#if defined(LIGHTUSD_HAVE_TINYVDB_GPU)
  if (lease.device) {
    st = tvdb_gpu_compute_mesh_to_level_set(lease.device, &tm, &o, pool.pool, grid, nullptr, &e);
    // "auto" falls back to the CPU when the device fails at run time (for
    // example out of device memory), not only when none can be opened.
    // Invalid input and budgets (TVDB_ERROR_INVALID_ARGUMENT) fail the same way
    // on the CPU, so only device errors are retried.
    if (st != TVDB_OK && st != TVDB_ERROR_INVALID_ARGUMENT && options.device == "auto") {
      std::memset(grid, 0, sizeof(tvdb_grid));
      std::memset(&e, 0, sizeof(e));
      st = tvdb_mesh_to_level_set(&tm, &o, pool.pool, grid, nullptr, &e);
      lease.name = "cpu";
    }
  } else
#endif
    st = tvdb_mesh_to_level_set(&tm, &o, pool.pool, grid, nullptr, &e);
  if (st != TVDB_OK) {
    std::free(grid);
    return Fail(err, "level set", e);
  }
  LevelSetAccess::Adopt(out, grid);
  if (device_used) *device_used = lease.name;
  return true;
}

namespace {
// tinyvdb's malloc'd VDB bytes for one level set.
uint8_t* WriteVDB(const LevelSet& level_set, size_t* size, std::string* err,
                  const tvdb_allocator_t* allocator = nullptr) {
  if (level_set.empty()) {
    if (err) *err = "VDB write: empty level set";
    return nullptr;
  }
  tvdb_file_t file;
  tvdb_file_init_for_write(&file, allocator);
  file.grids = const_cast<tvdb_grid*>(level_set.grid());  // read only
  file.num_grids = 1;
  uint8_t* data = nullptr;
  tvdb_error_t e;
  std::memset(&e, 0, sizeof(e));
  if (tvdb_write_to_memory(&file, TVDB_COMPRESS_ZIP | TVDB_COMPRESS_ACTIVE_MASK, 0, &data, size,
                           &e) != TVDB_OK) {
    Fail(err, "VDB write", e);
    return nullptr;
  }
  return data;
}
}  // namespace

bool WriteLevelSetVDB(const LevelSet& level_set, std::vector<uint8_t>* bytes,
                      std::string* err, size_t max_working_memory_bytes) {
  if (!bytes) {
    if (err) *err = "VDB write: no output buffer";
    return false;
  }
  LevelSetBudget budget;
  tvdb_allocator_t allocator = {LevelSetBudget::Allocate, LevelSetBudget::Reallocate,
                                LevelSetBudget::Free, &budget};
  const tvdb_allocator_t* allocator_ptr = nullptr;
  if (max_working_memory_bytes) {
    budget.limit = max_working_memory_bytes;
    allocator_ptr = &allocator;
  }
  size_t size = 0;
  uint8_t* data = WriteVDB(level_set, &size, err, allocator_ptr);
  if (!data) {
    if (budget.exhausted && err) *err = "VDB write: memory budget exceeded";
    return false;
  }
  if (allocator_ptr && (budget.peak > max_working_memory_bytes ||
                        size > max_working_memory_bytes - budget.peak)) {
    allocator.free_fn(data, size, &budget);
    if (err) *err = "VDB write: memory budget exceeded";
    return false;
  }
  bytes->assign(data, data + size);
  if (allocator_ptr) allocator.free_fn(data, size, &budget);
  else std::free(data);
  return true;
}

bool WriteLevelSetVDB(const LevelSet& level_set, std::string* bytes, std::string* err) {
  size_t size = 0;
  uint8_t* data = bytes ? WriteVDB(level_set, &size, err) : nullptr;
  if (!data) return false;
  bytes->assign(reinterpret_cast<const char*>(data), size);
  std::free(data);
  return true;
}

bool ReadLevelSetVDB(const uint8_t* data, size_t size, LevelSet* out, std::string* err,
                     size_t max_decode_bytes) {
  if (!data || !size || !out) {
    if (err) *err = "VDB read: no data";
    return false;
  }
  LevelSetBudget* budget = nullptr;
  if (max_decode_bytes) {
    const size_t fixed = sizeof(tvdb_file) + sizeof(LevelSetBudget);
    if (max_decode_bytes <= fixed) {
      if (err) *err = "VDB read: memory budget exceeded";
      return false;
    }
    budget = new (std::nothrow) LevelSetBudget();
    if (!budget) { if (err) *err = "VDB read: out of memory"; return false; }
    budget->limit = max_decode_bytes - fixed;
  }
  auto* file = static_cast<tvdb_file*>(std::calloc(1, sizeof(tvdb_file)));
  if (!file) {
    delete budget;
    if (err) *err = "VDB read: out of memory";
    return false;
  }
  tvdb_error_t e;
  std::memset(&e, 0, sizeof(e));
  uint8_t* is_float = nullptr;
  // Headers first (class and value type, no voxels); then only the grids
  // needed to pick one are decoded. A decoded grid owns its storage, so the
  // handle does not reference `data` afterwards.
  auto fail = [&](const char* what) {
    if (is_float) file->alloc.free_fn(is_float, file->num_grids, file->alloc.user_ctx);
    const bool exhausted = budget && budget->exhausted;
    tvdb_file_close(file);
    std::free(file);
    delete budget;
    if (exhausted) { if (err) *err = "VDB read: memory budget exceeded"; return false; }
    return Fail(err, what, e);
  };
  tvdb_allocator_t allocator{LevelSetBudget::Allocate, LevelSetBudget::Reallocate,
                             LevelSetBudget::Free, budget};
  if (tvdb_file_open_memory(file, data, size, budget ? &allocator : nullptr, &e) != TVDB_OK)
    return fail("VDB read");
  is_float = static_cast<uint8_t*>(file->alloc.malloc_fn(file->num_grids ? file->num_grids : 1,
                                                         file->alloc.user_ctx));
  if (!is_float) { e.status = TVDB_ERROR_OUT_OF_MEMORY; return fail("VDB read"); }
  for (size_t i = 0; i < file->num_grids; ++i) {
    if (tvdb_read_grid_header(file, i, &e) != TVDB_OK) return fail("VDB read");
    const tvdb_grid* g = file->grids + i;
    is_float[i] = g->tree.layout.num_levels >= 1 &&
                  g->tree.layout.levels[g->tree.layout.num_levels - 1].value_type == TVDB_VALUE_FLOAT;
  }
  // The first float grid classed "level set", else the first float grid with
  // a positive background.
  size_t pick = SIZE_MAX;
  for (size_t i = 0; i < file->num_grids && pick == SIZE_MAX; ++i) {
    if (!is_float[i] || std::strcmp(GridClass(file->grids + i), "level set")) continue;
    if (tvdb_read_grid(file, i, &e) != TVDB_OK) return fail("VDB read");
    if (file->grids[i].tree.num_nodes) pick = i;
  }
  for (size_t i = 0; i < file->num_grids && pick == SIZE_MAX; ++i) {
    if (!is_float[i]) continue;
    if (!file->grids[i].tree.num_nodes && tvdb_read_grid(file, i, &e) != TVDB_OK)
      return fail("VDB read");
    const tvdb_grid* g = file->grids + i;
    if (g->tree.num_nodes && g->tree.nodes[0].u.root.background.u.f > 0)
      pick = i;
    else
      tvdb_grid_destroy(file->grids + i, &file->alloc);  // not this one: free its voxels
  }
  if (pick == SIZE_MAX) {
    file->alloc.free_fn(is_float, file->num_grids, file->alloc.user_ctx);
    tvdb_file_close(file);
    std::free(file);
    delete budget;
    if (err) *err = "VDB read: no float level-set grid";
    return false;
  }
  file->alloc.free_fn(is_float, file->num_grids, file->alloc.user_ctx);
  LevelSetAccess::AdoptFile(out, file, pick, budget);
  return true;
}

bool SampleLevelSet(const LevelSet& level_set, const double* points, size_t count,
                    float* values, std::string* err) {
  if (level_set.empty() || (count && (!points || !values))) {
    if (err) *err = "level set sample: empty level set";
    return false;
  }
  double m[4][4];
  if (!tvdb_tree_matrix(&level_set.grid()->transform, m)) {
    if (err) *err = "level set sample: unsupported transform";
    return false;
  }
  const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  const double inv[3][3] = {
      {(m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det,
       (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det},
      {(m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det,
       (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det},
      {(m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det,
       (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det}};
  tvdb_tree_index index;
  std::memset(&index, 0, sizeof(index));
  tvdb_error_t e;
  std::memset(&e, 0, sizeof(e));
  if (tvdb_tree_index_create(level_set.grid(), 1, &index, &e) != TVDB_OK)
    return Fail(err, "level set sample", e);
  for (size_t i = 0; i < count; ++i) {
    const double* p = points + 3 * i;
    const double d[3] = {p[0] - m[0][3], p[1] - m[1][3], p[2] - m[2][3]};
    double q[3];
    int32_t base[3];
    bool ok = true;
    for (int a = 0; a < 3; ++a) {
      q[a] = inv[a][0] * d[0] + inv[a][1] * d[1] + inv[a][2] * d[2];
      const double fl = std::floor(q[a]);
      ok = ok && fl >= double(INT32_MIN) && fl < double(INT32_MAX);
      base[a] = ok ? int32_t(fl) : 0;
      q[a] -= fl;
    }
    if (!ok) {
      values[i] = level_set.background();
      continue;
    }
    double sum = 0;
    for (int c = 0; c < 8; ++c) {
      const int32_t at[3] = {base[0] + (c & 1), base[1] + ((c >> 1) & 1), base[2] + ((c >> 2) & 1)};
      const double w = ((c & 1) ? q[0] : 1 - q[0]) * (((c >> 1) & 1) ? q[1] : 1 - q[1]) *
                       (((c >> 2) & 1) ? q[2] : 1 - q[2]);
      sum += w * double(tvdb_tree_get(&index, at, nullptr));
    }
    values[i] = float(sum);
  }
  tvdb_tree_index_destroy(&index);
  return true;
}

bool LevelSetToMesh(const LevelSet& level_set, size_t max_cells, Mesh* out, std::string* err) {
  if (level_set.empty() || !out) {
    if (err) *err = "level set mesh: empty level set";
    return false;
  }
  tvdb_triangle_mesh tm;
  std::memset(&tm, 0, sizeof(tm));
  tvdb_error_t e;
  std::memset(&e, 0, sizeof(e));
  if (tvdb_grid_volume_to_mesh(level_set.grid(), 0.0f, 0.0, max_cells, &tm, &e) != TVDB_OK)
    return Fail(err, "level set mesh", e);
  out->positions.resize(3 * tm.vertex_count);
  for (size_t i = 0; i < tm.vertex_count; ++i) {
    out->positions[3 * i] = tm.vertices[i].x;
    out->positions[3 * i + 1] = tm.vertices[i].y;
    out->positions[3 * i + 2] = tm.vertices[i].z;
  }
  out->indices.resize(3 * tm.face_count);
  for (size_t i = 0; i < tm.face_count; ++i) {
    out->indices[3 * i] = int32_t(tm.faces[i].v0);
    out->indices[3 * i + 1] = int32_t(tm.faces[i].v1);
    out->indices[3 * i + 2] = int32_t(tm.faces[i].v2);
  }
  out->counts.clear();
  out->normals.clear();
  out->uvs.clear();
  tvdb_triangle_mesh_free(&tm);
  return true;
}

bool LevelSetToTetMesh(const LevelSet& level_set, const StuffingOptions& options,
                       TetMeshData* out, std::string* err, std::string* device_used) {
  if (level_set.empty() || !out || !std::isfinite(options.spacing) || !(options.spacing > 0) ||
      !std::isfinite(options.graded_coarse_spacing) || options.graded_coarse_spacing < 0 ||
      !std::isfinite(options.graded_growth) || options.graded_growth <= 0 ||
      (options.graded_surface_guard && options.graded_coarse_spacing <= 0) ||
      (options.graded_coarse_spacing > 0 && options.graded_coarse_spacing < options.spacing)) {
    if (err) *err = "tet mesh: empty level set or invalid spacing or grading";
    return false;
  }
  tvdb_stuffing_options_t o;
  std::memset(&o, 0, sizeof(o));
  o.spacing = options.spacing;
  o.max_tets = options.max_tets;
  o.max_lattice_vertices = options.max_lattice_vertices;
  if (options.graded_coarse_spacing > 0 && options.device != "cpu") {
    if (err) *err = "tet mesh: graded stuffing requires the cpu device";
    return false;
  }
  Pool pool;
  if (!pool.Make(options.threads, err)) return false;
  DeviceLease lease;
  if (!LeaseDevice(options.device, &lease, err)) return false;
  tvdb_tet_mesh tm;
  std::memset(&tm, 0, sizeof(tm));
  tvdb_error_t e;
  std::memset(&e, 0, sizeof(e));
  tvdb_status_t st;
#if defined(LIGHTUSD_HAVE_TINYVDB_GPU)
  if (lease.device) {
    st = tvdb_gpu_compute_level_set_to_tet_mesh(lease.device, level_set.grid(), &o, pool.pool, &tm, &e);
    if (st != TVDB_OK && st != TVDB_ERROR_INVALID_ARGUMENT &&
        options.device == "auto") {  // as in MeshToLevelSet
      tvdb_tet_mesh_free(&tm);
      std::memset(&tm, 0, sizeof(tm));
      std::memset(&e, 0, sizeof(e));
      st = tvdb_level_set_to_tet_mesh(level_set.grid(), &o, pool.pool, &tm, &e);
      lease.name = "cpu";
    }
  } else
#endif
    if (options.graded_coarse_spacing > 0) {
      tvdb_graded_stuffing_options_t graded = {};
      graded.fine_spacing = options.spacing;
      graded.coarse_spacing = options.graded_coarse_spacing;
      graded.grading = options.graded_growth;
      graded.warp_alpha = 0.2;
      graded.max_vertices = options.max_lattice_vertices;
      graded.max_background_tets = options.max_background_tets;
      graded.max_output_tets = options.max_tets;
      st = options.graded_surface_guard
        ? tvdb_level_set_to_tet_mesh_graded_guarded(level_set.grid(), &graded, &tm, &e)
        : tvdb_level_set_to_tet_mesh_graded(level_set.grid(), &graded, &tm, &e);
    } else {
      st = tvdb_level_set_to_tet_mesh(level_set.grid(), &o, pool.pool, &tm, &e);
    }
  if (st != TVDB_OK) return Fail(err, "tet mesh", e);
  if (device_used) *device_used = lease.name;
  out->points.assign(tm.positions, tm.positions + 3 * tm.vertex_count);
  out->tets.assign(tm.tets, tm.tets + 4 * tm.tet_count);
  out->surface.assign(tm.boundary, tm.boundary + 3 * tm.boundary_count);
  tvdb_tet_mesh_free(&tm);
  if (out->tets.empty()) {
    if (err) *err = "tet mesh: the level set has no interior at this spacing";
    return false;
  }
  return true;
}

bool IsResourceLimitError(const std::string& err) {
  for (const char* key : {"budget", "limit", "too large", "out of memory"})
    if (err.find(key) != std::string::npos) return true;
  return false;
}

}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
