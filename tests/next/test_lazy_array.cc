// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Lazy array reference tests
//
// Verifies that numeric POD arrays read from USDC come back as lazy references
// into the retained source buffer, materialize correctly on access, and share
// the backing buffer across copies (shared_ptr refcount).

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <vector>
#if defined(LIGHTUSD_ENABLE_THREAD)
#include <thread>
#endif
#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif

#include "next/composition/composition.hh"
#include "next/crate/crate-data-source.hh"
#include "next/crate/crate-allocation-budget.hh"
#include "next/crate/crate-format.hh"
#include "next/crate/crate-writer.hh"
#include "next/crate/lazy-array.hh"
#include "next/layer/layer.hh"
#include "next/pipeline/flatten.hh"
#include "next/pcp/cache.hh"
#include "next/reader/usdc-reader.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/stage/stage.hh"
#include "next/types/value.hh"
#include "next/types/value-view.hh"
#include "next/writer/stream-writer.hh"
#include "next/writer/value-printer.hh"
#include "next/writer/usdc-writer.hh"

using namespace lightusd::next;

namespace {
std::string LazyArrayScratchDirectory() {
#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
  char pattern[] = "/tmp/lightusd-next-lazy-XXXXXX";
  char* created = ::mkdtemp(pattern);
  return created ? std::string(created) : std::string("/tmp");
#else
  return "/tmp";
#endif
}
}  // namespace

static std::shared_ptr<CrateDataSource> MakeBudgetSource(LazyArrayRef* ref) {
  std::string bytes(16, '\0');
  const uint64_t count = 500;
  bytes.append(reinterpret_cast<const char*>(&count), sizeof(count));
  bytes.append(500 * sizeof(uint16_t), '\0');
  auto source = CrateDataSource::Adopt(std::move(bytes), CrateVersion{0, 8, 0});
  source->set_allocation_budget(std::make_shared<CrateAllocationBudget>(
      source->size(), 3500));
  assert(ProbeArrayBlock(source, ValueRep::Make(CrateTypeId::Half, 16, true),
                         500, ref));
  return source;
}

static void TestSharedDeferredBudget() {
  LazyArrayRef ref;
  auto source = MakeBudgetSource(&ref);
  Value first = Value::MakeLazyArray(ref);
  Value second = first;
  source.reset();
  ref.source.reset();
  Value decoded = first.materialized_copy();
  assert(decoded.as_float_array() && decoded.as_float_array()->size() == 500);
  // Each decode alone fits 3500 bytes; their cumulative 4000 bytes do not.
  assert(second.materialized_copy().is_empty());
  assert(first.is_lazy() && second.is_lazy());
#if defined(LIGHTUSD_ENABLE_THREAD)
  source = MakeBudgetSource(&ref);
  Value left = Value::MakeLazyArray(ref);
  Value right = left;
  Value results[2];
  std::thread a([&]() { results[0] = left.materialized_copy(); });
  std::thread b([&]() { results[1] = right.materialized_copy(); });
  a.join(); b.join();
  assert(results[0].is_empty() != results[1].is_empty());
#endif
  std::vector<uint8_t> flags{0, 1, 2, 128, 255};
  const uint8_t* storage = flags.data();
  Value bools = Value::MakeBoolByteArray(std::move(flags));
  assert(bools.type_id() == TypeId::Bool && bools.array_size() == 5);
  assert(bools.as_bool_array()->data() == storage);
  assert(*bools.as_bool_array() == std::vector<uint8_t>({0, 1, 1, 1, 1}));
}

int main() {
  TestSharedDeferredBudget();
  std::cout << "=== LightUSD Next Lazy Array Tests ===" << std::endl;

  // Empty ArrayView must be safe to use with begin()/end() range APIs.
  {
    Value empty = Value::MakeFloatArray(std::vector<float>{});
    ArrayScratch<float> scratch;
    ArrayView<float> view;
    assert(GetFloatArrayView(empty, &scratch, &view));
    assert(view.empty());
    assert(view.begin() == view.end());
    std::vector<float> copied(view.begin(), view.end());
    assert(copied.empty());
    assert(view.size_bytes() == 0);
  }

  // Capacity-hinted crate blob decompression should grow from an undersized
  // initial buffer and still reproduce the exact delta payload.
  {
    std::vector<uint32_t> values(4096);
    for (size_t i = 0; i < values.size(); ++i) {
      values[i] = static_cast<uint32_t>((i * 251u) ^ (i >> 1));
    }
    std::vector<uint8_t> delta = EncodeDeltaU32(values.data(), values.size());
    CompressResult cr = CompressCrateBlob(delta.data(), delta.size());
    assert(cr.success);
    DecompressResult dr = DecompressCrateBlobWithCapacityHint(
        cr.data.data(), cr.data.size(), delta.size(), 8);
    assert(dr.success);
    assert(dr.data == delta);
    std::cout << "  capacity-hinted crate blob decompression grows correctly"
              << std::endl;
  }

  // Pre-0.7 compressed arrays use a 4-byte count header. The direct lazy-int
  // stream printer must locate the following compressed-size and blob fields
  // from the source version instead of assuming the modern 8-byte header.
  {
    const std::vector<int32_t> values = {4, -9, 17, 17, 1024, -2048};
    std::vector<uint32_t> unsigned_values(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
      unsigned_values[i] = static_cast<uint32_t>(values[i]);
    }
    const std::vector<uint8_t> delta =
        EncodeDeltaU32(unsigned_values.data(), unsigned_values.size());
    const CompressResult cr = CompressCrateBlob(delta.data(), delta.size());
    assert(cr.success && !cr.data.empty());

    constexpr size_t kOffset = 16;
    std::string bytes(kOffset, '\0');
    const uint32_t count = static_cast<uint32_t>(values.size());
    const uint64_t compressed_size = cr.data.size();
    bytes.append(reinterpret_cast<const char*>(&count), sizeof(count));
    bytes.append(reinterpret_cast<const char*>(&compressed_size),
                 sizeof(compressed_size));
    bytes.append(reinterpret_cast<const char*>(cr.data.data()), cr.data.size());

    const ValueRep rep = ValueRep::Make(CrateTypeId::Int, kOffset,
                                        /*is_array=*/true,
                                        /*is_inlined=*/false,
                                        /*is_compressed=*/true);
    auto source =
        CrateDataSource::Adopt(std::move(bytes), CrateVersion{0, 6, 0});
    LazyArrayRef ref;
    assert(ProbeArrayBlock(source, rep, 1024, &ref));
    ref.max_elements = 1;
    Value lazy = Value::MakeLazyArray(ref);
    assert(lazy.is_lazy());

    // A lazy value must retain the reader's element policy when it is
    // materialized later; otherwise the deferred decode silently bypasses it.
    Value rejected = lazy.materialized_copy();
    assert(rejected.is_empty());

    std::string actual;
    StreamWriter writer(&actual);
    PrintValue(writer, lazy);
    assert(actual == PrintValue(Value::MakeIntArray(values)));
    assert(lazy.is_lazy() && !lazy.is_dirty());
    std::cout << "  pre-0.7 compressed lazy int stream prints correctly"
              << std::endl;
  }

  // ---- Build a stage with numeric POD arrays --------------------------------
  std::vector<float> points;        // 128 vec3f
  for (int i = 0; i < 128 * 3; i++) points.push_back(static_cast<float>(i) * 0.5f);
  std::vector<int32_t> indices;     // 256 ints
  for (int i = 0; i < 256; i++) indices.push_back(i * 7 - 11);
  std::vector<uint32_t> uids;       // 96 uints
  for (uint32_t i = 0; i < 96; i++) uids.push_back(i * 13u + 5u);
  std::vector<uint64_t> hashes;     // 32 uint64s
  for (uint64_t i = 0; i < 32; i++) hashes.push_back((i << 40) | (i * 17u));
  std::vector<uint32_t> bytes(64);  // 64 uchars
  for (uint32_t i = 0; i < bytes.size(); ++i) bytes[i] = i & 0xffu;
  std::vector<double> timecodes = {0.0, 1.25, 24.0, 1001.0};
  std::vector<float> quats;         // 64 quatf
  for (int i = 0; i < 64; i++) {
    quats.push_back(0.0f);
    quats.push_back(float(i));
    quats.push_back(0.0f);
    quats.push_back(1.0f);
  }
  std::vector<double> matrices;     // 8 matrix4d
  for (int i = 0; i < 8 * 16; i++) matrices.push_back(double(i) * 0.125);
  std::vector<float> tangents;      // 32 vec4f
  for (int i = 0; i < 32 * 4; i++) tangents.push_back(float(i) * 0.25f);
  std::vector<double> extents;      // 48 vec2d
  for (int i = 0; i < 48 * 2; i++) extents.push_back(double(i) * -0.5);
  std::vector<float> velocities0;   // 16 vec3f time sample
  std::vector<float> velocities1;   // 16 vec3f time sample
  for (int i = 0; i < 16 * 3; i++) {
    velocities0.push_back(float(i) * 0.125f);
    velocities1.push_back(float(i) * -0.25f + 7.0f);
  }

  StageBuilder sb;
  sb.SetDefaultPrim("Mesh1");
  LayerBuilder& lb = sb.GetLayerBuilder();
  lb.begin_prim("Mesh1", "Mesh");
  lb.add_property("points", Value::MakeFloatCompArray(
                                std::vector<float>(points), TypeId::Point3f, 3));
  lb.add_property("faceVertexIndices", Value::MakeIntArray(indices));
  lb.add_property("primvars:ids", Value::MakeUIntArray(uids));
  lb.add_property("primvars:hashes", Value::MakeUInt64Array(hashes));
  lb.add_property("primvars:bytes", Value::MakeUIntCompArray(
                                           std::vector<uint32_t>(bytes),
                                           TypeId::UChar, 1));
  lb.add_property("sampleTimes", Value::MakeDoubleCompArray(
                                      std::vector<double>(timecodes),
                                      TypeId::TimeCode, 1));
  lb.add_property("orientations",
                  Value::MakeFloatCompArray(std::vector<float>(quats),
                                            TypeId::Quatf, 4));
  lb.add_property("xforms",
                  Value::MakeDoubleCompArray(std::vector<double>(matrices),
                                             TypeId::Matrix4d, 16));
  lb.add_property("tangents",
                  Value::MakeFloatCompArray(std::vector<float>(tangents),
                                            TypeId::Float4, 4));
  lb.add_property("extent",
                  Value::MakeDoubleCompArray(std::vector<double>(extents),
                                             TypeId::Double2, 2));
  lb.add_time_sample("velocities", 0.0,
                     Value::MakeFloat3Array(std::vector<float>(velocities0)));
  lb.add_time_sample("velocities", 1.0,
                     Value::MakeFloat3Array(std::vector<float>(velocities1)));
  lb.end_prim();
  lb.finalize();
  Stage stage = sb.Build();

  // ---- Write to USDC in memory ---------------------------------------------
  std::vector<uint8_t> buf;
  USDCWriteResult wr = WriteUSDCToMemory(buf, stage);
  assert(wr.success);
  assert(!buf.empty());

  // ---- Read it back ---------------------------------------------------------
  USDCLoadResult lr = LoadUSDCFromMemory(buf.data(), buf.size());
  if (!lr.success) {
    std::cerr << "Load failed";
    if (!lr.errors.empty()) std::cerr << ": " << lr.errors[0].message;
    std::cerr << std::endl;
    return 1;
  }

  UsdPrim mesh = lr.stage.GetPrimAtPath("/Mesh1");
  assert(mesh.IsValid());
  const PrimSpec* ps = mesh.GetPrimSpec();
  assert(ps);

  // ---- TimeSamples: lazy array values --------------------------------------
  assert(mesh.HasTimeSamples("velocities"));
  std::vector<double> velocity_times = mesh.GetTimeSampleTimes("velocities");
  assert(velocity_times.size() == 2);
  assert(velocity_times[0] == 0.0);
  assert(velocity_times[1] == 1.0);
  const Value* v0 = mesh.GetValueAtTime("velocities", 0.0);
  const Value* v1 = mesh.GetValueAtTime("velocities", 1.0);
  assert(v0 && v1);
  assert(v0->is_array() && v1->is_array());
  assert(v0->is_lazy() && v1->is_lazy());
  assert(!v0->is_dirty() && !v1->is_dirty());
  assert(v0->array_size() == velocities0.size() / 3);
  assert(v1->array_size() == velocities1.size() / 3);
  {
    ArrayScratch<float> scratch;
    ArrayView<float> view;
    assert(GetFloatArrayView(*v0, &scratch, &view));
    assert(view.size == velocities0.size());
    for (size_t i = 0; i < velocities0.size(); i++) {
      assert(view[i] == velocities0[i]);
    }
    assert(v0->is_lazy());
  }
  Value v1_copy = v1->materialized_copy();
  assert(!v1_copy.is_lazy());
  assert(v1->is_lazy());
  const std::vector<float>* v1_arr = v1_copy.as_float_array();
  assert(v1_arr && *v1_arr == velocities1);
  std::cout << "  time-sampled array values came back lazy" << std::endl;

  // ---- points: lazy Vec3f -> declared Point3f role -------------------------
  const Value* pv = ps->property_value("points");
  assert(pv);
  assert(pv->is_array());
  assert(pv->is_lazy());                          // lazy BEFORE any access
  assert(pv->type_id() == TypeId::Point3f);
  assert(pv->lazy_ref() && pv->lazy_ref()->value_type == TypeId::Point3f);
  assert(!pv->is_dirty());
  assert(pv->array_size() == points.size() / 3);  // count without materializing
  std::cout << "  points came back lazy (count=" << pv->array_size() << ")" << std::endl;

  // Copy the lazy value: must stay lazy and SHARE the source buffer.
  {
    Value copy = *pv;
    assert(copy.is_lazy());
    assert(copy.lazy_ref() && pv->lazy_ref());
    assert(copy.lazy_ref()->source.get() == pv->lazy_ref()->source.get());
    long uc_before = pv->lazy_ref()->source.use_count();
    assert(uc_before >= 2);  // pv + copy both reference it
    // Materializing the copy must not disturb the original.
    const std::vector<float>* carr = copy.as_float_array();
    assert(carr && carr->size() == points.size());
    assert(!copy.is_lazy());
    assert(pv->is_lazy());   // original untouched
    (void)uc_before;
  }
  // After the copy is destroyed, the original still resolves.

  // materialized_copy(): decode into a temp WITHOUT disturbing the lazy original
  // (the writer's transient-decode path that keeps peak RSS bounded to ~one
  // array). The decoded temp must match the source bytes, and pv stays lazy.
  {
    Value mc = pv->materialized_copy();
    assert(!mc.is_lazy());                 // returned value is decoded
    assert(pv->is_lazy());                 // ORIGINAL untouched (still lazy)
    const std::vector<float>* mcarr = mc.as_float_array();
    assert(mcarr && mcarr->size() == points.size());
    for (size_t i = 0; i < points.size(); i++) assert((*mcarr)[i] == points[i]);
  }
  assert(pv->is_lazy());  // still lazy after the temp is destroyed

  // Borrowed read-only view: direct pointer into the retained crate payload for
  // uncompressed POD arrays, without materializing or dirtying the Value.
  {
    ArrayScratch<float> scratch;
    ArrayView<float> view;
    assert(GetFloatArrayView(*pv, &scratch, &view));
    assert(view.borrowed);
    assert(view.size == points.size());
    assert(scratch.materialized.is_empty());
    for (size_t i = 0; i < points.size(); i++) assert(view[i] == points[i]);
    assert(pv->is_lazy());
    assert(!pv->is_dirty());
    std::cout << "  points borrowed view reads without materializing" << std::endl;
  }

  // Large borrowable lazy arrays should use the serial range printer, so source
  // pages can be discarded per chunk without changing text or materializing the
  // original Value.
  {
    std::vector<float> big_points((64u * 1024u + 17u) * 3u);
    for (size_t i = 0; i < big_points.size(); ++i) {
      big_points[i] = static_cast<float>(i % 257) * 0.25f;
    }

    StageBuilder big_sb;
    big_sb.SetDefaultPrim("BigMesh");
    LayerBuilder& big_lb = big_sb.GetLayerBuilder();
    big_lb.begin_prim("BigMesh", "Mesh");
    big_lb.add_property("points", Value::MakeFloat3Array(big_points));
    big_lb.end_prim();
    big_lb.finalize();

    Stage big_stage = big_sb.Build();
    std::vector<uint8_t> big_buf;
    USDCWriteResult big_wr = WriteUSDCToMemory(big_buf, big_stage);
    assert(big_wr.success);
    USDCLoadResult big_lr = LoadUSDCFromMemory(big_buf.data(), big_buf.size());
    assert(big_lr.success);
    UsdPrim big_mesh = big_lr.stage.GetPrimAtPath("/BigMesh");
    assert(big_mesh.IsValid());
    const Value* lazy_points = big_mesh.GetPropertyValue("points");
    assert(lazy_points && lazy_points->is_lazy());

    Value eager = lazy_points->materialized_copy();
    const std::string expected = PrintValue(eager);
    std::string actual;
    {
      StreamWriter sw(&actual);
      PrintValue(sw, *lazy_points);
    }
    assert(actual == expected);
    assert(lazy_points->is_lazy());
    assert(!lazy_points->is_dirty());
    std::cout << "  serial range printer preserves lazy array text" << std::endl;

    // File-backed USDC arrays must remain mmap-backed after crossing an
    // external reference and PCP Stage reconstruction. The same composition
    // with mmap disabled stays lazy but retains an owned byte source instead.
    const std::string scratch = LazyArrayScratchDirectory();
    const std::string asset_path = scratch + "/asset.usdc";
    const std::string root_path = scratch + "/root.usda";
    USDCWriteResult file_wr = WriteUSDCToFile(asset_path, big_stage);
    assert(file_wr.success);
    {
      std::ofstream root(root_path);
      root << "#usda 1.0\n"
              "def Xform \"Composed\" (prepend references = "
              "@./asset.usdc@</BigMesh>)\n{\n}\n";
    }

    AssetResolver resolver;
    resolver.SetWorkingDirectory(scratch);
    pcp::CompositionOptions compose_options;
    Stage composed;
    std::string compose_warn, compose_err;
    assert(pcp::ComposeStageFromFile(root_path, resolver, &composed,
                                     compose_options, &compose_warn,
                                     &compose_err));
    UsdPrim composed_prim = composed.GetPrimAtPath("/Composed");
    assert(composed_prim.IsValid());
    const Value* composed_points = composed_prim.GetPropertyValue("points");
    assert(composed_points && composed_points->is_lazy());
    assert(composed_points->lazy_ref() && composed_points->lazy_ref()->source);
    assert(composed_points->lazy_ref()->source->is_mmapped());

    compose_options.usdc_use_mmap = false;
    Stage composed_owned;
    compose_warn.clear();
    compose_err.clear();
    assert(pcp::ComposeStageFromFile(root_path, resolver, &composed_owned,
                                     compose_options, &compose_warn,
                                     &compose_err));
    UsdPrim owned_prim = composed_owned.GetPrimAtPath("/Composed");
    const Value* owned_points = owned_prim.GetPropertyValue("points");
    assert(owned_points && owned_points->is_lazy());
    assert(owned_points->lazy_ref() && owned_points->lazy_ref()->source);
    assert(!owned_points->lazy_ref()->source->is_mmapped());
    std::remove(root_path.c_str());
    std::remove(asset_path.c_str());
    std::cout << "  PCP composition retains mmap-backed lazy arrays"
              << std::endl;
  }

  // Compressed lazy int arrays should print without materializing a decoded
  // int vector into the source Value.
  {
    std::vector<int32_t> big_indices(96u * 1024u);
    for (size_t i = 0; i < big_indices.size(); ++i) {
      big_indices[i] = static_cast<int32_t>((i * 17u) % 1009u) - 503;
    }

    StageBuilder big_sb;
    big_sb.SetDefaultPrim("BigInts");
    LayerBuilder& big_lb = big_sb.GetLayerBuilder();
    big_lb.begin_prim("BigInts", "Mesh");
    big_lb.add_property("faceVertexIndices", Value::MakeIntArray(big_indices));
    big_lb.end_prim();
    big_lb.finalize();

    std::vector<uint8_t> big_buf;
    USDCWriteResult big_wr = WriteUSDCToMemory(big_buf, big_sb.Build());
    assert(big_wr.success);
    USDCLoadResult big_lr = LoadUSDCFromMemory(big_buf.data(), big_buf.size());
    assert(big_lr.success);
    UsdPrim big_mesh = big_lr.stage.GetPrimAtPath("/BigInts");
    assert(big_mesh.IsValid());
    const Value* lazy_indices = big_mesh.GetPropertyValue("faceVertexIndices");
    assert(lazy_indices && lazy_indices->is_lazy());
    assert(lazy_indices->lazy_ref() && lazy_indices->lazy_ref()->is_compressed);

    Value eager = lazy_indices->materialized_copy();
    const std::string expected = PrintValue(eager);
    std::string actual;
    {
      StreamWriter sw(&actual);
      PrintValue(sw, *lazy_indices);
    }
    assert(actual == expected);
    assert(lazy_indices->is_lazy());
    assert(!lazy_indices->is_dirty());
    std::cout << "  compressed lazy int printer preserves text" << std::endl;
  }

  // Materialize the original via accessor; verify contents byte-for-byte.
  const std::vector<float>* arr = pv->as_float_array();
  assert(arr);
  assert(pv->type_id() == TypeId::Point3f);
  assert(arr->size() == points.size());
  for (size_t i = 0; i < points.size(); i++) assert((*arr)[i] == points[i]);
  assert(!pv->is_lazy());  // materialized after access
  std::cout << "  points materialized correctly (" << arr->size() << " floats)" << std::endl;

  // ---- faceVertexIndices: lazy Int -----------------------------------------
  const Value* iv = ps->property_value("faceVertexIndices");
  assert(iv);
  assert(iv->is_lazy());
  {
    ArrayScratch<int32_t> scratch;
    ArrayView<int32_t> view;
    assert(GetIntArrayView(*iv, &scratch, &view));
    assert(view.size == indices.size());
    for (size_t i = 0; i < indices.size(); i++) assert(view[i] == indices[i]);
    assert(iv->is_lazy());
    assert(!iv->is_dirty());
    std::cout << "  indices view reads without materializing source"
              << (view.borrowed ? " (borrowed)" : " (scratch)") << std::endl;
  }
  const std::vector<int32_t>* ia = iv->as_int_array();
  assert(ia);
  assert(*ia == indices);
  assert(!iv->is_lazy());
  std::cout << "  indices materialized correctly (" << ia->size() << " ints)" << std::endl;

  // ---- unsigned integer arrays: lazy UInt / UInt64 views --------------------
  const Value* uv = ps->property_value("primvars:ids");
  assert(uv && uv->is_lazy());
  {
    ArrayScratch<uint32_t> scratch;
    ArrayView<uint32_t> view;
    assert(GetUIntArrayView(*uv, &scratch, &view));
    assert(view.size == uids.size());
    assert(view.size_bytes() == uids.size() * sizeof(uint32_t));
    for (size_t i = 0; i < uids.size(); i++) assert(view[i] == uids[i]);
    assert(uv->is_lazy());
    assert(!uv->is_dirty());
  }
  const Value* u64v = ps->property_value("primvars:hashes");
  assert(u64v && u64v->is_lazy());
  {
    ArrayScratch<uint64_t> scratch;
    ArrayView<uint64_t> view;
    assert(GetUInt64ArrayView(*u64v, &scratch, &view));
    assert(view.size == hashes.size());
    assert(view.size_bytes() == hashes.size() * sizeof(uint64_t));
    for (size_t i = 0; i < hashes.size(); i++) assert(view[i] == hashes[i]);
    assert(u64v->is_lazy());
    assert(!u64v->is_dirty());
    std::cout << "  unsigned integer views read without materializing source"
              << (view.borrowed ? " (borrowed)" : " (scratch)") << std::endl;
  }
  const Value* bytev = ps->property_value("primvars:bytes");
  const Value* timev = ps->property_value("sampleTimes");
  assert(bytev && bytev->type_id() == TypeId::UChar && bytev->is_lazy());
  assert(timev && timev->type_id() == TypeId::TimeCode && timev->is_lazy());

  // ---- orientations / xforms: quaternion/matrix array laziness --------------
  // Quaternion crate bytes are imaginary-first, while Value is real-first.
  // They stay lazy, but their read-only view uses a swizzled scratch value
  // rather than incorrectly aliasing the on-disk component order.
  const Value* qv = ps->property_value("orientations");
  assert(qv && qv->is_array() && qv->is_lazy());
  assert(!CanBorrowLazyFlat(*qv));
  {
    ArrayScratch<float> scratch;
    ArrayView<float> view;
    assert(GetFloatArrayView(*qv, &scratch, &view));
    assert(view.size == quats.size());
    assert(!scratch.materialized.is_empty());
    for (size_t i = 0; i < quats.size(); ++i) assert(view[i] == quats[i]);
    assert(qv->is_lazy() && !qv->is_dirty());
  }
  Value qcopy = qv->materialized_copy();
  const std::vector<float>* qa = qcopy.as_float_array();
  assert(qa && *qa == quats);
  assert(qv->is_lazy());
  const Value* mv = ps->property_value("xforms");
  assert(mv && mv->is_array() && mv->is_lazy());
  const std::vector<double>* ma = mv->as_double_array();
  assert(ma && *ma == matrices);
  std::cout << "  quat/matrix arrays came back lazy and materialized correctly" << std::endl;

  // ---- vec4f / vec2d: doc-named POD types must also be lazy -----------------
  const Value* tv = ps->property_value("tangents");
  assert(tv && tv->is_array() && tv->is_lazy());
  const std::vector<float>* ta = tv->as_float_array();
  assert(ta && *ta == tangents);
  const Value* ev = ps->property_value("extent");
  assert(ev && ev->is_array() && ev->is_lazy());
  const std::vector<double>* ea = ev->as_double_array();
  assert(ea && *ea == extents);
  std::cout << "  vec4f/vec2d arrays came back lazy and materialized correctly" << std::endl;

  // ---- A2: composition (Clone + Compositor) preserves laziness -------------
  {
    USDCLoadResult lr2 = LoadUSDCFromMemory(buf.data(), buf.size());
    assert(lr2.success);
    const Layer* layer = lr2.stage.GetRootLayer();
    assert(layer);
    const PrimSpec* sps = layer->prim_at_path("/Mesh1");
    assert(sps);
    const Value* sv = sps->property_value("points");
    assert(sv && sv->is_lazy());
    const auto* src = sv->lazy_ref()->source.get();
    long uc0 = sv->lazy_ref()->source.use_count();

    // PrimSpec::Clone() preserves laziness and shares the source buffer.
    PrimSpec clone = sps->Clone();
    const Value* cv = clone.property_value("points");
    assert(cv && cv->is_lazy());
    assert(cv->lazy_ref()->source.get() == src);
    assert(sv->lazy_ref()->source.use_count() > uc0);  // buffer shared, not copied
    std::cout << "  Clone() preserved lazy + shared source" << std::endl;

    // Compositor::Compose() preserves laziness and shares the source buffer.
    Compositor comp;
    std::unique_ptr<Layer> composed = comp.Compose(*layer);
    assert(composed);
    const PrimSpec* cps = composed->prim_at_path("/Mesh1");
    assert(cps);
    const Value* compv = cps->property_value("points");
    assert(compv && compv->is_lazy());
    assert(compv->lazy_ref()->source.get() == src);
    std::cout << "  Compose() preserved lazy + shared source" << std::endl;

    // Materializing the composed value must not disturb the source layer value.
    const std::vector<float>* compArr = compv->as_float_array();
    assert(compArr && compArr->size() == points.size());
    assert(!compv->is_lazy());
    assert(sv->is_lazy());  // original opinion untouched
    std::cout << "  composition materialize is independent of source" << std::endl;
  }

  // ---- A3: writer byte pass-through ----------------------------------------
  {
    USDCLoadResult lr3 = LoadUSDCFromMemory(buf.data(), buf.size());
    assert(lr3.success);
    const Layer* layer = lr3.stage.GetRootLayer();
    assert(layer);

    CrateWriter writer;
    std::vector<uint8_t> out;
    CrateWriteResult wres = writer.WriteLayerToMemory(out, *layer);
    assert(wres.success);
    // Numeric arrays (points Vec3f, indices Int, ids UInt, hashes UInt64,
    // xforms Matrix4d, tangents Vec4f, extent Vec2d, and two velocities
    // TimeSamples and orientations Quatf) copied verbatim. Quaternion views
    // materialize through a swizzled scratch value, but an untouched lazy
    // quaternion block can still pass through without decoding.
    assert(wres.arrays_passed_through >= 12);
    assert(wres.arrays_reencoded == 0);
    std::cout << "  writer passed through " << wres.arrays_passed_through
              << " arrays (" << wres.arrays_reencoded << " reencoded)" << std::endl;

    // Re-read the pass-through output and verify contents are bit-identical.
    USDCLoadResult lr4 = LoadUSDCFromMemory(out.data(), out.size());
    assert(lr4.success);
    UsdPrim mesh4 = lr4.stage.GetPrimAtPath("/Mesh1");
    assert(mesh4.IsValid());
    const PrimSpec* ps4 = lr4.stage.GetRootLayer()->prim_at_path("/Mesh1");
    assert(ps4);
    const std::vector<float>* pa = ps4->property_value("points")->as_float_array();
    assert(pa && pa->size() == points.size());
    for (size_t i = 0; i < points.size(); i++) assert((*pa)[i] == points[i]);
    const std::vector<int32_t>* ix =
        ps4->property_value("faceVertexIndices")->as_int_array();
    assert(ix && *ix == indices);
    const std::vector<uint32_t>* ux =
        ps4->property_value("primvars:ids")->as_uint_array();
    assert(ux && *ux == uids);
    const std::vector<uint64_t>* u64x =
        ps4->property_value("primvars:hashes")->as_uint64_array();
    assert(u64x && *u64x == hashes);
    const std::vector<float>* oq =
        ps4->property_value("orientations")->as_float_array();
    assert(oq && *oq == quats);
    const std::vector<double>* xm =
        ps4->property_value("xforms")->as_double_array();
    assert(xm && *xm == matrices);
    const Value* rv0 = mesh4.GetValueAtTime("velocities", 0.0);
    const Value* rv1 = mesh4.GetValueAtTime("velocities", 1.0);
    assert(rv0 && rv1 && rv0->is_lazy() && rv1->is_lazy());
    const std::vector<float>* rv0a = rv0->as_float_array();
    const std::vector<float>* rv1a = rv1->as_float_array();
    assert(rv0a && *rv0a == velocities0);
    assert(rv1a && *rv1a == velocities1);
    std::cout << "  pass-through output re-reads identically" << std::endl;

    // Dirty flag: a mutable accessor marks the value so pass-through is skipped.
    Value dv = *layer->prim_at_path("/Mesh1")->property_value("points");
    assert(dv.is_lazy() && !dv.is_dirty());
    (void)dv.as_float_array();  // non-const accessor materializes + dirties
    assert(!dv.is_lazy() && dv.is_dirty());
    std::cout << "  mutable access sets dirty (disables pass-through)" << std::endl;
  }

  // ---- C1: pipeline facade (read -> flatten -> write) ----------------------
  {
    pipeline::FlattenOptions fopts;
    pipeline::FlattenStats fstats;
    std::vector<uint8_t> fout;
    std::string ferr;
    bool ok = pipeline::FlattenUSDCToUSDC(buf.data(), buf.size(), fout, fopts,
                                          &fstats, &ferr);
    assert(ok);
    assert(fstats.arrays_passed_through >= 12);
    assert(fstats.arrays_reencoded == 0);
    std::cout << "  FlattenUSDCToUSDC: " << fstats.input_bytes << " -> "
              << fstats.output_bytes << " bytes, passthrough="
              << fstats.arrays_passed_through << std::endl;

    // Flattened output re-reads with identical array contents.
    USDCLoadResult lr5 = LoadUSDCFromMemory(fout.data(), fout.size());
    assert(lr5.success);
    UsdPrim mesh5 = lr5.stage.GetPrimAtPath("/Mesh1");
    assert(mesh5.IsValid());
    const PrimSpec* ps5 = lr5.stage.GetRootLayer()->prim_at_path("/Mesh1");
    assert(ps5);
    const std::vector<float>* fpa = ps5->property_value("points")->as_float_array();
    assert(fpa && fpa->size() == points.size());
    for (size_t i = 0; i < points.size(); i++) assert((*fpa)[i] == points[i]);
    const std::vector<float>* fqa =
        ps5->property_value("orientations")->as_float_array();
    assert(fqa && *fqa == quats);
    const std::vector<double>* fma =
        ps5->property_value("xforms")->as_double_array();
    assert(fma && *fma == matrices);
    const Value* fv0 = mesh5.GetValueAtTime("velocities", 0.0);
    const Value* fv1 = mesh5.GetValueAtTime("velocities", 1.0);
    assert(fv0 && fv1);
    const std::vector<float>* fv0a = fv0->as_float_array();
    const std::vector<float>* fv1a = fv1->as_float_array();
    assert(fv0a && *fv0a == velocities0);
    assert(fv1a && *fv1a == velocities1);
    std::cout << "  facade output re-reads identically" << std::endl;

    // Filesystem facade: USDA root + USDA sublayer must compose and write USDC.
    {
      const char* sub_path = "/tmp/lightusd_next_flatten_usda_sub.usda";
      const char* ref_path = "/tmp/lightusd_next_flatten_usda_ref.usda";
      const char* root_path = "/tmp/lightusd_next_flatten_usda_root.usda";
      {
        std::ofstream sub(sub_path, std::ios::binary);
        sub << "#usda 1.0\n"
               "def Xform \"World\"\n"
               "{\n"
               "  def Mesh \"FromSub\"\n"
               "  {\n"
               "    int[] faceVertexCounts = [3]\n"
               "    int[] faceVertexIndices = [0, 1, 2]\n"
               "    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
               "  }\n"
               "}\n";
      }
      {
        std::ofstream ref(ref_path, std::ios::binary);
        ref << "#usda 1.0\n"
               "def Xform \"Referenced\"\n"
               "{\n"
               "  def Mesh \"FromReference\"\n"
               "  {\n"
               "    int[] faceVertexCounts = [3]\n"
               "    int[] faceVertexIndices = [0, 1, 2]\n"
               "    point3f[] points = [(0, 0, 0), (0, 1, 0), (0, 0, 1)]\n"
               "  }\n"
               "}\n";
      }
      {
        std::ofstream root(root_path, std::ios::binary);
        root << "#usda 1.0\n"
                "(\n"
                "  subLayers = [@./lightusd_next_flatten_usda_sub.usda@]\n"
                ")\n"
                "def Xform \"World\"\n"
                "{\n"
                "  def Xform \"Local\" {}\n"
                "  def Xform \"RefSlot\" (\n"
                "    prepend references = [@./lightusd_next_flatten_usda_ref.usda@</Referenced>]\n"
                "  ) {}\n"
                "}\n";
      }
      pipeline::FlattenStats file_stats;
      std::vector<uint8_t> file_out;
      std::string file_err;
      assert(pipeline::FlattenUSDFileToUSDC(root_path, file_out, fopts,
                                            &file_stats, &file_err));
      assert(!file_out.empty());
      USDCLoadResult file_lr = LoadUSDCFromMemory(file_out.data(),
                                                  file_out.size());
      assert(file_lr.success);
      assert(file_lr.stage.GetPrimAtPath("/World/FromSub").IsValid());
      assert(file_lr.stage.GetPrimAtPath("/World/Local").IsValid());
      assert(file_lr.stage.GetPrimAtPath("/World/RefSlot/FromReference").IsValid());
      assert(file_stats.prim_count >= 3);
      std::cout << "  FlattenUSDFileToUSDC composes USDA sublayers/references" << std::endl;
    }

    // Owned (single-copy) facade path produces the same result.
    pipeline::FlattenStats ostats;
    std::vector<uint8_t> oout;
    std::string oin(reinterpret_cast<const char*>(buf.data()), buf.size());
    bool ook = pipeline::FlattenUSDCToUSDCOwned(std::move(oin), oout, {}, &ostats, &ferr);
    assert(ook);
    assert(ostats.arrays_passed_through == fstats.arrays_passed_through);
    assert(oout.size() == fout.size());
    std::cout << "  owned facade path matches (passthrough="
              << ostats.arrays_passed_through << ")" << std::endl;
  }

  // ---- 8.3: mmap-backed CrateDataSource ------------------------------------
  {
    const char* path = "/tmp/next_lazy_mmap.usdc";
    {
      std::ofstream f(path, std::ios::binary);
      f.write(reinterpret_cast<const char*>(buf.data()),
              static_cast<std::streamsize>(buf.size()));
    }

    // Default load path memory-maps the file. Lazy values read straight from
    // the mapping; their CrateDataSource reports is_mmapped().
    USDCLoadResult lm = LoadUSDCFromFile(path);
    assert(lm.success);
    const PrimSpec* mps = lm.stage.GetRootLayer()->prim_at_path("/Mesh1");
    assert(mps);
    const Value* mpv = mps->property_value("points");
    assert(mpv && mpv->is_lazy());
    assert(mpv->lazy_ref()->source->is_mmapped() &&
           "default file load should be mmap-backed");
    const std::vector<float>* mpa = mpv->as_float_array();
    assert(mpa && mpa->size() == points.size());
    for (size_t i = 0; i < points.size(); i++) assert((*mpa)[i] == points[i]);
    // The mapping must outlive the reader: a lazy value materialized after the
    // load (above) already proved the shared_ptr keeps the mapping alive.
    assert(lm.source_was_mmap);
    std::cout << "  file load is mmap-backed and materializes correctly"
              << std::endl;

    // Opting out falls back to an owned heap buffer (not mmapped), same data.
    USDCLoadOptions opt;
    opt.crate_options.use_mmap = false;
    USDCLoadResult lo = LoadUSDCFromFile(path, opt);
    assert(lo.success);
    assert(!lo.source_was_mmap);
    const Value* opv =
        lo.stage.GetRootLayer()->prim_at_path("/Mesh1")->property_value("points");
    assert(opv && opv->is_lazy());
    assert(!opv->lazy_ref()->source->is_mmapped() &&
           "use_mmap=false must use the owned buffer");
    const std::vector<float>* opa = opv->as_float_array();
    assert(opa && *opa == *mpa);
    std::cout << "  use_mmap=false falls back to owned buffer, same data"
              << std::endl;

    pipeline::FlattenStats mmap_stats;
    std::vector<uint8_t> mmap_out;
    std::string mmap_err;
    assert(pipeline::FlattenUSDFileToUSDC(path, mmap_out, {}, &mmap_stats,
                                          &mmap_err));
    assert(mmap_stats.input_was_mmap);
    std::cout << "  file-path flatten reports mmap input attribution"
              << std::endl;

    std::remove(path);
  }

  std::cout << "All lazy array tests passed!" << std::endl;
  return 0;
}
