// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Writer Test

#include <iostream>
#include <cstring>
#include <sstream>
#include <fstream>
#include <cassert>
#include <cstdio>
#include <iterator>
#include <limits>
#include <algorithm>
#include <vector>

#include "next/stage/stage.hh"
#include "next/layer/layer.hh"
#include "next/types/value.hh"
#include "next/reader/usdc-reader.hh"
#include "next/strfmt.hh"
#include "next/writer/value-printer.hh"
#include "next/writer/prim-printer.hh"
#include "next/writer/usda-writer.hh"
#include "next/reader/usda-reader.hh"
#include "next/writer/usdc-writer.hh"
#include "next/writer/dtoa.hh"

using namespace lightusd::next;

// Helper to check if string contains substring
bool contains(const std::string& str, const std::string& substr) {
  return str.find(substr) != std::string::npos;
}

void assert_stream_value_matches_string(const Value& value,
                                        const PrintOptions& opts = {}) {
  const std::string expected = PrintValue(value, opts);
  std::string actual;
  {
    StreamWriter writer(&actual);
    PrintValue(writer, value, opts);
  }
  assert(actual == expected);
}

void test_value_printer() {
  std::cout << "Testing value-printer...\n";

  // Test scalar values
  {
    Value v = Value(42);
    std::string s = PrintValue(v);
    assert(s == "42");
    std::cout << "  Int: " << s << "\n";
  }

  {
    Value v = Value(3.14159f);
    std::string s = PrintValue(v);
    assert(contains(s, "3.14159"));
    std::cout << "  Float: " << s << "\n";
  }

  {
    Value v = Value(true);
    std::string s = PrintValue(v);
    assert(s == "1");  // pxr spells VALUE bools 1/0
    std::cout << "  Bool: " << s << "\n";
  }

  // Test vector values
  {
    Value v = Value::MakeFloat3(1.0f, 2.0f, 3.0f);
    std::string s = PrintValue(v);
    assert(contains(s, "("));
    assert(contains(s, "1"));
    assert(contains(s, "2"));
    assert(contains(s, "3"));
    std::cout << "  Float3: " << s << "\n";
  }

  // Test string values
  {
    Value v = Value(std::string("hello world"));
    std::string s = PrintValue(v);
    assert(contains(s, "\"hello world\""));
    std::cout << "  String: " << s << "\n";
  }

  // Test arrays
  {
    std::vector<float> arr = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    Value v = Value::MakeFloatArray(arr);
    std::string s = PrintValue(v);
    assert(contains(s, "["));
    assert(contains(s, "1"));
    assert(contains(s, "5"));
    assert_stream_value_matches_string(v);
    std::cout << "  Float array: " << s << "\n";
  }

  {
    assert_stream_value_matches_string(Value::MakeIntArray({1, -2, 3}));
    assert_stream_value_matches_string(Value::MakeUIntArray({1u, 2u, 3u}));
    assert_stream_value_matches_string(Value::MakeInt64Array({1, -2, 3}));
    assert_stream_value_matches_string(Value::MakeUInt64Array({1, 2, 3}));
    assert_stream_value_matches_string(Value::MakeBoolArray({true, false, true}));
    assert_stream_value_matches_string(Value::MakeTokenArray({"a", "b", "c"}));
    assert_stream_value_matches_string(Value::MakeFloat3Array({1, 2, 3, 4, 5, 6}));
    assert_stream_value_matches_string(Value::MakeDoubleArray({1.25, 2.5, 3.75}));

    PrintOptions snip;
    snip.max_array_elements = 2;
    assert_stream_value_matches_string(Value::MakeIntArray({1, 2, 3, 4}), snip);
    assert_stream_value_matches_string(Value::MakeFloat3Array({
        1, 2, 3,
        4, 5, 6,
        7, 8, 9}), snip);
  }

  std::cout << "  value-printer tests passed!\n\n";
}

// Hot-path regression coverage for the per-element number formatting rewrite
// (dtos_to / IntTo / UIntTo + the num_-free ChunkedStream): stream-vs-string
// parity across every hot array element type, including edge-case formatting
// (-0.0, inf/nan, INT64_MIN, scientific crossover, large magnitudes).
void test_hot_array_formatting_parity() {
  std::cout << "Testing hot-array number formatting parity...\n";

  const float fedge[] = {
      0.0f, -0.0f, 1.0f, -1.0f, 3.14159265f, 1e-30f, 1e30f, 123456.789f,
      0.0001f, 1234567.0f, std::numeric_limits<float>::infinity(),
      -std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::max(), std::numeric_limits<float>::min()};
  const double dedge[] = {
      0.0, -0.0, 1.0, -1.0, 3.141592653589793, 1e-300, 1e300, 13.944,
      0.000123456789, 9876543210.123, std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::max(), std::numeric_limits<double>::min()};

  // Scalar float / double arrays.
  {
    std::vector<float> fa(std::begin(fedge), std::end(fedge));
    assert_stream_value_matches_string(Value::MakeFloatArray(fa));
    std::vector<double> da(std::begin(dedge), std::end(dedge));
    assert_stream_value_matches_string(Value::MakeDoubleArray(da));
  }

  // float3 / double-comp (vec3) tuples exercise the per-component path.
  {
    std::vector<float> f3 = {0.0f, -0.0f, 1e30f, 3.14f, -1.0f, 1e-30f};
    assert_stream_value_matches_string(Value::MakeFloat3Array(f3));
    std::vector<double> d3 = {0.0, -0.0, 1e300, 3.14, -1.0, 1e-300};
    assert_stream_value_matches_string(
        Value::MakeDoubleCompArray(std::move(d3), TypeId::Double3, 3));
  }

  // matrix4d (16-component) tuples exercise the nested matrix path.
  {
    std::vector<double> m(32);
    for (size_t i = 0; i < m.size(); ++i) m[i] = double(i) * 0.5 - 4.0;
    assert_stream_value_matches_string(
        Value::MakeDoubleCompArray(std::move(m), TypeId::Matrix4d, 16));
  }

  // Integer arrays incl. INT64_MIN and UINT64_MAX.
  {
    assert_stream_value_matches_string(Value::MakeIntArray(
        {0, 1, -1, 2147483647, -2147483647 - 1}));
    assert_stream_value_matches_string(Value::MakeInt64Array(
        {0, 1, -1, std::numeric_limits<int64_t>::min(),
         std::numeric_limits<int64_t>::max()}));
    assert_stream_value_matches_string(Value::MakeUIntArray(
        {0u, 1u, std::numeric_limits<uint32_t>::max()}));
    assert_stream_value_matches_string(Value::MakeUInt64Array(
        {0u, 1u, std::numeric_limits<uint64_t>::max()}));
  }

  // token / bool arrays.
  {
    assert_stream_value_matches_string(
        Value::MakeTokenArray({"a", "b c", "quote\"here", "back\\slash"}));
    assert_stream_value_matches_string(
        Value::MakeBoolArray({true, false, true, true, false}));
  }

  // dtos_to / IntTo / UIntTo must be byte-identical to the append variants.
  {
    for (float v : fedge) {
      char buf[24];
      size_t n = dtos_to(buf, v);
      std::string s_append;
      dtos_append(s_append, v);
      assert(std::string(buf, n) == s_append);
    }
    for (double v : dedge) {
      char buf[32];
      size_t n = dtos_to(buf, v);
      std::string s_append;
      dtos_append(s_append, v);
      assert(std::string(buf, n) == s_append);
    }
    const int64_t ivals[] = {0, 1, -1, 123456789, -987654321,
                             std::numeric_limits<int64_t>::min(),
                             std::numeric_limits<int64_t>::max()};
    for (int64_t v : ivals) {
      char buf[21];
      size_t n = IntTo(buf, v);
      std::string s_append;
      AppendInt(s_append, v);
      assert(std::string(buf, n) == s_append);
    }
    const uint64_t uvals[] = {0u, 1u, 123456789u,
                              std::numeric_limits<uint64_t>::max()};
    for (uint64_t v : uvals) {
      char buf[20];
      size_t n = UIntTo(buf, v);
      std::string s_append;
      AppendUInt(s_append, v);
      assert(std::string(buf, n) == s_append);
    }
  }

  std::cout << "  hot-array number formatting parity passed!\n\n";
}

// Verify PrintArrayRangeToStream: concatenating element ranges (with open on the
// first, close on the last) is byte-identical to the full PrintValue. This is the
// correctness foundation of the parallel writer's intra-array splitting.
void check_range_reconstruction(const Value& v, const std::vector<size_t>& cuts) {
  PrintOptions opts;  // no truncation
  assert(IsChunkableArray(v, opts));
  const std::string full = PrintValue(v, opts);
  const size_t n = ArrayElementCount(v);

  // Build the boundary list: 0, cuts..., n (clamped, deduped, sorted).
  std::vector<size_t> b = {0};
  for (size_t c : cuts) if (c > 0 && c < n) b.push_back(c);
  b.push_back(n);
  std::sort(b.begin(), b.end());
  b.erase(std::unique(b.begin(), b.end()), b.end());

  std::string actual;
  {
    StreamWriter w(&actual);
    for (size_t k = 0; k + 1 < b.size(); ++k) {
      bool ok = PrintArrayRangeToStream(w, v, opts, b[k], b[k + 1],
                                        /*open=*/k == 0,
                                        /*close=*/k + 2 == b.size());
      assert(ok);
    }
  }
  assert(actual == full);
}

void test_array_range_split_parity() {
  std::cout << "Testing array range split parity...\n";

  // int / int64 / uint scalar arrays.
  {
    std::vector<int32_t> a;
    for (int i = 0; i < 50; ++i) a.push_back(i * 7 - 13);
    check_range_reconstruction(Value::MakeIntArray(a), {1, 10, 25, 49});
    std::vector<int64_t> b;
    for (int i = 0; i < 33; ++i) b.push_back(int64_t(i) * 1000000007LL - 5);
    check_range_reconstruction(Value::MakeInt64Array(b), {1, 16, 32});
    std::vector<uint32_t> u;
    for (int i = 0; i < 20; ++i) u.push_back(uint32_t(i) * 99u);
    check_range_reconstruction(Value::MakeUIntArray(u), {7, 13});
  }

  // float / double scalar arrays with edge values.
  {
    std::vector<float> f = {0.0f, -0.0f, 1.0f, -1.0f, 3.14159f, 1e30f, 1e-30f,
                            123456.789f, 0.5f, 9999.0f, 2.5f, 7.25f};
    check_range_reconstruction(Value::MakeFloatArray(f), {1, 3, 6, 11});
    std::vector<double> d = {0.0, -0.0, 1.0, 13.944, 1e300, 1e-300, 3.5, 6.25};
    check_range_reconstruction(Value::MakeDoubleArray(d), {1, 4, 7});
  }

  // float3 / double3 vector tuples (per-component decomposition).
  {
    std::vector<float> f3;
    for (int i = 0; i < 30; ++i) f3.push_back(float(i) * 0.5f - 3.0f);  // 10 float3
    check_range_reconstruction(Value::MakeFloat3Array(f3), {1, 4, 9});
    std::vector<double> d3;
    for (int i = 0; i < 24; ++i) d3.push_back(double(i) * 1.25 - 2.0);  // 8 double3
    check_range_reconstruction(
        Value::MakeDoubleCompArray(std::move(d3), TypeId::Double3, 3), {1, 5, 7});
  }

  // matrix4d (nested tuples).
  {
    std::vector<double> m(64);  // 4 matrix4d
    for (size_t i = 0; i < m.size(); ++i) m[i] = double(i) * 0.25 - 8.0;
    check_range_reconstruction(
        Value::MakeDoubleCompArray(std::move(m), TypeId::Matrix4d, 16), {1, 2, 3});
  }

  // Single-chunk (open && close on one range) must equal full print.
  {
    std::vector<int32_t> a = {5, 6, 7};
    check_range_reconstruction(Value::MakeIntArray(a), {});
  }

  // Non-chunkable types report false.
  {
    PrintOptions opts;
    assert(!IsChunkableArray(Value::MakeTokenArray({"a", "b"}), opts));
    assert(!IsChunkableArray(Value::MakeBoolArray({true, false}), opts));
    assert(!IsChunkableArray(Value(42), opts));  // scalar
    PrintOptions trunc;
    trunc.max_array_elements = 2;
    assert(!IsChunkableArray(Value::MakeIntArray({1, 2, 3, 4}), trunc));
  }

  // Invalid caller-supplied ranges must fail before indexing or emitting any
  // text. This covers both scalar and component-backed arrays.
  {
    PrintOptions opts;
    Value ints = Value::MakeIntArray({1, 2, 3});
    Value points = Value::MakeFloat3Array({0, 1, 2, 3, 4, 5});
    std::string out;
    StreamWriter writer(&out);
    assert(!PrintArrayRangeToStream(writer, ints, opts, 0, 4, true, true));
    assert(!PrintArrayRangeToStream(writer, ints, opts, 3, 2, true, true));
    assert(!PrintArrayRangeToStream(writer, points, opts, 1, 3, true, true));
    assert(out.empty());
  }

  std::cout << "  array range split parity passed!\n\n";
}

void test_lazy_usdc_stream_value_printer() {
  std::cout << "Testing lazy USDC array stream value-printer...\n";

  Layer layer;
  LayerBuilder builder(layer);
  builder.begin_prim("Root", "Mesh");
  std::vector<float> points;
  points.reserve(3000);
  for (int i = 0; i < 1000; ++i) {
    points.push_back(static_cast<float>(i));
    points.push_back(static_cast<float>(i + 1));
    points.push_back(static_cast<float>(i + 2));
  }
  std::vector<int32_t> indices;
  indices.reserve(1000);
  for (int i = 0; i < 1000; ++i) indices.push_back(i);
  builder.add_property("points", Value::MakeFloat3Array(points));
  builder.add_property("faceVertexIndices", Value::MakeIntArray(indices));
  builder.end_prim();
  builder.finalize();

  std::vector<uint8_t> usdc;
  USDCWriteResult wr = WriteLayerToUSDCMemory(usdc, layer);
  assert(wr.success);

  USDCLoadResult lr = LoadUSDCFromMemory(usdc.data(), usdc.size());
  assert(lr.success);
  const Layer* loaded = lr.stage.GetRootLayer();
  assert(loaded);
  const PrimSpec* prim = loaded->prim_at_path("/Root");
  assert(prim);
  const Value* lazy_points = prim->property_value("points");
  const Value* lazy_indices = prim->property_value("faceVertexIndices");
  assert(lazy_points && lazy_points->is_lazy());
  assert(lazy_indices && lazy_indices->is_lazy());

  const std::string expected_points = PrintValue(lazy_points->materialized_copy());
  std::string actual_points;
  {
    StreamWriter writer(&actual_points);
    PrintValue(writer, *lazy_points);
  }
  assert(actual_points == expected_points);

  const std::string expected_indices = PrintValue(lazy_indices->materialized_copy());
  std::string actual_indices;
  {
    StreamWriter writer(&actual_indices);
    PrintValue(writer, *lazy_indices);
  }
  assert(actual_indices == expected_indices);

  std::cout << "  lazy USDC array stream value-printer test passed!\n\n";
}

void test_layer_printer() {
  std::cout << "Testing prim-printer with Layer...\n";

  // Create a simple layer
  Layer layer;
  LayerBuilder builder(layer);

  // Set layer metadata
  layer.meta().defaultPrim = "World";
  layer.meta().upAxis = "Y";
  layer.meta().doc = "layer \"doc\" with \\ slash";

  // Create root prim
  builder.begin_prim("World", "Xform");
  builder.current()->meta().doc() = "prim \"doc\" with \\ slash";
  builder.current()->meta().references.push_back(
      "@asset.usda@</P>?layerOffset=10:2");
  builder.current()->meta().references.push_back("</Internal>");
  builder.current()->meta().references.push_back("bare.usda");
  builder.end_prim();

  // Create child mesh
  builder.begin_prim("Cube", "Mesh");
  builder.add_property("extent", Value::MakeFloat3(-1, -1, -1));

  std::vector<float> points = {
    -1, -1, -1,
     1, -1, -1,
     1,  1, -1,
    -1,  1, -1,
  };
  builder.add_property("points", Value::MakeFloat3Array(points));
  builder.end_prim();

  builder.finalize();

  // Print the layer
  std::string output = PrintLayer(layer);
  std::cout << "Layer output:\n" << output << "\n";

  assert(contains(output, "#usda 1.0"));
  assert(contains(output, "defaultPrim"));
  assert(contains(output, "World"));
  assert(contains(output, "Mesh"));
  assert(contains(output, "Cube"));
  assert(contains(output, "extent"));
  assert(contains(output, "points"));
  assert(contains(output, "doc = \"layer \\\"doc\\\" with \\\\ slash\""));
  assert(contains(output, "@asset.usda@</P> (offset = 10; scale = 2)"));
  assert(contains(output, "</Internal>"));
  assert(contains(output, "@bare.usda@"));
  LoadResult reparsed = LoadUSDAFromString(output.data(), output.size());
  assert(reparsed.success && "debug layer output must reparse");

  std::cout << "  prim-printer tests passed!\n\n";
}

void test_stage_writer() {
  std::cout << "Testing usda-writer with Stage...\n";

  // Create a Stage using StageBuilder
  StageBuilder stage_builder;
  stage_builder.SetDefaultPrim("Root");
  stage_builder.SetUpAxis("Y");
  stage_builder.SetMetersPerUnit(0.01);

  LayerBuilder& layer = stage_builder.GetLayerBuilder();

  // Create root prim
  layer.begin_prim("Root", "Xform");
  layer.end_prim();

  // Create materials
  layer.begin_prim("Materials", "Scope");
  layer.end_prim();

  layer.begin_prim("Metal", "Material");
  layer.add_property("inputs:roughness", Value(0.2f));
  layer.end_prim();

  layer.finalize();

  Stage stage = stage_builder.Build();

  // Write to string
  USDAWriteOptions opts;
  opts.float_precision = 3;
  std::string output = WriteUSDAToString(stage, opts);

  std::cout << "Stage USDA output:\n" << output << "\n";

  assert(contains(output, "#usda 1.0"));
  assert(contains(output, "defaultPrim"));
  assert(contains(output, "Root"));
  assert(contains(output, "Materials"));
  assert(contains(output, "Metal"));
  assert(contains(output, "Material"));
  assert(contains(output, "roughness"));

  // Test write to file
  USDAWriteResult result = WriteUSDAToFile("/tmp/test_output.usda", stage, opts);
  assert(result.success);
  assert(result.bytes_written > 0);
  std::cout << "  Wrote " << result.bytes_written << " bytes to /tmp/test_output.usda\n";

  std::cout << "  usda-writer tests passed!\n\n";
}

void test_deep_stage_writer() {
  std::cout << "Testing deep stage writer traversal...\n";

  constexpr size_t kDepth = 8192;
  StageBuilder builder;
  LayerBuilder& layer = builder.GetLayerBuilder();
  for (size_t i = 0; i < kDepth; ++i) {
    layer.begin_prim("P", "Xform");
  }
  Stage stage = builder.Build();

  USDAWriteOptions opts;
  opts.indent.clear();  // Keep the regression output O(depth), not O(depth^2).
  const std::string text = WriteUSDAToString(stage, opts);
  assert(!text.empty());
  size_t definitions = 0;
  for (size_t pos = 0; (pos = text.find("def Xform", pos)) != std::string::npos;
       pos += 9) {
    ++definitions;
  }
  assert(definitions == kDepth);
  std::cout << "  deep stage writer traversal passed!\n\n";
}

void test_deep_variant_writer() {
  std::cout << "Testing deep variant writer traversal...\n";

  // This depth overflowed the recursive writer stack (especially under ASan
  // and in WASM). Build bottom-up so test setup itself is non-recursive.
  constexpr size_t kDepth = 8192;
  VariantSetData nested;
  for (size_t i = 0; i < kDepth; ++i) {
    VariantData option;
    option.name = "o";
    if (!nested.name.empty()) {
      option.variantSets.push_back(std::move(nested));
    }

    VariantSetData outer;
    outer.name = "v";
    outer.variants.push_back(std::move(option));
    nested = std::move(outer);
  }

  StageBuilder builder;
  LayerBuilder& layer = builder.GetLayerBuilder();
  layer.begin_prim("Root", "Xform");
  layer.current()->meta().variantSets().push_back(std::move(nested));
  layer.end_prim();
  Stage stage = builder.Build();

  USDAWriteOptions opts;
  opts.indent.clear();  // Keep the regression output O(depth), not O(depth^2).
  const std::string text = WriteUSDAToString(stage, opts);
  assert(!text.empty());
  size_t definitions = 0;
  for (size_t pos = 0;
       (pos = text.find("variantSet \"v\"", pos)) != std::string::npos;
       pos += 14) {
    ++definitions;
  }
  assert(definitions == kDepth);

  // Copying recursively-owned variant data must use the same explicit-depth
  // discipline as writing and teardown.
  const PrimSpec* root = stage.GetRootLayer()->prim_at_path("/Root");
  assert(root && !root->meta().variantSets().empty());
  VariantData copied = root->meta().variantSets()[0].variants[0];
  size_t copied_depth = 1;
  const VariantData* current = &copied;
  while (!current->variantSets.empty()) {
    current = &current->variantSets[0].variants[0];
    ++copied_depth;
  }
  assert(copied_depth == kDepth);

  std::cout << "  deep variant writer traversal passed!\n\n";
}

void test_time_samples() {
  std::cout << "Testing time samples...\n";

  StageBuilder stage_builder;
  stage_builder.SetDefaultPrim("Animated");
  stage_builder.SetStartTimeCode(0.0);
  stage_builder.SetEndTimeCode(100.0);
  stage_builder.SetTimeCodesPerSecond(24.0);

  LayerBuilder& layer = stage_builder.GetLayerBuilder();

  // Create animated prim
  layer.begin_prim("Animated", "Xform");

  // Add time samples for translation
  layer.add_time_sample("xformOp:translate", 0.0, Value::MakeFloat3(0, 0, 0));
  layer.add_time_sample("xformOp:translate", 50.0, Value::MakeFloat3(10, 5, 0));
  layer.add_time_sample("xformOp:translate", 100.0, Value::MakeFloat3(20, 0, 0));

  layer.end_prim();
  layer.finalize();

  Stage stage = stage_builder.Build();

  // Write to USDA and check output
  std::string output = WriteUSDAToString(stage);
  std::cout << "Time samples output:\n" << output << "\n";

  assert(contains(output, ".timeSamples"));
  assert(contains(output, "0:"));
  assert(contains(output, "50:"));
  assert(contains(output, "100:"));

  // Test GetValueAtTime via UsdPrim
  auto prims = stage.GetRootPrims();
  assert(!prims.empty());

  const UsdPrim& prim = prims[0];
  assert(prim.HasTimeSamples("xformOp:translate"));

  auto times = prim.GetTimeSampleTimes("xformOp:translate");
  assert(times.size() == 3);
  assert(times[0] == 0.0);
  assert(times[1] == 50.0);
  assert(times[2] == 100.0);

  // Test GetValueAtTime
  const Value* val_at_0 = prim.GetValueAtTime("xformOp:translate", 0.0);
  assert(val_at_0 != nullptr);
  const float* v0 = val_at_0->as_float3();
  assert(v0 != nullptr);
  assert(v0[0] == 0.0f && v0[1] == 0.0f && v0[2] == 0.0f);

  const Value* val_at_50 = prim.GetValueAtTime("xformOp:translate", 50.0);
  assert(val_at_50 != nullptr);
  const float* v50 = val_at_50->as_float3();
  assert(v50 != nullptr);
  assert(v50[0] == 10.0f && v50[1] == 5.0f && v50[2] == 0.0f);

  // Test interpolation (held - should return previous sample)
  const Value* val_at_25 = prim.GetValueAtTime("xformOp:translate", 25.0);
  assert(val_at_25 != nullptr);
  const float* v25 = val_at_25->as_float3();
  assert(v25 != nullptr);
  // Should be same as t=0 (held interpolation)
  assert(v25[0] == 0.0f);

  std::cout << "  time samples test passed!\n\n";
}

// The parallel USDA writer (num_threads > 1) must be byte-identical to the serial
// writer. Builds several prims each carrying a large array so the offload/segment/
// chunk paths engage, then compares the 1-thread and N-thread outputs. (When the
// build has no threading, num_threads is ignored and both paths are serial -- the
// check still holds.)
void test_parallel_writer_parity() {
  std::cout << "Testing parallel-writer byte parity...\n";

  StageBuilder stage_builder;
  stage_builder.SetDefaultPrim("World");
  stage_builder.SetUpAxis("Y");
  LayerBuilder& layer = stage_builder.GetLayerBuilder();

  layer.begin_prim("World", "Xform");
  layer.end_prim();

  // Several mesh prims, each with a large points array (> the writer's chunk
  // threshold) plus an index array, to exercise chunking + segmentation.
  const size_t kPts = 200000;  // > kSplitMinElems (128Ki) -> chunked
  for (int g = 0; g < 6; ++g) {
    std::vector<float> pts;
    pts.reserve(kPts * 3);
    for (size_t i = 0; i < kPts; ++i) {
      float b = float(i) * 0.013f + float(g);
      pts.push_back(b);
      pts.push_back(-b * 2.0f);
      pts.push_back(b * 0.5f - 7.0f);
    }
    std::vector<int> idx;
    idx.reserve(kPts);
    for (size_t i = 0; i < kPts; ++i) idx.push_back(int(i) - 5);

    layer.begin_prim("mesh_" + std::to_string(g), "Mesh");
    layer.add_property("points", Value::MakeFloat3Array(std::move(pts)));
    layer.add_property("indices", Value::MakeIntArray(std::move(idx)));
    layer.end_prim();
  }
  layer.finalize();
  Stage stage = stage_builder.Build();

  USDAWriteOptions serial_opts;
  serial_opts.num_threads = 1;
  USDAWriteOptions par_opts;
  par_opts.num_threads = 8;

  const std::string serial = WriteUSDAToString(stage, serial_opts);
  const std::string parallel = WriteUSDAToString(stage, par_opts);
  assert(serial.size() > kPts);  // sanity: arrays were emitted
  assert(serial == parallel);

  std::cout << "  parallel-writer byte parity passed!\n\n";
}

void test_roundtrip() {
  std::cout << "Testing USDA roundtrip (write -> manual inspection)...\n";

  // Create a more complex stage
  StageBuilder stage_builder;
  stage_builder.SetDefaultPrim("World");
  stage_builder.SetUpAxis("Z");
  stage_builder.SetTimeCodesPerSecond(30.0);
  stage_builder.SetStartTimeCode(0.0);
  stage_builder.SetEndTimeCode(100.0);

  LayerBuilder& layer = stage_builder.GetLayerBuilder();

  // Root
  layer.begin_prim("World", "Xform");
  layer.end_prim();

  // Character
  layer.begin_prim("Character", "Xform");
  layer.end_prim();

  // Body mesh
  layer.begin_prim("Body", "Mesh");

  // Add properties
  std::vector<int> faceVertexCounts = {4, 4, 4, 4, 4, 4};
  std::vector<int> faceVertexIndices = {
    0, 1, 2, 3,
    4, 5, 6, 7,
    0, 4, 5, 1,
    1, 5, 6, 2,
    2, 6, 7, 3,
    3, 7, 4, 0
  };
  std::vector<float> points = {
    -1, -1, -1,
     1, -1, -1,
     1,  1, -1,
    -1,  1, -1,
    -1, -1,  1,
     1, -1,  1,
     1,  1,  1,
    -1,  1,  1
  };

  layer.add_property("faceVertexCounts", Value::MakeIntArray(faceVertexCounts));
  layer.add_property("faceVertexIndices", Value::MakeIntArray(faceVertexIndices));
  layer.add_property("points", Value::MakeFloat3Array(points));

  layer.set_active(true);
  layer.end_prim();

  layer.finalize();

  Stage stage = stage_builder.Build();

  // Write with different options
  USDAWriteOptions opts;
  opts.compact = false;
  opts.sort_properties = true;
  opts.float_precision = 4;

  std::string output = WriteUSDAToString(stage, opts);
  std::cout << "Complex Stage USDA output:\n" << output << "\n";

  // Verify structure
  assert(contains(output, "World"));
  assert(contains(output, "Character"));
  assert(contains(output, "Body"));
  assert(contains(output, "Mesh"));
  assert(contains(output, "faceVertexCounts"));
  assert(contains(output, "faceVertexIndices"));
  assert(contains(output, "points"));
  assert(contains(output, "upAxis = \"Z\""));
  assert(contains(output, "timeCodesPerSecond"));

  std::cout << "  roundtrip test passed!\n\n";
}

void test_usda_backend_parity() {
  std::cout << "Testing USDA writer backend parity...\n";

  StageBuilder stage_builder;
  stage_builder.SetDefaultPrim("Root");
  stage_builder.SetUpAxis("Z");
  LayerBuilder& layer = stage_builder.GetLayerBuilder();

  layer.begin_prim("Root", "Xform");
  layer.add_property("xformOp:translate", Value::MakeFloat3(1.25f, 2.5f, 3.75f));
  layer.add_property("userProperties:note", Value(std::string("stream parity")));
  layer.end_prim();

  layer.begin_prim("Mesh", "Mesh");
  layer.add_property("faceVertexCounts", Value::MakeIntArray({3}));
  layer.add_property("faceVertexIndices", Value::MakeIntArray({0, 1, 2}));
  layer.add_property("points", Value::MakeFloat3Array({
      0.0f, 0.0f, 0.0f,
      1.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f}));
  layer.end_prim();
  layer.finalize();

  Stage stage = stage_builder.Build();
  USDAWriteOptions opts;
  opts.float_precision = 4;
  opts.sort_properties = true;

  const std::string expected = WriteUSDAToString(stage, opts);
  assert(!expected.empty());

  std::string stream_string;
  {
    StreamWriter writer(&stream_string);
    USDAWriteResult result = WriteUSDA(writer, stage, opts);
    assert(result.success);
    assert(result.bytes_written == expected.size());
  }
  assert(stream_string == expected);

  std::string sink_string;
  {
    StreamWriter writer(
        [&sink_string](const char* data, size_t n) {
          sink_string.append(data, n);
          return true;
        },
        7);
    USDAWriteResult result = WriteUSDA(writer, stage, opts);
    assert(result.success);
    assert(result.bytes_written == expected.size());
  }
  assert(sink_string == expected);

  std::ostringstream oss;
  USDAWriteResult ostream_result = WriteUSDA(oss, stage, opts);
  assert(ostream_result.success);
  assert(ostream_result.bytes_written == expected.size());
  assert(oss.str() == expected);

  const char* path = "/tmp/lightusd_next_writer_backend_parity.usda";
  USDAWriteResult file_result = WriteUSDAToFile(path, stage, opts);
  assert(file_result.success);
  assert(file_result.bytes_written == expected.size());
  std::ifstream ifs(path, std::ios::binary);
  std::string file_text((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
  assert(file_text == expected);
  std::remove(path);

  std::cout << "  USDA writer backend parity test passed!\n\n";
}

void test_usda_layer_backend_parity() {
  std::cout << "Testing USDA layer writer backend parity...\n";

  Layer layer;
  layer.meta().defaultPrim = "LayerRoot";
  layer.meta().upAxis = "Z";
  layer.meta().comment = "layer backend parity";

  LayerBuilder builder(layer);
  builder.begin_prim("LayerRoot", "Xform");
  builder.add_property("xformOp:translate", Value::MakeFloat3(-1.0f, 2.0f, 4.0f));
  builder.end_prim();

  builder.begin_prim("Child", "Scope");
  builder.add_property("purpose", Value::MakeToken("render"));
  builder.end_prim();
  builder.finalize();

  USDAWriteOptions opts;
  opts.float_precision = 4;
  opts.sort_properties = true;

  const std::string expected = WriteLayerToString(layer, opts);
  assert(!expected.empty());

  std::string stream_string;
  {
    StreamWriter writer(&stream_string);
    USDAWriteResult result = WriteLayer(writer, layer, opts);
    assert(result.success);
    assert(result.bytes_written == expected.size());
  }
  assert(stream_string == expected);

  std::string sink_string;
  {
    StreamWriter writer(
        [&sink_string](const char* data, size_t n) {
          sink_string.append(data, n);
          return true;
        },
        5);
    USDAWriteResult result = WriteLayer(writer, layer, opts);
    assert(result.success);
    assert(result.bytes_written == expected.size());
  }
  assert(sink_string == expected);

  std::ostringstream oss;
  USDAWriteResult ostream_result = WriteLayer(oss, layer, opts);
  assert(ostream_result.success);
  assert(ostream_result.bytes_written == expected.size());
  assert(oss.str() == expected);

  const std::string path = "/tmp/lightusd_next_layer_backend_parity.usda";
  USDAWriteResult file_result = WriteLayerToFile(path, layer, opts);
  assert(file_result.success);
  assert(file_result.bytes_written == expected.size());
  std::ifstream ifs(path, std::ios::binary);
  std::string file_text((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
  assert(file_text == expected);
  std::remove(path.c_str());

  std::cout << "  USDA layer writer backend parity test passed!\n\n";
}

void test_usda_stream_failure() {
  std::cout << "Testing USDA StreamWriter failure propagation...\n";

  StageBuilder stage_builder;
  stage_builder.SetDefaultPrim("Root");
  LayerBuilder& layer = stage_builder.GetLayerBuilder();
  layer.begin_prim("Root", "Xform");
  layer.add_property("visibility", Value::MakeToken("inherited"));
  layer.end_prim();
  layer.finalize();
  Stage stage = stage_builder.Build();

  {
    StreamWriter writer(
        [](const char*, size_t) {
          return false;
        },
        8);
    USDAWriteResult result = WriteUSDA(writer, stage);
    assert(!result.success);
    assert(!result.error.empty());
  }

  Layer raw_layer;
  LayerBuilder builder(raw_layer);
  builder.begin_prim("LayerRoot", "Xform");
  builder.end_prim();
  builder.finalize();

  {
    StreamWriter writer(
        [](const char*, size_t) {
          return false;
        },
        8);
    USDAWriteResult result = WriteLayer(writer, raw_layer);
    assert(!result.success);
    assert(!result.error.empty());
  }

  std::cout << "  USDA StreamWriter failure propagation test passed!\n\n";
}

void test_usda_api_error_paths() {
  std::cout << "Testing USDA writer API error paths...\n";

  Stage empty_stage;
  std::string out;
  StreamWriter stream(&out);
  USDAWriteResult empty_stage_result = WriteUSDA(stream, empty_stage);
  assert(!empty_stage_result.success);
  assert(!empty_stage_result.error.empty());

  USDAWriteResult null_file_result =
      WriteUSDAToFile(static_cast<const char*>(nullptr), empty_stage);
  assert(!null_file_result.success);
  assert(!null_file_result.error.empty());

  std::cout << "  USDA writer API error path test passed!\n\n";
}

// The deprecated `custom` qualifier is OMITTED by default and re-emitted only
// when USDAWriteOptions::emit_custom is set (e.g. under --openusd-compat).
void test_custom_qualifier_opt_in() {
  std::cout << "Testing custom-qualifier opt-in...\n";
  StageBuilder stage_builder;
  LayerBuilder& layer = stage_builder.GetLayerBuilder();
  layer.begin_prim("Root", "Xform");
  layer.add_property("isAsset", Value(true), PropSlot::kFlagCustom);
  layer.add_property("plain", Value(1.0));  // non-custom control
  layer.end_prim();
  layer.finalize();
  Stage stage = stage_builder.Build();

  // Default: no `custom`, but the property itself is still emitted.
  std::string def = WriteUSDAToString(stage);
  assert(!contains(def, "custom ") && "custom emitted by default");
  assert(contains(def, "bool isAsset") && "custom property dropped entirely");

  // Opt-in: the `custom` qualifier is restored (legacy / openusd-compat).
  USDAWriteOptions opts;
  opts.emit_custom = true;
  std::string oc = WriteUSDAToString(stage, opts);
  assert(contains(oc, "custom bool isAsset") &&
         "custom qualifier missing under emit_custom");
  std::cout << "  custom opt-in test passed!\n\n";
}

// A time-sampled attribute that ALSO has metadata must emit the metadata on a
// bare declaration line BEFORE the `.timeSamples` block; appending it after the
// block is invalid USDA ("TimeSampled Attribute cannot have attribute metadata").
void test_timesamples_metadata_placement() {
  std::cout << "Testing timeSamples + metadata placement...\n";
  StageBuilder sb;
  LayerBuilder& layer = sb.GetLayerBuilder();
  layer.begin_prim("P", "Xform");
  PropNameId id = GetPropNameTable().intern("primvars:displayColor");
  layer.current()->add_property_slot(
      id, TypeId::Float3, PropSlot::kFlagTimeSampled | PropSlot::kFlagArray);
  layer.current()->set_property_type_name("primvars:displayColor", "color3f[]");
  layer.add_time_sample("primvars:displayColor", 0.0,
                        Value::MakeFloat3Array({1, 0, 0}));
  layer.add_time_sample("primvars:displayColor", 10.0,
                        Value::MakeFloat3Array({0, 1, 0}));
  PropMeta& pm = layer.current()->ensure_property_meta("primvars:displayColor");
  pm.interpolation = "constant";
  pm.authored |= PropMeta::kInterpolation;
  layer.end_prim();
  layer.finalize();
  Stage stage = sb.Build();
  std::string usda = WriteUSDAToString(stage);

  size_t decl = usda.find("color3f[] primvars:displayColor (");
  size_t interp = usda.find("interpolation = \"constant\"");
  size_t ts = usda.find("primvars:displayColor.timeSamples = {");
  assert(decl != std::string::npos && ts != std::string::npos &&
         "both the declaration and the .timeSamples block must be emitted");
  assert(decl < ts && interp != std::string::npos && interp < ts &&
         "metadata must precede the .timeSamples block");
  // The invalid form appends metadata right after the closing brace.
  assert(usda.find("}\n                ) ") == std::string::npos);
  assert(usda.find("} (") == std::string::npos &&
         "metadata must NOT follow the .timeSamples closing brace");
  std::cout << "  timeSamples metadata placement test passed!\n\n";
}


// An authored DEFAULT must survive alongside .timeSamples on the same
// attribute (pxr keeps both; the writer used to drop the default).
void test_default_with_timesamples() {
  std::cout << "Testing default + timeSamples coexistence...\n";
  const char* src =
      "#usda 1.0\n"
      "def Xform \"p\"\n{\n"
      "    double v = 5\n"
      "    double v.timeSamples = {\n        0: 1,\n        10: 2,\n    }\n"
      "}\n";
  LoadResult r = LoadUSDAFromString(src, std::strlen(src));
  assert(r.success);
  std::string usda = WriteUSDAToString(r.stage);
  assert(usda.find("double v = 5") != std::string::npos &&
         "authored default dropped when timeSamples coexist");
  assert(usda.find("v.timeSamples") != std::string::npos &&
         "timeSamples block missing");
  // Round-trip: both fields still there after re-parsing the output.
  LoadResult r2 = LoadUSDAFromString(usda.c_str(), usda.size());
  assert(r2.success);
  const PrimSpec* p2 = r2.stage.GetRootLayer()->prim_at_path("/p");
  assert(p2);
  const Value* def = p2->property_value("v");
  assert(def && def->as_double() && *def->as_double() == 5.0);
  PropNameId vid = GetPropNameTable().find("v");
  const auto* ts = p2->time_samples(vid);
  assert(ts && ts->size() == 2);
  std::cout << "  default + timeSamples test passed!\n\n";
}


// Regression coverage for the 2026-07 usda reader/writer audit: raw-half /
// role-typed / timecode scalar+array printing (previously "<unsupported
// type N>"), layer-offset arc syntax (pxr rejects the internal
// "?layerOffset=o:s" form), target-less relationships, and 32-bit integer
// saturation. Everything is verified as a parse -> write -> re-parse -> write
// fixpoint (the second write must be byte-identical).
void test_usda_audit_roundtrip() {
  std::cout << "Testing usda audit roundtrip fixpoint...\n";

  static const char kUsda[] = R"(#usda 1.0
(
    defaultPrim = "E"
)

def Scope "E" (
    prepend references = @./ref.usda@</P> (offset = 10; scale = 2)
)
{
    half h = 0.5
    half2 h2 = (0.5, 1.5)
    half3 h3 = (0.25, 0.5, 0.75)
    half4 h4 = (1, 2, 3, 4)
    quath qh = (1, 2, 3, 4)
    texCoord2f uv = (0.25, 0.75)
    texCoord3f uvw = (0.25, 0.5, 0.75)
    color3d cd = (0.1, 0.2, 0.3)
    color4d cd4 = (0.1, 0.2, 0.3, 0.4)
    texCoord2d uvd = (0.5, 0.25)
    timecode tc = 42
    timecode[] tca = [1, 2.5, 3]
    custom rel material:binding
    rel none_rel = None
    rel empty_rel = []
    int sat = 2147483647
    uint usat = 4294967295
}
)";

  LoadResult lr = LoadUSDAFromString(kUsda, sizeof(kUsda) - 1);
  assert(lr.success && "audit usda must parse");
  Stage& stage = lr.stage;

  USDAWriteOptions wopts;
  std::string out1 = WriteUSDAToString(stage, wopts);
  assert(!out1.empty());

  // No printer fallback markers.
  assert(out1.find("<unsupported type") == std::string::npos);
  // Half / role / timecode values render with real numbers.
  assert(out1.find("half h = 0.5") != std::string::npos);
  assert(out1.find("half2 h2 = (0.5, 1.5)") != std::string::npos);
  assert(out1.find("quath qh = (1, 2, 3, 4)") != std::string::npos);
  assert(out1.find("texCoord2f uv = (0.25, 0.75)") != std::string::npos);
  assert(out1.find("timecode[] tca = [1, 2.5, 3]") != std::string::npos);
  // Layer offsets re-emit in pxr syntax, not the internal form.
  assert(out1.find("?layerOffset") == std::string::npos);
  assert(out1.find("(offset = 10; scale = 2)") != std::string::npos);
  // A bare declaration, blocked targets, and an authored empty target vector
  // retain their distinct USDA spellings.
  assert(out1.find("rel material:binding") != std::string::npos);
  assert(out1.find("rel material:binding =") == std::string::npos);
  assert(out1.find("rel none_rel = None") != std::string::npos);
  assert(out1.find("rel empty_rel = []") != std::string::npos);
  // 32-bit integers saturate instead of truncating bits.
  assert(out1.find("int sat = 2147483647") != std::string::npos);
  assert(out1.find("uint usat = 4294967295") != std::string::npos);

  // Fixpoint: re-parse the output and write again; must be byte-identical.
  LoadResult lr2 = LoadUSDAFromString(out1.data(), out1.size());
  assert(lr2.success && "writer output must re-parse");
  Stage& stage2 = lr2.stage;
  std::string out2 = WriteUSDAToString(stage2, wopts);
  assert(out1 == out2 && "usda write must be a fixpoint");

  std::cout << "  usda audit roundtrip fixpoint passed!\n\n";
}


// Second wave of 2026-07 usda audit fixes: leading-dot floats, timeSample
// ordering, `references = None`, keyword/quoted dictionary keys, triple-@
// asset paths, uint-vector arrays, half-role arrays, control-char escaping,
// property doc shorthand, duplicate sibling prims, `delete apiSchemas`,
// authored active/hidden, and strict scalar integers.
void test_usda_audit_roundtrip2() {
  std::cout << "Testing usda audit roundtrip (wave 2)...\n";

  static const char kUsda[] = R"(#usda 1.0
def Scope "V" (
    active = true
    hidden = false
    references = None
    apiSchemas = ["A", "B"]
    delete apiSchemas = ["B"]
    customData = {
        bool add = 1
        string custom = "c"
        string "key with space" = "v"
    }
)
{
    float ld = .5
    float ld2 = -.25
    float3 trail = (1., 2., .5)
    float ts.timeSamples = { 10: 10, 0: 0, 20: 20 }
    float x = 1 ( "property doc string" )
    asset at = @@@pa@th@@@
    asset[] ata = [@@@x@y@@@, @plain.png@]
    uint2[] ua = [(1, 2)]
    uint3 u3 = (1, 2, 3)
    point3h[] p3h = [(1, 2, 3)]
    color4h[] c4h = [(0.5, 0.5, 0.5, 1)]
    string ctrl = "a\x01b"
    string esc_arr_parity = "\x41\103"
    string[] esc_arr = ["\x41\103"]
    double dts.timeSamples = { 0.3: 1, 0.30000000000000004: 2 }
}

def Scope "V"
{
    float merged = 3
}
)";

  LoadResult lr = LoadUSDAFromString(kUsda, sizeof(kUsda) - 1);
  assert(lr.success && "audit wave-2 usda must parse");
  Stage& stage = lr.stage;
  const Layer* layer = stage.GetRootLayer();

  // Duplicate sibling prim merged into ONE spec.
  {
    size_t v_count = 0;
    for (const auto& pr : layer->prims()) {
      if (pr.path().str() == "/V") ++v_count;
    }
    assert(v_count == 1 && "duplicate sibling prims must merge");
    const PrimSpec* v = layer->prim_at_path("/V");
    assert(v && v->property_value("merged") && "merged prim keeps opinions");
    // A later non-explicit list edit exits explicit mode. OpenUSD retains only
    // the delete sublist here, so applying it to an empty weaker base is empty.
    assert(v->meta().apiSchemas().empty());
    assert(v->meta().apiSchemaEdits().authored &&
           !v->meta().apiSchemaEdits().is_explicit &&
           v->meta().apiSchemaEdits().deleted ==
               std::vector<std::string>{"B"});
    // timeSamples stored sorted regardless of authored order.
    const auto* ts = v->time_samples(GetPropNameTable().intern("ts"));
    assert(ts && ts->size() == 3);
    assert((*ts)[0].first == 0.0 && (*ts)[1].first == 10.0 &&
           (*ts)[2].first == 20.0);
    // Array/scalar string escape parity (\x41 = 'A', \103 = octal 'C').
    const Value* sv = v->property_value("esc_arr_parity");
    assert(sv && sv->as_string() && *sv->as_string() == "AC");
    const Value* av = v->property_value("esc_arr");
    assert(av && av->as_token_array() && (*av->as_token_array())[0] == "AC");
    // Triple-@ asset paths.
    const Value* at = v->property_value("at");
    assert(at && at->as_asset_path() && *at->as_asset_path() == "pa@th");
    const Value* ata = v->property_value("ata");
    assert(ata && ata->as_token_array() && (*ata->as_token_array())[0] == "x@y");
    // uint-vector array parsed.
    const Value* ua = v->property_value("ua");
    assert(ua && ua->as_uint_array() && ua->as_uint_array()->size() == 2);
    // half-role array float-backed.
    const Value* p3h = v->property_value("p3h");
    assert(p3h && p3h->as_float_array() && p3h->as_float_array()->size() == 3);
    // Property bare-string shorthand = COMMENT (pxr mapping).
    const PropMeta* xm = v->property_meta("x");
    assert(xm && (xm->authored & PropMeta::kComment) &&
           xm->comment == "property doc string");
  }

  // Strict scalar integers: overflow-ish garbage must be a parse error now.
  {
    static const char kBad[] = "#usda 1.0\ndef \"B\"\n{\n    int i = 0x10\n}\n";
    LoadResult bad = LoadUSDAFromString(kBad, sizeof(kBad) - 1);
    assert(!bad.success && "hex int literal must be rejected");
  }

  // Write and verify wave-2 output shape.
  USDAWriteOptions wopts;
  std::string out1 = WriteUSDAToString(stage, wopts);
  assert(out1.find("<unsupported type") == std::string::npos);
  assert(out1.find("references = None") != std::string::npos);
  assert(out1.find("active = true") != std::string::npos);
  assert(out1.find("hidden = false") != std::string::npos);
  assert(out1.find("@@@pa@th@@@") != std::string::npos);
  assert(out1.find("\"key with space\"") != std::string::npos);
  assert(out1.find("\\x01") != std::string::npos);  // control byte escaped
  assert(out1.find("0.30000000000000004") != std::string::npos);  // exact key
  assert(out1.find("uint2[] ua = [(1, 2)]") != std::string::npos);
  assert(out1.find("point3h[] p3h = [(1, 2, 3)]") != std::string::npos);

  // Fixpoint.
  LoadResult lr2 = LoadUSDAFromString(out1.data(), out1.size());
  assert(lr2.success && "wave-2 writer output must re-parse");
  std::string out2 = WriteUSDAToString(lr2.stage, wopts);
  assert(out1 == out2 && "usda write must be a fixpoint (wave 2)");

  std::cout << "  usda audit roundtrip (wave 2) passed!\n\n";
}

int main() {
  std::cout << "=== LightUSD Next Writer Tests ===\n\n";

  try {
    test_value_printer();
    test_hot_array_formatting_parity();
    test_array_range_split_parity();
    test_lazy_usdc_stream_value_printer();
    test_timesamples_metadata_placement();
    test_default_with_timesamples();
    test_layer_printer();
    test_stage_writer();
    test_deep_stage_writer();
    test_deep_variant_writer();
    test_time_samples();
    test_roundtrip();
    test_parallel_writer_parity();
    test_usda_backend_parity();
    test_usda_layer_backend_parity();
    test_usda_stream_failure();
    test_usda_api_error_paths();
    test_custom_qualifier_opt_in();
    test_usda_audit_roundtrip();
    test_usda_audit_roundtrip2();

    std::cout << "=== All writer tests passed! ===\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed with exception: " << e.what() << "\n";
    return 1;
  }
}
