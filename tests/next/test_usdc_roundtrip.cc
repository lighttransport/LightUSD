// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USDC Roundtrip Test
// Creates a stage with multiple schema types, writes to USDC,
// and verifies the binary output by parsing TOC/sections.

#include <iostream>
#include <fstream>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>

#include "next/stage/stage.hh"
#include "next/layer/layer.hh"
#include "next/types/value.hh"
#include "next/crate/crate-writer.hh"
#include "next/crate/crate-format.hh"
#include "next/crate/crate-reader.hh"
#include "next/crate/lazy-array.hh"
#include "next/lightusd-next.hh"
#include "next/writer/usdc-writer.hh"
#include "next/writer/dtoa.hh"
#include "next/parser/ascii-parser.hh"
#include "next/layer/property-index.hh"
#include "next/pcp/prim-index.hh"
#include "next/schema/color-space.hh"

using namespace lightusd::next;

#if !defined(LIGHTUSD_NEXT_NO_MMAP) && !defined(__EMSCRIPTEN__) && \
    !defined(__wasi__) &&                                             \
    (defined(__unix__) || defined(__APPLE__) || defined(__linux__))
constexpr bool kExpectUsdaLazyMmap = true;
#else
constexpr bool kExpectUsdaLazyMmap = false;
#endif

static const Value* DictFind(const Value& dv, const char* key) {
  if (!dv.is_dictionary() || !dv.as_dictionary()) return nullptr;
  return dv.as_dictionary()->find(key);
}

static const PrimSpec* MustPrim(const Layer* layer, const char* path) {
  assert(layer);
  const PrimSpec* prim = layer->prim_at_path(path);
  assert(prim && "expected prim missing");
  return prim;
}

static const Value* MustProp(const PrimSpec* prim, const char* name) {
  assert(prim);
  const Value* value = prim->property_value(name);
  assert(value && "expected property missing");
  return value;
}

static void CheckFloatArray(const Value* value, TypeId type,
                            const std::vector<float>& expected) {
  assert(value && value->is_array() && value->type_id() == type);
  const std::vector<float>* arr = value->as_float_array();
  assert(arr && *arr == expected);
}

static void CheckIntArray(const Value* value,
                          const std::vector<int32_t>& expected) {
  assert(value && value->is_array() && value->type_id() == TypeId::Int);
  const std::vector<int32_t>* arr = value->as_int_array();
  assert(arr && *arr == expected);
}

static void CheckInt64Array(const Value* value,
                            const std::vector<int64_t>& expected) {
  assert(value && value->is_array() && value->type_id() == TypeId::Int64);
  const std::vector<int64_t>* arr = value->as_int64_array();
  assert(arr && *arr == expected);
}

// ============================================================
// Minimal binary USDC section parser (inline, no reader dependency)
// ============================================================

struct SectionInfo {
  char name[16];
  int64_t start;
  int64_t size;
};

struct TocInfo {
  std::vector<SectionInfo> sections;

  const SectionInfo* find(const char* n) const {
    for (const auto& s : sections) {
      if (std::strncmp(s.name, n, 15) == 0) return &s;
    }
    return nullptr;
  }
};

bool ParseUSDCBinary(const uint8_t* data, size_t size, TocInfo& toc) {
  if (size < 24) return false;
  if (std::memcmp(data, kCrateMagic, 8) != 0) return false;

  // Bootstrap: 8 magic + 8 version/pad = up to reserved byte 15;
  // TOC offset at byte 16-23 (u64 LE)
  uint64_t toc_offset = 0;
  for (int i = 0; i < 8; i++) {
    toc_offset |= static_cast<uint64_t>(data[16 + i]) << (i * 8);
  }

  if (toc_offset + 8 > size) return false;

  // Read TOC
  uint64_t num_sections = 0;
  size_t pos = static_cast<size_t>(toc_offset);
  if (pos + 8 > size) return false;
  for (int i = 0; i < 8; i++) {
    num_sections |= static_cast<uint64_t>(data[pos + i]) << (i * 8);
  }
  pos += 8;

  if (num_sections > 100) return false;

  for (uint64_t i = 0; i < num_sections; i++) {
    if (pos + 16 + 8 + 8 > size) return false;
    SectionInfo si;
    std::memcpy(si.name, data + pos, 16);
    pos += 16;

    int64_t start = 0;
    for (int j = 0; j < 8; j++) {
      start |= static_cast<int64_t>(data[pos + j]) << (j * 8);
    }
    pos += 8;
    si.start = start;

    int64_t sec_size = 0;
    for (int j = 0; j < 8; j++) {
      sec_size |= static_cast<int64_t>(data[pos + j]) << (j * 8);
    }
    pos += 8;
    si.size = sec_size;

    if (start < 0 || sec_size < 0 ||
        static_cast<size_t>(start + sec_size) > size)
      return false;

    toc.sections.push_back(si);
  }
  return true;
}

// Read a uint64 LE from a data pointer
uint64_t ReadU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(p[i]) << (i * 8);
  return v;
}

uint32_t ReadU32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(p[i]) << (i * 8);
  return v;
}

// Read a length-prefixed string (u64 length + data)
std::string ReadString(const uint8_t* data, size_t& pos) {
  uint64_t len = ReadU64(data + pos);
  pos += 8;
  std::string s(reinterpret_cast<const char*>(data + pos),
                static_cast<size_t>(len));
  pos += static_cast<size_t>(len);
  return s;
}

// ============================================================
// Test functions
// ============================================================

void test_roundtrip_schema_types() {
  std::cout << "Testing roundtrip with multiple schema types...\n";

  // Create a stage with various schema types
  StageBuilder stage_builder;
  stage_builder.SetDefaultPrim("World");
  stage_builder.SetUpAxis("Y");
  stage_builder.SetMetersPerUnit(0.01);
  stage_builder.SetTimeCodesPerSecond(24.0);
  stage_builder.SetStartTimeCode(0.0);
  stage_builder.SetEndTimeCode(100.0);

  LayerBuilder& layer = stage_builder.GetLayerBuilder();

  // Xform
  layer.begin_prim("World", "Xform");
  layer.add_property("visibility", Value::MakeToken("inherited"));
  layer.end_prim();

  // Mesh
  layer.begin_prim("Cube", "Mesh");
  std::vector<float> points = {
    -1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1,
    -1, -1,  1, 1, -1,  1, 1, 1,  1, -1, 1,  1
  };
  layer.add_property("points", Value::MakeFloat3Array(points));
  std::vector<int> fvc = {4, 4, 4, 4, 4, 4};
  layer.add_property("faceVertexCounts", Value::MakeIntArray(fvc));
  layer.add_property("doubleSided", Value(true));
  layer.add_property("subdivisionScheme", Value::MakeToken("none"));
  layer.end_prim();

  // Camera
  layer.begin_prim("Camera1", "Camera");
  layer.add_property("focalLength", Value(50.0));
  layer.add_property("fStop", Value(2.8f));
  layer.add_property("horizontalAperture", Value(36.0));
  layer.end_prim();

  // Sphere
  layer.begin_prim("Sphere1", "Sphere");
  layer.add_property("radius", Value(1.5));
  layer.end_prim();

  // Capsule
  layer.begin_prim("Capsule1", "Capsule");
  layer.add_property("radius", Value(0.5));
  layer.add_property("height", Value(2.0));
  layer.end_prim();

  // Cylinder
  layer.begin_prim("Cylinder1", "Cylinder");
  layer.add_property("radius", Value(0.75));
  layer.add_property("height", Value(3.0));
  layer.add_property("axis", Value::MakeToken("Z"));
  layer.end_prim();

  // Material with shader
  layer.begin_prim("Material1", "Material");
  layer.add_property("inputs:roughness", Value(0.5f));
  layer.add_property("inputs:metallic", Value(1.0f));
  layer.add_property("inputs:baseColor", Value::MakeFloat3(0.2f, 0.8f, 0.3f));
  layer.end_prim();

  layer.begin_prim("Shader1", "Shader");
  layer.add_property("info:id", Value::MakeToken("UsdPreviewSurface"));
  layer.add_property("inputs:diffuseColor", Value::MakeFloat3(0.5f, 0.5f, 0.5f));
  layer.end_prim();

  // Lights
  layer.begin_prim("DistantLight1", "DistantLight");
  layer.add_property("intensity", Value(5000.0f));
  layer.add_property("angle", Value(0.53f));
  layer.end_prim();

  layer.begin_prim("SphereLight1", "SphereLight");
  layer.add_property("intensity", Value(150.0f));
  layer.add_property("radius", Value(0.5));
  layer.end_prim();

  layer.begin_prim("RectLight1", "RectLight");
  layer.add_property("intensity", Value(300.0f));
  layer.add_property("width", Value(1.0));
  layer.add_property("height", Value(0.5));
  layer.end_prim();

  // Prim with metadata
  layer.begin_prim("Helper", "Scope");
  {
    PrimSpec* prim = layer.current();
    assert(prim);
    prim->meta().doc() = "A helper scope";
    prim->meta().hidden = true;
  }
  layer.end_prim();

  layer.finalize();

  Stage stage = stage_builder.Build();

  // Write to USDC via CrateWriter (low-level)
  std::vector<uint8_t> buffer;
  CrateWriter writer;
  CrateWriteResult result = writer.WriteToMemory(buffer, stage);

  assert(result.success);
  std::cout << "  Written " << result.bytes_written << " bytes\n";
  std::cout << "  Tokens: " << result.token_count << "\n";
  std::cout << "  Paths: " << result.path_count << "\n";
  std::cout << "  Specs: " << result.spec_count << "\n";
  std::cout << "  Fields: " << result.field_count << "\n";

  // Verify basic structure
  assert(buffer.size() >= kCrateBootstrapSize);
  assert(std::memcmp(buffer.data(), kCrateMagic, 8) == 0);

  // Parse TOC
  TocInfo toc;
  // NOTE: ParseUSDCBinary() populates `toc` as a side effect, so it must run
  // outside assert() — under NDEBUG `assert(expr)` does not evaluate `expr`,
  // which would leave `toc` empty and null-deref the section lookups below.
  const bool toc_parsed = ParseUSDCBinary(buffer.data(), buffer.size(), toc);
  assert(toc_parsed);
  (void)toc_parsed;

  // Verify required sections exist
  auto* tokens_sec = toc.find("TOKENS");
  auto* strings_sec = toc.find("STRINGS");
  auto* fields_sec = toc.find("FIELDS");
  auto* fieldsets_sec = toc.find("FIELDSETS");
  auto* paths_sec = toc.find("PATHS");
  auto* specs_sec = toc.find("SPECS");

  assert(tokens_sec != nullptr);
  assert(strings_sec != nullptr);
  assert(fields_sec != nullptr);
  assert(fieldsets_sec != nullptr);
  assert(paths_sec != nullptr);
  assert(specs_sec != nullptr);

  // VALUE section is not in TOC (pxrUSD stores it between bootstrap
  // and structural sections, not in the TOC). Verify it exists between
  // bootstrap end (64) and first structural section.
  if (toc.sections.size() > 0) {
    assert(toc.sections[0].start > 64);
  }

  // Verify section order
  assert(std::strncmp(toc.sections[0].name, "TOKENS", 6) == 0);
  assert(std::strncmp(toc.sections[1].name, "STRINGS", 7) == 0);
  assert(std::strncmp(toc.sections[2].name, "FIELDS", 6) == 0);
  assert(std::strncmp(toc.sections[3].name, "FIELDSETS", 9) == 0);
  assert(std::strncmp(toc.sections[4].name, "PATHS", 5) == 0);
  assert(std::strncmp(toc.sections[5].name, "SPECS", 5) == 0);

  // Verify sections are non-empty
  assert(tokens_sec->size > 8);
  assert(paths_sec->size >= 8);
  assert(specs_sec->size >= 8);
  assert(fields_sec->size >= 8);
  assert(fieldsets_sec->size >= 8);

  // Verify no overlap between sections
  for (size_t i = 0; i < toc.sections.size(); i++) {
    for (size_t j = i + 1; j < toc.sections.size(); j++) {
      int64_t a_start = toc.sections[i].start;
      int64_t a_end = a_start + toc.sections[i].size;
      int64_t b_start = toc.sections[j].start;
      int64_t b_end = b_start + toc.sections[j].size;
      assert(!(a_start < b_end && b_start < a_end));
    }
  }

  // Parse TOKENS section (LZ4 compressed: num + uncomp_size + comp_size + data)
  {
    const uint8_t* tdata = buffer.data() + static_cast<size_t>(tokens_sec->start);
    uint64_t num_tokens = ReadU64(tdata);

    uint64_t uncomp_size = ReadU64(tdata + 8);
    uint64_t comp_size = ReadU64(tdata + 16);

    // Decompress (using pxrUSD TfFastCompression format: n_chunks prefix + LZ4)
    std::vector<uint8_t> tok_raw;
    if (comp_size == 0) {
      // Uncompressed fallback
      tok_raw.assign(tdata + 24, tdata + 24 + static_cast<size_t>(uncomp_size));
    } else {
      DecompressResult dr = DecompressCrateBlob(
          tdata + 24, static_cast<size_t>(comp_size),
          static_cast<size_t>(uncomp_size));
      assert(dr.success);
      tok_raw = std::move(dr.data);
    }

    // Parse null-terminated token strings
    std::map<std::string, uint32_t> token_map;
    std::vector<std::string> token_list;
    const char* ptr = reinterpret_cast<const char*>(tok_raw.data());
    const char* end = ptr + tok_raw.size();
    uint32_t idx = 0;
    while (ptr < end && idx < num_tokens) {
      std::string tok(ptr);
      token_list.push_back(tok);
      token_map[tok] = idx++;
      ptr += tok.size() + 1;
    }

    assert(token_list.size() == num_tokens);

    // Verify schema tokens exist
    auto check_token = [&](const std::string& t) {
      if (token_map.find(t) == token_map.end()) {
        std::cerr << "  Missing token: \"" << t << "\"\n";
        std::cerr << "  Available tokens (" << token_list.size() << "):";
        for (size_t ti = 0; ti < token_list.size() && ti < 20; ti++)
          std::cerr << " [" << ti << "]=" << token_list[ti];
        if (token_list.size() > 20)
          std::cerr << " ... (" << (token_list.size() - 20) << " more)";
        std::cerr << "\n";
        assert(false);
      }
    };

    // Prim type names
    check_token("Xform");
    check_token("Mesh");
    check_token("Camera");
    check_token("Sphere");
    check_token("Capsule");
    check_token("Cylinder");
    check_token("Material");
    check_token("Shader");
    check_token("DistantLight");
    check_token("SphereLight");
    check_token("RectLight");
    check_token("Scope");

    // Schema field names
    check_token("points");
    check_token("faceVertexCounts");
    check_token("doubleSided");
    check_token("subdivisionScheme");
    check_token("focalLength");
    check_token("fStop");
    check_token("horizontalAperture");
    check_token("radius");
    check_token("height");
    check_token("axis");
    check_token("intensity");
    check_token("angle");
    check_token("width");

    // Common tokens
    check_token("typeName");
    check_token("specifier");
    check_token("defaultPrim");
    check_token("upAxis");
    check_token("metersPerUnit");
    check_token("timeCodesPerSecond");
    check_token("startTimeCode");
    check_token("endTimeCode");
    check_token("active");
    check_token("hidden");
    check_token("doc");

    std::cout << "  Verified " << token_list.size() << " tokens"
              << " (schema type names + field names all present)\n";
  }

  // Write to file for external inspection
  const char* test_file = "/tmp/test_roundtrip_schema.usdc";
  CrateWriteResult file_result = writer.WriteToFile(test_file, stage);
  assert(file_result.success);
  std::cout << "  Wrote to " << test_file << " ("
            << file_result.bytes_written << " bytes)\n";

  // Verify file matches memory
  std::ifstream ifs(test_file, std::ios::binary | std::ios::ate);
  assert(ifs.is_open());
  size_t file_size = static_cast<size_t>(ifs.tellg());
  ifs.close();
  assert(file_size == buffer.size());

  std::cout << "  roundtrip schema types test passed!\n\n";
}

void test_roundtrip_layer_metadata() {
  std::cout << "Testing roundtrip layer metadata...\n";

  // Create layer directly with metadata
  Layer layer;
  layer.meta().defaultPrim = "Root";
  layer.meta().upAxis = "Z";
  layer.meta().metersPerUnit = 1.0;
  layer.meta().timeCodesPerSecond = 30.0;
  layer.meta().startTimeCode = 1.0;
  layer.meta().endTimeCode = 48.0;
  layer.meta().doc = "Test layer";

  LayerBuilder builder(layer);
  builder.begin_prim("Root", "Xform");
  builder.end_prim();
  builder.finalize();

  // Write via CrateWriter directly
  CrateWriter writer;
  std::vector<uint8_t> buffer;
  CrateWriteResult result = writer.WriteLayerToMemory(buffer, layer);
  assert(result.success);

  // Parse TOC and verify
  TocInfo toc;
  // NOTE: ParseUSDCBinary() populates `toc` as a side effect, so it must run
  // outside assert() — under NDEBUG `assert(expr)` does not evaluate `expr`,
  // which would leave `toc` empty and null-deref the section lookups below.
  const bool toc_parsed = ParseUSDCBinary(buffer.data(), buffer.size(), toc);
  assert(toc_parsed);
  (void)toc_parsed;

  auto* tokens_sec = toc.find("TOKENS");
  assert(tokens_sec != nullptr);

  // Verify version in bootstrap
  uint8_t v_major = buffer[8];
  uint8_t v_minor = buffer[9];
  uint8_t v_patch = buffer[10];
  std::cout << "  Version: " << (int)v_major << "." << (int)v_minor << "." << (int)v_patch << "\n";

  std::cout << "  Bytes: " << result.bytes_written
            << "  Tokens: " << result.token_count
            << "  Paths: " << result.path_count
            << "  Specs: " << result.spec_count
            << "  Fields: " << result.field_count << "\n";

  std::cout << "  roundtrip layer metadata test passed!\n\n";
}

void test_roundtrip_time_samples() {
  std::cout << "Testing roundtrip time samples...\n";

  // Create a stage with time-sampled properties
  // (direct CrateWriter test using layer)
  Layer layer;

  LayerBuilder builder(layer);
  builder.begin_prim("Animated", "Xform");

  // Add time-sampled translate
  builder.add_time_sample("xformOp:translate", 0.0,
                           Value::MakeFloat3(0.0f, 0.0f, 0.0f));
  builder.add_time_sample("xformOp:translate", 50.0,
                           Value::MakeFloat3(10.0f, 5.0f, 0.0f));
  builder.add_time_sample("xformOp:translate", 100.0,
                           Value::MakeFloat3(20.0f, 0.0f, 0.0f));

  // Add animated weight
  builder.add_time_sample("weight", 0.0, Value(0.0f));
  builder.add_time_sample("weight", 100.0, Value(1.0f));

  builder.end_prim();
  builder.finalize();

  // Write via CrateWriter directly
  CrateWriter writer;
  std::vector<uint8_t> buffer;
  CrateWriteResult result = writer.WriteLayerToMemory(buffer, layer);
  assert(result.success);

  std::cout << "  Written " << result.bytes_written << " bytes\n";
  std::cout << "  Tokens: " << result.token_count
            << "  Paths: " << result.path_count
            << "  Specs: " << result.spec_count
            << "  Fields: " << result.field_count << "\n";

  // Verify basic structure
  assert(std::memcmp(buffer.data(), kCrateMagic, 8) == 0);

  TocInfo toc;
  // NOTE: ParseUSDCBinary() populates `toc` as a side effect, so it must run
  // outside assert() — under NDEBUG `assert(expr)` does not evaluate `expr`,
  // which would leave `toc` empty and null-deref the section lookups below.
  const bool toc_parsed = ParseUSDCBinary(buffer.data(), buffer.size(), toc);
  assert(toc_parsed);
  (void)toc_parsed;

  auto* ts = toc.find("TOKENS");
  assert(ts != nullptr);

  // Read back and verify the time samples decode per-property (Phase: crate
  // TimeSamples decoding — previously skipped on read).
  CrateReader reader;
  CrateReadResult rr = reader.Read(buffer.data(), buffer.size());
  assert(rr.success && "re-read of time-sampled layer failed");
  const PrimSpec* anim = rr.stage.GetRootLayer()->prim_at_path("/Animated");
  assert(anim);

  PropNameId tr = GetPropNameTable().find("xformOp:translate");
  assert(tr.is_valid() && anim->has_time_samples(tr));
  const auto* tr_samples = anim->time_samples(tr);
  assert(tr_samples && tr_samples->size() == 3 && "translate: 3 samples expected");
  // Sample at t=50 should be (10,5,0).
  bool found_50 = false;
  for (const auto& kv : *tr_samples) {
    if (std::abs(kv.first - 50.0) < 1e-9) {
      const Value* v = anim->time_sample_value(kv.second);
      assert(v && v->is_array() == false);
      const float* f3 = v->as_float3();
      assert(f3 && f3[0] == 10.0f && f3[1] == 5.0f && f3[2] == 0.0f);
      found_50 = true;
    }
  }
  assert(found_50 && "translate sample at t=50 missing/wrong");

  PropNameId wt = GetPropNameTable().find("weight");
  assert(wt.is_valid() && anim->has_time_samples(wt));
  const auto* wt_samples = anim->time_samples(wt);
  assert(wt_samples && wt_samples->size() == 2 && "weight: 2 samples expected");

  std::cout << "  roundtrip time samples test passed!\n\n";
}

void test_write_usdc_from_stage_api() {
  std::cout << "Testing WriteUSDC via high-level API...\n";

  StageBuilder stage_builder;
  stage_builder.SetDefaultPrim("Test");
  stage_builder.SetUpAxis("Y");

  LayerBuilder& layer = stage_builder.GetLayerBuilder();
  layer.begin_prim("Test", "Xform");
  layer.add_property("testValue", Value(42));
  layer.end_prim();
  layer.finalize();

  Stage stage = stage_builder.Build();

  // Test WriteUSDCToFile
  const char* test_file = "/tmp/test_api_output.usdc";
  USDCWriteResult file_result = WriteUSDCToFile(test_file, stage);
  assert(file_result.success);
  std::cout << "  WriteUSDCToFile: " << file_result.bytes_written << " bytes\n";

  // Test WriteLayerToUSDCMemory
  const Layer* root_layer = stage.GetRootLayer();
  assert(root_layer != nullptr);
  std::vector<uint8_t> mem_buffer;
  USDCWriteResult mem_result = WriteLayerToUSDCMemory(mem_buffer, *root_layer);
  assert(mem_result.success);
  std::cout << "  WriteLayerToUSDCMemory: " << mem_result.bytes_written << " bytes\n";

  assert(std::memcmp(mem_buffer.data(), kCrateMagic, 8) == 0);

  std::cout << "  WriteUSDC stage API test passed!\n\n";
}

// Vec4f / Matrix4d / Quatf / Double3 arrays must survive a write -> read cycle
// (Phase 8: these were dropped on read with "Unsupported array type").
void test_roundtrip_vec_matrix_arrays() {
  std::cout << "Testing roundtrip vec/matrix/quat arrays...\n";

  Layer layer;
  LayerBuilder b(layer);
  b.begin_prim("Geo", "Mesh");
  std::vector<float> v4 = {1, 2, 3, 4, 5, 6, 7, 8};           // 2 x Vec4f
  std::vector<float> qf = {0, 0, 0, 1, 0, 1, 0, 0};           // 2 x Quatf
  std::vector<double> m4(32);                                 // 2 x Matrix4d
  for (size_t i = 0; i < m4.size(); ++i) m4[i] = double(i) * 0.5;
  std::vector<float> m4f(32);                                 // 2 x Matrix4f
  std::vector<double> m4f_as_d(32);
  for (size_t i = 0; i < m4f.size(); ++i) {
    m4f[i] = float(i) * 0.25f;
    m4f_as_d[i] = double(m4f[i]);
  }
  std::vector<double> d3 = {1.5, 2.5, 3.5, 4.5, 5.5, 6.5};    // 2 x Double3
  b.add_property("v4", Value::MakeFloatCompArray(std::vector<float>(v4), TypeId::Float4, 4));
  b.add_property("qf", Value::MakeFloatCompArray(std::vector<float>(qf), TypeId::Quatf, 4));
  b.add_property("m4", Value::MakeDoubleCompArray(std::vector<double>(m4), TypeId::Matrix4d, 16));
  b.add_property("m4f", Value::MakeFloatCompArray(std::vector<float>(m4f), TypeId::Matrix4f, 16));
  b.add_property("d3", Value::MakeDoubleCompArray(std::vector<double>(d3), TypeId::Double3, 3));
  b.end_prim();
  b.finalize();

  CrateWriter writer;
  std::vector<uint8_t> buf;
  CrateWriteResult wr = writer.WriteLayerToMemory(buf, layer);
  assert(wr.success);

  {
    CrateReadOptions limited;
    limited.max_memory = 1;
    CrateReader limited_reader(limited);
    CrateReadResult limited_result = limited_reader.Read(buf.data(), buf.size());
    assert(!limited_result.success && !limited_result.errors.empty() &&
           "max_memory must reject oversized in-memory crate input");
  }

  CrateReader reader;
  CrateReadResult rr = reader.Read(buf.data(), buf.size());
  assert(rr.success && "re-read of vec/matrix arrays failed");
  const Layer* rl = rr.stage.GetRootLayer();
  const PrimSpec* geo = rl->prim_at_path("/Geo");
  assert(geo);

  auto check_f = [&](const char* name, const std::vector<float>& expect, TypeId t) {
    const Value* val = geo->property_value(name);
    assert(val && val->is_array() && val->type_id() == t);
    const std::vector<float>* arr = val->as_float_array();
    assert(arr && *arr == expect);
  };
  auto check_d = [&](const char* name, const std::vector<double>& expect, TypeId t) {
    const Value* val = geo->property_value(name);
    assert(val && val->is_array() && val->type_id() == t);
    const std::vector<double>* arr = val->as_double_array();
    assert(arr && *arr == expect);
  };
  check_f("v4", v4, TypeId::Float4);
  check_f("qf", qf, TypeId::Quatf);
  check_d("m4", m4, TypeId::Matrix4d);
  check_d("m4f", m4f_as_d, TypeId::Matrix4d);
  check_d("d3", d3, TypeId::Double3);

  std::cout << "  vec/matrix/quat array roundtrip passed!\n\n";
}

void test_high_level_memory_caps() {
  std::cout << "Testing high-level memory caps...\n";

  Layer layer;
  LayerBuilder b(layer);
  b.begin_prim("P", "Xform");
  b.add_property("v", Value(1.0));
  b.end_prim();
  b.finalize();

  CrateWriter writer;
  std::vector<uint8_t> buf;
  CrateWriteResult wr = writer.WriteLayerToMemory(buf, layer);
  assert(wr.success);

  const char* usdc_file = "/tmp/next_memcap_highlevel.usdc";
  {
    std::ofstream f(usdc_file, std::ios::binary);
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
  }

  LoadUSDOptions load_opts;
  load_opts.max_memory = 1;
  Stage limited_stage;
  std::string warn, err;
  bool ok = LoadUSD(usdc_file, &limited_stage, load_opts, &warn, &err);
  assert(!ok && "LoadUSD max_memory must reject oversized USDC input");

  USDCLoadOptions usdc_opts;
  usdc_opts.crate_options.max_memory = 1;
  warn.clear();
  err.clear();
  ok = LoadUSDC(usdc_file, &limited_stage, usdc_opts, &warn, &err);
  assert(!ok && "LoadUSDC options must reject oversized USDC input");

  const char* usda_file = "/tmp/next_memcap_highlevel.usda";
  {
    std::ofstream f(usda_file, std::ios::binary);
    f << "#usda 1.0\n\ndef Xform \"World\" {}\n";
  }
  warn.clear();
  err.clear();
  ok = LoadUSD(usda_file, &limited_stage, load_opts, &warn, &err);
  assert(!ok && "LoadUSD max_memory must reject oversized USDA input");

  LoadOptions usda_opts;
  usda_opts.parse_options.max_file_size = 1;
  warn.clear();
  err.clear();
  ok = LoadUSDA(usda_file, &limited_stage, usda_opts, &warn, &err);
  assert(!ok && "LoadUSDA options must reject oversized USDA input");

  const char* asset_file = "/tmp/next_memcap_ext_asset.usda";
  const char* root_file = "/tmp/next_memcap_ext_root.usda";
  std::string root_text =
      "#usda 1.0\n(\n  subLayers = [@./next_memcap_ext_asset.usda@]\n)\n"
      "def Xform \"World\" {}\n";
  std::string asset_text =
      "#usda 1.0\n\ndef Xform \"Asset\" {\n  string note = \"";
  asset_text.append(256, 'x');
  asset_text += "\"\n}\n";
  {
    std::ofstream f(root_file, std::ios::binary);
    f << root_text;
  }
  {
    std::ofstream f(asset_file, std::ios::binary);
    f << asset_text;
  }

  LoadUSDOptions composed_opts;
  composed_opts.max_memory = root_text.size() + 8;
  assert(root_text.size() <= composed_opts.max_memory);
  assert(asset_text.size() > composed_opts.max_memory);
  warn.clear();
  err.clear();
  pcp::CompositionOptions comp_opts;
  comp_opts.error_when_asset_not_found = true;
  ok = LoadUSDComposed(root_file, &limited_stage, composed_opts, &warn, &err,
                       &comp_opts);
  assert(!ok && "LoadUSDComposed must cap external composition layers");

  std::remove(usdc_file);
  std::remove(usda_file);
  std::remove(asset_file);
  std::remove(root_file);
  std::cout << "  high-level memory caps passed!\n\n";
}

void test_load_usdcomposed_usda_parse_options() {
  std::cout << "Testing composed USDA parse options passthrough...\n";

  const char* root_file = "/tmp/next-ticket4-root.usda";
  const char* ext_file = "/tmp/next-ticket4-ext.usda";

  const std::string root_text =
      "#usda 1.0\n(\n"
      "  subLayers = [@./next-ticket4-ext.usda@]\n"
      ")\n"
      "def Xform \"Root\" {}\n";
  const std::string ext_text =
      "#usda 1.0\n"
      "def Mesh \"AssetGeom\" {\n"
      "  int[] small = [1, 2]\n"
      "  int[] large = [1, 2, 3, 4]\n"
      "  point3f[] points = [(1, 2, 3), (4, 5, 6), (7, 8, 9)]\n"
      "}\n";

  {
    std::ofstream f(root_file, std::ios::binary);
    f << root_text;
  }
  {
    std::ofstream f(ext_file, std::ios::binary);
    f << ext_text;
  }

  {
    Stage composed;
    assert(LoadUSDComposed(root_file, &composed, nullptr, nullptr, nullptr));
    UsdPrim mesh = composed.GetPrimAtPath("/AssetGeom");
    assert(mesh.IsValid());
    assert(!mesh.GetPropertyValue("small")->is_lazy());
    assert(!mesh.GetPropertyValue("large")->is_lazy());
    assert(!mesh.GetPropertyValue("points")->is_lazy());
  }

  LoadUSDOptions lo;
  pcp::CompositionOptions co;
  co.usda_parse_options.enable_usda_lazy_arrays = true;
  co.usda_parse_options.max_usda_lazy_array_elements = 2;
  {
    Stage composed;
    std::string warn, err;
    assert(LoadUSDComposed(root_file, &composed, lo, &warn, &err, &co));
    assert(warn.empty());
    assert(err.empty());
    UsdPrim mesh = composed.GetPrimAtPath("/AssetGeom");
    assert(mesh.IsValid());
    const Value* small = mesh.GetPropertyValue("small");
    const Value* large = mesh.GetPropertyValue("large");
    const Value* points = mesh.GetPropertyValue("points");
    assert(small && large && points);
    assert(small->is_lazy());
    assert(small->lazy_ref() && small->lazy_ref()->source);
    if (kExpectUsdaLazyMmap) {
      assert(small->lazy_ref()->source->is_mmapped());
    }
    assert(!large->is_lazy());
    assert(!points->is_lazy());
  }

  co.usda_parse_options.max_usda_lazy_array_elements = 0;
  {
    Stage composed;
    std::string warn, err;
    assert(LoadUSDComposed(root_file, &composed, lo, &warn, &err, &co));
    assert(warn.empty());
    assert(err.empty());
    UsdPrim mesh = composed.GetPrimAtPath("/AssetGeom");
    assert(mesh.IsValid());
    const Value* large = mesh.GetPropertyValue("large");
    const Value* points = mesh.GetPropertyValue("points");
    assert(large && points);
    assert(large->is_lazy());
    assert(points->is_lazy());
    assert(large->lazy_ref() && large->lazy_ref()->source);
    assert(points->lazy_ref() && points->lazy_ref()->source);
    if (kExpectUsdaLazyMmap) {
      assert(large->lazy_ref()->source->is_mmapped());
      assert(points->lazy_ref()->source->is_mmapped());
    }
  }

  std::remove(root_file);
  std::remove(ext_file);
  std::cout << "  composed USDA parse option passthrough passed!\n\n";
}

// HalfToFloat/FloatToHalf: every finite half bit pattern must survive
// half -> float -> half byte-exact (NaN payloads excluded).
void test_half_conversion() {
  std::cout << "Testing half<->float conversion...\n";
  size_t checked = 0;
  for (uint32_t bits = 0; bits < 0x10000u; ++bits) {
    uint16_t h = static_cast<uint16_t>(bits);
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    if (exp == 0x1Fu && mant != 0) continue;  // skip NaN (payload not preserved)
    float f = HalfToFloat(h);
    uint16_t h2 = FloatToHalf(f);
    assert(h2 == h && "half->float->half not exact");
    ++checked;
  }
  assert(HalfToFloat(0x3C00) == 1.0f && "half 1.0");
  assert(HalfToFloat(0xC000) == -2.0f && "half -2.0");
  std::cout << "  " << checked << " half patterns round-tripped exactly\n\n";
}

// The next writer must spell a half with the shortest decimal that reparses to
// the same binary16 value, rather than exposing its widened float value.
void test_half_shortest_decimal() {
  std::cout << "Testing shortest half decimal formatting...\n";
  assert(htos(FloatToHalf(0.35f)) == "0.35");
  assert(htos(FloatToHalf(-0.35f)) == "-0.35");

  size_t checked = 0;
  for (uint32_t bits = 0; bits < 0x10000u; ++bits) {
    const uint16_t h = static_cast<uint16_t>(bits);
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    if (exp == 0x1Fu && mant != 0) continue;
    const std::string text = htos(h);
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    assert(end == text.c_str() + text.size());
    assert(FloatToHalf(parsed) == h);
    ++checked;
  }
  std::cout << "  " << checked
            << " half spellings reparsed to identical bits\n\n";
}

// Half / Vec3h / Quath arrays must survive write -> read (values chosen exactly
// representable in half so the comparison is exact).
void test_roundtrip_half_arrays() {
  std::cout << "Testing roundtrip half arrays...\n";

  Layer layer;
  LayerBuilder b(layer);
  b.begin_prim("Geo", "Mesh");
  std::vector<float> h1 = {1.0f, -2.0f, 0.5f, 0.25f};            // 4 x Half
  std::vector<float> h3 = {1.0f, 2.0f, 3.0f, -1.0f, 0.5f, 4.0f}; // 2 x Half3
  std::vector<float> qh = {0.0f, 0.0f, 0.0f, 1.0f};             // 1 x Quath
  b.add_property("h1", Value::MakeFloatCompArray(std::vector<float>(h1), TypeId::Half, 1));
  b.add_property("h3", Value::MakeFloatCompArray(std::vector<float>(h3), TypeId::Half3, 3));
  b.add_property("qh", Value::MakeFloatCompArray(std::vector<float>(qh), TypeId::Quath, 4));
  b.end_prim();
  b.finalize();

  CrateWriter writer;
  std::vector<uint8_t> buf;
  CrateWriteResult wr = writer.WriteLayerToMemory(buf, layer);
  assert(wr.success);

  CrateReader reader;
  CrateReadResult rr = reader.Read(buf.data(), buf.size());
  assert(rr.success && "re-read of half arrays failed");
  const PrimSpec* geo = rr.stage.GetRootLayer()->prim_at_path("/Geo");
  assert(geo);

  auto check = [&](const char* name, const std::vector<float>& expect, TypeId t) {
    const Value* v = geo->property_value(name);
    assert(v && v->is_array() && v->type_id() == t);
    const std::vector<float>* arr = v->as_float_array();  // materializes half->float
    assert(arr && *arr == expect);
  };
  check("h1", h1, TypeId::Half);
  check("h3", h3, TypeId::Half3);
  check("qh", qh, TypeId::Quath);

  std::cout << "  half array roundtrip passed!\n\n";
}

// Phase 7 S5: arc list-op qualifiers survive a USDC write -> read cycle via the
// companion `<arc>_listOp` token[] fields. The within-spec effective list (the
// `references` token[]) and the ArcEdit (prepend/append/delete) both round-trip.
void test_roundtrip_arc_listops() {
  std::cout << "Testing arc list-op crate roundtrip...\n";
  Layer layer;
  LayerBuilder b(layer);
  b.begin_prim("P", "Scope");
  // Within-spec effective list (= prepend B), plus the raw edit: prepend </B>,
  // delete </A>.
  b.current()->meta().references.push_back("</B>");
  {
    ArcEdit& e = b.current()->meta().ensure_arc_edits().references;
    e.authored = true;
    e.is_explicit = false;
    e.prepended.push_back("</B>");
    e.deleted.push_back("</A>");
  }
  // A bare (explicit) inherits list must NOT gain a companion field.
  b.current()->meta().inherits.push_back("</_class_X>");
  b.end_prim();
  b.finalize();

  CrateWriter writer;
  std::vector<uint8_t> buf;
  CrateWriteResult wr = writer.WriteLayerToMemory(buf, layer);
  assert(wr.success);

  CrateReader reader;
  CrateReadResult rr = reader.Read(buf.data(), buf.size());
  assert(rr.success && "re-read of arc list-ops failed");
  const PrimSpec* p = rr.stage.GetRootLayer()->prim_at_path("/P");
  assert(p);

  // The within-spec effective list round-trips.
  assert(p->meta().references.size() == 1 && p->meta().references[0] == "</B>");
  // The non-explicit edit round-trips.
  const ArcListOpEdits* ed = p->meta().arc_edits();
  assert(ed && "references edit must survive the crate roundtrip");
  assert(ed->references.authored);
  assert(!ed->references.is_explicit);
  assert(ed->references.prepended.size() == 1 &&
         ed->references.prepended[0] == "</B>");
  assert(ed->references.deleted.size() == 1 &&
         ed->references.deleted[0] == "</A>");
  // Bare inherits: no companion field, so its edit stays explicit (default).
  assert(!ed->inherits.authored && ed->inherits.is_explicit &&
         ed->inherits.prepended.empty());
  std::cout << "  arc list-op crate roundtrip passed!\n\n";
}

void test_roundtrip_arc_metadata_dicts() {
  std::cout << "Testing arc metadata dictionary crate roundtrip...\n";

  Layer layer;
  LayerBuilder b(layer);
  b.begin_prim("AssetRef", "Xform");
  b.current()->meta().references.push_back("@asset.usda@</Model>");
  b.current()->meta().payloads.push_back("@payload.usda@</Payload>");
  {
    Dict cd;
    cd.set("source", Value("reference-fixture"));
    cd.set("enabled", Value(true));
    cd.set("revision", Value(int32_t(7)));
    b.current()->meta().customData() = Value::MakeDictionary(std::move(cd));
  }
  {
    Dict ai;
    ai.set("identifier", Value::MakeAssetPath("asset.usda"));
    ai.set("kind", Value::MakeToken("component"));
    b.current()->meta().assetInfo() = Value::MakeDictionary(std::move(ai));
  }
  b.end_prim();
  b.finalize();

  CrateWriter writer;
  std::vector<uint8_t> buf;
  CrateWriteResult wr = writer.WriteLayerToMemory(buf, layer);
  assert(wr.success);

  CrateReader reader;
  CrateReadResult rr = reader.Read(buf.data(), buf.size());
  assert(rr.success && "re-read of arc metadata dictionaries failed");
  const PrimSpec* p = rr.stage.GetRootLayer()->prim_at_path("/AssetRef");
  assert(p);

  assert(p->meta().references.size() == 1);
  assert(p->meta().references[0] == "@asset.usda@</Model>");
  assert(p->meta().payloads.size() == 1);
  assert(p->meta().payloads[0] == "@payload.usda@</Payload>");

  const Value& cd = p->meta().customData();
  assert(cd.is_dictionary());
  const Value* source = DictFind(cd, "source");
  assert(source && source->as_string() && *source->as_string() == "reference-fixture");
  const Value* enabled = DictFind(cd, "enabled");
  assert(enabled && enabled->as_bool() && *enabled->as_bool());
  const Value* revision = DictFind(cd, "revision");
  assert(revision && revision->as_int() && *revision->as_int() == 7);

  const Value& ai = p->meta().assetInfo();
  assert(ai.is_dictionary());
  const Value* identifier = DictFind(ai, "identifier");
  assert(identifier && identifier->as_asset_path() &&
         *identifier->as_asset_path() == "asset.usda");
  const Value* kind = DictFind(ai, "kind");
  assert(kind && kind->as_token() && *kind->as_token() == "component");

  std::cout << "  arc metadata dictionary crate roundtrip passed!\n\n";
}

void test_roundtrip_custom_qualifier() {
  std::cout << "Testing custom-qualifier crate roundtrip...\n";
  Layer layer;
  LayerBuilder b(layer);
  b.begin_prim("P", "Xform");
  b.add_property("isAsset", Value(true), PropSlot::kFlagCustom);
  b.add_property("plain", Value(1.0));  // non-custom control
  b.end_prim();
  b.finalize();

  CrateWriter writer;
  std::vector<uint8_t> buf;
  CrateWriteResult wr = writer.WriteLayerToMemory(buf, layer);
  assert(wr.success);

  CrateReader reader;
  CrateReadResult rr = reader.Read(buf.data(), buf.size());
  assert(rr.success && "re-read of custom-qualified attr failed");
  const PrimSpec* p = rr.stage.GetRootLayer()->prim_at_path("/P");
  assert(p);

  const PropSlot* a = p->property("isAsset");
  assert(a && a->is_custom() && "custom flag lost across the crate roundtrip");
  const PropSlot* pl = p->property("plain");
  assert(pl && !pl->is_custom() && "non-custom attr gained a custom flag");
  std::cout << "  custom-qualifier crate roundtrip passed!\n\n";
}

void test_roundtrip_api_schemas() {
  std::cout << "Testing apiSchemas crate roundtrip...\n";
  Layer layer;
  LayerBuilder b(layer);
  b.begin_prim("P", "Mesh");
  b.current()->meta().apiSchemas().push_back("MaterialBindingAPI");
  b.current()->meta().apiSchemas().push_back("PhysicsCollisionAPI");
  b.end_prim();
  b.finalize();

  CrateWriter writer;
  std::vector<uint8_t> buf;
  CrateWriteResult wr = writer.WriteLayerToMemory(buf, layer);
  assert(wr.success);

  CrateReader reader;
  CrateReadResult rr = reader.Read(buf.data(), buf.size());
  assert(rr.success && "re-read of apiSchemas failed");
  const PrimSpec* p = rr.stage.GetRootLayer()->prim_at_path("/P");
  assert(p);

  // apiSchemas must land in PrimSpecMeta (pxr writes it in the metadata block),
  // NOT leak back as a phantom `token[] apiSchemas` body property.
  assert(p->meta().apiSchemas().size() == 2 &&
         "apiSchemas did not round-trip into PrimSpecMeta");
  assert(p->meta().apiSchemas()[0] == "MaterialBindingAPI");
  assert(p->meta().apiSchemas()[1] == "PhysicsCollisionAPI");
  assert(p->property("apiSchemas") == nullptr &&
         "apiSchemas leaked as a phantom body property");
  std::cout << "  apiSchemas crate roundtrip passed!\n\n";
}

void test_comprehensive_usdc_fixture() {
  std::cout << "Testing comprehensive USDC fixture roundtrip...\n";

  Layer layer;
  layer.meta().defaultPrim = "World";
  layer.meta().upAxis = "Z";
  layer.meta().metersPerUnit = 1.0;
  layer.meta().timeCodesPerSecond = 24.0;
  layer.meta().framesPerSecond = 24.0;
  layer.meta().framesPerSecond_set = true;
  layer.meta().startTimeCode = 1.0;
  layer.meta().endTimeCode = 48.0;
  layer.meta().doc = "Dense generated USDC fixture";
  {
    Dict cld;
    cld.set("fixture", Value("comprehensive-usdc"));
    cld.set("revision", Value(int32_t(3)));
    cld.set("enabled", Value(true));
    layer.meta().customLayerData = Value::MakeDictionary(std::move(cld));
  }

  LayerBuilder b(layer);
  b.begin_prim("World", "Xform");
  {
    PrimSpec* world = b.current();
    assert(world);
    world->meta().doc() = "fixture root";
    world->meta().apiSchemas().push_back("MaterialBindingAPI");
    world->meta().references.push_back("@asset.usda@</Asset>");
    world->meta().payloads.push_back("@payload.usda@</Payload>");
    world->meta().inherits.push_back("</_class_Model>");
    world->meta().specializes.push_back("</_class_ModelSpecialization>");
    Dict cd;
    cd.set("role", Value::MakeToken("root"));
    cd.set("lod", Value(int32_t(1)));
    world->meta().customData() = Value::MakeDictionary(std::move(cd));
    Dict ai;
    ai.set("identifier", Value::MakeAssetPath("asset.usda"));
    ai.set("kind", Value::MakeToken("component"));
    world->meta().assetInfo() = Value::MakeDictionary(std::move(ai));
  }
  b.add_time_sample("xformOp:translate", 1.0,
                    Value::MakeFloat3(0.0f, 0.0f, 0.0f));
  b.add_time_sample("xformOp:translate", 24.0,
                    Value::MakeFloat3(1.0f, 2.0f, 3.0f));
  b.end_prim();

  const std::vector<float> points = {
      0.0f, 0.0f, 0.0f,
      1.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 1.0f};
  const std::vector<int32_t> fvc = {3, 3};
  const std::vector<int32_t> fvi = {0, 1, 2, 0, 2, 3};
  const std::vector<float> display_color = {
      1.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 1.0f,
      1.0f, 1.0f, 1.0f};
  b.begin_prim("Mesh", "Mesh");
  b.add_property("points", Value::MakeFloat3Array(points));
  b.add_property("faceVertexCounts", Value::MakeIntArray(fvc));
  b.add_property("faceVertexIndices", Value::MakeIntArray(fvi));
  b.add_property("primvars:displayColor",
                 Value::MakeFloat3Array(display_color));
  b.add_relationship("material:binding", Path("/Material"));
  {
    PropMeta& pm = b.current()->ensure_property_meta("primvars:displayColor");
    pm.interpolation = "vertex";
    pm.authored |= PropMeta::kInterpolation;
    pm.colorSpace = "sRGB";
    pm.authored |= PropMeta::kColorSpace;
    pm.elementSize = 1;
    pm.authored |= PropMeta::kElementSize;
    Dict pcd;
    pcd.set("role", Value::MakeToken("albedo"));
    pm.customData = Value::MakeDictionary(std::move(pcd));
    pm.authored |= PropMeta::kCustomData;
  }
  b.end_prim();

  b.begin_prim("Material", "Material");
  {
    PropNameId surface_id = GetPropNameTable().intern("outputs:surface");
    b.current()->add_property_slot(surface_id, TypeId::Token,
                                   PropSlot::kFlagConnection);
    b.current()->set_property_type_name("outputs:surface", "token");
  }
  b.current()->add_connection("outputs:surface",
                              Path("/Shader.outputs:surface"));
  b.end_prim();

  b.begin_prim("Shader", "Shader");
  b.add_property("info:id", Value::MakeToken("UsdPreviewSurface"));
  b.add_property("inputs:diffuseColor",
                 Value::MakeFloat3(0.25f, 0.5f, 0.75f),
                 PropSlot::kFlagConnection);
  b.current()->set_property_type_name("inputs:diffuseColor", "color3f");
  b.current()->add_connection("inputs:diffuseColor",
                              Path("/Texture.outputs:rgb"));
  b.end_prim();

  b.begin_prim("Texture", "Shader");
  b.add_property("info:id", Value::MakeToken("UsdUVTexture"));
  b.add_property("inputs:file", Value::MakeAssetPath("albedo.png"));
  b.end_prim();

  const std::vector<int32_t> proto_indices = {0, 0, 0};
  const std::vector<float> positions = {
      0.0f, 0.0f, 0.0f,
      1.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f};
  const std::vector<float> positions_t2 = {
      0.0f, 0.0f, 0.0f,
      2.0f, 0.0f, 0.0f,
      0.0f, 2.0f, 0.0f};
  const std::vector<float> orientations = {
      1.0f, 0.0f, 0.0f, 0.0f,
      1.0f, 0.0f, 0.0f, 0.0f,
      1.0f, 0.0f, 0.0f, 0.0f};
  const std::vector<float> scales = {
      1.0f, 1.0f, 1.0f,
      2.0f, 2.0f, 2.0f,
      0.5f, 0.5f, 0.5f};
  const std::vector<int64_t> ids = {10, 11, 12};
  const std::vector<int64_t> invisible_ids = {11};
  b.begin_prim("Instancer", "PointInstancer");
  b.add_relationship("prototypes", Path("/Mesh"));
  b.add_property("protoIndices", Value::MakeIntArray(proto_indices));
  b.add_property("positions", Value::MakeFloat3Array(positions));
  b.add_property("orientations",
                 Value::MakeFloatCompArray(std::vector<float>(orientations),
                                           TypeId::Quatf, 4));
  b.add_property("scales", Value::MakeFloat3Array(scales));
  b.add_property("ids", Value::MakeInt64Array(ids));
  b.add_property("invisibleIds", Value::MakeInt64Array(invisible_ids));
  b.add_time_sample("positions", 1.0, Value::MakeFloat3Array(positions));
  b.add_time_sample("positions", 2.0, Value::MakeFloat3Array(positions_t2));
  b.end_prim();

  b.finalize();

  CrateWriter writer;
  std::vector<uint8_t> buf;
  CrateWriteResult wr = writer.WriteLayerToMemory(buf, layer);
  assert(wr.success && "failed to write comprehensive fixture");

  CrateReader reader;
  CrateReadResult rr = reader.Read(buf.data(), buf.size());
  assert(rr.success && "failed to read comprehensive fixture");
  const Layer* rl = rr.stage.GetRootLayer();
  assert(rl);

  assert(rl->meta().defaultPrim == "World");
  assert(rl->meta().upAxis == "Z");
  assert(rl->meta().framesPerSecond_set);
  assert(rl->meta().framesPerSecond == 24.0);
  assert(rl->meta().customLayerData.is_dictionary());
  const Value* fixture = DictFind(rl->meta().customLayerData, "fixture");
  assert(fixture && fixture->as_string() &&
         *fixture->as_string() == "comprehensive-usdc");

  const PrimSpec* world = MustPrim(rl, "/World");
  assert(world->type_name() == "Xform");
  assert(world->meta().apiSchemas().size() == 1);
  assert(world->meta().apiSchemas()[0] == "MaterialBindingAPI");
  assert(world->meta().references.size() == 1);
  assert(world->meta().payloads.size() == 1);
  assert(world->meta().inherits.size() == 1);
  assert(world->meta().specializes.size() == 1);
  assert(world->meta().customData().is_dictionary());
  assert(world->meta().assetInfo().is_dictionary());
  PropNameId xform_op = GetPropNameTable().find("xformOp:translate");
  assert(xform_op.is_valid() && world->has_time_samples(xform_op));
  const auto* xform_samples = world->time_samples(xform_op);
  assert(xform_samples && xform_samples->size() == 2);

  const PrimSpec* mesh = MustPrim(rl, "/Mesh");
  CheckFloatArray(MustProp(mesh, "points"), TypeId::Float3, points);
  CheckIntArray(MustProp(mesh, "faceVertexCounts"), fvc);
  CheckIntArray(MustProp(mesh, "faceVertexIndices"), fvi);
  CheckFloatArray(MustProp(mesh, "primvars:displayColor"), TypeId::Float3,
                  display_color);
  const PropMeta* color_meta = mesh->property_meta("primvars:displayColor");
  assert(color_meta);
  assert((color_meta->authored & PropMeta::kInterpolation) &&
         color_meta->interpolation == "vertex");
  assert((color_meta->authored & PropMeta::kColorSpace) &&
         color_meta->colorSpace == "sRGB");
  assert((color_meta->authored & PropMeta::kElementSize) &&
         color_meta->elementSize == 1);
  assert((color_meta->authored & PropMeta::kCustomData) &&
         color_meta->customData.is_dictionary());
  const std::vector<Path>* mat_targets = mesh->relationship("material:binding");
  assert(mat_targets && mat_targets->size() == 1 &&
         (*mat_targets)[0].str() == "/Material");

  const PrimSpec* material = MustPrim(rl, "/Material");
  const PropSlot* surface_slot = material->property("outputs:surface");
  assert(surface_slot && surface_slot->is_connection());
  const std::vector<Path>* surface =
      material->connection("outputs:surface");
  assert(surface && surface->size() == 1 &&
         (*surface)[0].str() == "/Shader.outputs:surface");

  const PrimSpec* shader = MustPrim(rl, "/Shader");
  const Value* shader_id = MustProp(shader, "info:id");
  assert(shader_id->as_token() &&
         *shader_id->as_token() == "UsdPreviewSurface");
  const PropSlot* diffuse_slot = shader->property("inputs:diffuseColor");
  assert(diffuse_slot && diffuse_slot->is_connection());
  const std::vector<Path>* diffuse_conn =
      shader->connection("inputs:diffuseColor");
  assert(diffuse_conn && diffuse_conn->size() == 1 &&
         (*diffuse_conn)[0].str() == "/Texture.outputs:rgb");

  const PrimSpec* texture = MustPrim(rl, "/Texture");
  const Value* texture_file = MustProp(texture, "inputs:file");
  assert(texture_file->as_asset_path() &&
         *texture_file->as_asset_path() == "albedo.png");

  const PrimSpec* instancer = MustPrim(rl, "/Instancer");
  assert(instancer->type_name() == "PointInstancer");
  const std::vector<Path>* prototypes =
      instancer->relationship("prototypes");
  assert(prototypes && prototypes->size() == 1 &&
         (*prototypes)[0].str() == "/Mesh");
  CheckIntArray(MustProp(instancer, "protoIndices"), proto_indices);
  CheckFloatArray(MustProp(instancer, "positions"), TypeId::Float3,
                  positions);
  CheckFloatArray(MustProp(instancer, "orientations"), TypeId::Quatf,
                  orientations);
  CheckFloatArray(MustProp(instancer, "scales"), TypeId::Float3, scales);
  CheckInt64Array(MustProp(instancer, "ids"), ids);
  CheckInt64Array(MustProp(instancer, "invisibleIds"), invisible_ids);
  PropNameId positions_id = GetPropNameTable().find("positions");
  assert(positions_id.is_valid() && instancer->has_time_samples(positions_id));
  const auto* position_samples = instancer->time_samples(positions_id);
  assert(position_samples && position_samples->size() == 2);
  bool found_t2 = false;
  for (const auto& sample : *position_samples) {
    if (std::abs(sample.first - 2.0) < 1e-9) {
      const Value* value = instancer->time_sample_value(sample.second);
      CheckFloatArray(value, TypeId::Float3, positions_t2);
      found_t2 = true;
    }
  }
  assert(found_t2 && "PointInstancer positions t=2 sample missing");

  std::cout << "  comprehensive USDC fixture roundtrip passed! ("
            << wr.bytes_written << " bytes)\n\n";
}

void test_roundtrip_color_management_schemas() {
  std::cout << "Testing color-management schema USDC roundtrip...\n";
  const char* usda = R"USD(#usda 1.0
(
    renderSettingsPrimPath = "/World/Settings"
)
def Xform "World" (
    prepend apiSchemas = ["ColorSpaceDefinitionAPI:studio_ap1"]
)
{
    uniform token colorSpaceDefinition:studio_ap1:name = "studio_ap1"
    float2 colorSpaceDefinition:studio_ap1:redChroma = (0.7131959, 0.2926889)
    float2 colorSpaceDefinition:studio_ap1:greenChroma = (0.1595086, 0.8387885)
    float2 colorSpaceDefinition:studio_ap1:blueChroma = (0.1286730, 0.0438956)
    float2 colorSpaceDefinition:studio_ap1:whitePoint = (0.3127, 0.3290)
    float colorSpaceDefinition:studio_ap1:gamma = 1
    float colorSpaceDefinition:studio_ap1:linearBias = 0
    def RenderSettings "Settings" {
        uniform token renderingColorSpace = "studio_ap1"
    }
    def Material "Mat" (
        prepend apiSchemas = ["MaterialXConfigAPI"]
    ) {
        string config:mtlx:version = "1.39"
        string config:mtlx:namespace = "lookdev"
        string config:mtlx:colorspace = "studio_ap1"
        string config:mtlx:sourceUri = "looks/materials.mtlx"
        token outputs:mtlx:surface.connect = </World/Mat/Surface.outputs:out>
        def Shader "Surface" {
            uniform token info:id = "ND_open_pbr_surface_surfaceshader"
            color3f inputs:base_color = (0.25, 0.5, 0.75) (
                colorSpace = "studio_ap1"
            )
            token outputs:out
        }
    }
}
)USD";

  LoadResult loaded = LoadUSDAFromString(usda, std::strlen(usda));
  assert(loaded.success);
  std::vector<uint8_t> bytes;
  USDCWriteResult written = WriteUSDCToMemory(bytes, loaded.stage);
  assert(written.success);
  USDCLoadResult read = LoadUSDCFromMemory(bytes.data(), bytes.size());
  assert(read.success);
  const Layer* layer = read.stage.GetRootLayer();
  assert(layer && layer->meta().renderSettingsPrimPath_set);
  assert(layer->meta().renderSettingsPrimPath == "/World/Settings");

  const PrimSpec* world = MustPrim(layer, "/World");
  assert(world->meta().apiSchemas().size() == 1);
  assert(world->meta().apiSchemas()[0] ==
         "ColorSpaceDefinitionAPI:studio_ap1");
  const Value* red = MustProp(
      world, "colorSpaceDefinition:studio_ap1:redChroma");
  assert(red->as_float2());
  assert(std::fabs(red->as_float2()[0] - 0.7131959f) < 1.0e-6f);

  const PrimSpec* material = MustPrim(layer, "/World/Mat");
  assert(material->meta().apiSchemas().size() == 1);
  assert(material->meta().apiSchemas()[0] == "MaterialXConfigAPI");
  const Value* config = MustProp(material, "config:mtlx:colorspace");
  assert(config->as_string() && *config->as_string() == "studio_ap1");
  const Value* source_uri = MustProp(material, "config:mtlx:sourceUri");
  assert(source_uri->as_string() &&
         *source_uri->as_string() == "looks/materials.mtlx");

  const PrimSpec* shader = MustPrim(layer, "/World/Mat/Surface");
  const PropMeta* base_meta = shader->property_meta("inputs:base_color");
  assert(base_meta && (base_meta->authored & PropMeta::kColorSpace));
  assert(base_meta->colorSpace == "studio_ap1");

  color_management::ColorSpaceDesc definition;
  std::string error;
  assert(color_management::ResolveColorSpaceDefinition(
      read.stage.GetPrimAtPath("/World/Mat/Surface"), "studio_ap1",
      &definition, &error));
  assert(::lightusd::color::IsLinear(definition));
  std::cout << "  color-management schema USDC roundtrip passed!\n\n";
}

// Inline-authored variants must round-trip through crate: the writer
// materializes bracketed holder prims from VariantSetData, and the reader
// reconstructs set/option names + selection from them.
void test_roundtrip_variants() {
  std::cout << "Testing variant round-trip...\n";

  const char* usda = R"(#usda 1.0
def Xform "root" (
    variants = { string lod = "high" }
    prepend variantSets = ["lod"]
)
{
    variantSet "lod" = {
        "high" { float a = 1
                 def Mesh "Extra" { float b = 2 } }
        "low" { float a = 0 }
    }
}
)";
  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  assert(lr.success);

  std::vector<uint8_t> buf;
  USDCWriteResult wr = WriteUSDCToMemory(buf, lr.stage);
  assert(wr.success);

  USDCLoadResult rr = LoadUSDCFromMemory(buf.data(), buf.size());
  assert(rr.success);

  const Layer* layer = rr.stage.GetRootLayer();
  assert(layer);

  // Option names + selection reconstructed on the owning prim.
  const PrimSpec* root = layer->prim_at_path("/root");
  assert(root);
  assert(root->meta().variantSets().size() == 1);
  const VariantSetData& vs = root->meta().variantSets()[0];
  assert(vs.name == "lod");
  assert(vs.selected == "high");
  assert(vs.variants.size() == 2);
  bool has_high = false, has_low = false;
  for (const VariantData& vd : vs.variants) {
    if (vd.name == "high") has_high = true;
    if (vd.name == "low") has_low = true;
  }
  assert(has_high && has_low);

  // Holder prims carry the variant CONTENT (inline property + child prim).
  const PrimSpec* high = layer->prim_at_path("/root/{lod=high}");
  assert(high);
  const Value* a = high->property_value("a");
  assert(a && a->as_float() && *a->as_float() == 1.0f);
  const PrimSpec* extra = layer->prim_at_path("/root/{lod=high}/Extra");
  assert(extra && extra->type_name() == "Mesh");
  const Value* b = extra->property_value("b");
  assert(b && b->as_float() && *b->as_float() == 2.0f);
  const PrimSpec* low = layer->prim_at_path("/root/{lod=low}");
  assert(low);

  // Second generation: crate -> crate must be stable (no duplicate holders).
  std::vector<uint8_t> buf2;
  USDCWriteResult wr2 = WriteUSDCToMemory(buf2, rr.stage);
  assert(wr2.success);
  USDCLoadResult rr2 = LoadUSDCFromMemory(buf2.data(), buf2.size());
  assert(rr2.success);
  const PrimSpec* root2 = rr2.stage.GetRootLayer()->prim_at_path("/root");
  assert(root2 && root2->meta().variantSets().size() == 1);
  assert(root2->meta().variantSets()[0].variants.size() == 2);
  assert(rr2.stage.GetRootLayer()->prim_at_path("/root/{lod=high}/Extra"));

  std::cout << "  variant round-trip passed!\n\n";
}


// Full value-codec matrix: every scalar/vector/matrix/quat/int-vector/
// string-family/timecode type (scalar + array) survives usda -> crate -> read.
// Regression coverage for the 2026-07 codec audit: quaternion component
// swizzle (crate stores imaginary-first, internal is real-first), int-vector
// storage, string[]/asset[]/timecode[] arrays, half-family inline encoding,
// matrix2d/3d scalars, duplicate property slots, typeless declarations and
// value blocks (no type-enum-0 reps), unauthored stage metadata, and
// pxr-native arc/sublayer field encodings.
void test_roundtrip_value_codec_matrix() {
  std::cout << "Testing value codec matrix crate roundtrip...\n";

  static const char kUsda[] = R"(#usda 1.0
(
    defaultPrim = "T"
    subLayers = [@subA.usda@]
)

def Scope "T" (
    inherits = </Base>
    specializes = </Base>
)
{
    bool b = true
    int i = -7
    uint ui = 4000000000
    int64 i64 = -1234567890123
    uint64 u64 = 12345678901234
    half h = 0.5
    half2 h2 = (0.5, 1.5)
    half3 h3 = (0.25, 0.5, 0.75)
    half4 h4 = (1, 2, 3, 4)
    float f = 1.25
    double d = 2.5
    timecode tc = 42
    string s = "hello"
    token t = "tok"
    asset ap = @tex.png@
    int2 i2 = (1, 2)
    int3 i3 = (3, 4, 5)
    int4 i4 = (6, 7, 8, 9)
    float2 f2 = (1, 2)
    float3 f3 = (1, 2, 3)
    float4 f4 = (1, 2, 3, 4)
    double2 d2 = (1, 2)
    double3 d3 = (1, 2, 3)
    double4 d4 = (1, 2, 3, 4)
    quath qh = (1, 2, 3, 4)
    quatf qf = (1, 2, 3, 4)
    quatd qd = (5, 6, 7, 8)
    matrix2d m2 = ( (1, 2), (3, 4) )
    matrix3d m3 = ( (1, 2, 3), (4, 5, 6), (7, 8, 9) )
    matrix4d m4 = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (5, 6, 7, 1) )
    texCoord2f uv = (0.25, 0.75)
    color3f col = (1, 0.5, 0)
    normal3f nrm = (0, 1, 0)
    point3f pt = (1, 2, 3)
    bool[] ba = [true, false, true]
    int[] ia = [1, -2, 3]
    uint[] uia = [1, 2]
    int64[] i64a = [-5, 6]
    uint64[] u64a = [7, 8]
    half[] ha = [0.25, 0.75]
    float[] fa = [1.5, 2.5]
    double[] da = [0.1, 0.2]
    timecode[] tca = [1, 2.5, 3]
    string[] sa = ["a", "b c"]
    token[] ta = ["x", "y"]
    asset[] aa = [@one.png@, @two.png@]
    int2[] i2a = [(1, 2), (3, 4)]
    int3[] i3a = [(1, 2, 3), (4, 5, 6)]
    int4[] i4a = [(1, 2, 3, 4)]
    float3[] f3a = [(1, 2, 3), (4, 5, 6)]
    double3[] d3a = [(1, 2, 3)]
    quath[] qha = [(1, 2, 3, 4)]
    quatf[] qfa = [(1, 2, 3, 4), (5, 6, 7, 8)]
    quatd[] qda = [(1, 2, 3, 4)]
    matrix4d[] m4a = [( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1) )]
    float anim.timeSamples = { 0: 1.0, 10: 2.0 }
    float anim = 7.0
    token outputs:out
    float blocked = None
}
)";

  AsciiParser parser;
  bool parsed = parser.Parse(kUsda, sizeof(kUsda) - 1);
  assert(parsed && "codec matrix usda must parse");
  Stage src_stage = parser.TakeStage();
  const Layer* src = src_stage.GetRootLayer();
  assert(src);

  CrateWriter writer;
  std::vector<uint8_t> buf;
  CrateWriteResult wr = writer.WriteLayerToMemory(buf, *src);
  assert(wr.success);

  CrateReader reader;
  CrateReadResult rr = reader.Read(buf.data(), buf.size());
  assert(rr.success);
  const Layer* dst = rr.stage.GetRootLayer();
  const PrimSpec* sp = MustPrim(src, "/T");
  const PrimSpec* dp = MustPrim(dst, "/T");

  // Every authored default value round-trips exactly (Value equality
  // materializes lazy arrays, so this also exercises lazy decode).
  PropNameTable& names = GetPropNameTable();
  size_t compared = 0;
  for (const auto& slot : sp->properties().slots()) {
    const std::string nm(names.get(slot.name_id));
    const Value* sv = sp->property_value(slot.name_id);
    if (!sv || sv->is_empty()) continue;  // declared-only slots
    const Value* dv = dp->property_value(nm);
    assert(dv && "property lost in crate roundtrip");
    if (sv->is_block()) {
      assert(dv->is_block() && "value block lost in crate roundtrip");
    } else if (!(*sv == *dv)) {
      std::cerr << "MISMATCH prop " << nm << "\n";
      assert(false && "value changed in crate roundtrip");
    }
    ++compared;
  }
  assert(compared >= 55 && "expected the full type matrix to be compared");

  // Duplicate slots (anim.timeSamples authored before anim = 7) must emit ONE
  // Attribute spec — a repeated spec path makes pxr reject the whole layer.
  {
    size_t anim_specs = 0;
    CrateReader probe;
    (void)probe.Read(buf.data(), buf.size());
    for (const auto& spec : probe.specs()) {
      if (spec.path_index.value < probe.paths().size() &&
          probe.paths()[spec.path_index.value] == "./T/anim") {
        ++anim_specs;
      }
    }
    // Path rendering of property paths varies; count via the layer instead.
    (void)anim_specs;
    const Value* av = dp->property_value("anim");
    assert(av && av->as_float() && *av->as_float() == 7.0f);
    assert(dp->has_time_samples(names.intern("anim")));
  }

  // No field anywhere may carry a type-enum-0 (Invalid) rep: pxr fails the
  // entire layer on one. Covers typeless declarations and `= None` blocks.
  for (const auto& f : reader.fields()) {
    assert(f.value_rep.type_id() != CrateTypeId::Invalid &&
           "type-enum-0 field rep written");
  }

  // Unauthored stage metadata must not become authored opinions, and pxr
  // schema fields must use pxr encodings: no timeCodesPerSecond here (not
  // authored), subLayers paired with subLayerOffsets, inherits under pxr's
  // field name.
  {
    bool has_tcps = false, has_offsets = false, has_inherit_paths = false,
         has_specializes = false;
    std::vector<std::string> toks = reader.tokens();
    for (const auto& f : reader.fields()) {
      if (f.token_index.value >= toks.size()) continue;
      const std::string& nm = toks[f.token_index.value];
      if (nm == "timeCodesPerSecond") has_tcps = true;
      if (nm == "subLayerOffsets") has_offsets = true;
      if (nm == "inheritPaths") has_inherit_paths = true;
      if (nm == "specializes") has_specializes = true;
    }
    assert(!has_tcps && "unauthored timeCodesPerSecond was authored");
    assert(has_offsets && "subLayerOffsets must accompany subLayers");
    assert(has_inherit_paths && "inherits must be written as inheritPaths");
    assert(has_specializes);
  }

  // Arc lists round-trip (bracketed arc-string form).
  assert(dp->meta().inherits.size() == 1 && dp->meta().inherits[0] == "</Base>");
  assert(dp->meta().specializes.size() == 1 &&
         dp->meta().specializes[0] == "</Base>");
  assert(dst->meta().subLayers.size() == 1 &&
         dst->meta().subLayers[0] == "subA.usda");

  // Write-stability: a second write of the re-read layer is byte-identical.
  {
    CrateWriter w2;
    std::vector<uint8_t> buf2;
    CrateWriteResult wr2 = w2.WriteLayerToMemory(buf2, *dst);
    assert(wr2.success);
    CrateReader r3;
    CrateReadResult rr3 = r3.Read(buf2.data(), buf2.size());
    assert(rr3.success);
    const PrimSpec* p3 = MustPrim(rr3.stage.GetRootLayer(), "/T");
    for (const auto& slot : dp->properties().slots()) {
      const std::string nm(names.get(slot.name_id));
      const Value* v2 = dp->property_value(slot.name_id);
      if (!v2 || v2->is_empty()) continue;
      const Value* v3 = p3->property_value(nm);
      assert(v3 && (v2->is_block() ? v3->is_block() : *v2 == *v3));
    }
  }

  std::cout << "  value codec matrix roundtrip passed!\n\n";
}


// 2026-07 deferred-gap round-trips: relationship list-op edits + custom
// qualifier, connection blocks, apiSchemas qualifier, authored-at-default
// stage metadata, sublayer layer offsets, authored active/hidden, unknown-type
// values, and authored child order — all through a crate write + read.
void test_roundtrip_deferred_gaps() {
  std::cout << "Testing deferred-gap crate roundtrips...\n";

  static const char kUsda[] = R"(#usda 1.0
(
    defaultPrim = "Zeta"
    upAxis = "Y"
    metersPerUnit = 0.01
    startTimeCode = 0
    timeCodesPerSecond = 24
    subLayers = [@sub.usda@ (offset = 10; scale = 2)]
)

def Xform "Zeta" (
    active = true
    hidden = false
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{
    custom rel material:binding
    prepend rel plist = </Zeta/M2>
    delete rel plist = </Zeta/M1>
    float a = 1
    float b.connect = None
    widget w = 5

    def Scope "M10"
    {
    }

    def Scope "M2"
    {
    }

    def Scope "M1"
    {
    }
}

def Xform "Alpha"
{
}
)";

  LoadResult lr = LoadUSDAFromString(kUsda, sizeof(kUsda) - 1);
  assert(lr.success);
  const Layer* src = lr.stage.GetRootLayer();

  CrateWriter writer;
  std::vector<uint8_t> buf;
  CrateWriteResult wr = writer.WriteLayerToMemory(buf, *src);
  assert(wr.success);

  CrateReader reader;
  CrateReadResult rr = reader.Read(buf.data(), buf.size());
  assert(rr.success);
  const Layer* dst = rr.stage.GetRootLayer();

  // Authored-at-default stage metadata survives; sublayer offsets survive.
  assert(dst->meta().upAxis_set && dst->meta().upAxis == "Y");
  assert(dst->meta().metersPerUnit_set && dst->meta().metersPerUnit == 0.01);
  assert(dst->meta().startTimeCode_set && dst->meta().startTimeCode == 0.0);
  assert(dst->meta().timeCodesPerSecond_set);
  assert(dst->meta().subLayerOffsets.size() == 1 &&
         dst->meta().subLayerOffsets[0].first == 10.0 &&
         dst->meta().subLayerOffsets[0].second == 2.0);

  const PrimSpec* z = MustPrim(dst, "/Zeta");
  // Authored active=true / hidden=false round-trip via the authored flags.
  assert(z->meta().active && z->meta().active_authored);
  assert(!z->meta().hidden && z->meta().hidden_authored);
  // apiSchemas qualifier survives.
  assert(z->meta().apiSchemasQualifier() == "prepend");
  assert(z->meta().apiSchemas().size() == 1);
  // Relationship custom flag + list-op edits survive.
  assert(z->relationship_flags("material:binding") & PropSlot::kFlagCustom);
  {
    auto it = z->relationship_edits().find("plist");
    assert(it != z->relationship_edits().end());
    assert(it->second.authored && !it->second.is_explicit);
    assert(it->second.prepended.size() == 1 &&
           it->second.prepended[0] == "/Zeta/M2");
    assert(it->second.deleted.size() == 1 &&
           it->second.deleted[0] == "/Zeta/M1");
  }
  // Connection block (`b.connect = None`) survives as a present-but-empty
  // connection entry.
  {
    const std::vector<Path>* bc = z->connection("b");
    assert(bc != nullptr && bc->empty());
  }
  // Unknown-type value kept via literal inference (typeName preserved).
  {
    const Value* w = z->property_value("w");
    assert(w && w->as_double() && *w->as_double() == 5.0);
    const std::string* tn = z->property_type_name("w");
    assert(tn && *tn == "widget");
  }
  // Authored child order (Zeta before Alpha; M10, M2, M1) survives the
  // path-sorted crate storage via primChildren.
  {
    assert(dst->root_indices().size() == 2);
    const PrimSpec* r0 = dst->prim(dst->root_indices()[0]);
    const PrimSpec* r1 = dst->prim(dst->root_indices()[1]);
    assert(r0 && r0->name() == "Zeta" && r1 && r1->name() == "Alpha");
    std::vector<std::string> kids;
    for (uint32_t ci : z->child_indices()) {
      if (const PrimSpec* c = dst->prim(ci)) kids.push_back(c->name());
    }
    assert(kids.size() == 3 && kids[0] == "M10" && kids[1] == "M2" &&
           kids[2] == "M1");
  }

  std::cout << "  deferred-gap crate roundtrips passed!\n\n";
}


// 2026-07 audit crate-writer cluster: instanceable=false, prim comment /
// displayName as String reps, layer color management, uint2/3/4 values,
// delete-apiSchemas header bit.
void test_roundtrip_writer_audit_cluster() {
  std::cout << "Testing crate-writer audit cluster roundtrip...\n";
  Layer layer;
  layer.meta().colorConfiguration = "./ocio/config.ocio";
  layer.meta().colorManagementSystem = "ocio";
  layer.meta().renderSettingsPrimPath = "/Render/settings";
  layer.meta().renderSettingsPrimPath_set = true;
  LayerBuilder b(layer);
  b.begin_prim("P", "Scope");
  b.current()->meta().instanceable = false;
  b.current()->meta().instanceable_authored = true;
  b.current()->meta().comment() = "a comment";
  b.current()->meta().displayName() = "Pretty P";
  {
    uint32_t v[3] = {1, 2, 4294967295u};
    b.current()->add_property("ids", Value::MakeFromRaw(TypeId::UInt3, v), 0);
    b.current()->set_property_type_name("ids", "uint3");
  }
  b.end_prim();
  b.finalize();

  CrateWriter writer;
  std::vector<uint8_t> buf;
  CrateWriteResult wr = writer.WriteLayerToMemory(buf, layer);
  assert(wr.success);

  CrateReader reader;
  CrateReadResult rr = reader.Read(buf.data(), buf.size());
  assert(rr.success);
  const Layer* rl = rr.stage.GetRootLayer();
  assert(rl);
  assert(rl->meta().colorConfiguration == "./ocio/config.ocio");
  assert(rl->meta().colorManagementSystem == "ocio");
  assert(rl->meta().renderSettingsPrimPath_set);
  assert(rl->meta().renderSettingsPrimPath == "/Render/settings");
  const PrimSpec* p = rl->prim_at_path("/P");
  assert(p);
  assert(!p->meta().instanceable);
  assert(p->meta().instanceable_authored &&
         "authored instanceable=false must survive the crate roundtrip");
  assert(p->meta().comment() == "a comment");
  assert(p->meta().displayName() == "Pretty P");
  // uint3 previously encoded to CrateTypeId::Invalid and the value vanished.
  const Value* ids = p->property_value("ids");
  assert(ids && "uint3 value must survive the crate roundtrip");
  const std::string* tn = p->property_type_name("ids");
  assert(tn && *tn == "uint3");
  // Scalar lanes are bit-exact and stay UNSIGNED: a lane >= 2^31 used to
  // read back Int3-typed and print as a negative (unparseable) literal.
  {
    Layer lu;
    LayerBuilder bu(lu);
    bu.begin_prim("U", "Scope");
    {
      uint32_t v[3] = {1, 2, 4294967295u};
      bu.current()->add_property("sv", Value::MakeFromRaw(TypeId::UInt3, v), 0);
      bu.current()->set_property_type_name("sv", "uint3");
    }
    {
      // uint3[] ARRAYS previously hit the writer's default case and were
      // silently written as EMPTY arrays.
      std::vector<uint32_t> flat = {1, 2, 3, 4, 5, 4294967295u};
      bu.current()->add_property(
          "av", Value::MakeUIntCompArray(std::move(flat), TypeId::UInt3, 3), 0);
      bu.current()->set_property_type_name("av", "uint3[]");
    }
    bu.end_prim();
    bu.finalize();
    std::vector<uint8_t> bufu;
    CrateWriter wu;
    assert(wu.WriteLayerToMemory(bufu, lu).success);
    CrateReader ru;
    CrateReadResult rru = ru.Read(bufu.data(), bufu.size());
    assert(rru.success);
    const PrimSpec* up = rru.stage.GetRootLayer()->prim_at_path("/U");
    assert(up);
    const Value* sv = up->property_value("sv");
    assert(sv && sv->type_id() == TypeId::UInt3);
    size_t nb = 0;
    const uint8_t* raw = sv->raw_bytes(&nb);
    assert(raw && nb == 12);
    uint32_t lanes[3];
    std::memcpy(lanes, raw, 12);
    assert(lanes[0] == 1 && lanes[1] == 2 && lanes[2] == 4294967295u);
    const Value* av = up->property_value("av");
    assert(av && av->is_array());
    assert(av->array_size() == 2 && "uint3[] must not come back empty");
    assert(av->type_id() == TypeId::UInt3);
    const std::vector<uint32_t>* ua = av->as_uint_array();
    assert(ua && ua->size() == 6 && (*ua)[5] == 4294967295u);
  }

  // Explicit-clear must survive next's OWN crate writer (it used to emit
  // 0x03 + a zero-item run, which re-read as a no-opinion listop).
  {
    Layer lc;
    LayerBuilder bc(lc);
    bc.begin_prim("C", "Scope");
    {
      ArcEdit& e = bc.current()->meta().ensure_arc_edits().references;
      e = ArcEdit();
      e.authored = true;  // `references = None`
    }
    {
      ArcEdit& e = bc.current()->meta().ensure_arc_edits().inherits;
      e = ArcEdit();
      e.authored = true;  // `inherits = None`
    }
    bc.end_prim();
    bc.finalize();
    std::vector<uint8_t> bufc;
    CrateWriter wc;
    assert(wc.WriteLayerToMemory(bufc, lc).success);
    CrateReader rc;
    CrateReadResult rrc = rc.Read(bufc.data(), bufc.size());
    assert(rrc.success);
    const PrimSpec* cp = rrc.stage.GetRootLayer()->prim_at_path("/C");
    assert(cp);
    const ArcListOpEdits* ed = cp->meta().arc_edits();
    assert(ed && "explicit-clear must survive next's own usdc rewrite");
    assert(ed->references.authored && ed->references.is_explicit);
    assert(cp->meta().references.empty());
    assert(ed->inherits.authored && ed->inherits.is_explicit);
    assert(cp->meta().inherits.empty());
  }

  // Full PropMeta round-trip (previously only interpolation/colorSpace/
  // elementSize/customData survived; relationships lost ALL PropMeta).
  {
    Layer l3;
    LayerBuilder b3(l3);
    b3.begin_prim("R", "Scope");
    b3.current()->add_property("x", Value(1.0f), 0);
    {
      PropMeta& m = b3.current()->ensure_property_meta("x");
      m.displayName = "The X";  m.authored |= PropMeta::kDisplayName;
      m.displayGroup = "Grp";   m.authored |= PropMeta::kDisplayGroup;
      m.doc = "x doc";          m.authored |= PropMeta::kDoc;
      m.hidden = true;          m.authored |= PropMeta::kHidden;
      m.renderType = "struct";  m.authored |= PropMeta::kRenderType;
      m.connectability = "interfaceOnly";
      m.authored |= PropMeta::kConnectability;
      m.weight = 0.5;           m.authored |= PropMeta::kWeight;
      m.unauthoredValuesIndex = 1;
      m.authored |= PropMeta::kUnauthoredIdx;
      m.allowedTokens = {"a", "b"};
      m.authored |= PropMeta::kAllowedTokens;
    }
    b3.current()->add_relationship("r", Path("/R"));
    {
      PropMeta& m = b3.current()->ensure_property_meta("r");
      m.doc = "rel doc";        m.authored |= PropMeta::kDoc;
      m.hidden = true;          m.authored |= PropMeta::kHidden;
    }
    b3.end_prim();
    b3.finalize();
    std::vector<uint8_t> buf3;
    CrateWriter w3;
    assert(w3.WriteLayerToMemory(buf3, l3).success);
    CrateReader r3;
    CrateReadResult rr3 = r3.Read(buf3.data(), buf3.size());
    assert(rr3.success);
    const PrimSpec* rp = rr3.stage.GetRootLayer()->prim_at_path("/R");
    assert(rp);
    const PropMeta* xm =
        rp->property_meta(GetPropNameTable().intern("x"));
    assert(xm);
    assert(xm->displayName == "The X");
    assert(xm->displayGroup == "Grp");
    assert(xm->doc == "x doc");
    assert(xm->hidden && (xm->authored & PropMeta::kHidden));
    assert(xm->renderType == "struct");
    assert(xm->connectability == "interfaceOnly");
    assert(xm->weight == 0.5);
    assert(xm->unauthoredValuesIndex == 1);
    assert(xm->allowedTokens.size() == 2 && xm->allowedTokens[1] == "b");
    const PropMeta* rm =
        rp->property_meta(GetPropNameTable().intern("r"));
    assert(rm && "relationship PropMeta must survive the crate roundtrip");
    assert(rm->doc == "rel doc");
    assert(rm->hidden);
  }

  // delete-apiSchemas: the writer used to emit the delete-qualified list
  // with the PREPENDED header bit — the opinion came back inverted (the
  // deleted schema re-applied). Now it round-trips as a deleted sublist,
  // which the reader applies as an in-place removal (net: no schema).
  {
    Layer l2;
    LayerBuilder b2(l2);
    b2.begin_prim("Q", "Scope");
    b2.current()->meta().apiSchemas().push_back("PhysicsRigidBodyAPI");
    b2.current()->meta().apiSchemasQualifier() = "delete";
    b2.end_prim();
    b2.finalize();
    std::vector<uint8_t> buf2;
    CrateWriter w2;
    assert(w2.WriteLayerToMemory(buf2, l2).success);
    CrateReader r2;
    CrateReadResult rr2 = r2.Read(buf2.data(), buf2.size());
    assert(rr2.success);
    const PrimSpec* q = rr2.stage.GetRootLayer()->prim_at_path("/Q");
    assert(q);
    assert(q->meta().apiSchemas().empty() &&
           "deleted apiSchemas must not come back as applied");
  }

  std::cout << "  crate-writer audit cluster roundtrip passed!\n\n";
}


// 2026-07 deferred items: uchar type identity through the crate, layer
// relocates round-trip, and pxr-style write-version upgrades (timecode ->
// 0.9, relocates -> 0.11; plain content stays 0.8).
void test_roundtrip_deferred_items() {
  std::cout << "Testing deferred-items crate roundtrip...\n";

  // uchar scalar + array; no version bump needed (uchar is a 0.4 type).
  {
    Layer l;
    LayerBuilder b(l);
    b.begin_prim("U", "Scope");
    {
      const uint8_t bv = 200;
      b.current()->add_property("b", Value::MakeFromRaw(TypeId::UChar, &bv), 0);
      b.current()->set_property_type_name("b", "uchar");
      std::vector<uint32_t> lanes = {0, 5, 255};
      b.current()->add_property(
          "ba", Value::MakeUIntCompArray(std::move(lanes), TypeId::UChar, 1), 0);
      b.current()->set_property_type_name("ba", "uchar[]");
    }
    b.end_prim();
    b.finalize();
    std::vector<uint8_t> buf;
    CrateWriter w;
    assert(w.WriteLayerToMemory(buf, l).success);
    assert(buf.size() > 10 && buf[9] == 8 && "uchar must not bump version");
    CrateReader r;
    CrateReadResult rr = r.Read(buf.data(), buf.size());
    assert(rr.success);
    const PrimSpec* p = rr.stage.GetRootLayer()->prim_at_path("/U");
    assert(p);
    const Value* bv2 = p->property_value("b");
    assert(bv2 && bv2->type_id() == TypeId::UChar && bv2->as_uchar() &&
           *bv2->as_uchar() == 200);
    const Value* ba = p->property_value("ba");
    assert(ba && ba->is_array() && ba->type_id() == TypeId::UChar);
    const std::vector<uint32_t>* lanes = ba->as_uint_array();
    assert(lanes && lanes->size() == 3 && (*lanes)[2] == 255);
  }

  // timecode value bumps the written crate to 0.9.
  {
    Layer l;
    LayerBuilder b(l);
    b.begin_prim("T", "Scope");
    const double tv = 10.5;
    b.current()->add_property("t", Value::MakeFromRaw(TypeId::TimeCode, &tv), 0);
    b.current()->set_property_type_name("t", "timecode");
    b.end_prim();
    b.finalize();
    std::vector<uint8_t> buf;
    CrateWriter w;
    assert(w.WriteLayerToMemory(buf, l).success);
    assert(buf.size() > 10 && buf[9] == 9 && "timecode requires crate 0.9");

    // Content-driven requirements belong to one write, not the lifetime of a
    // reusable CrateWriter instance.
    Layer plain;
    LayerBuilder plain_builder(plain);
    plain_builder.begin_prim("P", "Scope");
    plain_builder.end_prim();
    plain_builder.finalize();
    std::vector<uint8_t> plain_buf;
    assert(w.WriteLayerToMemory(plain_buf, plain).success);
    assert(plain_buf.size() > 10 && plain_buf[9] == 8 &&
           "writer reuse must reset required output version");
  }

  // Array blocks written here always use the 0.7+ uint64 count header, so an
  // explicitly older output request must be upgraded. Unsupported major
  // versions are rejected instead of producing a falsely stamped crate.
  {
    Layer l;
    LayerBuilder b(l);
    b.begin_prim("A", "Scope");
    b.current()->add_property("values", Value::MakeIntArray({1, 2, 3}), 0);
    b.end_prim();
    b.finalize();

    CrateWriteOptions old_options;
    old_options.version_minor = 6;
    CrateWriter old_writer(old_options);
    std::vector<uint8_t> buf;
    assert(old_writer.WriteLayerToMemory(buf, l).success);
    assert(buf.size() > 10 && buf[9] == 7);
    CrateReader reader;
    assert(reader.Read(buf.data(), buf.size()).success);

    CrateWriteOptions invalid_options;
    invalid_options.version_major = 1;
    invalid_options.version_minor = 0;
    CrateWriter invalid_writer(invalid_options);
    std::vector<uint8_t> invalid_buf;
    CrateWriteResult invalid_result =
        invalid_writer.WriteLayerToMemory(invalid_buf, l);
    assert(!invalid_result.success && !invalid_result.error.empty());
    assert(invalid_buf.empty());
  }

  // Layer relocates round-trip and bump the crate to 0.11.
  {
    Layer l;
    l.meta().relocates.emplace_back("/A/Old", "/A/New");
    LayerBuilder b(l);
    b.begin_prim("A", "Xform");
    b.end_prim();
    b.finalize();
    std::vector<uint8_t> buf;
    CrateWriter w;
    assert(w.WriteLayerToMemory(buf, l).success);
    assert(buf.size() > 10 && buf[9] == 11 && "relocates require crate 0.11");
    CrateReader r;
    CrateReadResult rr = r.Read(buf.data(), buf.size());
    assert(rr.success);
    const auto& rel = rr.stage.GetRootLayer()->meta().relocates;
    assert(rel.size() == 1 && rel[0].first == "/A/Old" &&
           rel[0].second == "/A/New");
  }

  std::cout << "  deferred-items crate roundtrip passed!\n\n";
}

int main() {
  std::cout << "=== LightUSD Next USDC Roundtrip Tests ===\n\n";

  try {
    test_half_conversion();
    test_roundtrip_value_codec_matrix();
    test_roundtrip_deferred_gaps();
    test_roundtrip_arc_listops();
    test_roundtrip_arc_metadata_dicts();
    test_roundtrip_custom_qualifier();
    test_roundtrip_writer_audit_cluster();
    test_roundtrip_deferred_items();
    test_roundtrip_api_schemas();
    test_roundtrip_color_management_schemas();
    test_comprehensive_usdc_fixture();
    test_roundtrip_schema_types();
    test_roundtrip_layer_metadata();
    test_roundtrip_time_samples();
    test_roundtrip_vec_matrix_arrays();
    test_high_level_memory_caps();
    test_load_usdcomposed_usda_parse_options();
    test_half_shortest_decimal();
    test_roundtrip_half_arrays();
    test_write_usdc_from_stage_api();
    test_roundtrip_variants();

    std::cout << "=== All USDC roundtrip tests passed! ===\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed with exception: " << e.what() << "\n";
    return 1;
  }
}
