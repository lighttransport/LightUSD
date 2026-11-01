// SPDX-License-Identifier: Apache 2.0
// Unit tests for UsdRender placeholders and UsdVol prim types. These are
// recognized as distinct prim *types* (not the generic Model fallback) and
// retain authored properties through USDA + USDC round-trips.

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-render-vol.h"
#include "lightusd.hh"
#include "usdc-writer.hh"
#include "usdGeom.hh"
#include "usdVol.hh"
#include "usdShade.hh"
#include "core/model-scope.hh"
#include "usdGeom.hh"
#include "tydra/vol/volume-field.hh"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace lightusd;

static const char *kRenderVolUSDA =
    "#usda 1.0\n"
    "\n"
    "def RenderSettings \"settings\"\n"
    "{\n"
    "    int2 resolution = (1920, 1080)\n"
    "}\n"
    "\n"
    "def RenderProduct \"product\"\n"
    "{\n"
    "    token productType = \"raster\"\n"
    "}\n"
    "\n"
    "def RenderVar \"var\"\n"
    "{\n"
    "    token dataType = \"color3f\"\n"
    "}\n"
    "\n"
    "def Volume \"vol\"\n"
    "{\n"
    "}\n"
    "\n"
    "def OpenVDBAsset \"density\"\n"
    "{\n"
    "    asset filePath = @density.vdb@\n"
    "}\n"
    "\n"
    "def Field3DAsset \"f3d\"\n"
    "{\n"
    "}\n"
    "\n"
    "def GenerativeProcedural \"proc\"\n"
    "{\n"
    "    token primvars:displayColor = \"foo\"\n"
    "}\n";

static bool LoadUSDA(const std::string &usda, Stage *stage) {
  std::string warn, err;
  bool ok = lightusd::LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(), "mem.usda",
      stage, &warn, &err);
  if (!ok) TEST_MSG("USDA parse failed: %s", err.c_str());
  return ok;
}

// Find a root prim by element name (root-prim order is not guaranteed to match
// authored order after a USDC round-trip).
static const Prim *FindRoot(const Stage &stage, const std::string &name) {
  for (const auto &p : stage.root_prims()) {
    if (std::string(p.element_name()) == name) return &p;
  }
  return nullptr;
}

// The 6 root prims are recognized as their concrete placeholder types (NOT the
// generic Model fallback), and an authored property is retained.
static void CheckTypes(const Stage &stage) {
  TEST_CHECK_(stage.root_prims().size() == 7, "expected 7 root prims, got %zu",
              stage.root_prims().size());

  const Prim *settings = FindRoot(stage, "settings");
  const Prim *proc = FindRoot(stage, "proc");
  TEST_CHECK(proc && proc->as<GenerativeProcedural>() != nullptr);
  const Prim *product = FindRoot(stage, "product");
  const Prim *var = FindRoot(stage, "var");
  const Prim *vol = FindRoot(stage, "vol");
  const Prim *density = FindRoot(stage, "density");
  const Prim *f3d = FindRoot(stage, "f3d");

  TEST_CHECK(settings && settings->as<RenderSettings>() != nullptr);
  TEST_CHECK(product && product->as<RenderProduct>() != nullptr);
  TEST_CHECK(var && var->as<RenderVar>() != nullptr);
  TEST_CHECK(vol && vol->as<Volume>() != nullptr);
  TEST_CHECK(density && density->as<OpenVDBAsset>() != nullptr);
  TEST_CHECK(f3d && f3d->as<Field3DAsset>() != nullptr);

  // Must NOT be misclassified as the generic Model fallback.
  TEST_CHECK(settings && settings->as<Model>() == nullptr);

  // Authored property retained generically.
  if (product) {
    if (const RenderProduct *p = product->as<RenderProduct>()) {
      TEST_CHECK(p->props.find("productType") != p->props.end());
    }
  }
}

void render_vol_usda_roundtrip_test(void) {
  Stage stage;
  TEST_CHECK(LoadUSDA(kRenderVolUSDA, &stage));
  CheckTypes(stage);

  std::string exported = stage.ExportToString();
  TEST_CHECK(exported.find("def RenderSettings \"settings\"") != std::string::npos);
  TEST_CHECK(exported.find("def RenderProduct \"product\"") != std::string::npos);
  TEST_CHECK(exported.find("def RenderVar \"var\"") != std::string::npos);
  TEST_CHECK(exported.find("def Volume \"vol\"") != std::string::npos);
  TEST_CHECK(exported.find("def OpenVDBAsset \"density\"") != std::string::npos);
  TEST_CHECK(exported.find("def Field3DAsset \"f3d\"") != std::string::npos);
  TEST_CHECK(exported.find("def GenerativeProcedural \"proc\"") != std::string::npos);

  Stage stage2;
  TEST_CHECK(LoadUSDA(exported, &stage2));
  CheckTypes(stage2);
}

void render_vol_crate_roundtrip_test(void) {
  Stage stage;
  TEST_CHECK(LoadUSDA(kRenderVolUSDA, &stage));

  std::vector<uint8_t> usdc;
  std::string w, e;
  bool sret = lightusd::usdc::SaveAsUSDCToMemory(stage, &usdc, &w, &e);
  TEST_CHECK_(sret, "SaveAsUSDCToMemory failed: %s", e.c_str());
  if (!sret) return;
  TEST_CHECK(!usdc.empty());

  Stage stage2;
  std::string w2, e2;
  bool lret = lightusd::LoadUSDCFromMemory(usdc.data(), usdc.size(), "mem.usdc",
                                           &stage2, &w2, &e2);
  TEST_CHECK_(lret, "LoadUSDCFromMemory failed: %s", e2.c_str());
  if (!lret) return;
  CheckTypes(stage2);
}

void usdvol_material_binding_test(void) {
  const std::string usda = R"USD(#usda 1.0
def Volume "Vol" (prepend apiSchemas = ["MaterialBindingAPI"]) {
    rel material:binding = </Mat>
}
def Material "Mat" {
    token outputs:volume.connect = </Mat/Shader.outputs:volume>
    def Shader "Shader" {
        uniform token info:id = "ND_standard_volume_volume"
        float inputs:density = 0.25
        color3f inputs:scattering_color = (0.1, 0.2, 0.3)
        color3f inputs:emission_color = (0.4, 0.5, 0.6)
        float inputs:emission_intensity = 2
        token outputs:volume
    }
}
)USD";
  Stage stage;
  TEST_CHECK(LoadUSDA(usda, &stage));
  const Prim *vol_prim = FindRoot(stage, "Vol");
  const Prim *mat_prim = FindRoot(stage, "Mat");
  TEST_ASSERT(vol_prim && mat_prim);
  const Volume *volume = vol_prim->as<Volume>();
  const Material *material = mat_prim->as<Material>();
  TEST_ASSERT(volume && material);
  Relationship binding;
  TEST_CHECK(volume->get_materialBinding(value::token(""), &binding));
  TEST_CHECK(material->volume.authored());
  TEST_CHECK(material->volume.get_connections().size() == 1);
  const Prim *shader_prim = nullptr;
  TEST_CHECK(stage.find_prim_at_path(Path("/Mat/Shader", ""), shader_prim));
  TEST_ASSERT(shader_prim && shader_prim->as<Shader>());
  const Shader *shader = shader_prim->as<Shader>();
  const ShaderNode *node = shader->value.as<ShaderNode>();
  TEST_ASSERT(node != nullptr);
  TEST_CHECK(node->props.find("inputs:density") != node->props.end());
  TEST_CHECK(node->props.find("inputs:emission_color") != node->props.end());
}

void usdvol_vdb_corpus_test(void) {
  const char *root = std::getenv("TINYVDBIO_DATA_DIR");
  if (!root || !root[0]) {
    TEST_MSG("SKIP: set TINYVDBIO_DATA_DIR to the tinyvdbio data directory");
    return;
  }
  struct Case { const char *file; const char *name; const char *type; };
  const Case cases[] = {
      {"reference/ref_bool.vdb", "bool_test", "bool"},
      {"reference/ref_float.vdb", "float_test", "float"},
      {"reference/ref_double.vdb", "double_test", "double"},
      {"reference/ref_int32.vdb", "int32_test", "int32"},
      {"reference/ref_int64.vdb", "int64_test", "int64"},
      {"reference/ref_vec3s.vdb", "vec3s_test", "vec3f-magnitude"},
  };
  for (const Case &c : cases) {
    std::vector<usdVol::VDBGrid> grids;
    std::string warn, err;
    const std::string path = std::string(root) + "/" + c.file;
    const bool ok = usdVol::ReadVDBFromFile(path, &grids, &warn, &err, 4096);
    TEST_CHECK_(ok, "%s: %s", path.c_str(), err.c_str());
    if (!ok || grids.empty()) continue;
    const usdVol::VDBGrid &grid = grids[0];
    TEST_CHECK_(grid.name == c.name, "%s: grid name '%s'", c.file,
                grid.name.c_str());
    TEST_CHECK_(grid.value_type == c.type, "%s: value type '%s'", c.file,
                grid.value_type.c_str());
    TEST_CHECK(grid.dim[0] == 4 && grid.dim[1] == 4 && grid.dim[2] == 4);
    TEST_CHECK(grid.data.size() == 64);
  }

  // A rejected dense extent must say how much was requested and permitted.
  {
    std::vector<usdVol::VDBGrid> grids;
    std::string warn, err;
    const std::string path = std::string(root) + "/reference/ref_float.vdb";
    TEST_CHECK(!usdVol::ReadVDBFromFile(path, &grids, &warn, &err, 32));
    TEST_CHECK_(warn.find("dense extent 64 voxels, limit 32") != std::string::npos,
                "missing voxel counts in warning: %s", warn.c_str());
  }

  // USDZ asset resolution supplies VDB bytes rather than a filesystem path.
  // Exercise the external-buffer ownership path separately.
  {
    const std::string path = std::string(root) + "/reference/ref_float.vdb";
    std::ifstream stream(path.c_str(), std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
    std::vector<usdVol::VDBGrid> grids;
    std::string warn, err;
    const bool ok = usdVol::ReadVDBFromMemory(bytes.data(), bytes.size(), path,
                                               &grids, &warn, &err, 4096);
    TEST_CHECK_(ok, "memory VDB: %s", err.c_str());
    TEST_CHECK(grids.size() == 1 && grids[0].data.size() == 64);
  }

  const Case volume_cases[] = {
      {"sphere-div1.vdb", "density", "float"},
      {"smoke.vdb", "density", "float"},
      {"suzanne.vdb", "density", "float"},
      {"fire.vdb", "density", "float"},
      {"explosion.vdb", "density", "float"},
  };
  for (const Case &c : volume_cases) {
    std::vector<usdVol::VDBGrid> grids;
    std::string warn, err;
    const std::string path = std::string(root) + "/" + c.file;
    const bool ok = usdVol::ReadVDBFromFile(path, &grids, &warn, &err,
                                            size_t(16) * 1024 * 1024);
    TEST_CHECK_(ok, "%s: %s %s", path.c_str(), warn.c_str(), err.c_str());
    if (!ok || grids.empty()) continue;
    const usdVol::VDBGrid *grid = nullptr;
    for (const auto &candidate : grids)
      if (candidate.name == c.name) { grid = &candidate; break; }
    TEST_CHECK_(grid != nullptr, "%s: missing grid '%s'", c.file, c.name);
    if (!grid) continue;
    TEST_CHECK(grid->value_type == c.type);
    TEST_CHECK(!grid->data.empty());
    TEST_CHECK(grid->dim[0] > 0 && grid->dim[1] > 0 && grid->dim[2] > 0);
    if (std::string(c.file) == "fire.vdb") {
      const usdVol::VDBGrid *temperature = nullptr;
      for (const auto &candidate : grids) {
        if (candidate.name == "temperature") temperature = &candidate;
      }
      TEST_CHECK_(temperature != nullptr,
                  "fire.vdb: temperature field must be decoded for volume shading");
      if (temperature) {
        TEST_CHECK(!temperature->data.empty());
        // This corpus intentionally exercises independent field lattices; the
        // renderer must sample each field through its own VDB transform.
        TEST_CHECK(temperature->dim[0] != grid->dim[0] ||
                   temperature->dim[1] != grid->dim[1] ||
                   temperature->dim[2] != grid->dim[2]);
      }
    }
  }

  // Header-only listing and single-field reads (fieldName, fieldIndex).
  {
    const std::string path = std::string(root) + "/fire.vdb";
    std::vector<usdVol::VDBGridInfo> infos;
    std::string err;
    TEST_CHECK_(usdVol::ReadVDBGridInfoFromFile(path, &infos, &err), "%s", err.c_str());
    TEST_CHECK(infos.size() >= 2);
    for (const auto &info : infos) {
      TEST_CHECK(info.has_transform && info.name_index == 0 && !info.point_grid);
      if (info.name == "density") TEST_CHECK(info.value_type == "float");
    }
    usdVol::VDBReadOptions opt;
    usdVol::VDBGrid temperature;
    std::string warn;
    TEST_CHECK_(usdVol::ReadVDBGridFromFile(path, "temperature", 0, opt, &temperature, &warn, &err),
                "%s", err.c_str());
    TEST_CHECK(temperature.name == "temperature" && !temperature.data.empty());
    TEST_CHECK(temperature.grid_class == "fogVolume");
    usdVol::VDBGrid missing;
    err.clear();
    TEST_CHECK(!usdVol::ReadVDBGridFromFile(path, "temperature", 1, opt, &missing, &warn, &err));
    TEST_CHECK(err.find("no grid 'temperature' #1") != std::string::npos);
    // Budget-driven downsampling keeps the extent and fits the cap.
    opt.max_voxels = 100000;
    opt.downsample = true;
    usdVol::VDBGrid downsampled;
    TEST_CHECK(usdVol::ReadVDBGridFromFile(path, "temperature", 0, opt, &downsampled, &warn, &err));
    TEST_CHECK(downsampled.stride > 1 &&
               size_t(downsampled.dim[0]) * size_t(downsampled.dim[1]) * size_t(downsampled.dim[2]) <= 100000);
    TEST_CHECK(downsampled.dim[0] == (temperature.dim[0] + downsampled.stride - 1) / downsampled.stride);
    // Without downsampling the same cap rejects the grid.
    opt.downsample = false;
    err.clear();
    TEST_CHECK(!usdVol::ReadVDBGridFromFile(path, "temperature", 0, opt, &downsampled, &warn, &err));
  }
  // Vector grids as three components.
  {
    const std::string path = std::string(root) + "/reference/ref_vec3s.vdb";
    usdVol::VDBReadOptions opt;
    opt.vector_components = true;
    usdVol::VDBGrid v;
    std::string warn, err;
    TEST_CHECK_(usdVol::ReadVDBGridFromFile(path, "", 0, opt, &v, &warn, &err), "%s", err.c_str());
    TEST_CHECK(v.components == 3 && v.value_type == "vec3f" && v.data.size() == 3 * 64);
  }
  // Active tiles (suzanne's interior) carry their values, not the background.
  {
    usdVol::VDBGrid g;
    std::string warn, err;
    usdVol::VDBReadOptions opt;
    opt.max_voxels = size_t(16) * 1024 * 1024;
    TEST_CHECK(usdVol::ReadVDBGridFromFile(std::string(root) + "/suzanne.vdb", "density", 0, opt,
                                           &g, &warn, &err));
    TEST_CHECK(g.grid_class == "unknown");  // no class metadata in this file
    // The center of the head lies inside the tiles.
    const size_t c = size_t(g.dim[0] / 2) +
                     size_t(g.dim[0]) * (size_t(g.dim[1] / 2) + size_t(g.dim[1]) * size_t(g.dim[2] / 2));
    TEST_CHECK_(c < g.data.size() && g.data[c] > 0.0f, "suzanne center density %g",
                c < g.data.size() ? double(g.data[c]) : -1.0);
  }
  // Half-saved densities keep their subnormal values (smoke.vdb).
  {
    usdVol::VDBGrid g;
    std::string warn, err;
    usdVol::VDBReadOptions opt;
    opt.max_voxels = size_t(16) * 1024 * 1024;
    TEST_CHECK(usdVol::ReadVDBGridFromFile(std::string(root) + "/smoke.vdb", "density", 0, opt, &g,
                                           &warn, &err));
    size_t tiny = 0;
    for (float v : g.data) tiny += v > 0.0f && v < 6.1e-5f;
    TEST_CHECK_(tiny > 0, "smoke.vdb: %zu subnormal-range densities", tiny);
  }

  // PointDataGrid is valid VDB but is not a voxel-valued UsdVol field. It must
  // be rejected cleanly instead of treating its auxiliary point payload as a
  // scalar leaf buffer.
  {
    std::vector<usdVol::VDBGrid> grids;
    std::string warn, err;
    const bool ok = usdVol::ReadVDBFromFile(
        std::string(root) + "/sphere_points.vdb", &grids, &warn, &err, 4096);
    TEST_CHECK(!ok);
    TEST_CHECK(grids.empty());
    TEST_CHECK(warn.find("point grid") != std::string::npos);
  }
}

// NanoVDB export and world-space sampling through the shared field resolver.
// Uses tinyvdbio's reference corpus, so it skips without TINYVDBIO_DATA_DIR.
void usdvol_nanovdb_sample_test(void) {
  const char *root = std::getenv("TINYVDBIO_DATA_DIR");
  if (!root || !root[0]) {
    TEST_MSG("SKIP: set TINYVDBIO_DATA_DIR to the tinyvdbio data directory");
    return;
  }
  const std::string float_vdb = std::string(root) + "/reference/ref_float.vdb";
  std::vector<uint8_t> bytes;
  {
    std::ifstream in(float_vdb.c_str(), std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  TEST_ASSERT(!bytes.empty());

  // Float grid -> NanoVDB float grid with the "NanoVDB1" grid magic (the
  // value NanoVDB's own GridHandle writes; "NanoVDB0" is the legacy file-level
  // number).
  {
    std::vector<uint8_t> nano;
    usdVol::VDBNanoVDBInfo info;
    std::string err;
    TEST_CHECK_(usdVol::ReadVDBGridNanoVDB(bytes.data(), bytes.size(), float_vdb, "", 0, &nano,
                                           &info, &err),
                "%s", err.c_str());
    TEST_CHECK(nano.size() >= 736);
    uint64_t magic = 0;
    if (nano.size() >= 8) std::memcpy(&magic, nano.data(), 8);
    TEST_CHECK(magic == 0x314244566f6e614eULL);
    TEST_CHECK(info.grid_type == 1 && info.components == 1);
    TEST_CHECK(info.value_type == "float");
    uint32_t header_type = 0;
    if (nano.size() >= 640) std::memcpy(&header_type, nano.data() + 636, 4);
    TEST_CHECK(header_type == info.grid_type);
  }

  // Vec3 grids keep three components.
  {
    const std::string vec_path = std::string(root) + "/reference/ref_vec3s.vdb";
    std::vector<uint8_t> vec_bytes;
    {
      std::ifstream in(vec_path.c_str(), std::ios::binary);
      vec_bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::vector<uint8_t> nano;
    usdVol::VDBNanoVDBInfo info;
    std::string err;
    TEST_CHECK_(usdVol::ReadVDBGridNanoVDB(vec_bytes.data(), vec_bytes.size(), vec_path, "", 0,
                                           &nano, &info, &err),
                "%s", err.c_str());
    TEST_CHECK(info.grid_type == 6 && info.components == 3);
  }

  // Bool grids have no NanoVDB type and are rejected.
  {
    const std::string bool_path = std::string(root) + "/reference/ref_bool.vdb";
    std::vector<uint8_t> bool_bytes;
    {
      std::ifstream in(bool_path.c_str(), std::ios::binary);
      bool_bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::vector<uint8_t> nano;
    std::string err;
    TEST_CHECK(!usdVol::ReadVDBGridNanoVDB(bool_bytes.data(), bool_bytes.size(), bool_path, "", 0,
                                          &nano, nullptr, &err));
    TEST_CHECK(err.find("no NanoVDB representation") != std::string::npos);
  }

  // FieldCache: NanoVDB conversion is cached per grid, and world-space
  // sampling reads the sparse grid's own values (nearest == the voxel value
  // at its center, trilinear is a convex combination of neighbours).
  {
    lightusd::tydra::vol::FieldCache cache;
    lightusd::tydra::vol::FieldRequest request;
    request.binding = "density";
    request.field_prim = "/vol/density";
    request.prim_type = "OpenVDBAsset";
    request.file_path = float_vdb;
    request.field_name = "float_test";
    lightusd::tydra::vol::ResolvedField field;
    std::string err;
    TEST_CHECK_(cache.Resolve(request, &field, &err), "%s", err.c_str());
    const std::vector<uint8_t> *nano = cache.NanoVDB(field, nullptr, &err);
    TEST_CHECK_(nano != nullptr, "%s", err.c_str());
    const std::vector<uint8_t> *again = cache.NanoVDB(field, nullptr, &err);
    TEST_CHECK(nano == again);  // cached, same buffer

    // The reference grid is a 4x4x4 unit-voxel grid centred on the origin:
    // sample the center of voxel (0,0,0) in world space twice.
    const double point[3] = {0.5, 0.5, 0.5};
    std::vector<float> values;
    int components = 0;
    TEST_CHECK_(cache.SampleWorld(field, point, 1, /*trilinear=*/false, &values, &components,
                                  &err),
                "%s", err.c_str());
    TEST_CHECK(components == 1 && values.size() == 1);

    // Nearest at the center equals one of the stored voxel values; the
    // trilinear result stays within the grid's value range.
    usdVol::VDBGrid dense;
    usdVol::VDBReadOptions options;
    const usdVol::VDBGrid *decoded = cache.Dense(field, options, nullptr, &err);
    TEST_CHECK_(decoded != nullptr, "%s", err.c_str());
    if (decoded) {
      float lo = decoded->data[0], hi = decoded->data[0];
      for (float v : decoded->data) {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
      }
      TEST_CHECK(values[0] >= lo - 1e-6f && values[0] <= hi + 1e-6f);
      std::vector<float> tri;
      TEST_CHECK_(cache.SampleWorld(field, point, 1, /*trilinear=*/true, &tri, &components, &err),
                  "%s", err.c_str());
      TEST_CHECK(tri.size() == 1 && tri[0] >= lo - 1e-6f && tri[0] <= hi + 1e-6f);
    }
  }

  // Header, sparse decode and sampler failures must honor cache limits and
  // release partial allocations.
  {
    std::ifstream in(float_vdb.c_str(), std::ios::binary | std::ios::ate);
    const size_t file_size = static_cast<size_t>(in.tellg());
    lightusd::tydra::vol::FieldCache cache(lightusd::tydra::vol::AssetReader{},
                                           file_size + 64);
    lightusd::tydra::vol::FieldRequest request;
    request.field_prim = "/vol/density";
    request.prim_type = "OpenVDBAsset";
    request.file_path = float_vdb;
    request.field_name = "float_test";
    lightusd::tydra::vol::ResolvedField field;
    std::string err;
    const double point[3] = {0.5, 0.5, 0.5};
    std::vector<float> values;
    int components = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
      err.clear();
      TEST_CHECK(!cache.Resolve(request, &field, &err));
      TEST_CHECK_(err.find("memory budget") != std::string::npos, "%s", err.c_str());
      TEST_CHECK(cache.bytes_used() == 0);
    }

    lightusd::tydra::vol::FieldCache probe;
    TEST_CHECK_(probe.Resolve(request, &field, &err), "%s", err.c_str());
    TEST_CHECK_(probe.SampleWorld(field, point, 1, true, &values, &components, &err),
                "%s", err.c_str());
    const size_t sampled_bytes = probe.bytes_used();
    TEST_CHECK(sampled_bytes > file_size);
    size_t low = file_size, high = sampled_bytes;
    while (low < high) {
      const size_t mid = low + (high - low) / 2;
      lightusd::tydra::vol::FieldCache candidate(lightusd::tydra::vol::AssetReader{}, mid);
      lightusd::tydra::vol::ResolvedField candidate_field;
      if (candidate.Resolve(request, &candidate_field, nullptr)) high = mid;
      else low = mid + 1;
    }
    lightusd::tydra::vol::FieldCache decode_limited(lightusd::tydra::vol::AssetReader{}, high);
    TEST_CHECK_(decode_limited.Resolve(request, &field, &err), "%s", err.c_str());
    const size_t baseline = decode_limited.bytes_used();
    for (int attempt = 0; attempt < 2; ++attempt) {
      err.clear();
      TEST_CHECK(!decode_limited.SampleWorld(field, point, 1, true, &values, &components, &err));
      TEST_CHECK_(err.find("sparse grid exceeds the memory budget") != std::string::npos,
                  "%s", err.c_str());
      TEST_CHECK(decode_limited.bytes_used() == baseline);
    }
    lightusd::tydra::vol::FieldCache limited(lightusd::tydra::vol::AssetReader{},
                                             sampled_bytes - 1);
    TEST_CHECK_(limited.Resolve(request, &field, &err), "%s", err.c_str());
    err.clear();
    TEST_CHECK(!limited.SampleWorld(field, point, 1, true, &values, &components, &err));
    // Decode can transiently exceed its final retained size, so the limit
    // just below the final footprint may reject either decode or the index.
    TEST_CHECK_(err.find("exceeds the memory budget") != std::string::npos,
                "%s", err.c_str());
    TEST_CHECK(limited.bytes_used() < sampled_bytes);
  }
}
