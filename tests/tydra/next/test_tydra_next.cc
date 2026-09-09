// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// Tydra Next - Unit Tests

#include <algorithm>
#include <iostream>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <limits>
#include <unordered_set>
#include <atomic>
#include <thread>

#include "tydra/next/chunked-array.hh"
#include "tydra/next/materialx.hh"
#include "tydra/next/render-data.hh"
#include "tydra/next/scene-access.hh"
#include "tydra/next/render-extract.hh"
#include "tydra/next/render-converter.hh"
#include "tydra/next/mem-budget.hh"
#include "tydra/next/usdz-entry-match.hh"
#include "tydra/next/render-session.hh"
#include "tydra/next/resource-budget.hh"
#include "tydra/next/texture-cache.hh"
#include "tydra/next/urdf-to-usd.hh"
#include "next/pcp/cache.hh"
#include "next/reader/usda-reader.hh"
#include "next/schema/geom-mesh.hh"
#include "next/schema/geom-point-instancer.hh"
#include "next/schema/color-space.hh"
#include "next/schema/geom-xform.hh"
#include "next/schema/usd-skel.hh"

using namespace lightusd::tydra::next;
using namespace lightusd::next;

//
// ChunkedArray Tests
//

void TestChunkedArrayBasic() {
  std::cout << "Testing ChunkedArray basic operations...\n";

  ChunkedArray<float> arr;
  assert(arr.empty());
  assert(arr.size() == 0);

  // Push elements
  arr.push_back(1.0f);
  arr.push_back(2.0f);
  arr.push_back(3.0f);

  assert(arr.size() == 3);
  assert(!arr.empty());
  assert(arr[0] == 1.0f);
  assert(arr[1] == 2.0f);
  assert(arr[2] == 3.0f);

  // Front/back
  assert(arr.front() == 1.0f);
  assert(arr.back() == 3.0f);

  std::cout << "  ChunkedArray basic: PASSED\n";
}

void TestChunkedArrayShrinkToFit() {
  std::cout << "Testing ChunkedArray shrink_to_fit (exact-size tail)...\n";

  // Small array: the 64KB minimum chunk must compact to the exact size.
  // (Thousands of small meshes each paying full chunks OOM'd wasm32.)
  ChunkedArray<float> small;
  for (size_t i = 0; i < 100; ++i) small.push_back(static_cast<float>(i));
  assert(small.memory_usage() >= 64 * 1024);
  small.shrink_to_fit();
  assert(small.memory_usage() < 100 * sizeof(float) + 2 * sizeof(small));
  assert(small.size() == 100);
  for (size_t i = 0; i < 100; ++i) {
    assert(small[i] == static_cast<float>(i));
  }

  // Appending after shrink transparently re-expands the tail chunk.
  small.push_back(100.0f);
  assert(small.size() == 101);
  for (size_t i = 0; i <= 100; ++i) {
    assert(small[i] == static_cast<float>(i));
  }

  // Multi-chunk array: full chunks stay, only the tail compacts; data intact.
  constexpr size_t kPerChunk = ChunkedArray<float>::kElementsPerChunk;
  const size_t n = kPerChunk * 2 + 7;
  ChunkedArray<float> big;
  for (size_t i = 0; i < n; ++i) big.push_back(static_cast<float>(i));
  big.shrink_to_fit();
  assert(big.size() == n);
  assert(big.chunk_count() == 3);
  assert(big.memory_usage() <
         2 * 64 * 1024 + 7 * sizeof(float) + 2 * sizeof(big));
  for (size_t i = 0; i < n; ++i) {
    assert(big[i] == static_cast<float>(i));
  }
  std::vector<float> flat = big.flatten();
  assert(flat.size() == n);
  assert(flat[n - 1] == static_cast<float>(n - 1));

  // Exact multiple of the chunk size: nothing to compact, nothing lost.
  ChunkedArray<float> exact;
  exact.resize(kPerChunk, 3.0f);
  exact.shrink_to_fit();
  assert(exact.size() == kPerChunk);
  assert(exact[kPerChunk - 1] == 3.0f);

  // Empty array: shrink frees everything.
  ChunkedArray<float> reserved;
  reserved.reserve(kPerChunk * 4);
  reserved.shrink_to_fit();
  assert(reserved.memory_usage() <= sizeof(reserved));
  assert(reserved.empty());

  std::cout << "  ChunkedArray shrink_to_fit: PASSED\n";
}

void TestChunkedArrayAllocFailure() {
  std::cout << "Testing ChunkedArray allocation-failure semantics...\n";

  // An impossible growth request must fail cleanly (nothrow) instead of
  // aborting: false return, alloc_failed() latched, contents untouched.
  ChunkedArray<float> arr;
  for (size_t i = 0; i < 10; ++i) arr.push_back(static_cast<float>(i));
  assert(!arr.alloc_failed());

  const size_t impossible = (std::numeric_limits<size_t>::max)() / 8;
  assert(!arr.resize(impossible));
  assert(arr.alloc_failed());
  assert(arr.size() == 10);
  for (size_t i = 0; i < 10; ++i) {
    assert(arr[i] == static_cast<float>(i));
  }

  // reserve/append on the same impossible scale also fail cleanly.
  assert(!arr.reserve(impossible));
  float dummy = 0.0f;
  assert(arr.append(&dummy, 0));  // zero-count append is a no-op success
  assert(!arr.append(&dummy, (std::numeric_limits<size_t>::max)()));
  assert(arr.size() == 10);
  assert(!arr.append(nullptr, 1));

  // Small growth still works after a failed attempt (failure is latched for
  // inspection but does not poison the array).
  const size_t idx = arr.push_back(10.0f);
  assert(idx == 10);
  assert(arr.size() == 11);
  assert(arr[10] == 10.0f);

  std::cout << "  ChunkedArray allocation failure: PASSED\n";
}

void TestChunkedArrayStorageEdges() {
  // Repeated tail compaction must preserve capacity and budget accounting.
  const size_t saved_cap = MemBudget::Get().Cap();
  MemBudget::Get().InitBytes(size_t(1) << 30);
  const size_t base = MemBudget::Get().Tracked();
  MemBudget::InstallChunkedArrayTracking();
  {
    ChunkedArray<uint32_t> a;
    assert(a.resize(17, 42));
    a.shrink_to_fit();
    const size_t compact = MemBudget::Get().Tracked();
    a.shrink_to_fit();
    assert(a.capacity() == 17);
    assert(MemBudget::Get().Tracked() == compact);
    assert(a.resize(9));
    a.shrink_to_fit();
    assert(a.capacity() == 9);
    assert(MemBudget::Get().Tracked() == base + 9 * sizeof(uint32_t));

    // An append source may be a borrowed view of the tail being expanded.
    const auto& read = a;
    assert(a.append(read.chunk_data(0), 9));
    assert(a.size() == 18);
    for (size_t i = 0; i < a.size(); ++i) assert(read[i] == 42);
    ChunkedArray<uint32_t> b;
    b.share_from(a);
    const auto& shared = b;
    assert(b.append(shared.chunk_data(0), 18));
    assert(a.size() == 18 && b.size() == 36);
    for (size_t i = 0; i < b.size(); ++i) assert(shared[i] == 42);

    ChunkedArray<uint32_t> moved(std::move(b));
    assert(b.empty() && b.capacity() == 0);
    assert(b.push_back(7) == 0);
    assert(moved.size() == 36 && moved[0] == 42);
  }
  assert(MemBudget::Get().Tracked() == base);
  MemBudget::UninstallChunkedArrayTracking();
  MemBudget::Get().InitBytes(saved_cap);

  struct alignas(64) Wide { uint32_t values[16]; };
  ChunkedArray<Wide, 256> aligned;
  const Wide value{{123}};
  assert(aligned.resize(5, value));
  aligned.shrink_to_fit();
  assert(reinterpret_cast<uintptr_t>(aligned.chunk_data(0)) % 64 == 0);
  assert(reinterpret_cast<uintptr_t>(aligned.chunk_data(1)) % 64 == 0);
  ChunkedArray<Wide, 256> copy;
  copy.share_from(aligned);
  assert(std::distance(copy.begin(), copy.end()) == 5);
  assert(copy.is_shared());
  copy.mutable_chunk_data(1)[0].values[0] = 234;
  assert(aligned[4].values[0] == 123 && copy[4].values[0] == 234);
  copy.mutable_at(4).values[0] = 456;
  assert(aligned[4].values[0] == 123 && copy[4].values[0] == 456);
}

// Regression: `&arr[i]` followed by pointer indexing read past the chunk
// allocation. FloatChunked holds 16384 elements per 64KB chunk and
// 16384 % 3 == 1, so an xyz triple straddles a chunk boundary every ~5461
// points; the tail chunk is exact-sized, so even a 2-element run overruns
// there. read2()/read3() must return the right values across every boundary.
void TestChunkedArrayStraddlingReads() {
  std::cout << "Testing ChunkedArray straddling multi-element reads...\n";

  constexpr size_t kPerChunk = ChunkedArray<float>::kElementsPerChunk;
  static_assert(kPerChunk % 3 == 1, "test targets the straddling geometry");

  // Enough points to cross several chunk boundaries, with a partial tail.
  const size_t npoints = kPerChunk * 2 + 777;
  ChunkedArray<float> pts;
  pts.reserve(npoints * 3);
  for (size_t i = 0; i < npoints * 3; ++i) {
    pts.push_back(static_cast<float>(i));
  }
  assert(pts.chunk_count() > 1);

  for (size_t p = 0; p < npoints; ++p) {
    float xyz[3];
    pts.read3(p * 3, xyz);
    for (int c = 0; c < 3; ++c) {
      assert(xyz[c] == static_cast<float>(p * 3 + c));
    }
  }

  // read2 over the very last pair, i.e. right against the exact-sized tail.
  ChunkedArray<float> uv;
  const size_t nuv = kPerChunk + 5;
  uv.reserve(nuv * 2);
  for (size_t i = 0; i < nuv * 2; ++i) uv.push_back(static_cast<float>(i));
  for (size_t p = 0; p < nuv; ++p) {
    float st[2];
    uv.read2(p * 2, st);
    assert(st[0] == static_cast<float>(p * 2));
    assert(st[1] == static_cast<float>(p * 2 + 1));
  }

  std::cout << "  ChunkedArray straddling reads passed!\n";
}

// Copy-on-write sharing: share_from() must alias the chunks (O(1), no byte
// duplication) and the FIRST write through EITHER holder must take a private
// copy so the other is unaffected.
void TestChunkedArrayShareCow() {
  std::cout << "Testing ChunkedArray copy-on-write sharing...\n";

  constexpr size_t N = 50000;  // spans many chunks
  ChunkedArray<uint32_t> a;
  for (size_t i = 0; i < N; ++i) a.push_back(static_cast<uint32_t>(i));
  assert(!a.is_shared());

  ChunkedArray<uint32_t> b;
  b.share_from(a);
  assert(b.size() == N);
  assert(a.is_shared() && b.is_shared());
  // Read-only views never detach, even on a mutable array.
  {
    const ChunkedArray<uint32_t>& ca = a;
    const ChunkedArray<uint32_t>& cb = b;
    assert(ca.chunk_data(0) == cb.chunk_data(0));
  }
  for (size_t i = 0; i < N; i += 997) assert(b[i] == static_cast<uint32_t>(i));

  // Write through the clone: it detaches, the original keeps its values.
  b.mutable_at(42) = 999999u;
  assert(b[42] == 999999u);
  assert(a[42] == 42u);
  {
    const ChunkedArray<uint32_t>& ca = a;
    const ChunkedArray<uint32_t>& cb = b;
    assert(ca.chunk_data(0) != cb.chunk_data(0));
  }
  for (size_t i = 0; i < N; i += 997) {
    if (i == 42) continue;
    assert(a[i] == static_cast<uint32_t>(i));
    assert(b[i] == static_cast<uint32_t>(i));
  }

  // Writing through the ORIGINAL of a live share must detach too.
  ChunkedArray<uint32_t> c;
  c.share_from(a);
  a.mutable_at(7) = 12345u;
  assert(a[7] == 12345u);
  assert(c[7] == 7u);

  // append() after clear() writes into existing capacity -- it must still
  // detach, or it would scribble over the array it shares with.
  ChunkedArray<uint32_t> d;
  d.share_from(c);
  d.clear();
  const uint32_t fill[4] = {8, 8, 8, 8};
  d.append(fill, 4);
  assert(d.size() == 4 && d[0] == 8u);
  assert(c.size() == N && c[0] == 0u);

  // Sharing an empty array is a no-op, not a crash.
  ChunkedArray<uint32_t> e, f;
  f.share_from(e);
  assert(f.size() == 0 && !f.is_shared());

  std::cout << "  ChunkedArray copy-on-write sharing passed!\n";
}

// A failed copy-on-write detachment must not let a recoverable mutator write
// into the still-shared chunks. This happens under a tight conversion budget.
void TestChunkedArrayShareCowBudgetFailure() {
  std::cout << "Testing ChunkedArray copy-on-write budget failure...\n";

  ChunkedArray<uint32_t> source;
  if (source.push_back(7u) != 0) std::abort();
  ChunkedArray<uint32_t> copy;
  copy.share_from(source);
  assert(source.is_shared() && copy.is_shared());

  const size_t saved_cap = MemBudget::Get().Cap();
  MemBudget::Get().InitBytes(1);
  MemBudget::InstallChunkedArrayTracking();

  const uint32_t appended = 9u;
  const bool append_ok = copy.append(&appended, 1);
  if (append_ok) std::abort();
  assert(copy.alloc_failed());
  assert(copy.size() == 1);
  {
    const ChunkedArray<uint32_t>& source_read = source;
    const ChunkedArray<uint32_t>& copy_read = copy;
    assert(source_read[0] == 7u);
    assert(copy_read[0] == 7u);
  }

  MemBudget::UninstallChunkedArrayTracking();
  MemBudget::Get().InitBytes(saved_cap);

  // The failed clone remains shareable and can detach once resources return.
  copy.mutable_at(0) = 11u;
  assert(copy[0] == 11u);
  assert(source[0] == 7u);

  std::cout << "  ChunkedArray copy-on-write budget failure: PASSED\n";
}

// Budget-tracked chunk allocation. Two properties matter: the accounting
// must BALANCE (every charge released, including through copy-on-write shares
// and shrink_to_fit's tail rewrite), and a refusal must surface as
// alloc_failed() rather than a crash or a silent short array.
void TestChunkedArrayBudgetTracking() {
  std::cout << "Testing ChunkedArray budget tracking...\n";

  const size_t saved_cap = MemBudget::Get().Cap();
  MemBudget::Get().InitBytes(size_t(1) << 30);   // generous
  const size_t base = MemBudget::Get().Tracked();
  MemBudget::InstallChunkedArrayTracking();

  {
    ChunkedArray<float> a;
    for (size_t i = 0; i < 200000; ++i) a.push_back(float(i));
    assert(!a.alloc_failed());
    // The budget now sees the geometry, which is the whole point.
    assert(MemBudget::Get().Tracked() > base);

    ChunkedArray<float> b;
    b.share_from(a);                 // share: no new charge
    const size_t shared = MemBudget::Get().Tracked();
    assert(b[0] == 0.0f && b.chunk_data(0) == a.chunk_data(0));
    assert(MemBudget::Get().Tracked() == shared);
    b.mutable_at(0) = 1.0f;                     // detach: takes a private copy
    assert(MemBudget::Get().Tracked() > shared);

    a.shrink_to_fit();
    b.shrink_to_fit();
  }
  // Everything destroyed -> every charge released.
  assert(MemBudget::Get().Tracked() == base);

  // A cap that cannot be met must be reported, not crashed through.
  MemBudget::Get().InitBytes(1);
  {
    ChunkedArray<float> tiny;
    for (size_t i = 0; i < 100000; ++i) tiny.push_back(float(i));
    assert(tiny.alloc_failed());
    // Whatever it did keep is self-consistent and readable.
    for (size_t i = 0; i < tiny.size(); ++i) (void)tiny[i];
  }

  MemBudget::UninstallChunkedArrayTracking();
  MemBudget::Get().InitBytes(saved_cap);
  // With the hooks removed, allocation is unaffected again.
  {
    ChunkedArray<float> c;
    for (size_t i = 0; i < 100000; ++i) c.push_back(float(i));
    assert(!c.alloc_failed());
  }

  std::cout << "  ChunkedArray budget tracking passed!\n";
}

void TestChunkedArrayLarge() {
  std::cout << "Testing ChunkedArray with large data...\n";

  constexpr size_t N = 100000;
  ChunkedArray<float> arr;
  arr.reserve(N);

  for (size_t i = 0; i < N; ++i) {
    arr.push_back(static_cast<float>(i));
  }

  assert(arr.size() == N);
  assert(arr.chunk_count() > 1);  // Should span multiple chunks

  // Verify data
  for (size_t i = 0; i < N; ++i) {
    assert(arr[i] == static_cast<float>(i));
  }

  // Test flatten
  std::vector<float> flat = arr.flatten();
  assert(flat.size() == N);
  for (size_t i = 0; i < N; ++i) {
    assert(flat[i] == static_cast<float>(i));
  }

  std::cout << "  ChunkedArray large: PASSED\n";
  std::cout << "    Chunk count: " << arr.chunk_count() << "\n";
  std::cout << "    Memory usage: " << arr.memory_usage() / 1024 << " KB\n";
}

void TestChunkedArrayAppend() {
  std::cout << "Testing ChunkedArray append...\n";

  ChunkedArray<int32_t> arr;
  std::vector<int32_t> data = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

  arr.append(data.data(), data.size());

  assert(arr.size() == 10);
  for (size_t i = 0; i < 10; ++i) {
    assert(arr[i] == static_cast<int32_t>(i + 1));
  }

  std::cout << "  ChunkedArray append: PASSED\n";
}

void TestChunkedArrayIterator() {
  std::cout << "Testing ChunkedArray iterator...\n";

  ChunkedArray<int> arr;
  for (int i = 0; i < 100; ++i) {
    arr.push_back(i);
  }

  int sum = 0;
  for (auto it = arr.begin(); it != arr.end(); ++it) {
    sum += *it;
  }
  assert(sum == 4950);  // Sum of 0..99

  // Range-based for
  sum = 0;
  for (int val : arr) {
    sum += val;
  }
  assert(sum == 4950);

  std::cout << "  ChunkedArray iterator: PASSED\n";
}

void TestRenderDataSafety() {
  std::cout << "Testing render-data safety helpers...\n";

  VertexAttribute ints;
  ints.format = VertexFormat::IVec3;
  ints.int_data.append(std::vector<int32_t>{1, 2, 3, 4, 5, 6}.data(), 6);
  assert(ints.element_count() == 2);

  VertexAttribute uints;
  uints.format = VertexFormat::UVec2;
  const uint32_t values[] = {1, 2, 3, 4};
  assert(uints.uint_data.append(values, 4));
  assert(uints.element_count() == 2);

  RenderCamera camera;
  camera.focal_length = 0.0f;
  camera.vertical_aperture = 24.0f;
  camera.horizontal_aperture = 36.0f;
  assert(camera.fov_x() == 0.0f && camera.fov_y() == 0.0f);

  RenderPointInstancer instancer;
  instancer.prototype_paths.resize(1);
  instancer.prototype_node_ids.resize(1);
  instancer.prototype_mesh_offsets = {1, 0};
  assert(instancer.prototype_mesh_count(0) == 0);
  assert(!instancer.has_valid_prototype_mesh_bindings());

  RenderPoints points;
  points.points.push_back(1.0f);
  points.compact();
  assert(!points.has_alloc_failure());
  RenderCurves curves;
  curves.tessellated_points.push_back(1.0f);
  curves.compact();
  assert(!curves.has_alloc_failure());

  std::cout << "  render-data safety helpers: PASSED\n";
}

void TestTextureBudgetLease() {
  std::cout << "Testing resident texture budget leases...\n";
  auto state = std::make_shared<lightusd::next::TextureBudgetState>(8);
  assert(state->try_add(8));
  {
    DecodedImage first;
    first.pixels.resize(8);
    first.budget_lease = std::make_shared<lightusd::next::TextureBudgetLease>(
        state, 8);
    DecodedImage second = first;
    first.budget_lease.reset();
    assert(state->resident.load(std::memory_order_relaxed) == 8);
    second.budget_lease.reset();
  }
  assert(state->resident.load(std::memory_order_relaxed) == 0);
  std::cout << "  resident texture budget leases: PASSED\n";
}

//
// RenderData Tests
//

void TestRenderMesh() {
  std::cout << "Testing RenderMesh...\n";

  RenderMesh mesh;
  mesh.name = "TestMesh";
  mesh.prim_path = "/World/TestMesh";

  // Add a simple quad
  mesh.face_vertex_counts.push_back(4);
  mesh.face_vertex_indices.push_back(0);
  mesh.face_vertex_indices.push_back(1);
  mesh.face_vertex_indices.push_back(2);
  mesh.face_vertex_indices.push_back(3);

  // Add points
  float points[] = {
    -1, -1, 0,
     1, -1, 0,
     1,  1, 0,
    -1,  1, 0
  };
  mesh.points.append(points, 12);

  assert(mesh.point_count() == 4);
  assert(mesh.face_count() == 1);
  assert(!mesh.has_normals());
  assert(!mesh.is_triangulated);

  std::cout << "  RenderMesh: PASSED\n";
  std::cout << "    Memory usage: " << mesh.memory_usage() << " bytes\n";
}

void TestRenderScene() {
  std::cout << "Testing RenderScene...\n";

  RenderScene scene;
  scene.name = "TestScene";
  scene.up_axis = RenderScene::UpAxis::Y;
  scene.meters_per_unit = 0.01f;

  // Add a node
  SceneNode node;
  node.name = "Root";
  node.prim_path = "/Root";
  node.type = NodeType::Xform;
  scene.nodes.push_back(std::move(node));
  scene.root_nodes.push_back(0);

  // Add a mesh
  RenderMesh mesh;
  mesh.name = "Cube";
  mesh.prim_path = "/Root/Cube";
  scene.meshes.push_back(std::move(mesh));

  auto stats = scene.get_stats();
  assert(stats.node_count == 1);
  assert(stats.mesh_count == 1);
  assert(scene.get_node(0) == &scene.nodes[0]);
  assert(scene.get_mesh(0) == &scene.meshes[0]);
  assert(scene.get_node(-1) == nullptr);
  assert(scene.get_mesh(9) == nullptr);
  assert(scene.get_material(-1) == nullptr);
  assert(scene.get_point_instancer(0) == nullptr);
  assert(scene.get_point_instance_draw(0) == nullptr);

  std::cout << "  RenderScene: PASSED\n";
}

//
// Scene Access Tests
//

void TestSceneAccess() {
  std::cout << "Testing Scene Access...\n";

  // Create a simple stage
  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
)

def Xform "World"
{
    def Mesh "Cube"
    {
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
        point3f[] points = [(-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (-0.5, 0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, -0.5), (0.5, 0.5, -0.5), (-0.5, -0.5, -0.5), (0.5, -0.5, -0.5)]
    }

    def Camera "MainCamera"
    {
        float focalLength = 50
        float horizontalAperture = 36
        float verticalAperture = 24
    }
}
)";

  LoadResult result = LoadUSDAFromString(usda, std::strlen(usda));
  if (!result.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: " << result.error_summary << ")\n";
    return;
  }

  Stage& stage = result.stage;

  // Find meshes
  auto meshes = FindMeshes(stage);
  assert(meshes.size() == 1);
  assert(meshes[0].GetName() == "Cube");
  assert(::lightusd::next::IsMesh(meshes[0]));

  // Find cameras
  auto cameras = FindCameras(stage);
  assert(cameras.size() == 1);
  assert(cameras[0].GetName() == "MainCamera");
  assert(IsCamera(cameras[0]));

  // Test attribute access
  float focal_length = 0;
  bool got_fl = GetFloat(cameras[0], "focalLength", &focal_length);
  assert(got_fl);
  assert(std::abs(focal_length - 50.0f) < 0.001f);

  std::cout << "  Scene Access: PASSED\n";
}

void TestDeepSceneAccess() {
  std::cout << "Testing deep Scene Access traversal...\n";

  // USDA input is depth-limited, but public builders can create deeper valid
  // layers. This used to recurse once per prim in GetDescendants and could
  // exhaust the comparatively small WASM stack.
  constexpr size_t kDepth = 8192;
  StageBuilder builder;
  LayerBuilder& layer = builder.GetLayerBuilder();
  for (size_t i = 0; i < kDepth; ++i) {
    layer.begin_prim("P", "Xform");
  }
  layer.begin_prim("Leaf", "Mesh");
  Stage stage = builder.Build();
  UsdPrim root = stage.GetRootPrims().front();
  const std::vector<UsdPrim> descendants = GetDescendants(root);
  assert(descendants.size() == kDepth);

  std::vector<UsdPrim> meshes;
  GatherMeshPrims(root, &meshes);
  assert(meshes.size() == 1);
  assert(meshes.front().GetName() == "Leaf");

  RenderExtractOptions extract_options;
  extract_options.collect_records = false;
  extract_options.max_depth = 0;  // Exercise the iterative path past the default cap.
  RenderExtractResult extracted;
  assert(CollectRenderPrims(stage, extract_options, &extracted));
  assert(extracted.meshes.size() == 1);
  assert(extracted.meshes.front().prim.GetName() == "Leaf");

  size_t traversed = 0;
  stage.Traverse([&](const UsdPrim&) {
    ++traversed;
    return true;
  });
  assert(traversed == kDepth + 1);
  std::cout << "  Deep Scene Access: PASSED\n";
}

void TestRenderExtract() {
  std::cout << "Testing RenderExtract...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    token purpose = "render"

    def Mesh "MeshA"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }

    def Mesh "HiddenMesh" (
        hidden = true
    )
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]

        def Mesh "ChildMesh"
        {
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        }
    }

    def PointInstancer "Inst"
    {
        rel prototypes = </World/MeshA>
        point3f[] positions = [(0, 0, 0), (3, 0, 0)]
        int[] protoIndices = [0, 0]
        quatf[] orientations = [(1, 0, 0, 0), (1, 0, 0, 0)]
        float3[] scales = [(1, 1, 1), (2, 2, 2)]
        float3[] velocities = [(0, 0, 1), (0, 0, 2)]
        float3[] angularVelocities = [(0, 1, 0), (0, 2, 0)]
        int64[] ids = [10, 11]
        int64[] invisibleIds = [11]
    }

    def Camera "Cam" {}
    def RectLight "Key" {}
    def Material "Mat" {}
    def Volume "Fog" {}
    def BasisCurves "Curve" {}
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  if (!lr.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: " << lr.error_summary << ")\n";
    return;
  }

  RenderExtractOptions opts;
  opts.time_code = 0.0;
  opts.stop_at_point_instancers = true;
  opts.stop_at_native_instances = true;
  RenderExtractResult er;
  assert(CollectRenderPrims(lr.stage, opts, &er));
  // `hidden` is editor metadata, not render visibility, and does not hide
  // either the marked prim or its children from render extraction.
  assert(er.meshes.size() == 3);
  assert(er.point_instancers.size() == 1);
  assert(er.cameras.size() == 1);
  assert(er.lights.size() == 1);
  assert(er.materials.size() == 1);
  assert(er.volumes.size() == 1);
  assert(er.curves.size() == 1);
  assert(er.meshes[0].purpose == "render");
  assert(er.meshes[0].path == "/World/MeshA");
  PointInstancerData pid;
  assert(ReadPointInstancerData(er.point_instancers[0].prim, 0.0, &pid));
  assert(pid.valid);
  assert(pid.path == "/World/Inst");
  assert(pid.prototypes.size() == 1);
  assert(pid.prototypes[0].str() == "/World/MeshA");
  assert(pid.proto_indices == std::vector<int32_t>({0, 0}));
  assert(pid.positions == std::vector<float>({0, 0, 0, 3, 0, 0}));
  assert(pid.scales == std::vector<float>({1, 1, 1, 2, 2, 2}));
  assert(pid.velocities == std::vector<float>({0, 0, 1, 0, 0, 2}));
  assert(pid.angular_velocities == std::vector<float>({0, 1, 0, 0, 2, 0}));
  assert(pid.ids == std::vector<int64_t>({10, 11}));
  assert(pid.invisible_ids == std::vector<int64_t>({11}));
  assert(pid.transforms.size() == 2);
  assert(pid.transforms[1].matrix[0] == 2.0);
  assert(pid.transforms[1].matrix[12] == 3.0);

  std::unordered_set<std::string> prototypes;
  CollectPrototypePaths(lr.stage, &prototypes);
  assert(prototypes.empty());

  std::cout << "  RenderExtract: PASSED\n";
}

//
// Converter Tests
//

class RetainedStreamSink : public SceneSink {
 public:
  bool BeginScene(RenderScene&& catalog) override {
    scene = std::move(catalog);
    return true;
  }
  bool AddMesh(int32_t id, RenderMesh&& mesh) override {
    if (id < 0 || static_cast<size_t>(id) >= scene.meshes.size()) return false;
    scene.meshes[static_cast<size_t>(id)] = std::move(mesh);
    return true;
  }
  bool AddPoints(int32_t id, RenderPoints&& points) override {
    if (id < 0 || static_cast<size_t>(id) >= scene.points.size()) return false;
    scene.points[static_cast<size_t>(id)] = std::move(points);
    return true;
  }
  bool AddCurves(int32_t id, RenderCurves&& curves) override {
    if (id < 0 || static_cast<size_t>(id) >= scene.curves.size()) return false;
    scene.curves[static_cast<size_t>(id)] = std::move(curves);
    return true;
  }
  bool EndScene() override { return true; }

  RenderScene scene;
};

class SelectiveStreamSink : public RetainedStreamSink {
 public:
  GeometryDisposition SelectGeometry(const GeometryInfo& info) override {
    inspected.push_back(info);
    if (cancel_on_first) return GeometryDisposition::Cancel;
    if (info.prim_path == "/World/Plane") {
      return GeometryDisposition::Proxy;
    }
    return GeometryDisposition::Skip;
  }
  void AbortScene() override { aborted = true; }

  bool cancel_on_first = false;
  bool aborted = false;
  std::vector<GeometryInfo> inspected;
};

void TestRenderConverter() {
  std::cout << "Testing RenderConverter...\n";

  const ResourceBudget target_budget =
      ComputeResourceBudget(GiB(32), GiB(16));
  assert(target_budget.host_limit == GiB(30));
  assert(target_budget.vram_limit == GiB(8));
  assert(target_budget.upload_staging_limit == MiB(512));
  assert(target_budget.gpu_geometry_limit > target_budget.gpu_texture_limit);
  assert(ComputeVramLimit(GiB(8)) == GiB(6));
  assert(ComputeVramLimit(GiB(10)) == GiB(8));
  assert(ComputeVramLimit(GiB(24)) == GiB(12));
  const ResourceBudget extreme_budget = ComputeResourceBudget(
      (std::numeric_limits<uint64_t>::max)(),
      (std::numeric_limits<uint64_t>::max)());
  assert(extreme_budget.host_limit <=
         (std::numeric_limits<uint64_t>::max)());
  assert(extreme_budget.gpu_texture_limit <=
         (std::numeric_limits<uint64_t>::max)());

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
    upAxis = "Y"
    metersPerUnit = 0.01
)

def Xform "World"
{
    def Xform "Animated"
    {
        double3 xformOp:translate.timeSamples = {
            0: (0, 0, 0),
            1: (1, 2, 3),
        }
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }

    def Xform "Rotating"
    {
        double xformOp:rotateY.timeSamples = {
            0: 0,
            1: 90,
        }
        uniform token[] xformOpOrder = ["xformOp:rotateY"]
    }

    def Xform "Hidden"
    {
        token visibility = "invisible"

        def Mesh "HiddenPlane"
        {
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        }
    }

    def Mesh "Plane"
    {
        rel material:binding = </World/AnimatedMaterial>
        float3[] extent = [(-1, 0, -1), (1, 0, 1)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
        normal3f[] normals = [(0, 1, 0), (0, 1, 0), (0, 1, 0), (0, 1, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
    }

    def Xform "ProtoGroup"
    {
        rel material:binding = </World/AnimatedMaterial>

        def Mesh "NestedPlane"
        {
            double3 xformOp:translate = (1, 0, 0)
            uniform token[] xformOpOrder = ["xformOp:translate"]
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            point3f[] points = [(0, 0, 0), (0, 0, 1), (1, 0, 0)]
        }
    }

    def PointInstancer "Inst"
    {
        rel prototypes = [</World/Plane>, </World/ProtoGroup>]
        point3f[] positions = [(0, 0, 0), (4, 0, 0), (8, 0, 0)]
        int[] protoIndices = [0, 1, 1]
        quatf[] orientations = [(1, 0, 0, 0), (1, 0, 0, 0), (1, 0, 0, 0)]
        float3[] scales = [(1, 1, 1), (0.5, 0.5, 0.5), (2, 2, 2)]
        float3[] velocities = [(0, 0, 1), (0, 0, 2), (0, 0, 3)]
        float3[] angularVelocities = [(0, 1, 0), (0, 2, 0), (0, 3, 0)]
        int64[] ids = [20, 21, 22]
        int64[] invisibleIds = [22]
    }

    def Skeleton "Rig"
    {
        uniform token[] joints = ["Root", "Root/Spine"]
    }

    def Camera "AnimatedCamera" (
        prepend apiSchemas = ["BackPlateAPI:hero"]
    )
    {
        double shutter:open = -0.25
        double shutter:close = 0.25
        asset backPlate:hero:image = @plates/hero.exr@
        asset backPlate:hero:alpha:image = @plates/hero-alpha.exr@
        float2 backPlate:hero:scale:tweak = (1.25, 0.75)
        float3 backPlate:hero:luma:gain = (1.1, 1.2, 1.3)
        token backPlate:hero:plateVisibility = "render"
        float focalLength.timeSamples = {
            0: 35,
            1: 50,
        }
    }

    def SphereLight "AnimatedLight"
    {
        float intensity.timeSamples = {
            0: 10,
            1: 20,
        }
    }

    def Material "AnimatedMaterial"
    {
        float renderer:opacity.timeSamples = {
            0: 1,
            1: 0.5,
        }
        def Shader "PreviewSurface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.18, 0.18, 0.18)
        }
    }
}

)";

  LoadResult load_result = LoadUSDAFromString(usda, std::strlen(usda));
  if (!load_result.success) {
    std::cout << "  SKIPPED (failed to parse test USDA)\n";
    return;
  }

  ConverterConfig config;
  config.mesh.triangulate = true;
  config.mesh.compute_normals = false;  // Already has normals

  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(load_result.stage);

  if (!result.success) {
    std::cout << "  FAILED: " << result.error << "\n";
    return;
  }

  RenderSceneConverter stream_converter(config);
  RetainedStreamSink stream_sink;
  StreamConvertResult streamed =
      stream_converter.ConvertToSink(load_result.stage, &stream_sink);
  assert(streamed.success);
  assert(stream_sink.scene.meshes.size() == result.scene.meshes.size());
  assert(stream_sink.scene.materials.size() == result.scene.materials.size());
  assert(stream_sink.scene.point_instancers.size() ==
         result.scene.point_instancers.size());
  assert(streamed.mesh_count == result.scene.meshes.size());

  RenderSceneConverter selective_converter(config);
  SelectiveStreamSink selective_sink;
  StreamConvertResult selected =
      selective_converter.ConvertToSink(load_result.stage, &selective_sink);
  assert(selected.success);
  assert(selected.mesh_count == 1);
  assert(!selective_sink.aborted);
  assert(!selective_sink.inspected.empty());
  const auto selected_info = std::find_if(
      selective_sink.inspected.begin(), selective_sink.inspected.end(),
      [](const GeometryInfo& info) {
        return info.prim_path == "/World/Plane";
      });
  assert(selected_info != selective_sink.inspected.end());
  assert(selected_info->point_count == 4);
  assert(selected_info->index_count == 4);
  assert(selected_info->estimated_resident_bytes > 0);
  const int32_t selected_plane_id =
      selective_sink.scene.mesh_by_path.at("/World/Plane");
  const RenderMesh& selected_plane =
      selective_sink.scene.meshes[static_cast<size_t>(selected_plane_id)];
  assert(selected_plane.is_proxy);
  assert(selected_plane.point_count() == 8);
  assert(selected_plane.triangulated_indices.size() == 36);

  RenderSceneConverter cancelled_converter(config);
  SelectiveStreamSink cancelled_sink;
  cancelled_sink.cancel_on_first = true;
  StreamConvertResult cancelled =
      cancelled_converter.ConvertToSink(load_result.stage, &cancelled_sink);
  assert(!cancelled.success);
  assert(cancelled.cancelled);
  assert(cancelled_sink.aborted);
  assert(cancelled.mesh_count == 0);

  assert(result.scene.meshes.size() == 3);
  assert(result.scene.point_instancers.size() == 1);
  assert(result.scene.get_stats().point_instancer_count == 1);
  assert(result.scene.get_stats().point_instance_count == 3);
  assert(result.scene.get_stats().visible_point_instance_count == 2);
  assert(result.scene.get_stats().point_instance_draw_count == 2);
  assert(result.scene.has_valid_point_instance_draw_ranges());

  const RenderMesh* mesh = &result.scene.meshes[0];
  if (mesh->name != "Plane") {
    mesh = &result.scene.meshes[1];
  }
  assert(mesh->name == "Plane");
  assert(mesh->point_count() == 4);
  assert(mesh->is_triangulated);
  assert(mesh->triangulated_indices.size() == 6);  // 2 triangles

  // Verify triangulation
  // Quad 0,1,2,3 should become triangles (0,1,2) and (0,2,3)
  assert(mesh->triangulated_indices[0] == 0);
  assert(mesh->triangulated_indices[1] == 1);
  assert(mesh->triangulated_indices[2] == 2);
  assert(mesh->triangulated_indices[3] == 0);
  assert(mesh->triangulated_indices[4] == 2);
  assert(mesh->triangulated_indices[5] == 3);

  auto plane_node_it = result.scene.node_by_path.find("/World/Plane");
  assert(plane_node_it != result.scene.node_by_path.end());
  assert(result.scene.nodes[plane_node_it->second].data_id >= 0);
  assert(static_cast<size_t>(result.scene.nodes[plane_node_it->second].data_id) <
         result.scene.meshes.size());
  assert(result.scene.meshes[result.scene.nodes[plane_node_it->second].data_id].name ==
         "Plane");
  assert(result.scene.meshes[result.scene.nodes[plane_node_it->second].data_id].material_id >=
         0);

  auto inst_node_it = result.scene.node_by_path.find("/World/Inst");
  assert(inst_node_it != result.scene.node_by_path.end());
  const SceneNode& inst_node = result.scene.nodes[inst_node_it->second];
  assert(inst_node.type == NodeType::PointInstancer);
  assert(inst_node.data_id == 0);
  const RenderPointInstancer& instancer = result.scene.point_instancers[0];
  assert(instancer.name == "Inst");
  assert(instancer.prim_path == "/World/Inst");
  assert(instancer.valid);
  assert(instancer.prototype_paths ==
         std::vector<std::string>({"/World/Plane", "/World/ProtoGroup"}));
  assert(instancer.prototype_node_ids.size() == 2);
  assert(instancer.prototype_node_ids[0] >= 0);
  assert(instancer.prototype_node_ids[1] >= 0);
  assert(instancer.prototype_mesh_offsets == std::vector<uint32_t>({0, 1, 2}));
  assert(instancer.has_valid_prototype_mesh_bindings());
  assert(instancer.prototype_mesh_ids.size() == 2);
  assert(instancer.prototype_mesh_transforms.size() == 2);
  assert(instancer.prototype_mesh_count(0) == 1);
  assert(instancer.prototype_mesh_count(1) == 1);
  assert(std::abs(instancer.prototype_mesh_transforms[1].m[12] - 1.0f) < 0.001f);
  assert(instancer.proto_indices == std::vector<int32_t>({0, 1, 1}));
  assert(instancer.positions == std::vector<float>({0, 0, 0, 4, 0, 0, 8, 0, 0}));
  assert(instancer.scales == std::vector<float>({1, 1, 1, 0.5f, 0.5f, 0.5f, 2, 2, 2}));
  assert(instancer.velocities == std::vector<float>({0, 0, 1, 0, 0, 2, 0, 0, 3}));
  assert(instancer.angular_velocities == std::vector<float>({0, 1, 0, 0, 2, 0, 0, 3, 0}));
  assert(instancer.has_velocities());
  assert(instancer.has_angular_velocities());
  assert(instancer.ids == std::vector<int64_t>({20, 21, 22}));
  assert(instancer.invisible_ids == std::vector<int64_t>({22}));
  assert(instancer.instance_visible == std::vector<uint8_t>({1, 1, 0}));
  assert(instancer.visible_instance_count() == 2);
  assert(instancer.transforms.size() == 3);
  assert(std::abs(instancer.transforms[1].m[0] - 0.5f) < 0.001f);
  assert(std::abs(instancer.transforms[1].m[12] - 4.0f) < 0.001f);
  assert(result.scene.point_instance_draws.size() == 2);
  assert(instancer.draw_start == 0);
  assert(instancer.draw_count == 2);
  assert(instancer.has_valid_draw_range(result.scene.point_instance_draws.size()));
  assert(result.scene.get_point_instancer(0) == &result.scene.point_instancers[0]);
  assert(result.scene.get_point_instance_draw(0) == &result.scene.point_instance_draws[0]);
  assert(result.scene.get_point_instance_draw(9) == nullptr);
  RenderPointInstanceDrawRange draw_range = result.scene.get_point_instancer_draws(0);
  assert(draw_range.size == 2);
  assert(!draw_range.empty());
  assert(draw_range.data == result.scene.point_instance_draws.data());
  assert(draw_range.begin() == result.scene.point_instance_draws.data());
  assert(draw_range.end() == result.scene.point_instance_draws.data() + 2);
  assert(draw_range[1].instance_index == 1);
  assert(result.scene.get_point_instancer_draws(-1).empty());
  assert(result.scene.get_point_instancer_draws(9).empty());
  RenderPointInstanceDrawView draw_view = result.scene.get_point_instance_draw_view(0);
  assert(draw_view.valid());
  assert(draw_view.instancer == &instancer);
  assert(draw_view.mesh == result.scene.get_mesh(result.scene.point_instance_draws[0].mesh_id));
  assert(draw_view.material ==
         result.scene.get_material(result.scene.point_instance_draws[0].material_id));
  assert(!result.scene.get_point_instance_draw_view(9).valid());
  assert(result.scene.point_instance_draws[0].point_instancer_id == 0);
  assert(result.scene.point_instance_draws[0].instance_index == 0);
  assert(result.scene.point_instance_draws[0].prototype_index == 0);
  assert(result.scene.point_instance_draws[0].mesh_id == instancer.prototype_mesh_ids[0]);
  assert(result.scene.get_mesh(result.scene.point_instance_draws[0].mesh_id));
  assert(result.scene.point_instance_draws[0].material_id >= 0);
  assert(result.scene.get_material(result.scene.point_instance_draws[0].material_id));
  assert(result.scene.point_instance_draws[0].expanded_mesh_id == -1);
  assert(result.scene.point_instance_draws[1].point_instancer_id == 0);
  assert(result.scene.point_instance_draws[1].instance_index == 1);
  assert(result.scene.point_instance_draws[1].prototype_index == 1);
  assert(result.scene.point_instance_draws[1].mesh_id == instancer.prototype_mesh_ids[1]);
  assert(result.scene.point_instance_draws[1].material_id >= 0);
  assert(result.scene.point_instance_draws[0].material_id ==
         result.scene.point_instance_draws[1].material_id);
  assert(std::abs(result.scene.point_instance_draws[1].transform.m[12] - 4.5f) < 0.001f);

  ConverterConfig compact_config = config;
  compact_config.point_instancer.compact_instances = true;
  compact_config.point_instancer.retain_source_arrays = false;
  compact_config.point_instancer.build_instance_transforms = false;
  compact_config.point_instancer.build_instance_draws = false;
  RenderSceneConverter compact_converter(compact_config);
  ConvertResult compact_result = compact_converter.Convert(load_result.stage);
  assert(compact_result.success);
  assert(compact_result.scene.point_instancers.size() == 1);
  const RenderPointInstancer& compact_instancer =
      compact_result.scene.point_instancers[0];
  assert(compact_instancer.compact_instances.size() == 3);
  assert(compact_instancer.instance_count() == 3);
  assert(compact_instancer.visible_instance_count() == 2);
  assert(compact_instancer.proto_indices.empty());
  assert(compact_instancer.positions.empty());
  assert(compact_instancer.transforms.empty());
  assert(compact_instancer.instance_visible.empty());
  assert(compact_result.scene.point_instance_draws.empty());
  assert(compact_instancer.compact_instances[1].prototype_index == 1);
  assert(compact_instancer.compact_instances[2].flags == 0);
  assert(compact_instancer.memory_usage() < instancer.memory_usage());

  ConverterConfig duplicate_config = config;
  duplicate_config.point_instancer.duplicate_meshes = true;
  RenderSceneConverter duplicate_converter(duplicate_config);
  ConvertResult duplicate_result = duplicate_converter.Convert(load_result.stage);
  assert(duplicate_result.success);
  assert(duplicate_result.scene.point_instance_draws.size() == 2);
  assert(duplicate_result.scene.meshes.size() == 5);
  assert(duplicate_result.scene.point_instance_draws[0].expanded_mesh_id >= 3);
  assert(duplicate_result.scene.point_instance_draws[1].expanded_mesh_id >= 3);
  RenderPointInstanceDrawView duplicate_view =
      duplicate_result.scene.get_point_instance_draw_view(1);
  assert(duplicate_view.valid());
  assert(duplicate_view.expanded_mesh);
  assert(duplicate_view.expanded_mesh->material_id ==
         duplicate_view.draw->material_id);
  assert(duplicate_view.expanded_mesh->point_count() == duplicate_view.mesh->point_count());
  assert(std::abs(duplicate_view.expanded_mesh->points[0] - 4.5f) < 0.001f);
  assert(std::abs(duplicate_view.expanded_mesh->points[1]) < 0.001f);
  assert(std::abs(duplicate_view.expanded_mesh->points[2]) < 0.001f);
  assert(duplicate_result.scene.has_valid_point_instance_draw_ranges());

  auto inst_lookup_it = result.scene.point_instancer_by_path.find("/World/Inst");
  assert(inst_lookup_it != result.scene.point_instancer_by_path.end());
  assert(inst_lookup_it->second == 0);

  auto hidden_it = result.scene.node_by_path.find("/World/Hidden/HiddenPlane");
  assert(hidden_it != result.scene.node_by_path.end());
  assert(!result.scene.nodes[hidden_it->second].visible);
  assert(result.scene.nodes[hidden_it->second].data_id >= 0);

  const AnimationClip* translate_clip = nullptr;
  const AnimationClip* rotate_clip = nullptr;
  const AnimationClip* camera_clip = nullptr;
  const AnimationClip* light_clip = nullptr;
  const AnimationClip* material_clip = nullptr;
  for (const AnimationClip& clip : result.scene.animations) {
    if (clip.prim_path == "/World/Animated") translate_clip = &clip;
    if (clip.prim_path == "/World/Rotating") rotate_clip = &clip;
    if (clip.prim_path == "/World/AnimatedCamera") camera_clip = &clip;
    if (clip.prim_path == "/World/AnimatedLight") light_clip = &clip;
    if (clip.prim_path == "/World/AnimatedMaterial") material_clip = &clip;
  }
  assert(translate_clip);
  assert(translate_clip->channels.size() == 1);
  assert(translate_clip->channels[0].target_path == AnimationChannel::TargetPath::Translation);
  assert(translate_clip->channels[0].keyframes.size() == 2);
  assert(std::abs(translate_clip->channels[0].keyframes[1].value.x - 1.0f) < 0.001f);
  assert(std::abs(translate_clip->channels[0].keyframes[1].value.y - 2.0f) < 0.001f);
  assert(std::abs(translate_clip->channels[0].keyframes[1].value.z - 3.0f) < 0.001f);

  assert(rotate_clip);
  assert(rotate_clip->channels.size() == 1);
  assert(rotate_clip->channels[0].target_path == AnimationChannel::TargetPath::Rotation);
  assert(rotate_clip->channels[0].keyframes.size() == 2);
  // Euler rotate ops are emitted as xyzw quaternions: 90 deg about Y ->
  // (0, sin45, 0, cos45).
  assert(std::abs(rotate_clip->channels[0].keyframes[1].value.x) < 0.001f);
  assert(std::abs(rotate_clip->channels[0].keyframes[1].value.y - 0.70711f) < 0.001f);
  assert(std::abs(rotate_clip->channels[0].keyframes[1].value.z) < 0.001f);
  assert(std::abs(rotate_clip->channels[0].keyframes[1].value.w - 0.70711f) < 0.001f);

  auto assert_custom_clip = [](const AnimationClip* clip,
                               const char* prop_name,
                               float expected_last) {
    assert(clip);
    assert(clip->channels.size() == 1);
    const AnimationChannel& ch = clip->channels[0];
    assert(ch.target_path == AnimationChannel::TargetPath::CustomProperty);
    assert(ch.property_name == prop_name);
    assert(ch.target_prim_path == clip->prim_path);
    assert(ch.keyframes.size() == 2);
    assert(std::abs(ch.keyframes[1].value.x - expected_last) < 0.001f);
  };
  assert_custom_clip(camera_clip, "focalLength", 50.0f);
  assert_custom_clip(light_clip, "intensity", 20.0f);
  assert_custom_clip(material_clip, "renderer:opacity", 0.5f);
  assert(result.scene.cameras.size() == 1);
  assert(std::fabs(result.scene.cameras[0].shutter_open + 0.25) < 0.001);
  assert(std::fabs(result.scene.cameras[0].shutter_close - 0.25) < 0.001);
  assert(result.scene.cameras[0].back_plates.size() == 1);
  const RenderBackPlate& plate = result.scene.cameras[0].back_plates[0];
  assert(plate.instance_name == "hero");
  assert(plate.image == "plates/hero.exr");
  assert(plate.alpha_image == "plates/hero-alpha.exr");
  assert(std::fabs(plate.scale_tweak.x - 1.25f) < 0.001f);
  assert(std::fabs(plate.scale_tweak.y - 0.75f) < 0.001f);
  assert(std::fabs(plate.luma_gain.z - 1.3f) < 0.001f);
  assert(plate.plate_visibility == "render");

  assert(result.scene.skeletons.size() == 1);
  assert(result.scene.skeletons[0].joints.size() == 2);
  assert(result.scene.skeletons[0].joints[1].parent_id == 0);
  auto rig_node_it = result.scene.node_by_path.find("/World/Rig");
  assert(rig_node_it != result.scene.node_by_path.end());
  assert(result.scene.nodes[rig_node_it->second].data_id == 0);

  // Metadata-only consumers retain scene relationships/material IDs while
  // dropping all bulk mesh arrays after conversion.
  ConverterConfig metadata_config = config;
  metadata_config.mesh.retain_geometry = false;
  RenderSceneConverter metadata_converter(metadata_config);
  ConvertResult metadata_result = metadata_converter.Convert(load_result.stage);
  assert(metadata_result.success);
  assert(metadata_result.scene.meshes.size() == result.scene.meshes.size());
  for (const RenderMesh& metadata_mesh : metadata_result.scene.meshes) {
    assert(metadata_mesh.points.empty());
    assert(metadata_mesh.normals.empty());
    assert(metadata_mesh.texcoords_0.empty());
    assert(metadata_mesh.face_vertex_counts.empty());
    assert(metadata_mesh.face_vertex_indices.empty());
    assert(metadata_mesh.triangulated_indices.empty());
    assert(metadata_mesh.triangulated_face_vertex_indices.empty());
    assert(metadata_result.scene.mesh_by_path.count(metadata_mesh.prim_path) ==
           1);
  }
  const auto metadata_plane =
      metadata_result.scene.mesh_by_path.find("/World/Plane");
  assert(metadata_plane != metadata_result.scene.mesh_by_path.end());
  assert(metadata_result.scene.meshes[metadata_plane->second].material_id >= 0);

  std::cout << "  RenderConverter: PASSED\n";

  // Print warnings
  for (const auto& warn : result.warnings) {
    std::cout << "    Warning: " << warn << "\n";
  }
}

void TestRenderConverterHalfGaussian() {
  std::cout << "Testing half-precision Gaussian conversion...\n";
  const char* usda = R"(#usda 1.0
def ParticleField3DGaussianSplat "Splat"
{
    point3h[] positionsh = [(0, 0, 0), (1, 2, 3)]
    half3[] scalesh = [(1, 1, 1), (2, 3, 4)]
}
)";
  LoadResult loaded = LoadUSDAFromString(usda, std::strlen(usda));
  assert(loaded.success);
  RenderSceneConverter converter;
  ConvertResult result = converter.Convert(loaded.stage);
  assert(result.success);
  assert(result.scene.points.size() == 1);
  const RenderPoints& points = result.scene.points[0];
  assert(points.point_count() == 2);
  assert(points.widths.size() == 2);
  assert(std::fabs(points.widths[0] - 2.0f) < 0.001f);
  assert(std::fabs(points.widths[1] - 8.0f) < 0.001f);
  std::cout << "  half-precision Gaussian conversion: PASSED\n";
}

void TestRenderConverterMaterials() {
  std::cout << "Testing RenderConverter material extraction...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Material "PreviewMat"
    {
        token outputs:surface.connect = </World/PreviewMat/Surface.outputs:surface>

        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.2, 0.4, 0.6)
            float inputs:roughness = 0.25
            float inputs:metallic.connect = </World/PreviewMat/MetallicTex.outputs:r>
            float inputs:opacity = 0.5
            int inputs:useSpecularWorkflow = 1
            token outputs:surface
        }

        def Shader "MetallicTex"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @metallic.png@
            token inputs:wrapS = "repeat"
            token inputs:wrapT = "clamp"
            float4 inputs:scale = (2, 1, 1, 1)
            float4 inputs:bias = (0.1, 0, 0, 0)
            token inputs:sourceColorSpace = "raw"
            float outputs:r
        }
    }

    def Material "OpenPBRMat"
    {
        string config:mtlx:version = "1.39"
        token outputs:surface.connect = </World/OpenPBRMat/PreviewFallback.outputs:surface>
        token outputs:mtlx:surface.connect = </World/OpenPBRMat/Surface.outputs:surface>

        def Shader "PreviewFallback"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (1.0, 1.0, 1.0)
            token outputs:surface
        }

        def Shader "Surface"
        {
            uniform token info:id = "ND_open_pbr_surface_surfaceshader"
            color3f inputs:base_color.connect = </World/OpenPBRMat/NG.outputs:out>
            float inputs:base_metalness = 0.8
            float inputs:base_roughness.connect = </World/OpenPBRMat/NG.outputs:roughness>
            float inputs:specular_anisotropy = 0.11
            float inputs:specular_roughness_anisotropy = 0.12
            float inputs:transmission_dispersion = 0.13
            float inputs:transmission_dispersion_scale = 0.14
            float inputs:coat_anisotropy = 0.15
            float inputs:coat_roughness_anisotropy = 0.16
            float inputs:thin_film_weight = 0.17
            float inputs:thin_film_thickness = 450
            float inputs:thin_film_ior = 1.7
            float inputs:geometry_opacity.connect = </World/OpenPBRMat/NG.outputs:opacity>
            normal3f inputs:geometry_normal.connect = </World/OpenPBRMat/NG.outputs:normal>
            vector3f inputs:geometry_tangent.connect = </World/OpenPBRMat/NG.outputs:tangent>
            token outputs:surface
        }

        def NodeGraph "NG"
        {
            color3f outputs:out.connect = </World/OpenPBRMat/NG/Choose.outputs:out>
            float outputs:roughness.connect = </World/OpenPBRMat/NG/RoughnessSqrt.outputs:out>
            float outputs:opacity.connect = </World/OpenPBRMat/NG/OpacityExtract.outputs:out>
            normal3f outputs:normal.connect = </World/OpenPBRMat/NG/NormalMap.outputs:out>
            vector3f outputs:tangent.connect = </World/OpenPBRMat/NG/Rotate.outputs:out>

            def Shader "Constant"
            {
                uniform token info:id = "ND_constant_color3"
                color3f inputs:value = (1.0, 0.5, 0.1) (
                    colorSpace = "srgb_rec709_scene"
                )
                color3f outputs:out
            }

            def Shader "RoughnessSqrt"
            {
                uniform token info:id = "ND_sqrt_float"
                float inputs:in = 0.25
                float outputs:out
            }

            def Shader "Subtract"
            {
                uniform token info:id = "ND_subtract_color3"
                color3f inputs:in1 = (1.0, 1.0, 1.0)
                color3f inputs:in2.connect = </World/OpenPBRMat/NG/Constant.outputs:out>
                color3f outputs:out
            }

            def Shader "Mix"
            {
                uniform token info:id = "ND_mix_color3"
                color3f inputs:bg.connect = </World/OpenPBRMat/NG/Constant.outputs:out>
                color3f inputs:fg.connect = </World/OpenPBRMat/NG/Subtract.outputs:out>
                float inputs:mix = 1.0
                color3f outputs:out
            }

            # Keep a moderately deep, acyclic graph in this regression. Each
            # node/input/connection hop consumes evaluator depth; the old
            # limit of 16 silently replaced this valid result with white.
            def Shader "Pass1" { uniform token info:id = "ND_convert_color3_color3" color3f inputs:in.connect = </World/OpenPBRMat/NG/Mix.outputs:out> color3f outputs:out }
            def Shader "Pass2" { uniform token info:id = "ND_convert_color3_color3" color3f inputs:in.connect = </World/OpenPBRMat/NG/Pass1.outputs:out> color3f outputs:out }
            def Shader "Pass3" { uniform token info:id = "ND_convert_color3_color3" color3f inputs:in.connect = </World/OpenPBRMat/NG/Pass2.outputs:out> color3f outputs:out }
            def Shader "Pass4" { uniform token info:id = "ND_convert_color3_color3" color3f inputs:in.connect = </World/OpenPBRMat/NG/Pass3.outputs:out> color3f outputs:out }
            def Shader "Pass5" { uniform token info:id = "ND_convert_color3_color3" color3f inputs:in.connect = </World/OpenPBRMat/NG/Pass4.outputs:out> color3f outputs:out }
            def Shader "Pass6" { uniform token info:id = "ND_convert_color3_color3" color3f inputs:in.connect = </World/OpenPBRMat/NG/Pass5.outputs:out> color3f outputs:out }
            def Shader "Pass7" { uniform token info:id = "ND_convert_color3_color3" color3f inputs:in.connect = </World/OpenPBRMat/NG/Pass6.outputs:out> color3f outputs:out }
            def Shader "Pass8" { uniform token info:id = "ND_convert_color3_color3" color3f inputs:in.connect = </World/OpenPBRMat/NG/Pass7.outputs:out> color3f outputs:out }

            def Shader "Choose"
            {
                uniform token info:id = "ND_ifgreatereq_color3"
                float inputs:value1 = 0.5
                float inputs:value2 = 0.5
                color3f inputs:in1.connect = </World/OpenPBRMat/NG/Pass8.outputs:out>
                color3f inputs:in2 = (1, 0, 0)
                color3f outputs:out
            }

            def Shader "NormalMap"
            {
                uniform token info:id = "ND_normalmap_float"
                vector3f inputs:in = (0, 0, 1)
                float inputs:scale = 0.35
                normal3f outputs:out
            }

            def Shader "Normalize"
            {
                uniform token info:id = "ND_normalize_vector3"
                vector3f inputs:in = (2, 0, 0)
                vector3f outputs:out
            }

            def Shader "Rotate"
            {
                uniform token info:id = "ND_rotate3d_vector3"
                vector3f inputs:in.connect = </World/OpenPBRMat/NG/Normalize.outputs:out>
                float inputs:amount = 45
                vector3f outputs:out
            }

            def Shader "OpacityImage"
            {
                uniform token info:id = "ND_image_color4"
                asset inputs:file = @mask.png@
                string inputs:uaddressmode = "mirror"
                color4f outputs:out
            }

            def Shader "OpacityConvert"
            {
                uniform token info:id = "ND_convert_color4_color3"
                color4f inputs:in.connect = </World/OpenPBRMat/NG/OpacityImage.outputs:out>
                color3f outputs:out
            }

            def Shader "OpacityExtract"
            {
                uniform token info:id = "ND_extract_color3"
                color3f inputs:in.connect = </World/OpenPBRMat/NG/OpacityConvert.outputs:out>
                integer inputs:index = 0
                float outputs:out
            }
        }
    }
}
)";

  LoadResult load_result = LoadUSDAFromString(usda, std::strlen(usda));
  if (!load_result.success) {
    std::cout << "  SKIPPED (failed to parse material USDA: "
              << load_result.error_summary << ")\n";
    return;
  }

  ConverterConfig config;
  config.material.load_textures = false;

  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(load_result.stage);
  assert(result.success);
  assert(result.scene.materials.size() == 2);

  auto preview_it = result.scene.material_by_path.find("/World/PreviewMat");
  assert(preview_it != result.scene.material_by_path.end());
  const RenderMaterial& preview = result.scene.materials[preview_it->second];
  assert(preview.shader_type == RenderMaterial::ShaderType::PreviewSurface);
  assert(preview.preview_surface);
  assert(std::abs(preview.preview_surface->diffuse_color.value.x - 0.2f) < 0.001f);
  assert(std::abs(preview.preview_surface->diffuse_color.value.y - 0.4f) < 0.001f);
  assert(std::abs(preview.preview_surface->diffuse_color.value.z - 0.6f) < 0.001f);
  assert(std::abs(preview.preview_surface->roughness.value.x - 0.25f) < 0.001f);
  assert(preview.preview_surface->metallic.is_texture());
  assert(preview.preview_surface->use_specular_workflow);
  assert(preview.alpha_mode == RenderMaterial::AlphaMode::Blend);

  assert(result.scene.images.size() == 2);
  assert(result.scene.textures.size() == 2);
  const int32_t metallic_texture_id = preview.preview_surface->metallic.texture_id;
  assert(metallic_texture_id >= 0);
  assert(static_cast<size_t>(metallic_texture_id) < result.scene.textures.size());
  const RenderTexture& tex = result.scene.textures[metallic_texture_id];
  assert(tex.image_id >= 0);
  assert(static_cast<size_t>(tex.image_id) < result.scene.images.size());
  assert(result.scene.images[tex.image_id].resolved_path == "metallic.png");
  assert(result.scene.images[tex.image_id].color_space == ColorSpace::Raw);
  assert(tex.output_channel == RenderTexture::Channel::R);
  assert(tex.wrap_s == WrapMode::Repeat);
  assert(tex.wrap_t == WrapMode::Clamp);
  assert(std::abs(tex.scale_value.x - 2.0f) < 0.001f);
  assert(std::abs(tex.bias.x - 0.1f) < 0.001f);

  auto openpbr_it = result.scene.material_by_path.find("/World/OpenPBRMat");
  assert(openpbr_it != result.scene.material_by_path.end());
  const RenderMaterial& openpbr = result.scene.materials[openpbr_it->second];
  assert(openpbr.shader_type == RenderMaterial::ShaderType::OpenPBR);
  assert(openpbr.openpbr);
  assert(std::abs(openpbr.openpbr->base_color.value.x - 0.0f) < 0.001f);
  // Each authored source enters the graph's linear working space before node
  // math: 1 - linearized_sRGB(1, 0.5, 0.1).
  assert(std::abs(openpbr.openpbr->base_color.value.y - 0.785959f) < 0.001f);
  assert(std::abs(openpbr.openpbr->base_color.value.z - 0.989977f) < 0.001f);
  assert(std::abs(openpbr.openpbr->base_metalness.value.x - 0.8f) < 0.001f);
  assert(std::abs(openpbr.openpbr->base_roughness.value.x - 0.5f) < 0.001f);
  assert(std::abs(openpbr.openpbr->specular_anisotropy.value.x - 0.11f) < 0.001f);
  assert(std::abs(openpbr.openpbr->specular_roughness_anisotropy.value.x -
                  0.12f) < 0.001f);
  assert(std::abs(openpbr.openpbr->transmission_dispersion.value.x - 0.13f) <
         0.001f);
  assert(std::abs(openpbr.openpbr->transmission_dispersion_scale.value.x -
                  0.14f) < 0.001f);
  assert(std::abs(openpbr.openpbr->coat_anisotropy.value.x - 0.15f) < 0.001f);
  assert(std::abs(openpbr.openpbr->coat_roughness_anisotropy.value.x - 0.16f) <
         0.001f);
  assert(std::abs(openpbr.openpbr->thin_film_weight.value.x - 0.17f) < 0.001f);
  assert(std::abs(openpbr.openpbr->thin_film_thickness.value.x - 450.0f) <
         0.001f);
  assert(std::abs(openpbr.openpbr->thin_film_ior.value.x - 1.7f) < 0.001f);
  assert(std::abs(openpbr.openpbr->normal_map_scale - 0.35f) < 0.001f);
  assert(std::abs(openpbr.openpbr->tangent_rotation - 45.0f) < 0.001f);
  assert(openpbr.openpbr->opacity.is_texture());
  assert(openpbr.alpha_mode == RenderMaterial::AlphaMode::Blend);
  const int32_t opacity_texture_id = openpbr.openpbr->opacity.texture_id;
  assert(opacity_texture_id >= 0);
  assert(static_cast<size_t>(opacity_texture_id) < result.scene.textures.size());
  const RenderTexture& opacity_tex = result.scene.textures[opacity_texture_id];
  assert(opacity_tex.image_id >= 0);
  assert(static_cast<size_t>(opacity_tex.image_id) < result.scene.images.size());
  assert(result.scene.images[opacity_tex.image_id].resolved_path == "mask.png");
  assert(opacity_tex.wrap_s == WrapMode::Mirror);
  // MaterialX image nodedefs default to periodic, unlike UsdUVTexture's
  // useMetadata/clamp fallback. This is required by assets that intentionally
  // author negative or tiled texture coordinates.
  assert(opacity_tex.wrap_t == WrapMode::Repeat);

  std::cout << "  RenderConverter materials: PASSED\n";
}

void TestRenderConverterNestedMaterialXGraph() {
  std::cout << "Testing nested MaterialX NodeGraph retention...\n";

  const char* usda = R"(#usda 1.0
def Xform "World"
{
    def Material "Mat"
    {
        token outputs:mtlx:surface.connect = </World/Mat/Surface.outputs:surface>

        def Shader "Surface"
        {
            uniform token info:id = "ND_open_pbr_surface_surfaceshader"
            color3f inputs:base_color.connect = </World/Mat/Outer.outputs:base>
            float inputs:base_roughness.connect = </World/Mat/Roughness.outputs:value>
            token outputs:surface
        }

        def NodeGraph "Outer"
        {
            color3f outputs:base.connect = </World/Mat/Outer/Inner.outputs:forwarded>

            def NodeGraph "Inner"
            {
                color3f outputs:forwarded.connect = </World/Mat/Outer/Inner/Add.outputs:out>

                def Shader "Const"
                {
                    uniform token info:id = "ND_constant_color3"
                    color3f inputs:value = (0.1, 0.2, 0.3)
                    color3f outputs:out
                }

                def Shader "Add"
                {
                    uniform token info:id = "ND_add_color3"
                    color3f inputs:in1.connect = </World/Mat/Outer/Inner/Const.outputs:out>
                    color3f inputs:in2 = (0.4, 0.3, 0.2)
                    color3f outputs:out
                }
            }
        }

        def NodeGraph "Roughness"
        {
            float outputs:value.connect = </World/Mat/Roughness/Const.outputs:out>
            def Shader "Const"
            {
                uniform token info:id = "ND_constant_float"
                float inputs:value = 0.35
                float outputs:out
            }
        }
    }
}
)";

  LoadResult load_result = LoadUSDAFromString(usda, std::strlen(usda));
  assert(load_result.success);
  RenderSceneConverter converter;
  ConvertResult result = converter.Convert(load_result.stage);
  assert(result.success);
  auto material_it = result.scene.material_by_path.find("/World/Mat");
  assert(material_it != result.scene.material_by_path.end());
  const RenderMaterial& material = result.scene.materials[material_it->second];
  assert(material.openpbr);
  const std::string& graph = material.openpbr->nodegraph_json;
  assert(!graph.empty());
  assert(graph.find("\"name\":\"Outer_Inner_Const\"") != std::string::npos);
  assert(graph.find("\"name\":\"Outer_Inner_Add\"") != std::string::npos);
  assert(graph.find("\"name\":\"Roughness_Const\"") != std::string::npos);
  assert(graph.find("\"nodename\":\"Outer_Inner_Add\"") != std::string::npos);
  assert(graph.find("\"output\":\"Outer_base\"") != std::string::npos);
  assert(graph.find("\"output\":\"Roughness_value\"") != std::string::npos);
  assert(graph.find("\"nodename\":\"Inner\"") == std::string::npos);

  std::cout << "  nested MaterialX NodeGraph retention: PASSED\n";
}

// Regression: a material whose `outputs:surface` connects to a path that is
// absent from the composed stage (Unreal USD exports connect to a
// reference-SOURCE path that vanishes after composition), but which carries
// the real UsdPreviewSurface as a child. The converter must fall back to the
// child shader and must NOT fail the whole scene load. An unsupported custom
// shader child is placed first to exercise candidate iteration.
void TestRenderConverterUnrealSurfaceFallback() {
  std::cout << "Testing RenderConverter Unreal-style surface fallback...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Material "UnrealMat"
    {
        token outputs:surface.connect = </MI_Missing_Source/SurfaceShader.outputs:surface>

        def Shader "UnrealShader"
        {
            uniform token info:id = "UnrealCustomSurface"
            token outputs:surface
        }

        def Shader "SurfaceShader"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.5, 0.3, 0.1)
            float inputs:roughness = 0.4
            token outputs:surface
        }
    }

    def Mesh "M"
    {
        rel material:binding = </World/UnrealMat>
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }
}
)";

  LoadResult load_result = LoadUSDAFromString(usda, std::strlen(usda));
  if (!load_result.success) {
    std::cout << "  SKIPPED (failed to parse Unreal-fallback USDA)\n";
    return;
  }

  ConverterConfig config;
  config.material.load_textures = false;

  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(load_result.stage);

  // The broken outputs:surface connection must not abort the scene load.
  assert(result.success);

  auto it = result.scene.material_by_path.find("/World/UnrealMat");
  assert(it != result.scene.material_by_path.end());
  const RenderMaterial& mat = result.scene.materials[it->second];

  // Fell back to the child UsdPreviewSurface (not the broken connection, not
  // the unsupported UnrealShader child that iterates first).
  assert(mat.shader_type == RenderMaterial::ShaderType::PreviewSurface);
  assert(mat.preview_surface);
  assert(std::abs(mat.preview_surface->diffuse_color.value.x - 0.5f) < 0.001f);
  assert(std::abs(mat.preview_surface->diffuse_color.value.y - 0.3f) < 0.001f);
  assert(std::abs(mat.preview_surface->diffuse_color.value.z - 0.1f) < 0.001f);
  assert(std::abs(mat.preview_surface->roughness.value.x - 0.4f) < 0.001f);

  std::cout << "  RenderConverter Unreal surface fallback: PASSED\n";
}

// Regression: a Material whose only surface is an Unreal `sourceAsset` shader
// (info:implementationSource = "sourceAsset", no UsdPreviewSurface child) must
// not be dropped. Familiar authored PBR constants on the engine shader and its
// Material interface must survive in a per-material degraded PreviewSurface.
// Geometry and GeomSubset binding must remain intact through that fallback.
void TestRenderConverterUnrealSourceAssetFallback() {
  std::cout << "Testing RenderConverter Unreal source-asset default material...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Material "M_Hide"
    {
        color3f inputs:emissiveColor = (0.03, 0.04, 0.05)
        token outputs:unreal:surface.connect = </World/M_Hide/UnrealShader.outputs:out>

        def Shader "UnrealShader"
        {
            uniform token info:implementationSource = "sourceAsset"
            uniform asset info:unreal:sourceAsset = @/Game/Materials/M_Hide.M_Hide@
            color3f inputs:baseColor = (0.2, 0.4, 0.6)
            float inputs:metalness = 0.75
            float inputs:roughness = 0.27
            float inputs:opacity = 0.4
            float inputs:transmission_weight.connect = </World/M_Hide/AdvancedTex.outputs:r>
            color3f inputs:transmission_color = (0.9, 0.8, 0.7)
            float inputs:subsurface_weight = 0.31
            color3f inputs:subsurface_color = (0.7, 0.5, 0.3)
            float inputs:sheen_weight = 0.41
            float inputs:specular_anisotropy = 0.51
            float inputs:thin_film_weight = 0.61
            float inputs:transmission_dispersion = 0.71
            token outputs:out
        }

        def Shader "AdvancedTex"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @missing-advanced.png@
            token inputs:sourceColorSpace = "raw"
            float outputs:r
        }
    }

    def Material "M_Subset"
    {
        token outputs:surface.connect = </World/M_Subset/Unknown.outputs:out>
        def Shader "Unknown"
        {
            uniform token info:id = "UnsupportedSurface_1"
            color3f inputs:diffuseColor = (0.8, 0.1, 0.2)
            token outputs:out
        }
    }

    def Mesh "M"
    {
        rel material:binding = </World/M_Hide>
        int[] faceVertexCounts = [3, 3]
        int[] faceVertexIndices = [0, 1, 2, 0, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]

        def GeomSubset "UnsupportedLook"
        {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [1]
            rel material:binding = </World/M_Subset>
        }
    }
}
)";

  LoadResult load_result = LoadUSDAFromString(usda, std::strlen(usda));
  if (!load_result.success) {
    std::cout << "  SKIPPED (failed to parse Unreal source-asset USDA)\n";
    return;
  }

  ConverterConfig config;
  config.material.load_textures = false;

  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(load_result.stage);

  // A source-asset-only material must not abort or vanish.
  assert(result.success);

  auto it = result.scene.material_by_path.find("/World/M_Hide");
  assert(it != result.scene.material_by_path.end());
  const RenderMaterial& mat = result.scene.materials[it->second];

  // Degraded PreviewSurface (no fully convertible shader was found), with
  // authored constants recovered from the engine shader and Material input.
  assert(mat.shader_type == RenderMaterial::ShaderType::PreviewSurface);
  assert(mat.preview_surface);
  assert(mat.default_fallback);
  assert(!mat.diagnostics.empty());
  bool saw_unsupported_node = false;
  bool saw_degraded_diagnostic = false;
  for (const auto& diagnostic : mat.diagnostics) {
    if (diagnostic.kind == MaterialDiagnosticKind::UnsupportedMaterialXNode ||
        diagnostic.kind == MaterialDiagnosticKind::UnsupportedShader) {
      saw_unsupported_node = !diagnostic.node_path.empty();
    }
    if (diagnostic.kind == MaterialDiagnosticKind::DegradedMaterial) {
      saw_degraded_diagnostic = true;
    }
  }
  assert(saw_unsupported_node);
  assert(saw_degraded_diagnostic);
  assert(std::abs(mat.preview_surface->diffuse_color.value.x - 0.2f) < 0.001f);
  assert(std::abs(mat.preview_surface->diffuse_color.value.y - 0.4f) < 0.001f);
  assert(std::abs(mat.preview_surface->diffuse_color.value.z - 0.6f) < 0.001f);
  assert(std::abs(mat.preview_surface->metallic.value.x - 0.75f) < 0.001f);
  assert(std::abs(mat.preview_surface->roughness.value.x - 0.27f) < 0.001f);
  assert(std::abs(mat.preview_surface->opacity.value.x - 0.4f) < 0.001f);
  assert(std::abs(mat.preview_surface->emissive_color.value.x - 0.03f) < 0.001f);
  assert(mat.alpha_mode == RenderMaterial::AlphaMode::Blend);

  // Unsupported advanced lobes are not evaluated as PreviewSurface, but every
  // successfully extracted constant/connection remains available to a future
  // evaluator. In particular, a texture connection must survive as a texture
  // id rather than collapsing to its fallback scalar.
  const auto retained = [&mat](const char* name)
      -> const RetainedMaterialParam* {
    for (const RetainedMaterialParam& param : mat.retained_params) {
      if (param.name == name) return &param;
    }
    return nullptr;
  };
  const RetainedMaterialParam* transmission =
      retained("transmission_weight");
  assert(transmission && transmission->value.texture_id >= 0);
  assert(static_cast<size_t>(transmission->value.texture_id) <
         result.scene.textures.size());
  assert(retained("transmission_color"));
  assert(std::abs(retained("transmission_color")->value.value.x - 0.9f) <
         0.001f);
  assert(retained("subsurface_weight"));
  assert(std::abs(retained("subsurface_weight")->value.value.x - 0.31f) <
         0.001f);
  assert(retained("subsurface_color"));
  assert(retained("sheen_weight"));
  assert(retained("specular_anisotropy"));
  assert(retained("thin_film_weight"));
  assert(retained("transmission_dispersion"));

  auto subset_mat_it = result.scene.material_by_path.find("/World/M_Subset");
  assert(subset_mat_it != result.scene.material_by_path.end());
  const RenderMaterial& subset_mat =
      result.scene.materials[static_cast<size_t>(subset_mat_it->second)];
  assert(subset_mat.default_fallback && subset_mat.preview_surface);
  assert(std::abs(subset_mat.preview_surface->diffuse_color.value.x - 0.8f) <
         0.001f);

  // A failed shader implementation must never drop bound geometry. Both
  // triangles survive, and the second is routed to the degraded subset look.
  auto mesh_it = result.scene.mesh_by_path.find("/World/M");
  assert(mesh_it != result.scene.mesh_by_path.end());
  const RenderMesh& mesh =
      result.scene.meshes[static_cast<size_t>(mesh_it->second)];
  assert(mesh.triangulated_indices.size() == 6);
  assert(mesh.material_id == it->second);
  assert(mesh.material_subsets.size() == 1);
  assert(mesh.material_subsets[0].face_start == 1);
  assert(mesh.material_subsets[0].face_count == 1);
  assert(mesh.material_subsets[0].material_id == subset_mat_it->second);

  bool reported_degraded = false;
  for (const std::string& warning : result.warnings) {
    if (warning.find("using a degraded material") != std::string::npos) {
      reported_degraded = true;
      break;
    }
  }
  assert(reported_degraded);

  std::cout << "  RenderConverter degraded material: PASSED\n";
}

// Regression: UsdSkel binding inheritance. `skel:skeleton` may be authored on
// an ancestor (the enclosing SkelRoot) rather than on the skinned mesh, in
// which case the mesh inherits it. The converter must resolve the inherited
// skeleton (skeleton_id >= 0) so the mesh skins instead of rendering in bind
// pose. MetaHuman's standalone face/body exports bind this way.
void TestRenderConverterInheritedSkelBinding() {
  std::cout << "Testing RenderConverter inherited skel:skeleton binding...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "Root"
)

def Xform "Root"
{
    def SkelRoot "SR" (
        prepend apiSchemas = ["SkelBindingAPI"]
    )
    {
        rel skel:skeleton = </Root/SR/Skel>

        def Skeleton "Skel"
        {
            uniform token[] joints = ["root", "root/child"]
            uniform matrix4d[] bindTransforms = [
                ( (1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1) ),
                ( (1,0,0,0),(0,1,0,0),(0,0,1,0),(0,1,0,1) ) ]
            uniform matrix4d[] restTransforms = [
                ( (1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1) ),
                ( (1,0,0,0),(0,1,0,0),(0,0,1,0),(0,1,0,1) ) ]
        }

        def Mesh "M" (
            prepend apiSchemas = ["SkelBindingAPI"]
        )
        {
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
            int[] primvars:skel:jointIndices = [0, 0, 1] (
                elementSize = 1
                interpolation = "vertex"
            )
            float[] primvars:skel:jointWeights = [1, 1, 1] (
                elementSize = 1
                interpolation = "vertex"
            )
        }
    }
}
)";

  LoadResult load_result = LoadUSDAFromString(usda, std::strlen(usda));
  if (!load_result.success) {
    std::cout << "  SKIPPED (failed to parse inherited-skel USDA)\n";
    return;
  }

  RenderSceneConverter converter;
  ConvertResult result = converter.Convert(load_result.stage);
  assert(result.success);
  assert(result.scene.skeletons.size() == 1);

  const RenderMesh* mesh = nullptr;
  for (const auto& m : result.scene.meshes) {
    if (m.skin) { mesh = &m; break; }
  }
  assert(mesh && "mesh with a skin binding should exist");
  // The mesh authors no skel:skeleton of its own — it must inherit the one on
  // the SkelRoot and resolve to a real skeleton id.
  assert(mesh->skin->skeleton_id >= 0 &&
         "inherited skel:skeleton must resolve to a skeleton_id");

  std::cout << "  RenderConverter inherited skel binding: PASSED\n";
}

void TestRenderConverterPointInstancerWarnings() {
  std::cout << "Testing RenderConverter PointInstancer diagnostics...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def PointInstancer "Inst"
    {
        rel prototypes = </World/Missing>
        point3f[] positions = [(0, 0, 0)]
        int[] protoIndices = [0]
    }
}
)";

  LoadResult load_result = LoadUSDAFromString(usda, std::strlen(usda));
  if (!load_result.success) {
    std::cout << "  SKIPPED (failed to parse diagnostic USDA)\n";
    return;
  }

  RenderSceneConverter converter;
  ConvertResult result = converter.Convert(load_result.stage);
  assert(result.success);
  assert(result.scene.point_instancers.size() == 1);
  assert(result.scene.point_instancers[0].valid);
  assert(result.scene.point_instancers[0].prototype_node_ids.size() == 1);
  assert(result.scene.point_instancers[0].prototype_node_ids[0] == -1);
  assert(result.scene.point_instancers[0].draw_count == 0);
  assert(result.scene.point_instance_draws.empty());
  assert(result.scene.get_point_instancer_draws(0).empty());
  assert(result.scene.has_valid_point_instance_draw_ranges());

  bool found_warning = false;
  for (const std::string& warning : result.warnings) {
    if (warning.find("Unresolved PointInstancer prototype") != std::string::npos &&
        warning.find("/World/Missing") != std::string::npos) {
      found_warning = true;
    }
  }
  assert(found_warning);

  std::cout << "  RenderConverter PointInstancer diagnostics: PASSED\n";
}

void TestRenderConverterPointInstancerIndexVisibility() {
  std::cout << "Testing RenderConverter PointInstancer index visibility...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Mesh "Proto"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }

    def PointInstancer "Inst"
    {
        rel prototypes = </World/Proto>
        point3f[] positions = [(0, 0, 0), (2, 0, 0), (4, 0, 0)]
        int[] protoIndices = [0, 0, 0]
        int64[] invisibleIds = [2]
        int64[] inactiveIds = [0]
    }
}
)";

  LoadResult load_result = LoadUSDAFromString(usda, std::strlen(usda));
  if (!load_result.success) {
    std::cout << "  SKIPPED (failed to parse index-visibility USDA)\n";
    return;
  }

  RenderSceneConverter converter;
  ConvertResult result = converter.Convert(load_result.stage);
  assert(result.success);
  assert(result.scene.point_instancers.size() == 1);
  assert(result.scene.point_instance_draws.size() == 1);
  assert(result.scene.get_stats().point_instance_count == 3);
  assert(result.scene.get_stats().visible_point_instance_count == 1);
  assert(result.scene.get_stats().point_instance_draw_count == 1);
  assert(result.scene.has_valid_point_instance_draw_ranges());

  const RenderPointInstancer& instancer = result.scene.point_instancers[0];
  assert(instancer.ids.empty());
  assert(instancer.invisible_ids == std::vector<int64_t>({2}));
  assert(instancer.inactive_ids == std::vector<int64_t>({0}));
  assert(instancer.instance_visible == std::vector<uint8_t>({0, 1, 0}));
  assert(instancer.draw_start == 0);
  assert(instancer.draw_count == 1);

  const RenderPointInstanceDraw& draw = result.scene.point_instance_draws[0];
  assert(draw.point_instancer_id == 0);
  assert(draw.instance_index == 1);
  assert(draw.prototype_index == 0);
  assert(std::abs(draw.transform.m[12] - 2.0f) < 0.001f);
  RenderPointInstanceDrawRange range = result.scene.get_point_instancer_draws(0);
  assert(range.size == 1);
  assert(range[0].instance_index == 1);

  std::cout << "  RenderConverter PointInstancer index visibility: PASSED\n";
}

void TestRenderConverterPointInstancerInvalidArrays() {
  std::cout << "Testing RenderConverter PointInstancer invalid arrays...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Mesh "Proto"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    }

    def PointInstancer "Inst"
    {
        rel prototypes = </World/Proto>
        point3f[] positions = [(0, 0, 0)]
        int[] protoIndices = [0, 0]
    }
}
)";

  LoadResult load_result = LoadUSDAFromString(usda, std::strlen(usda));
  if (!load_result.success) {
    std::cout << "  SKIPPED (failed to parse invalid-array USDA)\n";
    return;
  }

  RenderSceneConverter converter;
  ConvertResult result = converter.Convert(load_result.stage);
  assert(result.success);
  assert(result.scene.point_instancers.size() == 1);
  assert(result.scene.point_instance_draws.empty());
  assert(result.scene.get_stats().point_instance_count == 2);
  assert(result.scene.get_stats().point_instance_draw_count == 0);
  assert(result.scene.has_valid_point_instance_draw_ranges());

  const RenderPointInstancer& instancer = result.scene.point_instancers[0];
  assert(!instancer.valid);
  assert(instancer.validation_error == "positions size does not match protoIndices");
  assert(instancer.instance_count() == 2);
  assert(instancer.positions == std::vector<float>({0, 0, 0}));
  assert(instancer.transforms.empty());
  assert(instancer.draw_start == 0);
  assert(instancer.draw_count == 0);

  bool found_warning = false;
  for (const std::string& warning : result.warnings) {
    if (warning.find("Invalid PointInstancer data at /World/Inst") !=
            std::string::npos &&
        warning.find("positions size does not match protoIndices") !=
            std::string::npos) {
      found_warning = true;
    }
  }
  assert(found_warning);

  std::cout << "  RenderConverter PointInstancer invalid arrays: PASSED\n";
}

void TestRenderConverterPointInstancerDuplicateMeshMetadata() {
  std::cout << "Testing RenderConverter PointInstancer duplicate metadata...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "Root"
)

def Xform "Root"
{
    def SkelRoot "SR"
    {
        def Mesh "Proto" (
            prepend apiSchemas = ["SkelBindingAPI"]
        )
        {
            uniform token orientation = "leftHanded"
            int[] faceVertexCounts = [4, 3]
            int[] faceVertexIndices = [0, 1, 2, 3, 0, 2, 4]
            int[] holeIndices = [1]
            point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (2, 0, 0)]
            float[] primvars:temperature = [0.25, 0.75] (
                interpolation = "vertex"
            )
            int[] primvars:temperature:indices = [0, 1, 0, 1, 0]
            uniform token[] skel:blendShapes = ["smile"]
            prepend rel skel:blendShapeTargets = </Root/SR/Proto/Smile>

            def BlendShape "Smile"
            {
                uniform vector3f[] offsets = [(0, 0.5, 0), (0, 0.25, 0)]
                uniform int[] pointIndices = [1, 3]
            }
        }
    }

    def PointInstancer "Inst"
    {
        rel prototypes = </Root/SR/Proto>
        point3f[] positions = [(10, 0, 0)]
        int[] protoIndices = [0]
    }
}
)";

  LoadResult load_result = LoadUSDAFromString(usda, std::strlen(usda));
  if (!load_result.success) {
    std::cout << "  SKIPPED (failed to parse duplicate-metadata USDA)\n";
    return;
  }

  ConverterConfig config;
  config.mesh.triangulate = true;
  config.point_instancer.duplicate_meshes = true;
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(load_result.stage);
  assert(result.success);
  assert(result.scene.point_instancers.size() == 1);
  assert(result.scene.point_instance_draws.size() == 1);
  assert(result.scene.point_instance_draws[0].expanded_mesh_id >= 0);

  const RenderPointInstanceDrawView view =
      result.scene.get_point_instance_draw_view(0);
  assert(view.valid());
  assert(view.mesh);
  assert(view.expanded_mesh);
  const RenderMesh& src = *view.mesh;
  const RenderMesh& expanded = *view.expanded_mesh;

  assert(src.left_handed);
  assert(expanded.left_handed);
  assert(src.hole_faces == std::vector<uint32_t>({1}));
  assert(expanded.hole_faces == src.hole_faces);
  assert(src.is_triangulated);
  assert(expanded.is_triangulated);
  assert(src.triangulated_indices.size() == 6);
  assert(expanded.triangulated_indices.size() == src.triangulated_indices.size());
  assert(expanded.triangulated_face_vertex_indices.size() ==
         src.triangulated_face_vertex_indices.size());
  for (size_t i = 0; i < src.triangulated_face_vertex_indices.size(); ++i) {
    assert(expanded.triangulated_face_vertex_indices[i] ==
           src.triangulated_face_vertex_indices[i]);
  }

  const VertexAttribute* temp_src = nullptr;
  const VertexAttribute* temp_expanded = nullptr;
  for (const VertexAttribute& pv : src.primvars) {
    if (pv.name == "temperature") temp_src = &pv;
  }
  for (const VertexAttribute& pv : expanded.primvars) {
    if (pv.name == "temperature") temp_expanded = &pv;
  }
  assert(temp_src);
  assert(temp_expanded);
  assert(temp_expanded->indices.size() == temp_src->indices.size());
  assert(temp_expanded->indices.size() == 5);
  assert(temp_expanded->indices[1] == 1);

  assert(src.blend_shapes.size() == 1);
  assert(expanded.blend_shapes.size() == 1);
  assert(src.blend_shapes[0].point_indices == std::vector<uint32_t>({1, 3}));
  assert(expanded.blend_shapes[0].point_indices == src.blend_shapes[0].point_indices);
  assert(expanded.blend_shapes[0].point_offsets.size() ==
         src.blend_shapes[0].point_offsets.size());

  assert(expanded.point_count() == src.point_count());
  assert(std::abs(expanded.points[0] - 10.0f) < 0.001f);
  assert(std::abs(expanded.points[3] - 11.0f) < 0.001f);
  assert(expanded.has_bbox);
  assert(result.scene.has_valid_point_instance_draw_ranges());

  std::cout << "  RenderConverter PointInstancer duplicate metadata: PASSED\n";
}

void TestMaterialXUtilities() {
  std::cout << "Testing MaterialX utilities...\n";

  const char* mtlx = R"(<materialx version="1.38">
  <surfacematerial name="Mat" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="MatSurface" />
  </surfacematerial>
  <nodegraph name="NG">
    <output name="out" type="color3" nodename="Scale" />
    <node name="Color" category="constant" type="color3">
      <input name="value" type="color3" value="0.8, 0.4, 0.2" />
    </node>
    <node name="Scale" category="multiply" type="color3">
      <input name="in1" type="color3" nodename="Color" />
      <input name="in2" type="color3" value="0.5, 0.5, 0.5" />
    </node>
  </nodegraph>
  <node name="MatSurface" category="standard_surface" type="surfaceshader">
    <input name="base" type="float" value="1.0" />
    <input name="base_color" type="color3" nodegraph="NG" output="out" />
    <input name="metalness" type="float" value="0.25" />
    <input name="specular_roughness" type="float" value="0.35" />
    <input name="opacity" type="color3" value="0.5, 0.5, 0.5" />
  </node>
</materialx>
)";

  MtlxConverter converter;
  RenderMaterial from_string;
  if (!converter.ConvertToRenderMaterial(mtlx, "Mat", &from_string)) {
    std::cout << "  MaterialX string conversion failed: "
              << converter.GetError() << "\n";
    assert(false);
  }
  assert(from_string.shader_type == RenderMaterial::ShaderType::PreviewSurface);
  assert(from_string.preview_surface);
  assert(std::abs(from_string.preview_surface->diffuse_color.value.x - 0.4f) < 0.001f);
  assert(std::abs(from_string.preview_surface->diffuse_color.value.y - 0.2f) < 0.001f);
  assert(std::abs(from_string.preview_surface->diffuse_color.value.z - 0.1f) < 0.001f);
  assert(std::abs(from_string.preview_surface->metallic.value.x - 0.25f) < 0.001f);
  assert(std::abs(from_string.preview_surface->roughness.value.x - 0.35f) < 0.001f);
  assert(std::abs(from_string.preview_surface->opacity.value.x - 0.5f) < 0.001f);

  const char* path = "next_tydra_materialx_test.mtlx";
  {
    std::ofstream ofs(path, std::ios::out | std::ios::binary);
    assert(ofs);
    ofs << mtlx;
    assert(ofs.good());
  }

  RenderMaterial from_file;
  if (!converter.ConvertFileToRenderMaterial(path, "Mat", &from_file)) {
    std::cout << "  MaterialX file conversion failed: "
              << converter.GetError() << "\n";
    assert(false);
  }
  std::remove(path);
  assert(from_file.shader_type == RenderMaterial::ShaderType::PreviewSurface);
  assert(from_file.preview_surface);
  assert(std::abs(from_file.preview_surface->diffuse_color.value.x - 0.4f) < 0.001f);

  // A material wrapper may bind its shader through a nodegraph output rather
  // than a direct nodename. The converter must resolve that output to the
  // graph's shader node before dispatching the shader conversion.
  const char* graph_material = R"(<materialx version="1.38">
  <surfacematerial name="GraphMat" type="material">
    <input name="surfaceshader" type="surfaceshader" nodegraph="Graph" output="surface" />
  </surfacematerial>
  <nodegraph name="Graph">
    <output name="surface" type="surfaceshader" nodename="GraphSurface" />
    <node name="GraphSurface" category="standard_surface" type="surfaceshader">
      <input name="base" type="float" value="1.0" />
      <input name="base_color" type="color3" value="0.2, 0.3, 0.4" />
      <input name="metalness" type="float" value="0.6" />
      <input name="specular_roughness" type="float" value="0.25" />
    </node>
  </nodegraph>
</materialx>)";
  RenderMaterial graph_material_out;
  if (!converter.ConvertToRenderMaterial(graph_material, "GraphMat",
                                         &graph_material_out)) {
    std::cout << "  MaterialX nodegraph material conversion failed: "
              << converter.GetError() << "\n";
    assert(false);
  }
  assert(graph_material_out.shader_type == RenderMaterial::ShaderType::PreviewSurface);
  assert(graph_material_out.preview_surface);
  assert(std::abs(graph_material_out.preview_surface->diffuse_color.value.x - 0.2f) < 0.001f);
  assert(std::abs(graph_material_out.preview_surface->metallic.value.x - 0.6f) < 0.001f);

  const char* usda = R"(#usda 1.0

def Material "Mat"
{
    string config:mtlx:sourceUri = "lookdev/materials.mtlx"
    token outputs:mtlx:surface.connect = </Mat/OpenPBR.outputs:out>

    def Shader "OpenPBR"
    {
        uniform token info:id = "ND_open_pbr_surface_surfaceshader"
        color3f inputs:base_color = (0.1, 0.2, 0.3)
        float inputs:base_metalness = 0.4
        token outputs:out
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  assert(lr.success);
  UsdPrim mat = lr.stage.GetPrimAtPath("/Mat");
  assert(mat);
  assert(HasMtlxBinding(mat));
  assert(GetMtlxFilePath(mat) == "lookdev/materials.mtlx");

  RenderMaterial usd_mat;
  if (!converter.ConvertUsdMtlxMaterial(lr.stage, mat, &usd_mat)) {
    std::cout << "  USD MaterialX conversion failed: "
              << converter.GetError() << "\n";
    assert(false);
  }
  assert(usd_mat.shader_type == RenderMaterial::ShaderType::OpenPBR);
  assert(usd_mat.openpbr);
  assert(std::abs(usd_mat.openpbr->base_color.value.x - 0.1f) < 0.001f);
  assert(std::abs(usd_mat.openpbr->base_metalness.value.x - 0.4f) < 0.001f);

  // Directly authored OpenPBR values belong to the typed material block. They
  // must not also be lowered into executable graph constants: doing so makes
  // the graph evaluator compete with the authoritative typed values at hit
  // time and needlessly promotes the Vulkan material path.
  const char* direct_constants_usda = R"(#usda 1.0

def Material "DirectConstants"
{
    token outputs:surface.connect = </DirectConstants/OpenPBR.outputs:out>

    def Shader "OpenPBR"
    {
        uniform token info:id = "ND_open_pbr_surface_surfaceshader"
        color3f inputs:base_color = (0.12, 0.34, 0.56)
        float inputs:base_metalness = 0.42
        float inputs:specular_ior = 1.72
        float inputs:specular_f0 = 0.17
        float inputs:coat_weight = 0.31
        color3f inputs:coat_color = (0.7, 0.8, 0.9)
        token outputs:out
    }
}
)";
  LoadResult direct_lr = LoadUSDAFromString(
      direct_constants_usda, std::strlen(direct_constants_usda));
  assert(direct_lr.success);
  RenderSceneConverter direct_converter;
  RenderMaterial direct_material;
  if (!direct_converter.ConvertMaterial(
          direct_lr.stage,
          direct_lr.stage.GetPrimAtPath("/DirectConstants"),
          &direct_material)) {
    std::cout << "  direct OpenPBR constant conversion failed: "
              << direct_converter.GetLastError() << "\n";
    assert(false);
  }
  assert(direct_material.shader_type == RenderMaterial::ShaderType::OpenPBR);
  assert(direct_material.openpbr);
  assert(std::abs(direct_material.openpbr->base_color.value.x - 0.12f) <
         0.001f);
  assert(std::abs(direct_material.openpbr->base_color.value.y - 0.34f) <
         0.001f);
  assert(std::abs(direct_material.openpbr->base_color.value.z - 0.56f) <
         0.001f);
  assert(std::abs(direct_material.openpbr->base_metalness.value.x - 0.42f) <
         0.001f);
  assert(std::abs(direct_material.openpbr->specular_ior.value.x - 1.72f) <
         0.001f);
  // specular_f0 is not a valid OpenPBR input (deprecated) — converter ignores it
  // (previously tested as specular_f0 = 0.17f, now removed from OpenPBRSurfaceShader)
  assert(std::abs(direct_material.openpbr->coat_weight.value.x - 0.31f) <
         0.001f);
  assert(std::abs(direct_material.openpbr->coat_color.value.z - 0.9f) <
         0.001f);
  assert(direct_material.openpbr->nodegraph_json.empty());

  // Blender authors a textured MaterialX terminal alongside an untextured
  // PreviewSurface fallback. The MaterialX terminal must win, otherwise web
  // renderers receive a white material and no texture records.
  const char* dual_terminal_usda = R"(#usda 1.0
def Xform "World"
{
    def Mesh "Quad"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        rel material:binding = </World/Mat>
    }
    def Material "Mat"
    {
        token outputs:surface.connect = </World/Mat/Preview.outputs:surface>
        token outputs:mtlx:surface.connect = </World/Mat/OpenPBR.outputs:out>
        def Shader "Preview"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (1, 1, 1)
            token outputs:surface
        }
        def Shader "OpenPBR"
        {
            uniform token info:id = "ND_open_pbr_surface_surfaceshader"
            color3f inputs:base_color.connect = </World/Mat/Image.outputs:out>
            token outputs:out
        }
        def Shader "Image"
        {
            uniform token info:id = "ND_image_color3"
            asset inputs:file = @./textures/albedo.png@
            color3f outputs:out
        }
    }
}
)";
  LoadResult dual_lr = LoadUSDAFromString(
      dual_terminal_usda, std::strlen(dual_terminal_usda));
  assert(dual_lr.success);
  RenderSceneConverter render_converter;
  ConvertResult dual_result = render_converter.Convert(dual_lr.stage);
  assert(dual_result.success);
  assert(dual_result.scene.materials.size() == 1);
  assert(dual_result.scene.textures.size() == 1);
  const RenderMaterial& dual_mat = dual_result.scene.materials[0];
  assert(dual_mat.shader_type == RenderMaterial::ShaderType::OpenPBR);
  assert(dual_mat.openpbr && dual_mat.openpbr->base_color.is_texture());
  assert(dual_result.scene.textures[0].asset_path ==
         "./textures/albedo.png");

  std::cout << "  MaterialX utilities: PASSED\n";
}

void TestExternalMaterialXImageGraph() {
  std::cout << "Testing external MaterialX image graph composition...\n";

  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path fixture =
      fs::absolute("next_tydra_external_mtlx_test", ec);
  assert(!ec);
  fs::remove_all(fixture, ec);
  ec.clear();
  const fs::path materials = fixture / "materials";
  fs::create_directories(materials, ec);
  assert(!ec);

  const char* root = R"(#usda 1.0
(
    defaultPrim = "Scene"
)
def Xform "Scene"
{
    def "Mat" (
        references = @materials/test.mtlx@</MaterialX/Materials/M_Test>
    )
    {
    }
    def "OpenPBRMat" (
        references = @materials/test.mtlx@</MaterialX/Materials/M_OpenPBR>
    )
    {
    }
}
)";
  const char* mtlx = R"(<?xml version="1.0"?>
<materialx version="1.38" colorspace="lin_rec709">
  <nodegraph name="NG_Test">
    <image name="base_image" type="color3">
      <input name="file" type="filename" value="tex/base.jpg"
             colorspace="srgb_texture" />
    </image>
    <image name="metal_image" type="float">
      <input name="file" type="filename" value="tex/metal.jpg" />
    </image>
    <image name="rough_image" type="float">
      <input name="file" type="filename" value="tex/rough.jpg" />
    </image>
    <image name="normal_image" type="vector3">
      <input name="file" type="filename" value="tex/normal.jpg" />
    </image>
    <normalmap name="normal_map" type="vector3">
      <input name="in" type="vector3" nodename="normal_image" />
    </normalmap>
    <output name="base" type="color3" nodename="base_image" />
    <output name="metal" type="float" nodename="metal_image" />
    <output name="rough" type="float" nodename="rough_image" />
    <output name="normal" type="vector3" nodename="normal_map" />
  </nodegraph>
  <standard_surface name="Test" type="surfaceshader">
    <input name="base_color" type="color3"
           nodegraph="NG_Test" output="base" />
    <input name="metalness" type="float"
           nodegraph="NG_Test" output="metal" />
    <input name="specular_roughness" type="float"
           nodegraph="NG_Test" output="rough" />
    <input name="normal" type="vector3"
           nodegraph="NG_Test" output="normal" />
    <input name="subsurface_scale" type="float" value="0.003" />
  </standard_surface>
  <surfacematerial name="M_Test" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="Test" />
  </surfacematerial>
  <open_pbr_surface name="OpenPBRTest" type="surfaceshader">
    <input name="base_weight" type="float" value="0" />
    <input name="transmission_weight" type="float" value="1" />
    <input name="transmission_color" type="color3" value="0.942, 1, 0.9884" />
    <input name="geometry_opacity" type="float" value="0.35" />
    <input name="subsurface_weight" type="float" value="0.75" />
    <input name="subsurface_color" type="color3" value="1, 0.22, 0.493" />
    <input name="subsurface_radius_scale" type="color3" value="1, 0, 0.068" />
    <input name="subsurface_radius" type="float" value="0.0325" />
  </open_pbr_surface>
  <surfacematerial name="M_OpenPBR" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="OpenPBRTest" />
  </surfacematerial>
</materialx>
)";

  {
    std::ofstream ofs(fixture / "root.usda",
                      std::ios::out | std::ios::binary);
    assert(ofs);
    ofs << root;
    assert(ofs.good());
  }
  {
    std::ofstream ofs(materials / "test.mtlx",
                      std::ios::out | std::ios::binary);
    assert(ofs);
    ofs << mtlx;
    assert(ofs.good());
  }

  Stage stage;
  AssetResolver resolver;
  std::string warn;
  std::string err;
  const bool composed = pcp::ComposeStageFromFile(
      (fixture / "root.usda").string(), resolver, &stage, {}, &warn, &err);
  if (!composed) std::abort();

  const UsdPrim material = stage.GetPrimAtPath("/Scene/Mat");
  assert(material.IsValid());
  const std::vector<Path>* surface =
      material.GetRelationship("mtlx:surface:source");
  assert(surface && surface->size() == 1);
  assert((*surface)[0].str() == "/Scene/Mat/Test");

  const UsdPrim shader = stage.GetPrimAtPath("/Scene/Mat/Test");
  assert(shader.IsValid());
  const std::vector<Path>* base =
      shader.GetPrimSpec()->connection("inputs:base_color");
  assert(base && base->size() == 1);
  assert((*base)[0].str() == "/Scene/Mat/NG_Test.outputs:base");

  const UsdPrim graph = stage.GetPrimAtPath("/Scene/Mat/NG_Test");
  assert(graph.IsValid());
  const std::vector<Path>* graph_base =
      graph.GetPrimSpec()->connection("outputs:base");
  assert(graph_base && graph_base->size() == 1);
  assert((*graph_base)[0].str() ==
         "/Scene/Mat/NG_Test/base_image.outputs:out");

  ConverterConfig config;
  config.material.load_textures = false;
  config.asset_base_dir = fixture.string();
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(stage);
  assert(result.success);
  assert(result.scene.images.size() == 4);
  assert(result.scene.textures.size() == 4);

  const auto material_it = result.scene.material_by_path.find("/Scene/Mat");
  assert(material_it != result.scene.material_by_path.end());
  const RenderMaterial& render_material =
      result.scene.materials[static_cast<size_t>(material_it->second)];
  assert(render_material.shader_type ==
         RenderMaterial::ShaderType::OpenPBR);
  assert(render_material.openpbr);
  assert(render_material.openpbr->base_color.is_texture());
  assert(render_material.openpbr->base_metalness.is_texture());
  assert(render_material.openpbr->specular_roughness.is_texture());
  assert(render_material.openpbr->normal.is_texture());
  assert(std::fabs(render_material.openpbr->subsurface_scale.value.x -
                   0.003f) < 1.0e-6f);
  assert(!render_material.default_fallback);

  const auto openpbr_it =
      result.scene.material_by_path.find("/Scene/OpenPBRMat");
  assert(openpbr_it != result.scene.material_by_path.end());
  const RenderMaterial& openpbr_material =
      result.scene.materials[static_cast<size_t>(openpbr_it->second)];
  assert(openpbr_material.shader_type ==
         RenderMaterial::ShaderType::OpenPBR);
  assert(openpbr_material.openpbr);
  assert(!openpbr_material.default_fallback);
  assert(std::fabs(openpbr_material.openpbr->transmission_weight.value.x -
                   1.0f) < 1.0e-6f);
  assert(std::fabs(openpbr_material.openpbr->transmission_color.value.x -
                   0.942f) < 1.0e-6f);
  assert(std::fabs(openpbr_material.openpbr->opacity.value.x - 0.35f) <
         1.0e-6f);
  assert(openpbr_material.alpha_mode == RenderMaterial::AlphaMode::Blend);
  assert(std::fabs(openpbr_material.openpbr->subsurface_weight.value.x -
                   0.75f) < 1.0e-6f);
  assert(std::fabs(openpbr_material.openpbr->subsurface_radius.value.x -
                   1.0f) < 1.0e-6f);
  assert(std::fabs(openpbr_material.openpbr->subsurface_radius.value.y) <
         1.0e-6f);
  assert(std::fabs(openpbr_material.openpbr->subsurface_radius.value.z -
                   0.068f) < 1.0e-6f);
  assert(std::fabs(openpbr_material.openpbr->subsurface_scale.value.x -
                   0.0325f) < 1.0e-6f);

  auto texture_for = [&](const char* asset) -> const RenderTexture& {
    for (const RenderTexture& texture : result.scene.textures) {
      if (texture.asset_path == asset) return texture;
    }
    assert(false);
    return result.scene.textures[0];
  };
  const RenderTexture& base_texture = texture_for("tex/base.jpg");
  const RenderTexture& metal_texture = texture_for("tex/metal.jpg");
  const RenderTexture& rough_texture = texture_for("tex/rough.jpg");
  const RenderTexture& normal_texture = texture_for("tex/normal.jpg");
  assert(base_texture.output_channel == RenderTexture::Channel::RGB);
  assert(metal_texture.output_channel == RenderTexture::Channel::R);
  assert(rough_texture.output_channel == RenderTexture::Channel::R);
  assert(normal_texture.output_channel == RenderTexture::Channel::RGB);

  const std::string expected_prefix = (materials / "tex").string() + "/";
  for (const TextureImage& image : result.scene.images) {
    assert(image.resolved_path.rfind(expected_prefix, 0) == 0);
  }

  fs::remove_all(fixture, ec);
  std::cout << "  External MaterialX image graph: PASSED\n";
}

//
// Main
//


// 2026-07 tydra audit regressions: ChunkedArray over-reserve accounting,
// malformed topology sanitization, authored-normals interpolation, leftHanded
// winding, holeIndices, GeomSubset material bindings, NodeGraph texture
// indirection, UsdTransform2d + primvar-reader varname, purpose bindings,
// surface-output shader selection, spot lights, generic/indexed primvars.
void TestAudit2026_07() {
  std::cout << "TestAudit2026_07..." << std::endl;

  // --- ChunkedArray: reserve() beyond the fill must not leak whole chunks
  // into copy_to()/flatten() (was a heap-buffer-overflow through the C API).
  {
    ChunkedArray<uint32_t> a;
    a.reserve(20000);  // 2+ chunks
    for (uint32_t i = 0; i < 10; ++i) a.push_back(i);
    assert(a.size() == 10);
    assert(a.chunk_size(0) == 10);
    assert(a.chunk_size(1) == 0);
    std::vector<uint32_t> flat = a.flatten();
    assert(flat.size() == 10 && flat[9] == 9);
    uint32_t dst[10];
    a.copy_to(dst);
    assert(dst[0] == 0 && dst[9] == 9);
    assert(a.is_contiguous());  // logical size fits chunk 0
  }

  const char* usda = R"(#usda 1.0
def Xform "Root"
{
    def Mesh "M"
    {
        rel material:binding:preview = </Root/Mat>
        int[] faceVertexCounts = [4, 4]
        int[] faceVertexIndices = [0, 1, 2, 3, 1, 4, 5, 2]
        int[] holeIndices = [1]
        point3f[] points = [(0,0,0), (1,0,0), (1,1,0), (0,1,0), (2,0,0), (2,1,0)]
        normal3f[] normals = [(0,0,1), (0,0,1)] (
            interpolation = "uniform"
        )
        float[] primvars:heat = [7, 9] (
            interpolation = "vertex"
        )
        int[] primvars:heat:indices = [0, 1, 1, 0, 1, 0]

        def GeomSubset "SubA"
        {
            uniform token familyName = "materialBind"
            int[] indices = [0]
            rel material:binding = </Root/Mat2>
        }

        def GeomSubset "SubHole"
        {
            uniform token familyName = "materialBind"
            int[] indices = [1]
            rel material:binding = </Root/Mat2>
        }
    }

    def Mesh "LH"
    {
        uniform token orientation = "leftHanded"
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
    }

    def Mesh "Bad"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, -1, 99]
        point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
    }

    def Material "Mat"
    {
        token outputs:surface.connect = </Root/Mat/Graph.outputs:out>

        def Shader "PS"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </Root/Mat/Graph.outputs:tex>
            token outputs:surface
        }

        def "Graph"
        {
            token outputs:out.connect = </Root/Mat/PS.outputs:surface>
            color3f outputs:tex.connect = </Root/Mat/Tex.outputs:rgb>
        }

        def Shader "Tex"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @checker.png@
            float2 inputs:st.connect = </Root/Mat/Xf.outputs:result>
            color3f outputs:rgb
        }

        def Shader "Xf"
        {
            uniform token info:id = "UsdTransform2d"
            float2 inputs:translation = (0.5, 0.25)
            float inputs:rotation = 90
            float2 inputs:scale = (2, 3)
            float2 inputs:in.connect = </Root/Mat/Reader.outputs:result>
            float2 outputs:result
        }

        def Shader "Reader"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            token inputs:varname = "uvSet1"
            float2 outputs:result
        }
    }

    def Material "Mat2"
    {
        def Shader "PS2"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0, 1, 0)
            token outputs:surface
        }
    }

    def SphereLight "Spot"
    {
        float inputs:shaping:cone:angle = 45
        bool inputs:shadow:enable = false
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  assert(lr.success);

  ConverterConfig cfg;
  RenderSceneConverter conv(cfg);
  ConvertResult res = conv.Convert(lr.stage);
  assert(res.success);
  RenderScene& scene = res.scene;

  // Mesh M: hole face skipped in triangulation, uniform normals kept as 2
  // elements, subset binding on face 1, indexed generic primvar preserved.
  {
    auto it = scene.mesh_by_path.find("/Root/M");
    assert(it != scene.mesh_by_path.end());
    RenderMesh& m = scene.meshes[static_cast<size_t>(it->second)];
    assert(m.triangulated_indices.size() == 6);  // 1 quad, hole skipped
    assert(m.normals.size() == 6 &&
           m.normals_interp == Interpolation::Uniform);
    assert(m.material_id >= 0);  // purpose binding (material:binding:preview)
    // Subsets are TRIANGLE ranges after triangulation: authored face 0 (a
    // quad -> 2 triangles) covers triangles [0, 2); the subset on the HOLE
    // face 1 contributes no renderable triangles and is dropped.
    assert(m.material_subsets.size() == 1);
    assert(m.material_subsets[0].face_start == 0 &&
           m.material_subsets[0].face_count == 2);
    assert(m.material_subsets[0].material_id != m.material_id);
    assert(m.primvars.size() == 1 && m.primvars[0].name == "heat");
    assert(m.primvars[0].indices.size() == 6);
  }

  // Mesh LH: reversed winding + flipped computed normal.
  {
    auto it = scene.mesh_by_path.find("/Root/LH");
    assert(it != scene.mesh_by_path.end());
    RenderMesh& m = scene.meshes[static_cast<size_t>(it->second)];
    assert(m.left_handed);
    assert(m.triangulated_indices.size() == 3);
    assert(m.triangulated_indices[1] == 2 && m.triangulated_indices[2] == 1);
    assert(m.normals.size() >= 3 && m.normals[2] < 0.0f);  // z of vertex 0
  }

  // Mesh Bad: invalid indices dropped without crashing.
  {
    auto it = scene.mesh_by_path.find("/Root/Bad");
    assert(it != scene.mesh_by_path.end());
    RenderMesh& m = scene.meshes[static_cast<size_t>(it->second)];
    assert(m.face_count() == 0);
  }

  // Material Mat: texture found through the NodeGraph indirection, with the
  // Transform2d UV transform and the primvar-reader varname.
  {
    auto it = scene.material_by_path.find("/Root/Mat");
    assert(it != scene.material_by_path.end());
    RenderMaterial& mat = scene.materials[static_cast<size_t>(it->second)];
    assert(mat.preview_surface);
    assert(mat.preview_surface->diffuse_color.is_texture());
    const RenderTexture& tex = scene.textures[static_cast<size_t>(
        mat.preview_surface->diffuse_color.texture_id)];
    assert(tex.offset.x == 0.5f && tex.offset.y == 0.25f);
    assert(tex.scale.x == 2.0f && tex.scale.y == 3.0f);
    assert(std::fabs(tex.rotation - 1.5707963f) < 1e-4f);
    assert(tex.uv_primvar == "uvSet1");
  }

  // Light: cone shaping -> Spot; shadow:enable honored.
  {
    assert(scene.lights.size() == 1);
    const RenderLight& l = scene.lights[0];
    assert(l.type == LightType::Spot);
    assert(std::fabs(l.params.spot.angle - 45.0f * 3.14159265f / 180.0f) < 1e-4f);
    assert(!l.enable_shadow);
  }

  std::cout << "  TestAudit2026_07 PASSED" << std::endl;
}


// 2026-07 tydra-next gap follow-ups: tangent generation, faceVarying corner
// remap table, DomeLight environment texture, blendshape extraction.
void TestAudit2026_07_Gaps() {
  std::cout << "TestAudit2026_07_Gaps..." << std::endl;

  const char* usda = R"(#usda 1.0
def "Root"
{
    def Mesh "Quad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0,0,0), (1,0,0), (1,1,0), (0,1,0)]
        texCoord2f[] primvars:st = [(0,0), (1,0), (1,1), (0,1)] (
            interpolation = "vertex"
        )
        normal3f[] normals = [(0,0,1), (0,0,1), (0,0,1), (0,0,1)] (
            interpolation = "vertex"
        )
    }

    def Mesh "Mirror"
    {
        int[] faceVertexCounts = [3, 3]
        int[] faceVertexIndices = [0, 1, 2, 0, 2, 3]
        point3f[] points = [(0,0,0), (1,0,0), (1,1,0), (0,1,0)]
        texCoord2f[] primvars:st = [(0,0), (1,0), (1,1), (0,0), (-1,1), (0,1)] (
            interpolation = "faceVarying"
        )
        normal3f[] normals = [(0,0,1), (0,0,1), (0,0,1), (0,0,1)] (
            interpolation = "vertex"
        )
    }

    def Mesh "DegenerateUV"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
        texCoord2f[] primvars:st = [(0,0), (0,0), (0,0)] (
            interpolation = "vertex"
        )
        normal3f[] normals = [(0,0,1), (0,0,1), (0,0,1)] (
            interpolation = "vertex"
        )
    }

    def SkelRoot "SR"
    {
        def Skeleton "Skel"
        {
            uniform token[] joints = ["root", "root/child"]
            rel skel:animationSource = </Root/Anim>
        }

        def Mesh "BSMesh" (
            prepend apiSchemas = ["SkelBindingAPI"]
        )
        {
            int[] faceVertexCounts = [3]
            int[] faceVertexIndices = [0, 1, 2]
            point3f[] points = [(0,0,0), (1,0,0), (0,1,0)]
            uniform token[] skel:blendShapes = ["smile"]
            prepend rel skel:blendShapeTargets = </Root/SR/BSMesh/Smile>

            def BlendShape "Smile"
            {
                uniform vector3f[] offsets = [(0, 0.5, 0), (0, 0.5, 0)]
                uniform int[] pointIndices = [0, 2]
            }
        }
    }

    def SkelAnimation "Anim"
    {
        uniform token[] joints = ["root", "root/child"]
        uniform token[] blendShapes = ["smile"]
        float3[] translations.timeSamples = {
            0: [(0, 0, 0), (1, 0, 0)],
            1: [(0, 1, 0), (1, 1, 0)]
        }
        quatf[] rotations.timeSamples = {
            0: [(1, 0, 0, 0), (1, 0, 0, 0)],
            1: [(1, 0, 0, 0), (0, 0, 0, 1)]
        }
        half3[] scales.timeSamples = {
            0: [(1, 1, 1), (1, 1, 1)],
            1: [(1, 1, 1), (2, 2, 2)]
        }
        float[] blendShapeWeights.timeSamples = {
            0: [0],
            1: [1]
        }
    }

    def DomeLight "Env"
    {
        asset inputs:texture:file = @env.hdr@
        rel light:link = </Root/Quad>
        rel shadow:link = </Root/SR/BSMesh>
        rel filters = </Root/EnvFilter>
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  assert(lr.success);

  ConverterConfig cfg;
  cfg.mesh.compute_tangents = true;
  RenderSceneConverter conv(cfg);
  ConvertResult res = conv.Convert(lr.stage);
  assert(res.success);
  RenderScene& scene = res.scene;

  // Tangents + faceVarying corner remap on the quad.
  {
    auto it = scene.mesh_by_path.find("/Root/Quad");
    assert(it != scene.mesh_by_path.end());
    RenderMesh& m = scene.meshes[static_cast<size_t>(it->second)];
    // Unit quad, axis-aligned UVs -> tangent +X, w=+1.
    assert(m.tangents.size() == m.point_count() * 4);
    assert(std::fabs(m.tangents[0] - 1.0f) < 1e-4f);
    assert(std::fabs(m.tangents[3] - 1.0f) < 1e-4f);  // handedness w
    // Corner remap parallel to triangulated_indices.
    assert(m.triangulated_face_vertex_indices.size() ==
           m.triangulated_indices.size());
    assert(m.triangulated_face_vertex_indices[0] == 0);
    assert(m.triangulated_face_vertex_indices[2] == 2);
    assert(m.triangulated_face_vertex_indices[5] == 3);
  }

  // Blendshape extraction (sparse, with pointIndices).
  {
    auto it = scene.mesh_by_path.find("/Root/SR/BSMesh");
    assert(it != scene.mesh_by_path.end());
    RenderMesh& m = scene.meshes[static_cast<size_t>(it->second)];
    assert(m.blend_shapes.size() == 1);
    assert(m.blend_shapes[0].name == "smile");
    assert(m.blend_shapes[0].point_offsets.size() == 6);  // 2 points x3
    assert(m.blend_shapes[0].point_indices.size() == 2 &&
           m.blend_shapes[0].point_indices[0] == 0 &&
           m.blend_shapes[0].point_indices[1] == 2);
  }

  // Face-varying mirrored UVs require per-corner tangents and negative
  // handedness on at least one corner.
  {
    auto it = scene.mesh_by_path.find("/Root/Mirror");
    assert(it != scene.mesh_by_path.end());
    RenderMesh& m = scene.meshes[static_cast<size_t>(it->second)];
    assert(m.tangents_interp == Interpolation::FaceVarying);
    assert(m.tangents.size() == m.triangulated_indices.size() * 4);
    bool saw_negative = false;
    for (size_t i = 3; i < m.tangents.size(); i += 4) {
      if (m.tangents[i] < 0.0f) saw_negative = true;
    }
    assert(saw_negative);
  }

  // Degenerate UVs should not produce NaN/Inf tangent data.
  {
    auto it = scene.mesh_by_path.find("/Root/DegenerateUV");
    assert(it != scene.mesh_by_path.end());
    RenderMesh& m = scene.meshes[static_cast<size_t>(it->second)];
    assert(!m.tangents.empty());
    for (size_t i = 0; i < m.tangents.size(); ++i) {
      assert(std::isfinite(m.tangents[i]));
    }
  }

  // SkelAnimation channels preserve full per-joint/per-weight arrays.
  {
    assert(scene.animations.size() == 1);
    const AnimationClip& clip = scene.animations[0];
    assert(clip.channels.size() == 4);
    bool saw_translations = false;
    bool saw_rotations = false;
    bool saw_scales = false;
    bool saw_weights = false;
    for (const AnimationChannel& ch : clip.channels) {
        assert(ch.is_skeletal);
        assert(ch.keyframes.size() == 2);
        assert(ch.target_skeleton == 0);
        assert(ch.target_skeleton_path == "/Root/SR/Skel");
        if (ch.property_name == "translations") {
          saw_translations = true;
          assert(ch.value_stride == 3);
          assert(ch.element_count == 2);
          assert(ch.joint_order.size() == 2);
          assert(ch.joint_remap.size() == 2);
          assert(ch.joint_remap[0] == 0);
          assert(ch.joint_remap[1] == 1);
          assert(ch.array_values.size() == 12);
          assert(std::fabs(ch.array_values[3] - 1.0f) < 1e-4f);
      } else if (ch.property_name == "rotations") {
        saw_rotations = true;
        assert(ch.value_stride == 4);
        assert(ch.element_count == 2);
        assert(ch.array_values.size() == 16);
        // Channels are xyzw (three.js quaternion order); authored USDA quats
        // are real-first. Frame 0 joint 0 = identity -> (0,0,0,1); frame 1
        // joint 1 authored (0,0,0,1) [180deg about Z] -> (0,0,1,0).
        assert(std::fabs(ch.array_values[3] - 1.0f) < 1e-4f);
        assert(std::fabs(ch.array_values[14] - 1.0f) < 1e-4f);
        assert(std::fabs(ch.array_values[15]) < 1e-4f);
      } else if (ch.property_name == "scales") {
        saw_scales = true;
        assert(ch.value_stride == 3);
        assert(ch.element_count == 2);
        assert(ch.array_values.size() == 12);
        assert(std::fabs(ch.array_values[9] - 2.0f) < 1e-4f);
      } else if (ch.property_name == "blendShapeWeights") {
        saw_weights = true;
        assert(ch.value_stride == 1);
        assert(ch.element_count == 1);
        assert(ch.blend_shape_order.size() == 1);
        assert(ch.array_values.size() == 2);
        assert(std::fabs(ch.array_values[1] - 1.0f) < 1e-4f);
      }
    }
    assert(saw_translations && saw_rotations && saw_scales && saw_weights);
    assert(scene.skeletons.size() == 1);
    assert(scene.skeletons[0].animation_id == 0);
    assert(scene.skeletons[0].animation_source_path == "/Root/Anim");
  }

  // DomeLight environment texture imported as an image.
  {
    assert(scene.lights.size() == 1);
    const RenderLight& l = scene.lights[0];
    assert(l.type == LightType::Dome);
    assert(l.params.dome.texture_id >= 0);
    assert(static_cast<size_t>(l.params.dome.texture_id) < scene.images.size());
    assert(l.light_link_targets.size() == 1 &&
           l.light_link_targets[0] == "/Root/Quad");
    assert(l.shadow_link_targets.size() == 1 &&
           l.shadow_link_targets[0] == "/Root/SR/BSMesh");
    assert(l.filter_targets.size() == 1 &&
           l.filter_targets[0] == "/Root/EnvFilter");
  }

  std::cout << "  TestAudit2026_07_Gaps PASSED" << std::endl;
}

void TestPhysicsAnnotations() {
  std::cout << "Testing USD Physics annotations...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "Root"
    upAxis = "Z"
)

def Xform "Root"
{
    def PhysicsScene "Scene"
    {
    }

    def Xform "Body" (
        prepend apiSchemas = [
            "PhysicsRigidBodyAPI",
            "PhysicsMassAPI",
            "PhysicsArticulationRootAPI",
            "PhysicsFilteredPairsAPI"
        ]
    )
    {
        bool physics:kinematicEnabled = true
        vector3f physics:velocity = (1, 2, 3)
        float physics:mass = 4
        float3 physics:diagonalInertia = (5, 6, 7)
        uniform int mjc:freeJoint = 1
        rel physics:filteredPairs = </Root/Collider>
    }

    def Mesh "Collider" (
        prepend apiSchemas = [
            "PhysicsCollisionAPI",
            "PhysicsMeshCollisionAPI",
            "PhysicsMaterialAPI"
        ]
    )
    {
        bool physics:collisionEnabled = false
        token physics:approximation = "convexHull"
        float physics:staticFriction = 0.7
    }

    def PhysicsRevoluteJoint "Joint"
    {
        rel physics:body0 = </Root/Body>
        rel physics:body1 = </Root/Collider>
        token physics:axis = "Z"
        float physics:lowerLimit = -90
        float physics:upperLimit = 90
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  assert(lr.success);

  RenderSceneConverter conv;
  ConvertResult res = conv.Convert(lr.stage);
  assert(res.success);
  const RenderScene& scene = res.scene;

  assert(scene.physics.scenes.size() == 1);
  assert(scene.physics.scenes[0].prim_path == "/Root/Scene");
  assert(std::fabs(scene.physics.scenes[0].gravity_magnitude - 9.81f) <
         0.001f);
  assert(std::fabs(scene.physics.scenes[0].gravity_direction.z + 1.0f) <
         0.001f);

  assert(scene.physics.rigid_bodies.size() == 1);
  const PhysicsRigidBodyAnnotation& body = scene.physics.rigid_bodies[0];
  assert(body.prim_path == "/Root/Body");
  assert(body.kinematic_enabled);
  assert(body.has_mass);
  assert(std::fabs(body.mass - 4.0f) < 0.001f);
  assert(std::fabs(body.diagonal_inertia.z - 7.0f) < 0.001f);
  assert(!body.extension_properties.empty());
  assert(body.extension_properties[0].name == "mjc:freeJoint");

  assert(scene.physics.colliders.size() == 1);
  assert(scene.physics.colliders[0].prim_path == "/Root/Collider");
  assert(!scene.physics.colliders[0].collision_enabled);
  assert(scene.physics.colliders[0].has_mesh_collision);
  assert(scene.physics.colliders[0].approximation == "convexHull");

  assert(scene.physics.materials.size() == 1);
  assert(std::fabs(scene.physics.materials[0].static_friction - 0.7f) <
         0.001f);

  assert(scene.physics.joints.size() == 1);
  const PhysicsJointAnnotation& joint = scene.physics.joints[0];
  assert(joint.type_name == "PhysicsRevoluteJoint");
  assert(joint.body0 == "/Root/Body");
  assert(joint.body1 == "/Root/Collider");
  assert(std::fabs(joint.axis.z - 1.0f) < 0.001f);
  assert(std::fabs(joint.lower_limit + 90.0f) < 0.001f);

  assert(scene.physics.filtered_pairs.size() == 1);
  assert(scene.physics.filtered_pairs[0].filtered_pair_paths.size() == 1);
  assert(scene.physics.filtered_pairs[0].filtered_pair_paths[0] ==
         "/Root/Collider");
  assert(scene.physics.articulation_roots.size() == 1);
  assert(scene.physics.articulation_roots[0] == "/Root/Body");

  std::cout << "  USD Physics annotations: PASSED\n";
}

// Regression for the ChunkedArray straddle OOB in the tangent generator.
// The Lengyel path read points/normals/uvs via `&mesh->points[i*3]` and then
// indexed off that pointer, which runs past the chunk allocation once a mesh
// exceeds kElementsPerChunk/3 (~5461) points. Build a grid well past that and
// require every tangent to come out finite and correct.
void TestLargeMeshTangents() {
  std::cout << "Testing tangents on a multi-chunk mesh...\n";

  constexpr uint32_t N = 100;  // 10000 points > 16384/3
  std::string pts, sts, nrm, counts, indices;
  for (uint32_t y = 0; y < N; ++y) {
    for (uint32_t x = 0; x < N; ++x) {
      if (!pts.empty()) { pts += ", "; sts += ", "; nrm += ", "; }
      pts += "(" + std::to_string(x) + ", " + std::to_string(y) + ", 0)";
      sts += "(" + std::to_string(x) + ", " + std::to_string(y) + ")";
      nrm += "(0, 0, 1)";
    }
  }
  for (uint32_t y = 0; y + 1 < N; ++y) {
    for (uint32_t x = 0; x + 1 < N; ++x) {
      const uint32_t i0 = y * N + x, i1 = i0 + 1, i2 = i0 + N + 1,
                     i3 = i0 + N;
      if (!counts.empty()) { counts += ", "; indices += ", "; }
      counts += "4";
      indices += std::to_string(i0) + ", " + std::to_string(i1) + ", " +
                 std::to_string(i2) + ", " + std::to_string(i3);
    }
  }

  const std::string usda =
      "#usda 1.0\n"
      "def Mesh \"Grid\"\n{\n"
      "  point3f[] points = [" + pts + "]\n"
      "  normal3f[] normals = [" + nrm + "] (interpolation = \"vertex\")\n"
      "  texCoord2f[] primvars:st = [" + sts +
      "] (interpolation = \"vertex\")\n"
      "  int[] faceVertexCounts = [" + counts + "]\n"
      "  int[] faceVertexIndices = [" + indices + "]\n}\n";

  LoadResult lr = LoadUSDAFromString(usda.c_str(), usda.size());
  assert(lr.success);

  const MeshConfig::TangentComputationMethod methods[] = {
      MeshConfig::TangentComputationMethod::Lengyel,
      MeshConfig::TangentComputationMethod::Hybrid,
      MeshConfig::TangentComputationMethod::MikkTSpace,
  };
  for (auto method : methods) {
    ConverterConfig cfg;
    cfg.mesh.compute_tangents = true;
    cfg.mesh.tangent_method = method;
    cfg.mesh.build_vertex_indices = false;  // keep the authored point order
    RenderSceneConverter conv(cfg);
    ConvertResult res = conv.Convert(lr.stage);
    assert(res.success);
    auto it = res.scene.mesh_by_path.find("/Grid");
    assert(it != res.scene.mesh_by_path.end());
    RenderMesh& m = res.scene.meshes[static_cast<size_t>(it->second)];
    assert(m.point_count() > 5461);  // must span multiple float chunks
    assert(m.tangents.size() % 4 == 0);
    assert(m.tangents.size() > 0);
    for (size_t i = 0; i < m.tangents.size(); ++i) {
      assert(std::isfinite(m.tangents[i]));
    }
    // Planar grid with axis-aligned UVs: tangent is +X everywhere.
    for (size_t v = 0; v + 3 < m.tangents.size(); v += 4) {
      assert(std::fabs(m.tangents[v + 0] - 1.0f) < 1e-3f);
      assert(std::fabs(m.tangents[v + 1]) < 1e-3f);
      assert(std::fabs(m.tangents[v + 2]) < 1e-3f);
    }
  }

  std::cout << "  multi-chunk mesh tangents passed!\n";
}

// MemBudget wiring: with a cap below what the scene needs, conversion must
// degrade gracefully (warn + stop adding geometry) instead of running to OOM.
// The converter's other limits are all per-prim, so this specifically covers
// MANY small meshes summing past the cap -- the case none of them can see.
void TestConverterMemoryBudget() {
  std::cout << "Testing converter cumulative memory budget...\n";

  // Many small meshes; each one individually passes every per-prim probe.
  std::string usda = "#usda 1.0\n";
  const int kMeshes = 400;
  for (int i = 0; i < kMeshes; ++i) {
    usda += "def Mesh \"M" + std::to_string(i) +
            "\"\n{\n"
            "  point3f[] points = [(0,0,0), (1,0,0), (1,1,0), (0,1,0)]\n"
            "  int[] faceVertexCounts = [4]\n"
            "  int[] faceVertexIndices = [0, 1, 2, 3]\n}\n";
  }
  LoadResult lr = LoadUSDAFromString(usda.c_str(), usda.size());
  assert(lr.success);

  const size_t saved_cap = MemBudget::Get().Cap();

  // Baseline: the generous default cap converts everything.
  {
    ConverterConfig cfg;
    RenderSceneConverter conv(cfg);
    ConvertResult res = conv.Convert(lr.stage);
    assert(res.success);
    assert(res.scene.meshes.size() == static_cast<size_t>(kMeshes));
  }

  // A cap far below current RSS trips the guard on the first check.
  {
    MemBudget::Get().InitBytes(1);  // 1 byte: unsatisfiable by construction
    ConverterConfig cfg;
    RenderSceneConverter conv(cfg);
    ConvertResult res = conv.Convert(lr.stage);
    // Graceful: no crash, fewer meshes than authored, and a diagnostic.
    assert(res.scene.meshes.size() < static_cast<size_t>(kMeshes));
    bool warned = false;
    for (const std::string& w : res.warnings) {
      if (w.find("Memory budget reached") != std::string::npos) warned = true;
    }
    assert(warned);
  }

  MemBudget::Get().InitBytes(saved_cap);

  // A converter reused across APIs must not retain a previous operation's
  // latched budget state. Trip the global guard in Convert(), restore it, then
  // stream the same stage through the same converter.
  {
    ConverterConfig cfg;
    RenderSceneConverter conv(cfg);
    MemBudget::Get().InitBytes(1);
    ConvertResult limited = conv.Convert(lr.stage);
    assert(limited.scene.meshes.size() < static_cast<size_t>(kMeshes));
    MemBudget::Get().InitBytes(saved_cap);
    RetainedStreamSink sink;
    StreamConvertResult streamed = conv.ConvertToSink(lr.stage, &sink);
    assert(streamed.success);
    assert(streamed.mesh_count == static_cast<size_t>(kMeshes));
  }

  // The hardened, operation-owned cap must work without mutating the global
  // process budget, and converter reuse must reset its accounting per run.
  {
    ConverterConfig cfg = MakeHardenedConverterConfig(1);
    RenderSceneConverter conv(cfg);
    ConvertResult first = conv.Convert(lr.stage);
    ConvertResult second = conv.Convert(lr.stage);
    assert(first.scene.meshes.size() < static_cast<size_t>(kMeshes));
    assert(second.scene.meshes.size() == first.scene.meshes.size());
    bool warned = false;
    for (const std::string& w : first.warnings) {
      if (w.find("Operation memory limit reached") != std::string::npos) {
        warned = true;
      }
    }
    assert(warned);
  }

  std::cout << "  converter cumulative memory budget passed!\n";
}

// Light-link collection resolution after the sorted-prefix-range rewrite.
// The important trap is the sibling whose name EXTENDS an include target
// ("/W/C" vs "/W/C2"): a naive prefix test matches both, so the range must
// stop at the '/' boundary.
void TestLightLinkCollectionRanges() {
  std::cout << "Testing light-link collection path ranges...\n";

  const char* usda = R"(#usda 1.0
def Xform "W"
{
    def Xform "A"
    {
        def Mesh "M0" { point3f[] points = [(0,0,0), (1,0,0), (1,1,0)] int[] faceVertexCounts = [3] int[] faceVertexIndices = [0, 1, 2] }
        def Xform "B"
        {
            def Mesh "M1" { point3f[] points = [(0,0,0), (1,0,0), (1,1,0)] int[] faceVertexCounts = [3] int[] faceVertexIndices = [0, 1, 2] }
        }
    }
    def Xform "C"
    {
        def Mesh "M2" { point3f[] points = [(0,0,0), (1,0,0), (1,1,0)] int[] faceVertexCounts = [3] int[] faceVertexIndices = [0, 1, 2] }
    }
    def Xform "C2"
    {
        def Mesh "M3" { point3f[] points = [(0,0,0), (1,0,0), (1,1,0)] int[] faceVertexCounts = [3] int[] faceVertexIndices = [0, 1, 2] }
    }
    def SphereLight "L"
    {
        rel collection:lightLink:includes = [</W/A>, </W/C>]
        rel collection:lightLink:excludes = [</W/A/B>]
    }
}
)";
  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  assert(lr.success);
  ConverterConfig cfg;
  RenderSceneConverter conv(cfg);
  ConvertResult res = conv.Convert(lr.stage);
  assert(res.success);
  assert(res.scene.lights.size() == 1);

  const RenderLight& light = res.scene.lights[0];
  assert(!light.light_links_all);

  std::set<std::string> linked;
  for (int32_t mi : light.light_link_mesh_indices) {
    linked.insert(res.scene.meshes[static_cast<size_t>(mi)].prim_path);
  }
  // /W/A subtree minus the excluded /W/A/B, plus the /W/C subtree.
  assert(linked.count("/W/A/M0") == 1);
  assert(linked.count("/W/C/M2") == 1);
  assert(linked.count("/W/A/B/M1") == 0);  // excluded
  // "/W/C2" merely shares a prefix with "/W/C" -- it is NOT a descendant.
  assert(linked.count("/W/C2/M3") == 0);
  assert(linked.size() == 2);

  // Emitted in ascending mesh-index order, as the previous full scan did.
  for (size_t i = 1; i < light.light_link_mesh_indices.size(); ++i) {
    assert(light.light_link_mesh_indices[i - 1] <
           light.light_link_mesh_indices[i]);
  }

  std::cout << "  light-link collection path ranges passed!\n";
}

// .usdz entry matching. TextureDecoder prefilters candidate entries by
// basename, which is only valid because EVERY tier below implies the
// basenames are equal -- these cases pin that invariant along with the
// matching rules themselves.
void TestUsdzEntryMatching() {
  std::cout << "Testing usdz entry matching...\n";
  using lightusd::tydra::next::UsdzEntryMatches;

  // Exact.
  assert(UsdzEntryMatches("tex.png", "tex.png"));
  // Leading "./" on the authored path is ignored.
  assert(UsdzEntryMatches("tex.png", "./tex.png"));
  assert(UsdzEntryMatches("textures/tex.png", "./textures/tex.png"));
  // Entry carries a directory prefix the authored path omits.
  assert(UsdzEntryMatches("assets/textures/tex.png", "textures/tex.png"));
  assert(UsdzEntryMatches("a/b/c/tex.png", "tex.png"));
  // Basename fallback: differing directories still match.
  assert(UsdzEntryMatches("one/tex.png", "two/tex.png"));

  // Suffix must land on a '/' boundary, not mid-name. "othertex.png" ends with
  // "tex.png" but is a different file -- it may only match by basename, which
  // it does not.
  assert(!UsdzEntryMatches("othertex.png", "tex.png"));
  assert(!UsdzEntryMatches("tex.png", "othertex.png"));
  // Different basenames never match.
  assert(!UsdzEntryMatches("a/other.png", "b/tex.png"));
  assert(!UsdzEntryMatches("", "tex.png"));

  // The prefilter invariant: any match implies equal basenames.
  auto basename = [](const std::string& p) {
    const size_t q = p.find_last_of('/');
    return q == std::string::npos ? p : p.substr(q + 1);
  };
  const char* entries[] = {"tex.png", "a/tex.png", "a/b/tex.png",
                           "othertex.png", "a/other.png", "tex.PNG"};
  const char* assets[] = {"tex.png", "./tex.png", "textures/tex.png",
                          "other.png", "othertex.png"};
  for (const char* e : entries) {
    for (const char* a : assets) {
      if (!UsdzEntryMatches(e, a)) continue;
      std::string an = a;
      if (an.rfind("./", 0) == 0) an = an.substr(2);
      assert(basename(e) == basename(an));
    }
  }

  std::cout << "  usdz entry matching passed!\n";
}

// faceVertexIndices pointing past the points array must be rejected by
// TriangulateMesh itself, not merely by SanitizeMeshTopology upstream: the
// triangulator is also reached from ConvertGeomPrimitive/ConvertBoundsProxy
// and from the self-triangulation inside ComputeVertexNormals/Tangents, none
// of which sanitize first. Every index feeds mesh->points lookups (the quad
// diagonal test, the earcut projection) and lands in triangulated_indices,
// which normal/tangent generation later dereferences.
void TestTriangulateOutOfRangeIndices() {
  std::cout << "Testing triangulation with out-of-range indices...\n";

  // 4 points, but faces reference vertex 9 (quad path) and vertex 12 (n-gon
  // earcut path). One face is entirely valid and must survive.
  const char* usda = R"(#usda 1.0
def Mesh "M"
{
    point3f[] points = [(0,0,0), (1,0,0), (1,1,0), (0,1,0)]
    int[] faceVertexCounts = [3, 4, 5]
    int[] faceVertexIndices = [
        0, 1, 2,
        0, 1, 2, 9,
        0, 1, 2, 3, 12
    ]
}
)";
  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  assert(lr.success);

  for (bool compute_normals : {false, true}) {
    ConverterConfig cfg;
    cfg.mesh.triangulate = true;
    cfg.mesh.compute_normals = compute_normals;
    cfg.mesh.build_vertex_indices = false;
    RenderSceneConverter conv(cfg);
    ConvertResult res = conv.Convert(lr.stage);
    assert(res.success);
    auto it = res.scene.mesh_by_path.find("/M");
    if (it == res.scene.mesh_by_path.end()) continue;  // dropped is also safe
    const RenderMesh& m = res.scene.meshes[static_cast<size_t>(it->second)];

    // Whatever survived must be in range -- this is the invariant every
    // downstream consumer relies on.
    const uint32_t np = static_cast<uint32_t>(m.point_count());
    for (size_t i = 0; i < m.triangulated_indices.size(); ++i) {
      assert(m.triangulated_indices[i] < np);
    }
    // face_triangle_offsets stays monotonic and consistent with the output.
    if (!m.face_triangle_offsets.empty()) {
      for (size_t i = 1; i < m.face_triangle_offsets.size(); ++i) {
        assert(m.face_triangle_offsets[i] >= m.face_triangle_offsets[i - 1]);
      }
      assert(m.face_triangle_offsets.back() ==
             m.triangulated_indices.size() / 3);
    }
    if (compute_normals && m.has_normals()) {
      for (size_t i = 0; i < m.normals.size(); ++i) {
        assert(std::isfinite(m.normals[i]));
      }
    }
  }

  std::cout << "  triangulation with out-of-range indices passed!\n";
}

void TestRenderConverterCurves() {
  std::cout << "Testing RenderConverter curves...\n";

  const char* usda = R"(#usda 1.0
def Xform "World"
{
    def BasisCurves "Linear"
    {
        uniform token type = "linear"
        uniform token wrap = "nonperiodic"
        int[] curveVertexCounts = [3, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0),
                            (2, 0, 0), (2, 1, 0)]
        float[] widths = [0.1, 0.6]
        color3f[] primvars:displayColor = [(0.25, 0.5, 0.75)] (
            interpolation = "constant"
        )
        float[] primvars:displayOpacity = [0.1, 0.3, 0.5, 0.7, 0.9] (
            interpolation = "vertex"
        )
    }

    def BasisCurves "Bezier"
    {
        uniform token type = "cubic"
        uniform token basis = "bezier"
        uniform token wrap = "nonperiodic"
        int[] curveVertexCounts = [4]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (2, 1, 0)]
    }

    def HermiteCurves "Hermite"
    {
        int[] curveVertexCounts = [2]
        point3f[] points = [(0, 0, 0), (1, 0, 0)]
        vector3f[] tangents = [(1, 2, 0), (1, -2, 0)]
    }

    def BasisCurves "PeriodicCatmullRom"
    {
        uniform token type = "cubic"
        uniform token basis = "catmullRom"
        uniform token wrap = "periodic"
        int[] curveVertexCounts = [4]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    }

    def NurbsCurves "Nurbs"
    {
        int[] curveVertexCounts = [4]
        int[] order = [3]
        double[] knots = [0, 0, 0, 1, 2, 2, 2]
        double2[] ranges = [(0, 2)]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (2, 1, 0)]
    }

    def NurbsCurves "BadNurbs"
    {
        int[] curveVertexCounts = [4]
        int[] order = [3]
        double[] knots = [0, 0]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (2, 1, 0)]
    }
}
)";

  auto test_urdf_mjcf_conversion = []() {
  std::cout << "Testing URDF/MJCF next-core conversion...\n";

  const char* payload = R"JSON({
    "name": "test_robot",
    "sourceFormat": "mjcf",
    "upAxis": "Z",
    "gravity": [0, 0, -1],
    "timestep": 0.001,
    "newton": {"selfCollisionEnabled": false},
    "links": [
      {
        "name": "base-link",
        "inertial": {
          "mass": 2.5,
          "centerOfMass": [0.1, 0.2, 0.3],
          "diagonalInertia": [1, 2, 3]
        },
        "visuals": [{"name": "body", "meshRef": "body_mesh"}],
        "collisions": [{
          "name": "collider",
          "shape": {"type": "sphere", "radius": 0.25},
          "mjc": {"group": 4, "contype": 2, "conaffinity": 3}
        }]
      },
      {"name": "tip", "visuals": [], "collisions": []}
    ],
    "joints": [{
      "name": "hinge",
      "type": "revolute",
      "parent": "base-link",
      "child": "tip",
      "axis": [0, 0, 1],
      "localPos0": [0, 0, 0.5],
      "localPos1": [0, 0, 0],
      "limit": {"lower": -1, "upper": 1},
      "dynamics": {"damping": 0.5, "stiffness": 3, "armature": 0.02}
    }],
    "tendons": [{"name": "cable", "stiffness": 10}],
    "mjcActuators": [{"name": "motor", "gear": [2]}],
    "keyframes": [{"name": "home", "qpos": [0]}],
    "sensors": [{"name": "position", "type": "jointpos"}]
  })JSON";

  lightusd::tydra::next::URDFMeshBuffer mesh;
  mesh.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  mesh.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
  mesh.uvs = {0, 0, 1, 0, 0, 1};
  mesh.indices = {0, 1, 2};
  std::map<std::string, lightusd::tydra::next::URDFMeshBuffer> meshes;
  meshes["body_mesh"] = mesh;

  lightusd::next::Stage stage;
  std::string warn;
  std::string err;
  assert(lightusd::tydra::next::ConvertURDFJsonToUSDStage(
      payload, &meshes, &stage, &warn, &err));
  assert(err.empty());
  assert(stage.GetMeta().defaultPrim == "World");
  assert(stage.GetMeta().upAxis == "Z");

  UsdPrim scene = stage.GetPrimAtPath("/World/PhysicsScene");
  assert(scene.IsValid());
  assert(scene.GetTypeName() == "PhysicsScene");
  const Value* timestep = scene.GetPropertyValue("mjc:timestep");
  assert(timestep && timestep->as_double());
  assert(std::fabs(*timestep->as_double() - 0.001) < 1.0e-9);

  UsdPrim base = stage.GetPrimAtPath("/World/Links/base_link");
  assert(base.IsValid());
  const Value* mass = base.GetPropertyValue("physics:mass");
  assert(mass && mass->as_float());
  assert(std::fabs(*mass->as_float() - 2.5f) < 0.001f);
  assert(std::find(base.GetMeta().apiSchemas().begin(),
                   base.GetMeta().apiSchemas().end(),
                   "PhysicsRigidBodyAPI") != base.GetMeta().apiSchemas().end());

  UsdPrim visual = stage.GetPrimAtPath("/World/Links/base_link/body");
  assert(visual.IsValid());
  assert(visual.GetTypeName() == "Mesh");
  assert(visual.GetPropertyValue("points"));

  UsdPrim collider = stage.GetPrimAtPath("/World/Links/base_link/collider");
  assert(collider.IsValid());
  assert(collider.GetTypeName() == "Sphere");
  const Value* group = collider.GetPropertyValue("mjc:group");
  assert(group && group->as_int() && *group->as_int() == 4);

  UsdPrim joint = stage.GetPrimAtPath("/World/Joints/hinge");
  assert(joint.IsValid());
  assert(joint.GetTypeName() == "PhysicsRevoluteJoint");
  const std::vector<Path>* body0 = joint.GetRelationship("physics:body0");
  const std::vector<Path>* body1 = joint.GetRelationship("physics:body1");
  assert(body0 && body0->size() == 1 && (*body0)[0].str() == "/World/Links/base_link");
  assert(body1 && body1->size() == 1 && (*body1)[0].str() == "/World/Links/tip");
  const Value* damping = joint.GetPropertyValue("mjc:damping");
  assert(damping && damping->as_double());
  assert(std::fabs(*damping->as_double() - 0.5) < 1.0e-9);

  assert(stage.HasPrimAtPath("/World/Tendons/cable"));
  assert(stage.HasPrimAtPath("/World/MjcActuators/motor"));
  assert(stage.HasPrimAtPath("/World/Keyframes/home"));
  assert(stage.HasPrimAtPath("/World/Sensors/position"));

  std::cout << "  URDF/MJCF next-core conversion: PASSED\n";
  };
  test_urdf_mjcf_conversion();

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  assert(lr.success);

  ConverterConfig cfg;
  cfg.curves.tessellation_segments = 2;
  RenderSceneConverter converter(cfg);
  ConvertResult result = converter.Convert(lr.stage);
  assert(result.success);
  assert(result.scene.curves.size() == 6);
  assert(result.scene.get_stats().curves_count == 6);
  assert(result.scene.get_stats().curve_count == 7);

  auto curve = [&](const char* path) -> const RenderCurves& {
    auto it = result.scene.curves_by_path.find(path);
    assert(it != result.scene.curves_by_path.end());
    const RenderCurves* value = result.scene.get_curves(it->second);
    assert(value);
    return *value;
  };

  const RenderCurves& linear = curve("/World/Linear");
  assert(linear.type == CurveType::Linear);
  assert(linear.curve_vertex_counts == std::vector<uint32_t>({3, 2}));
  assert(linear.tessellated_vertex_counts ==
         std::vector<uint32_t>({3, 2}));
  assert(linear.tessellated_widths.size() == 5);
  assert(linear.widths_interp == Interpolation::Uniform);
  assert(std::fabs(linear.tessellated_widths[0] - 0.1f) < 0.001f);
  assert(std::fabs(linear.tessellated_widths[2] - 0.1f) < 0.001f);
  assert(std::fabs(linear.tessellated_widths[3] - 0.6f) < 0.001f);
  assert(std::fabs(linear.tessellated_widths[4] - 0.6f) < 0.001f);
  assert(linear.colors.size() == 3);
  assert(linear.colors_interp == Interpolation::Constant);
  assert(linear.tessellated_colors.size() == 15);
  assert(linear.opacities.size() == 5);
  assert(linear.opacities_interp == Interpolation::Vertex);
  assert(linear.tessellated_opacities.size() == 5);
  assert(std::fabs(linear.tessellated_opacities[0] - 0.1f) < 0.001f);
  assert(std::fabs(linear.tessellated_opacities[4] - 0.9f) < 0.001f);
  assert(linear.has_bbox);

  const RenderCurves& bezier = curve("/World/Bezier");
  assert(bezier.type == CurveType::Cubic);
  assert(bezier.basis == CurveBasis::Bezier);
  assert(bezier.tessellated_vertex_counts == std::vector<uint32_t>({3}));
  assert(std::fabs(bezier.tessellated_points[0]) < 0.001f);
  assert(std::fabs(bezier.tessellated_points[8] - 0.0f) < 0.001f);
  assert(std::fabs(bezier.tessellated_points[6] - 2.0f) < 0.001f);
  assert(std::fabs(bezier.tessellated_points[7] - 1.0f) < 0.001f);

  const RenderCurves& hermite = curve("/World/Hermite");
  assert(hermite.is_hermite);
  assert(hermite.tessellated_vertex_counts == std::vector<uint32_t>({3}));
  assert(std::fabs(hermite.tessellated_points[0]) < 0.001f);
  assert(std::fabs(hermite.tessellated_points[3] - 0.5f) < 0.001f);
  assert(std::fabs(hermite.tessellated_points[4] - 0.5f) < 0.001f);
  assert(std::fabs(hermite.tessellated_points[6] - 1.0f) < 0.001f);
  assert(std::fabs(hermite.tessellated_points[7]) < 0.001f);

  const RenderCurves& periodic = curve("/World/PeriodicCatmullRom");
  assert(periodic.wrap == CurveWrap::Periodic);
  assert(periodic.tessellated_vertex_counts == std::vector<uint32_t>({9}));
  const size_t last = periodic.tessellated_points.size() - 3;
  assert(std::fabs(periodic.tessellated_points[0] -
                   periodic.tessellated_points[last]) < 0.001f);
  assert(std::fabs(periodic.tessellated_points[1] -
                   periodic.tessellated_points[last + 1]) < 0.001f);

  const RenderCurves& nurbs = curve("/World/Nurbs");
  assert(nurbs.is_nurbs);
  assert(nurbs.tessellated_vertex_counts == std::vector<uint32_t>({5}));
  assert(std::fabs(nurbs.tessellated_points[0]) < 0.001f);
  assert(std::fabs(nurbs.tessellated_points[nurbs.tessellated_points.size() - 3] -
                   2.0f) < 0.001f);

  const RenderCurves& bad_nurbs = curve("/World/BadNurbs");
  assert(bad_nurbs.tessellated_vertex_counts ==
         std::vector<uint32_t>({4}));
  bool saw_fallback = false;
  for (const std::string& warning : result.warnings) {
    if (warning.find("BadNurbs") != std::string::npos &&
        warning.find("control-polygon passthrough") != std::string::npos) {
      saw_fallback = true;
    }
  }
  assert(saw_fallback);

  auto node_it = result.scene.node_by_path.find("/World/Bezier");
  assert(node_it != result.scene.node_by_path.end());
  const SceneNode& node = result.scene.nodes[node_it->second];
  assert(node.type == NodeType::Curves);
  assert(node.data_id >= 0);

  std::cout << "  RenderConverter curves: PASSED\n";
}

void TestLegacyParityExtraction() {
  std::cout << "Testing legacy schema parity extraction...\n";

  const char* usda = R"(#usda 1.0
def Xform "Root"
{
    def Material "Mat"
    {
        string config:mtlx:version = "1.38"
        token config:mtlx:namespace = "mtlx"
        token config:mtlx:colorspace = "lin_rec709"
        asset config:mtlx:sourceUri = @looks/test.mtlx@

        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.25, 0.5, 0.75)
            token outputs:surface
        }
    }

    def Cube "Box"
    {
        rel material:binding = </Root/Mat>
        double size = 4
    }

    def Sphere "Ball"
    {
        double radius = 1.5
    }

    def Cylinder "Pipe"
    {
        token axis = "Z"
        double radius = 0.5
        double height = 3
    }

    def Plane "Ground"
    {
        token axis = "Z"
        double width = 6
        double length = 8
    }

    def PointLight "Point"
    {
        bool inputs:enableColorTemperature = true
        float inputs:colorTemperature = 4200
        float inputs:diffuse = 0.75
        float inputs:specular = 0.25
        float inputs:shaping:focus = 0.5
        color3f inputs:shaping:focusTint = (0.2, 0.3, 0.4)
        float inputs:shaping:cone:softness = 0.1
        asset inputs:shaping:ies:file = @profiles/key.ies@
        float inputs:shaping:ies:angleScale = 1.5
        bool inputs:shaping:ies:normalize = true
        color3f inputs:shadow:color = (0.1, 0.2, 0.3)
        float inputs:shadow:distance = 12
        float inputs:shadow:falloff = 2
        float inputs:shadow:falloffGamma = 1.5
    }
    def GeometryLight "GeoLight" {}
    def DomeLight_1 "Env" {}
    def LightFilter "Filter" {}

    def BasisCurves "Curve" {}
    def TetMesh "Tets"
    {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0),
                            (0, 0, 1), (0, 0, -1)]
        int4[] tetVertexIndices = [(0, 1, 2, 3), (0, 2, 1, 4)]
    }
    def Points "Pts"
    {
        point3f[] points = [(0, 0, 0), (1, 2, 3), (-1, 0, 2)]
        float[] widths = [0.1, 0.2, 0.3]
        color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0), (0, 0, 1)] (
            interpolation = "vertex"
        )
        float[] primvars:displayOpacity = [0.2, 0.6, 1] (
            interpolation = "vertex"
        )
    }
    def Volume "Fog" {}
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  assert(lr.success);

  RenderExtractOptions opts;
  opts.collect_other = true;
  RenderExtractResult er;
  assert(CollectRenderPrims(lr.stage, opts, &er));
  assert(er.meshes.size() == 5);
  assert(er.lights.size() == 4);
  assert(er.curves.size() == 1);
  assert(er.volumes.size() == 1);

  RenderSceneConverter conv;
  ConvertResult res = conv.Convert(lr.stage);
  assert(res.success);
  RenderScene& scene = res.scene;

  auto box_it = scene.mesh_by_path.find("/Root/Box");
  assert(box_it != scene.mesh_by_path.end());
  const RenderMesh& box = scene.meshes[static_cast<size_t>(box_it->second)];
  assert(box.point_count() == 8);
  assert(box.face_count() == 6);
  assert(box.is_triangulated);
  assert(box.material_id >= 0);
  assert(std::fabs(box.bbox_min.x + 2.0f) < 0.001f);
  assert(std::fabs(box.bbox_max.x - 2.0f) < 0.001f);

  auto pipe_it = scene.mesh_by_path.find("/Root/Pipe");
  assert(pipe_it != scene.mesh_by_path.end());
  const RenderMesh& pipe = scene.meshes[static_cast<size_t>(pipe_it->second)];
  assert(pipe.point_count() > 0);
  assert(pipe.has_bbox);
  assert(std::fabs(pipe.bbox_min.z + 1.5f) < 0.001f);
  assert(std::fabs(pipe.bbox_max.z - 1.5f) < 0.001f);

  auto tet_it = scene.mesh_by_path.find("/Root/Tets");
  assert(tet_it != scene.mesh_by_path.end());
  const RenderMesh& tets = scene.meshes[static_cast<size_t>(tet_it->second)];
  assert(tets.point_count() == 5);
  assert(tets.face_count() == 6);  // shared face is removed
  assert(tets.triangulated_indices.size() == 18);
  assert(tets.has_bbox);
  assert(std::fabs(tets.bbox_min.z + 1.0f) < 0.001f);
  assert(std::fabs(tets.bbox_max.z - 1.0f) < 0.001f);

  auto mat_it = scene.material_by_path.find("/Root/Mat");
  assert(mat_it != scene.material_by_path.end());
  const RenderMaterial& mat =
      scene.materials[static_cast<size_t>(mat_it->second)];
  assert(mat.mtlx_config.authored);
  assert(mat.mtlx_config.version == "1.38");
  assert(mat.mtlx_config.name_space == "mtlx");
  assert(mat.mtlx_config.colorspace == "lin_rec709");
  assert(mat.mtlx_config.source_uri == "looks/test.mtlx");

  bool saw_point = false;
  bool saw_geometry = false;
  bool saw_dome = false;
  bool saw_filter_fallback = false;
  for (const RenderLight& light : scene.lights) {
    if (light.prim_path == "/Root/Point" && light.type == LightType::Point) {
      saw_point = true;
      assert(light.enable_color_temperature);
      assert(std::fabs(light.color_temperature - 4200.0f) < 0.001f);
      assert(std::fabs(light.diffuse - 0.75f) < 0.001f);
      assert(std::fabs(light.specular - 0.25f) < 0.001f);
      assert(std::fabs(light.shaping_focus - 0.5f) < 0.001f);
      assert(std::fabs(light.shaping_focus_tint.x - 0.2f) < 0.001f);
      assert(std::fabs(light.shaping_focus_tint.y - 0.3f) < 0.001f);
      assert(std::fabs(light.shaping_focus_tint.z - 0.4f) < 0.001f);
      assert(std::fabs(light.shaping_cone_softness - 0.1f) < 0.001f);
      assert(light.shaping_ies_file == "profiles/key.ies");
      assert(std::fabs(light.shaping_ies_angle_scale - 1.5f) < 0.001f);
      assert(light.shaping_ies_normalize);
      assert(std::fabs(light.shadow_color.x - 0.1f) < 0.001f);
      assert(std::fabs(light.shadow_color.y - 0.2f) < 0.001f);
      assert(std::fabs(light.shadow_color.z - 0.3f) < 0.001f);
      assert(std::fabs(light.shadow_distance - 12.0f) < 0.001f);
      assert(std::fabs(light.shadow_falloff - 2.0f) < 0.001f);
      assert(std::fabs(light.shadow_falloff_gamma - 1.5f) < 0.001f);
    }
    if (light.prim_path == "/Root/GeoLight" &&
        light.type == LightType::Geometry) {
      saw_geometry = true;
    }
    if (light.prim_path == "/Root/Env" && light.type == LightType::Dome) {
      saw_dome = true;
    }
    if (light.prim_path == "/Root/Filter" && light.type == LightType::Point &&
        light.intensity == 0.0f) {
      saw_filter_fallback = true;
    }
  }
  assert(saw_point);
  assert(saw_geometry);
  assert(saw_dome);
  assert(saw_filter_fallback);

  assert(scene.points.size() == 1);
  const RenderPoints& pts = scene.points[0];
  assert(pts.prim_path == "/Root/Pts");
  assert(pts.point_count() == 3);
  assert(pts.widths.size() == 3);
  assert(pts.colors.size() == 9);
  assert(pts.colors_interp == Interpolation::Vertex);
  assert(pts.opacities.size() == 3);
  assert(pts.opacities_interp == Interpolation::Vertex);
  assert(std::fabs(pts.opacities[0] - 0.2f) < 0.001f);
  assert(std::fabs(pts.opacities[2] - 1.0f) < 0.001f);
  assert(pts.has_bbox);
  assert(std::fabs(pts.bbox_min.x + 1.0f) < 0.001f);
  assert(std::fabs(pts.bbox_max.y - 2.0f) < 0.001f);
  auto pts_node = scene.node_by_path.find("/Root/Pts");
  assert(pts_node != scene.node_by_path.end());
  assert(scene.nodes[static_cast<size_t>(pts_node->second)].type == NodeType::Points);
  assert(scene.nodes[static_cast<size_t>(pts_node->second)].data_id == 0);

  // BasisCurves are converted now (the empty def fails conversion with a
  // warning), so only Volume remains unsupported.
  assert(scene.unsupported_renderables.size() == 1);
  assert(scene.unsupported_renderables[0].type_name == "Volume");
  assert(scene.curves.empty());  // "/Root/Curve" has no points/counts
  bool saw_curve_warning = false;
  for (const std::string& w : res.warnings) {
    if (w.find("Failed to convert curves prim: /Root/Curve") !=
        std::string::npos) {
      saw_curve_warning = true;
    }
  }
  assert(saw_curve_warning);

  std::cout << "  Legacy schema parity extraction: PASSED\n";
}

void TestValueClipBaking() {
  std::cout << "Testing next value-clip baking...\n";

  const std::string root = R"(#usda 1.0
(
    startTimeCode = 0
    endTimeCode = 2
)
def Xform "Root" (
    clips = {
        dictionary default = {
            double2[] active = [(0, 0), (2, 1)]
            asset[] assetPaths = [@clip_0.usda@, @clip_1.usda@]
            string primPath = "/Root"
            double2[] times = [(0, 0), (2, 2)]
            bool interpolateMissingClipValues = true
        }
    }
)
{
}
)";
  const std::map<std::string, std::string> assets = {
      {"clip_0.usda", R"(#usda 1.0
def Xform "Root"
{
    double3 xformOp:translate = (1, 2, 3)
}
)"},
      {"clip_1.usda", R"(#usda 1.0
def Xform "Root"
{
    double3 xformOp:translate = (5, 6, 7)
}
)"}};

  LoadResult loaded = LoadUSDAFromString(root);
  assert(loaded.success);
  ConverterConfig config;
  config.animation.clip_stage_loader =
      [&assets](const std::string& path, Stage* stage, std::string* warn,
                std::string* err) {
        const auto it = assets.find(path);
        if (it == assets.end()) {
          if (err) *err = "missing test clip";
          return false;
        }
        LoadResult clip = LoadUSDAFromString(it->second);
        if (!clip.success) {
          if (err) *err = clip.error_summary;
          return false;
        }
        if (warn && !clip.warnings.empty()) *warn = clip.warnings.front();
        *stage = std::move(clip.stage);
        return true;
      };
  RenderSceneConverter converter(config);
  ConvertResult converted = converter.Convert(loaded.stage);
  assert(converted.success);
  assert(converted.scene.animations.size() == 1);
  const AnimationClip& animation = converted.scene.animations[0];
  assert(animation.value_clip_baked);
  assert(animation.clip_asset_paths.size() == 2);
  assert(animation.start_time == 0.0);
  assert(animation.end_time == 2.0);
  assert(animation.channels.size() == 1);
  const AnimationChannel& channel = animation.channels[0];
  assert(channel.target_path == AnimationChannel::TargetPath::Translation);
  assert(channel.target_prim_path == "/Root");
  assert(channel.keyframes.size() == 3);
  assert(std::fabs(channel.keyframes[0].value.x - 1.0f) < 0.001f);
  assert(std::fabs(channel.keyframes[1].value.x - 1.0f) < 0.001f);
  assert(std::fabs(channel.keyframes[2].value.x - 5.0f) < 0.001f);

  std::cout << "  Next value-clip baking: PASSED\n";
}

// Regression: the animation-extraction gate
// (config_.animation.enabled && (stage.HasTimeSamples() ||
// stage.HasValueClips())) must (a) NOT skip stages whose animation arrives
// entirely from value clips (no authored time samples anywhere — this was
// broken by an earlier gate that only checked HasTimeSamples()), (b) skip
// static stages, and (c) honor config_.animation.enabled = false.
void TestAnimationExtractionGate() {
  std::cout << "Testing animation extraction gate...\n";

  // (1) Authored time samples, no clips.
  const std::string ts_stage = R"(#usda 1.0
def Xform "Root"
{
    double3 xformOp:translate.timeSamples = {
        0: (1, 2, 3),
        2: (5, 6, 7)
    }
}
)";

  // (2) Value clips only: the composed root layer has NO authored time
  // samples; animation comes from the external clip stages.
  const std::string clip_stage = R"(#usda 1.0
(
    startTimeCode = 0
    endTimeCode = 2
)
def Xform "Root" (
    clips = {
        dictionary default = {
            double2[] active = [(0, 0), (2, 1)]
            asset[] assetPaths = [@clip_0.usda@, @clip_1.usda@]
            string primPath = "/Root"
            double2[] times = [(0, 0), (2, 2)]
            bool interpolateMissingClipValues = true
        }
    }
)
{
}
)";

  // (3) Static: neither time samples nor clips.
  const std::string static_stage = R"(#usda 1.0
def Xform "Root"
{
    double3 xformOp:translate = (1, 2, 3)
}
)";

  const std::map<std::string, std::string> assets = {
      {"clip_0.usda", R"(#usda 1.0
def Xform "Root"
{
    double3 xformOp:translate = (1, 2, 3)
}
)"},
      {"clip_1.usda", R"(#usda 1.0
def Xform "Root"
{
    double3 xformOp:translate = (5, 6, 7)
}
)"}};

  auto make_loader = [&assets](const std::string& path, Stage* stage,
                               std::string* warn, std::string* err) {
    const auto it = assets.find(path);
    if (it == assets.end()) {
      if (err) *err = "missing test clip";
      return false;
    }
    LoadResult clip = LoadUSDAFromString(it->second);
    if (!clip.success) {
      if (err) *err = clip.error_summary;
      return false;
    }
    *stage = std::move(clip.stage);
    return true;
  };

  // (1) Authored time samples -> animations emitted; HasTimeSamples true,
  // HasValueClips false.
  {
    LoadResult loaded = LoadUSDAFromString(ts_stage);
    assert(loaded.success);
    assert(loaded.stage.HasTimeSamples());
    assert(!loaded.stage.HasValueClips());
    ConverterConfig config;
    RenderSceneConverter converter(config);
    ConvertResult converted = converter.Convert(loaded.stage);
    assert(converted.success);
    assert(converted.scene.animations.size() == 1 &&
           "authored time samples must produce an AnimationClip");
  }

  // (2) Value clips only -> animations STILL emitted (regression gate).
  {
    LoadResult loaded = LoadUSDAFromString(clip_stage);
    assert(loaded.success);
    assert(!loaded.stage.HasTimeSamples() &&
           "clip-only stage must have no authored time samples");
    assert(loaded.stage.HasValueClips() &&
           "clip-only stage must be detected by HasValueClips");
    ConverterConfig config;
    config.animation.clip_stage_loader = make_loader;
    RenderSceneConverter converter(config);
    ConvertResult converted = converter.Convert(loaded.stage);
    assert(converted.success);
    assert(converted.scene.animations.size() == 1 &&
           "clip-only stage must still be animation-extracted");
  }

  // (3) Static stage -> no animations, no animation reserve.
  {
    LoadResult loaded = LoadUSDAFromString(static_stage);
    assert(loaded.success);
    assert(!loaded.stage.HasTimeSamples());
    assert(!loaded.stage.HasValueClips());
    ConverterConfig config;
    RenderSceneConverter converter(config);
    ConvertResult converted = converter.Convert(loaded.stage);
    assert(converted.success);
    assert(converted.scene.animations.empty() &&
           "static stage must produce no AnimationClips");
  }

  // (4) animation.enabled = false -> no animations even with time samples.
  {
    LoadResult loaded = LoadUSDAFromString(ts_stage);
    assert(loaded.success);
    ConverterConfig config;
    config.animation.enabled = false;
    RenderSceneConverter converter(config);
    ConvertResult converted = converter.Convert(loaded.stage);
    assert(converted.success);
    assert(converted.scene.animations.empty() &&
           "animation.enabled=false must suppress AnimationClips");
  }

  // (5) The same gate must hold on the ConvertToSink path.
  {
    // (5a) clip-only stage -> animations still emitted through the sink.
    LoadResult loaded = LoadUSDAFromString(clip_stage);
    assert(loaded.success);
    ConverterConfig config;
    config.animation.clip_stage_loader = make_loader;
    RenderSceneConverter converter(config);
    RetainedStreamSink sink;
    StreamConvertResult streamed = converter.ConvertToSink(loaded.stage, &sink);
    assert(streamed.success);
    assert(sink.scene.animations.size() == 1 &&
           "sink: clip-only stage must still be animation-extracted");

    // (5b) static stage -> no animations through the sink.
    LoadResult static_loaded = LoadUSDAFromString(static_stage);
    assert(static_loaded.success);
    RenderSceneConverter static_converter(ConverterConfig{});
    RetainedStreamSink static_sink;
    StreamConvertResult streamed_static =
        static_converter.ConvertToSink(static_loaded.stage, &static_sink);
    assert(streamed_static.success);
    assert(static_sink.scene.animations.empty() &&
           "sink: static stage must produce no AnimationClips");

    // (5c) enabled = false -> no animations through the sink.
    LoadResult ts_loaded = LoadUSDAFromString(ts_stage);
    assert(ts_loaded.success);
    ConverterConfig disabled_config;
    disabled_config.animation.enabled = false;
    RenderSceneConverter disabled_converter(disabled_config);
    RetainedStreamSink disabled_sink;
    StreamConvertResult streamed_disabled =
        disabled_converter.ConvertToSink(ts_loaded.stage, &disabled_sink);
    assert(streamed_disabled.success);
    assert(disabled_sink.scene.animations.empty() &&
           "sink: animation.enabled=false must suppress AnimationClips");
  }

  std::cout << "  Animation extraction gate: PASSED\n";
}

// Regression: schema/tydra accessors must never unfreeze the global
// PropNameTable during the render phase. The compose fill pass freezes it for
// lock-free concurrent reads; a render-phase intern() MISS (a name neither
// registered by register_common_names() nor authored in the scene) unfreezes
// it permanently and races lock-free readers. Every accessor name must be
// pre-registered so intern() always HITs.
void TestNameTableFreezeStability() {
  std::cout << "Testing frozen name-table stability under accessors...\n";

  // Every prim deliberately OMITS the optional names the accessors query
  // (uv, normals, widths, ids, rotateX, inputs:*, focalLength, ...) so any
  // missing registration is a first-time intern on the frozen table.
  const char* usda = R"(#usda 1.0
def Xform "World"
{
    def Mesh "Bare"
    {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
    }
    def Xform "Xf"
    {
        double3 xformOp:translate = (1, 2, 3)
    }
    def BasisCurves "Curve"
    {
        int[] curveVertexCounts = [2]
        point3f[] points = [(0, 0, 0), (1, 1, 1)]
    }
    def PointInstancer "Inst"
    {
        int[] protoIndices = [0]
        point3f[] positions = [(0, 0, 0)]
    }
    def SphereLight "Light"
    {
    }
    def Camera "Cam"
    {
    }
}
)";

  LoadResult loaded = LoadUSDAFromString(usda, std::strlen(usda));
  if (!loaded.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: "
              << loaded.error_summary << ")\n";
    return;
  }

  PropNameTable& table = GetPropNameTable();
  const size_t size_before = table.size();
  table.freeze();

  // Schema accessors on the fixtures (geom-xform, geom-mesh,
  // geom-point-instancer intern()s).
  {
    auto xf = loaded.stage.GetPrimAtPath("/World/Xf");
    assert(xf.IsValid());
    UsdGeomXform xform(xf);
    float rot[3] = {0, 0, 0};
    xform.GetRotation(&rot[0], &rot[1], &rot[2]);  // rotateXYZ/rotateX/Y/Z
    float w = 0, x = 0, y = 0, z = 0;
    xform.GetOrientation(&w, &x, &y, &z);  // xformOp:orient
    auto ops = xform.GetXformOps();  // xformOpOrder

    auto bare = loaded.stage.GetPrimAtPath("/World/Bare");
    assert(bare.IsValid());
    UsdGeomMesh mesh(bare);
    mesh.GetUVs();       // primvars:st / primvars:uv
    mesh.HasUVs();       // primvars:st / primvars:uv
    mesh.GetUVIndices(); // primvars:st:indices / primvars:uv:indices

    auto inst = loaded.stage.GetPrimAtPath("/World/Inst");
    assert(inst.IsValid());
    UsdGeomPointInstancer pi(inst);
    pi.GetPositions(0.0);
    pi.GetProtoIndices(0.0);
    pi.GetOrientations(0.0);
    pi.GetScales(0.0);
    pi.GetVelocities(0.0);
    pi.GetAngularVelocities(0.0);
    pi.GetIds(0.0);
    pi.GetInvisibleIds(0.0);
    pi.GetInactiveIds();
  }

  // Converter on the same stage: touches the render-converter kId* names
  // (inputs:*, focalLength, projection, clippingRange, shutter:*, widths,
  // displayColor/displayOpacity, tetVertexIndices, ...) and, with
  // retain_geometry = false, the stage.cc IsStaticGeometryArray names.
  {
    ConverterConfig config;
    config.mesh.retain_geometry = false;
    RenderSceneConverter converter(config);
    ConvertResult result = converter.Convert(loaded.stage);
    assert(result.success);
  }

#if defined(LIGHTUSD_ENABLE_THREAD)
  assert(table.is_frozen() &&
         "render-phase accessors must not unfreeze the name table");
#endif
  assert(table.size() == size_before &&
         "render-phase accessors must not intern new names");

  // Sensitivity: a genuinely new name must still be reachable and grow the
  // table -- proves this harness would catch a regression that lets any
  // intern miss through. The table itself stays frozen: a new intern only
  // mutates the live map/deque (which the published snapshot does not
  // alias), so the snapshot stays valid -- just incomplete -- rather than
  // flipping frozen_ back to false, which would race a concurrent lock-free
  // reader still walking it (see the freeze() doc comment in
  // property-index.hh).
  const size_t size_before_probe = table.size();
  PropNameId probe_id = table.intern("zzz_unregistered_probe_name_7f3a");
  assert(table.size() == size_before_probe + 1 &&
         "new name must have been inserted");
  assert(table.find("zzz_unregistered_probe_name_7f3a") == probe_id &&
         "new name must be findable after interning");
#if defined(LIGHTUSD_ENABLE_THREAD)
  assert(table.is_frozen() &&
         "interning a new name must not unfreeze a published snapshot");
#endif
  table.unfreeze();

  std::cout << "  Frozen name-table stability: PASSED\n";
}

void TestMeshParityCleanups() {
  const char* usda = R"(#usda 1.0
def Xform "World"
{
    def Mesh "Concave"
    {
        point3f[] points = [(0, 0, 0), (2, 0, 0), (2, 2, 0), (1, 1, 0), (0, 2, 0)]
        int[] faceVertexCounts = [5]
        int[] faceVertexIndices = [0, 1, 2, 3, 4]
        int[] primvars:skel:jointIndices = [0,1,2,3,4,5, 0,1,2,3,4,5, 0,1,2,3,4,5, 0,1,2,3,4,5, 0,1,2,3,4,5] (
            elementSize = 6
            interpolation = "vertex"
        )
        float[] primvars:skel:jointWeights = [.1,.3,.2,.05,.25,.1, .1,.3,.2,.05,.25,.1, .1,.3,.2,.05,.25,.1, .1,.3,.2,.05,.25,.1, .1,.3,.2,.05,.25,.1] (
            elementSize = 6
            interpolation = "vertex"
        )
        uniform token[] skel:blendShapes = ["Smile"]
        rel skel:blendShapeTargets = </World/Smile>
    }
    def BlendShape "Smile"
    {
        uniform vector3f[] offsets = [(0,0,1), (0,0,1), (0,0,1), (0,0,1), (0,0,1)]
        uniform vector3f[] inbetweens:half = [(0,0,.5), (0,0,.5), (0,0,.5), (0,0,.5), (0,0,.5)] (
            weight = 0.5
        )
    }
}
)";
  LoadResult loaded = LoadUSDAFromString(usda, std::strlen(usda));
  assert(loaded.success);
  ConverterConfig config;
  config.mesh.triangulation_method = MeshConfig::TriangulationMethod::Earcut;
  config.mesh.enable_bone_reduction = true;
  config.mesh.target_bone_count = 4;
  RenderSceneConverter converter(config);
  ConvertResult converted = converter.Convert(loaded.stage);
  assert(converted.success);
  assert(converted.scene.meshes.size() == 1);
  const RenderMesh& mesh = converted.scene.meshes[0];
  assert(mesh.triangulated_indices.size() == 9);
  for (size_t i = 0; i < mesh.triangulated_indices.size(); i += 3) {
    const uint32_t ia = mesh.triangulated_indices[i] * 3;
    const uint32_t ib = mesh.triangulated_indices[i + 1] * 3;
    const uint32_t ic = mesh.triangulated_indices[i + 2] * 3;
    const float area =
        (mesh.points[ib] - mesh.points[ia]) *
            (mesh.points[ic + 1] - mesh.points[ia + 1]) -
        (mesh.points[ib + 1] - mesh.points[ia + 1]) *
            (mesh.points[ic] - mesh.points[ia]);
    assert(std::fabs(area) > 0.001f);
  }
  assert(mesh.skin);
  assert(mesh.skin->influences_per_vertex == 4);
  assert(mesh.skin->joint_indices.size() == 20);
  assert(mesh.skin->joint_indices[0] == 1);
  assert(mesh.skin->joint_indices[1] == 4);
  float weight_sum = 0.0f;
  for (size_t i = 0; i < 4; ++i) weight_sum += mesh.skin->joint_weights[i];
  assert(std::fabs(weight_sum - 1.0f) < 0.001f);
  assert(mesh.blend_shapes.size() == 1);
  assert(mesh.blend_shapes[0].inbetweens.size() == 1);
  assert(mesh.blend_shapes[0].inbetweens[0].name == "half");
  assert(std::fabs(mesh.blend_shapes[0].inbetweens[0].weight - 0.5f) <
         0.001f);
  std::cout << "  Mesh parity cleanups: PASSED\n";
}

// Regression: authored half-precision xformOps (raw half-bit SBO scalars)
// must evaluate through the converting to_float* reads — as_float3() cannot
// see them, which used to collapse scale/rotate to identity.
void TestHalfPrecisionXformOps() {
  std::cout << "Testing half-precision xformOps...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Xform "HalfScaled"
    {
        half3 xformOp:scale = (2, 3, 4)
        uniform token[] xformOpOrder = ["xformOp:scale"]
    }

    def Xform "HalfOriented"
    {
        # 180 degrees about Z: diag(-1, -1, 1) regardless of convention.
        quath xformOp:orient = (0, 0, 0, 1)
        uniform token[] xformOpOrder = ["xformOp:orient"]
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  if (!lr.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: " << lr.error_summary << ")\n";
    return;
  }

  UsdPrim scaled = lr.stage.GetPrimAtPath("/World/HalfScaled");
  assert(scaled.IsValid());
  double m[16];
  assert(ComputeLocalTransform(scaled, m, 0.0));
  assert(std::fabs(m[0] - 2.0) < 0.001);
  assert(std::fabs(m[5] - 3.0) < 0.001);
  assert(std::fabs(m[10] - 4.0) < 0.001);

  UsdPrim oriented = lr.stage.GetPrimAtPath("/World/HalfOriented");
  assert(oriented.IsValid());
  assert(ComputeLocalTransform(oriented, m, 0.0));
  assert(std::fabs(m[0] + 1.0) < 0.001);
  assert(std::fabs(m[5] + 1.0) < 0.001);
  assert(std::fabs(m[10] - 1.0) < 0.001);

  std::cout << "  Half-precision xformOps: PASSED\n";
}

// Regression: compound Euler rotations must retain USD's authored axis order.
// Reversing it displaces multi-axis, non-uniformly scaled decal meshes from the
// surfaces they were authored against.
void TestCompoundRotationTransformParity() {
  std::cout << "Testing compound-rotation transform parity...\n";

  const char* usda = R"(#usda 1.0
def Xform "Decal" {
    double3 xformOp:translate = (287.6815490722656, 33.344390869140625, 8.929546356201172)
    float3 xformOp:rotateXYZ = (89.99446, 7.4118885e-13, -179.99998)
    float3 xformOp:scale = (3.2136297, 1.6068149, 0.87708235)
    uniform token[] xformOpOrder = [
        "xformOp:translate", "xformOp:rotateXYZ", "xformOp:scale"
    ]
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  assert(lr.success);
  UsdPrim decal = lr.stage.GetPrimAtPath("/Decal");
  assert(decal.IsValid());

  double extracted[16];
  double schema[16];
  assert(ComputeLocalTransform(decal, extracted, 0.0));
  assert(UsdGeomXform(decal).ComputeLocalTransform(schema));
  for (int i = 0; i < 16; ++i) {
    assert(std::fabs(extracted[i] - schema[i]) < 1.0e-5);
  }
  assert(std::fabs(extracted[0] + 3.2136297) < 1.0e-5);
  assert(std::fabs(extracted[6] - 1.6068149) < 1.0e-5);
  assert(std::fabs(extracted[9] - 0.87708235) < 1.0e-5);
  assert(std::fabs(extracted[12] - 287.6815490722656) < 1.0e-9);
  assert(std::fabs(extracted[13] - 33.344390869140625) < 1.0e-9);
  assert(std::fabs(extracted[14] - 8.929546356201172) < 1.0e-9);

  std::cout << "  Compound-rotation transform parity: PASSED\n";
}

// Multi-skeleton joint-order remap: two skeletons whose SkelAnimations
// author `joints` in an order DIFFERENT from the skeleton's own joint order.
// Channels must target the right skeleton and joint_remap must map each
// animation-order token back to the skeleton's joint index.
void TestMultiSkeletonJointRemap() {
  std::cout << "Testing multi-skeleton joint-order remap...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
    startTimeCode = 1
    endTimeCode = 2
)

def Xform "World"
{
    def SkelRoot "CharA"
    {
        def Skeleton "SkelA"
        {
            uniform token[] joints = ["A_root", "A_root/A_mid", "A_root/A_mid/A_tip"]
            uniform matrix4d[] bindTransforms = [
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(1,0,0,1)),
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(2,0,0,1))
            ]
            uniform matrix4d[] restTransforms = [
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(1,0,0,1)),
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(1,0,0,1))
            ]
            rel skel:animationSource = </World/CharA/SkelA/AnimA>

            def SkelAnimation "AnimA"
            {
                # Reversed relative to SkelA's joint order.
                uniform token[] joints = ["A_root/A_mid/A_tip", "A_root/A_mid", "A_root"]
                float3[] translations.timeSamples = {
                    1: [(30, 0, 0), (20, 0, 0), (10, 0, 0)],
                    2: [(31, 0, 0), (21, 0, 0), (11, 0, 0)],
                }
                quatf[] rotations.timeSamples = {
                    1: [(1,0,0,0), (1,0,0,0), (1,0,0,0)],
                }
                half3[] scales.timeSamples = {
                    1: [(1,1,1), (1,1,1), (1,1,1)],
                }
            }
        }
    }

    def SkelRoot "CharB"
    {
        def Skeleton "SkelB"
        {
            uniform token[] joints = ["B_root", "B_root/B_tip"]
            uniform matrix4d[] bindTransforms = [
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,1,0,1))
            ]
            uniform matrix4d[] restTransforms = [
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,1,0,1))
            ]
            rel skel:animationSource = </World/CharB/SkelB/AnimB>

            def SkelAnimation "AnimB"
            {
                uniform token[] joints = ["B_root/B_tip", "B_root"]
                float3[] translations.timeSamples = {
                    1: [(200, 0, 0), (100, 0, 0)],
                }
            }
        }
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  if (!lr.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: " << lr.error_summary << ")\n";
    return;
  }

  ConverterConfig config;
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(lr.stage);
  assert(result.success);

  assert(result.scene.skeletons.size() == 2);
  int skel_a = -1;
  int skel_b = -1;
  for (size_t si = 0; si < result.scene.skeletons.size(); ++si) {
    const std::string& path = result.scene.skeletons[si].prim_path;
    if (path == "/World/CharA/SkelA") skel_a = static_cast<int>(si);
    if (path == "/World/CharB/SkelB") skel_b = static_cast<int>(si);
  }
  assert(skel_a >= 0 && skel_b >= 0);

  const AnimationClip* clip_a = nullptr;
  const AnimationClip* clip_b = nullptr;
  for (const AnimationClip& clip : result.scene.animations) {
    if (clip.prim_path == "/World/CharA/SkelA/AnimA") clip_a = &clip;
    if (clip.prim_path == "/World/CharB/SkelB/AnimB") clip_b = &clip;
  }
  assert(clip_a && clip_b);

  auto find_channel = [](const AnimationClip* clip,
                         AnimationChannel::TargetPath path) -> const AnimationChannel* {
    for (const AnimationChannel& ch : clip->channels) {
      if (ch.target_path == path && ch.is_skeletal) return &ch;
    }
    return nullptr;
  };

  // AnimA: translation channel targets SkelA; remap reverses the order
  // (animation joint 0 = A_tip = skeleton joint 2, ...).
  const AnimationChannel* trans_a =
      find_channel(clip_a, AnimationChannel::TargetPath::Translation);
  assert(trans_a);
  assert(trans_a->target_skeleton == skel_a);
  assert(trans_a->joint_order.size() == 3);
  assert(trans_a->joint_remap.size() == 3);
  assert(trans_a->joint_remap[0] == 2);
  assert(trans_a->joint_remap[1] == 1);
  assert(trans_a->joint_remap[2] == 0);
  // Values stay in ANIMATION joint order: frame 0, joint 0 (A_tip) = 30.
  assert(trans_a->value_stride == 3);
  assert(trans_a->element_count == 3);
  assert(trans_a->array_values.size() >= 2u * 3u * 3u);
  assert(std::fabs(trans_a->array_values[0] - 30.0f) < 0.001f);
  assert(std::fabs(trans_a->array_values[3] - 20.0f) < 0.001f);
  assert(std::fabs(trans_a->array_values[6] - 10.0f) < 0.001f);
  // Frame 1 continues after all frame-0 elements.
  assert(std::fabs(trans_a->array_values[9] - 31.0f) < 0.001f);

  // half3[] scales must survive as a skeletal channel too (array half
  // materializes to float).
  const AnimationChannel* scale_a =
      find_channel(clip_a, AnimationChannel::TargetPath::Scale);
  assert(scale_a);
  assert(scale_a->target_skeleton == skel_a);
  assert(std::fabs(scale_a->array_values[0] - 1.0f) < 0.001f);

  // AnimB: targets SkelB (not SkelA), reversed remap for 2 joints.
  const AnimationChannel* trans_b =
      find_channel(clip_b, AnimationChannel::TargetPath::Translation);
  assert(trans_b);
  assert(trans_b->target_skeleton == skel_b);
  assert(trans_b->joint_remap.size() == 2);
  assert(trans_b->joint_remap[0] == 1);
  assert(trans_b->joint_remap[1] == 0);
  assert(std::fabs(trans_b->array_values[0] - 200.0f) < 0.001f);

  // Each skeleton is linked to its own animation.
  assert(result.scene.skeletons[static_cast<size_t>(skel_a)].animation_id >= 0);
  assert(result.scene.skeletons[static_cast<size_t>(skel_b)].animation_id >= 0);
  assert(result.scene.skeletons[static_cast<size_t>(skel_a)].animation_id !=
         result.scene.skeletons[static_cast<size_t>(skel_b)].animation_id);

  std::cout << "  Multi-skeleton joint-order remap: PASSED\n";
}

// UsdLux depth: DomeLight inputs:texture:format token and CollectionAPI
// light/shadow-link resolution to RenderScene mesh index sets.
void TestLightLinkingAndDomeFormat() {
  std::cout << "Testing light linking + dome texture format...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Mesh "GroundPlane"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(-1, 0, -1), (1, 0, -1), (1, 0, 1), (-1, 0, 1)]
    }

    def Xform "Props"
    {
        def Mesh "Crate"
        {
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        }

        def Mesh "Barrel"
        {
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            point3f[] points = [(2, 0, 0), (3, 0, 0), (3, 1, 0), (2, 1, 0)]
        }
    }

    def SphereLight "KeyLight"
    {
        float inputs:intensity = 5
        rel collection:lightLink:includes = [</World/Props>]
        rel collection:lightLink:excludes = [</World/Props/Barrel>]
        uniform token collection:lightLink:expansionRule = "expandPrims"
        rel collection:shadowLink:includes = [</World/GroundPlane>]
        uniform token collection:shadowLink:expansionRule = "explicitOnly"
    }

    def SphereLight "FillLight"
    {
        float inputs:intensity = 1
    }

    def SphereLight "ExpressionLight"
    {
        uniform pathExpression collection:base:membershipExpression = "/World/Props/Crate"
        uniform pathExpression collection:lightLink:membershipExpression = "%:base + /World/Ground*"
        uniform pathExpression collection:shadowLink:membershipExpression = "/World//*{isa:Mesh} - /World/Props/Barrel"
    }

    def DomeLight "Sky"
    {
        asset inputs:texture:file = @sky.exr@
        token inputs:texture:format = "latlong"
    }
    def RectLight "DefaultRect" {}
    def DiskLight "DefaultDisk" {}
    def CylinderLight "DefaultCylinder" {}
    def DistantLight "DefaultDistant" {}
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  if (!lr.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: " << lr.error_summary << ")\n";
    return;
  }

  ConverterConfig config;
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(lr.stage);
  assert(result.success);
  assert(result.scene.meshes.size() == 3);

  int ground = -1;
  int crate = -1;
  int barrel = -1;
  for (size_t mi = 0; mi < result.scene.meshes.size(); ++mi) {
    const std::string& path = result.scene.meshes[mi].prim_path;
    if (path == "/World/GroundPlane") ground = static_cast<int>(mi);
    if (path == "/World/Props/Crate") crate = static_cast<int>(mi);
    if (path == "/World/Props/Barrel") barrel = static_cast<int>(mi);
  }
  assert(ground >= 0 && crate >= 0 && barrel >= 0);

  const RenderLight* key = nullptr;
  const RenderLight* fill = nullptr;
  const RenderLight* expression = nullptr;
  const RenderLight* sky = nullptr;
  const RenderLight* rect = nullptr;
  const RenderLight* disk = nullptr;
  const RenderLight* cylinder = nullptr;
  const RenderLight* distant = nullptr;
  for (const RenderLight& light : result.scene.lights) {
    if (light.prim_path == "/World/KeyLight") key = &light;
    if (light.prim_path == "/World/FillLight") fill = &light;
    if (light.prim_path == "/World/ExpressionLight") expression = &light;
    if (light.prim_path == "/World/Sky") sky = &light;
    if (light.prim_path == "/World/DefaultRect") rect = &light;
    if (light.prim_path == "/World/DefaultDisk") disk = &light;
    if (light.prim_path == "/World/DefaultCylinder") cylinder = &light;
    if (light.prim_path == "/World/DefaultDistant") distant = &light;
  }
  assert(key && fill && expression && sky && rect && disk && cylinder && distant);

  // KeyLight: lightLink includes /World/Props subtree minus Barrel.
  assert(!key->light_links_all);
  assert(key->light_link_mesh_indices.size() == 1);
  assert(key->light_link_mesh_indices[0] == crate);
  // shadowLink explicitOnly: only the exact GroundPlane path matches.
  assert(!key->shadow_links_all);
  assert(key->shadow_link_mesh_indices.size() == 1);
  assert(key->shadow_link_mesh_indices[0] == ground);

  // FillLight: no collections authored -> links everything.
  assert(fill->light_links_all);
  assert(fill->shadow_links_all);

  // membershipExpression supports set algebra, predicates, and references to
  // another CollectionAPI instance on the same prim.
  assert(!expression->light_links_all);
  assert(expression->light_link_mesh_indices.size() == 2);
  assert(std::find(expression->light_link_mesh_indices.begin(),
                   expression->light_link_mesh_indices.end(), ground) !=
         expression->light_link_mesh_indices.end());
  assert(std::find(expression->light_link_mesh_indices.begin(),
                   expression->light_link_mesh_indices.end(), crate) !=
         expression->light_link_mesh_indices.end());
  assert(!expression->shadow_links_all);
  assert(expression->shadow_link_mesh_indices.size() == 2);

  // DomeLight format token.
  assert(sky->type == LightType::Dome);
  assert(sky->params.dome.texture_format ==
         RenderLight::DomeTextureFormat::Latlong);
  // Schema defaults must be initialized even when no size attribute is
  // authored; zero-area defaults break normalize and raster/RT parity.
  assert(std::fabs(key->params.sphere.radius - 0.5f) < 0.001f);
  assert(std::fabs(rect->params.rect.width - 1.0f) < 0.001f);
  assert(std::fabs(rect->params.rect.height - 1.0f) < 0.001f);
  assert(std::fabs(disk->params.disk.radius - 0.5f) < 0.001f);
  assert(std::fabs(cylinder->params.cylinder.radius - 0.5f) < 0.001f);
  assert(std::fabs(cylinder->params.cylinder.length - 1.0f) < 0.001f);
  assert(std::fabs(distant->params.distant.angle - 0.53f) < 0.001f);

  std::cout << "  Light linking + dome texture format: PASSED\n";
}


// Regression: a material whose surface shader is a MaterialX
// standard_surface (unconvertible by both ConvertMaterial and the
// standalone MaterialX path) used to mutually recurse
// ConvertMaterial <-> ConvertUsdMtlxMaterial until stack overflow.
void TestStandardSurfaceNoRecursion() {
  std::cout << "Testing standard_surface conversion does not recurse...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Mesh "Quad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        rel material:binding = </World/Mat>
    }

    def Material "Mat"
    {
        token outputs:surface.connect = </World/Mat/SS.outputs:out>

        def Shader "SS"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.8, 0.2, 0.1)
            float inputs:specular = 0.7
            color3f inputs:specular_color = (0.2, 0.3, 0.4)
            float inputs:specular_roughness = 0.35
            float inputs:metalness = 0.9
            float inputs:coat = 0.6
            float inputs:coat_roughness = 0.15
            float inputs:emission = 2.0
            color3f inputs:emission_color = (0.1, 0.4, 0.7)
            color3f inputs:opacity = (0.8, 0.8, 0.8)
            vector3f inputs:normal = (0.1, 0.2, 0.97)
            float inputs:displacement = 0.125
            token outputs:out
        }
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  if (!lr.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: " << lr.error_summary << ")\n";
    return;
  }

  ConverterConfig config;
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(lr.stage);
  // Must terminate (no stack overflow), keep the mesh, and now convert the
  // standard_surface through the OpenPBR mapping (not the gray fallback).
  assert(result.success);
  assert(result.scene.meshes.size() == 1);
  const auto mat_it = result.scene.material_by_path.find("/World/Mat");
  assert(mat_it != result.scene.material_by_path.end());
  const RenderMaterial& mat =
      result.scene.materials[static_cast<size_t>(mat_it->second)];
  assert(mat.shader_type == RenderMaterial::ShaderType::OpenPBR);
  assert(mat.openpbr);
  assert(std::fabs(mat.openpbr->base_color.value.x - 0.8f) < 0.001f);
  assert(std::fabs(mat.openpbr->base_color.value.y - 0.2f) < 0.001f);
  assert(std::fabs(mat.openpbr->base_color.value.z - 0.1f) < 0.001f);
  assert(std::fabs(mat.openpbr->specular_weight.value.x - 0.7f) < 0.001f);
  assert(std::fabs(mat.openpbr->specular_color.value.y - 0.3f) < 0.001f);
  assert(std::fabs(mat.openpbr->specular_roughness.value.x - 0.35f) < 0.001f);
  assert(std::fabs(mat.openpbr->base_metalness.value.x - 0.9f) < 0.001f);
  assert(std::fabs(mat.openpbr->coat_weight.value.x - 0.6f) < 0.001f);
  assert(std::fabs(mat.openpbr->coat_roughness.value.x - 0.15f) < 0.001f);
  assert(std::fabs(mat.openpbr->emission_luminance.value.x - 2.0f) < 0.001f);
  assert(std::fabs(mat.openpbr->emission_color.value.z - 0.7f) < 0.001f);
  assert(std::fabs(mat.openpbr->opacity.value.x - 0.8f) < 0.001f);
  assert(std::fabs(mat.openpbr->normal.value.y - 0.2f) < 0.001f);
  assert(std::fabs(mat.openpbr->displacement.value.x - 0.125f) < 0.001f);

  std::cout << "  standard_surface no-recursion: PASSED\n";
}


// UsdSkel P0 parity fixes: sparse joint topology, mesh-local skel:joints
// remap, SkelRoot-containment skeleton fallback, rest-from-bind derivation,
// and skel:animationSource authored on the SkelRoot.
void TestUsdSkelParityFixes() {
  std::cout << "Testing UsdSkel parity fixes...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
    startTimeCode = 1
    endTimeCode = 2
)

def Xform "World"
{
    def SkelRoot "Char" (
    )
    {
        rel skel:animationSource = </World/Char/Anim>

        # Sparse joint list: Spine's parent is Root (nearest listed
        # ancestor), not the unlisted "Root/Pelvis". No restTransforms:
        # rest must derive from bind * inverse(parentBind).
        def Skeleton "Skel"
        {
            uniform token[] joints = ["Root", "Root/Pelvis/Spine"]
            uniform matrix4d[] bindTransforms = [
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,3,0,1))
            ]
        }

        def SkelAnimation "Anim"
        {
            uniform token[] joints = ["Root"]
            float3[] translations.timeSamples = { 1: [(0, 1, 0)], }
        }

        # Mesh: no skel:skeleton rel anywhere (containment fallback), and a
        # REVERSED mesh-local skel:joints list — jointIndices are local.
        def Mesh "Body"
        {
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
            uniform token[] skel:joints = ["Root/Pelvis/Spine", "Root"]
            int[] primvars:skel:jointIndices = [0, 0, 0, 0] (elementSize = 1)
            float[] primvars:skel:jointWeights = [1, 1, 1, 1] (elementSize = 1)
        }
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  if (!lr.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: " << lr.error_summary << ")\n";
    return;
  }

  ConverterConfig config;
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(lr.stage);
  assert(result.success);
  assert(result.scene.skeletons.size() == 1);
  const Skeleton& skel = result.scene.skeletons[0];

  // Sparse topology: Spine's parent is Root (index 0), not a flattened root.
  assert(skel.joints.size() == 2);
  assert(skel.joints[1].parent_id == 0);

  // Rest-from-bind: Spine rest = bind(Spine) * inverse(bind(Root)) ->
  // translation (0, 3, 0), NOT identity.
  assert(std::fabs(skel.joints[1].rest_transform.m[13] - 3.0f) < 0.001f);

  // animationSource authored on the SkelRoot resolves onto the skeleton.
  assert(skel.animation_source_path == "/World/Char/Anim");

  // Containment fallback: the mesh binds the SkelRoot's skeleton with no
  // authored rel...
  assert(result.scene.meshes.size() == 1);
  const RenderMesh& mesh = result.scene.meshes[0];
  assert(mesh.skin);
  assert(mesh.skin->skeleton_id == 0);

  // ...and the mesh-local reversed skel:joints remaps: local index 0 =
  // "Root/Pelvis/Spine" = skeleton joint 1.
  assert(mesh.skin->joint_indices.size() == 4);
  for (size_t i = 0; i < 4; ++i) {
    assert(mesh.skin->joint_indices[i] == 1);
  }

  std::cout << "  UsdSkel parity fixes: PASSED\n";
}

// Regression: the skeleton joint-order remap builds an O(1) lookup keyed on
// each joint's full path (or name). A mesh skel:joints token in LEAF form
// (e.g. "Spine" for "Root/Pelvis/Spine") misses that map and must resolve
// through the JointTokenMatches fallback scan -- the map-key path only
// matches exact path/name equality.
void TestSkeletonJointRemapLeafTokens() {
  std::cout << "Testing skeleton joint remap with leaf-name tokens...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def SkelRoot "Char"
    {
        def Skeleton "Skel"
        {
            uniform token[] joints = ["Root", "Root/Pelvis/Spine"]
            uniform matrix4d[] bindTransforms = [
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1)),
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,3,0,1))
            ]
        }

        # Mesh skel:joints uses LEAF names; jointIndices are mesh-local.
        def Mesh "Body"
        {
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
            uniform token[] skel:joints = ["Spine", "Root"]
            int[] primvars:skel:jointIndices = [0, 1, 0, 1] (elementSize = 1)
            float[] primvars:skel:jointWeights = [1, 1, 1, 1] (elementSize = 1)
        }
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  if (!lr.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: "
              << lr.error_summary << ")\n";
    return;
  }

  ConverterConfig config;
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(lr.stage);
  assert(result.success);
  assert(result.scene.skeletons.size() == 1);
  assert(result.scene.meshes.size() == 1);
  const RenderMesh& mesh = result.scene.meshes[0];
  assert(mesh.skin);
  assert(mesh.skin->skeleton_id == 0);

  // Local index 0 = "Spine" (leaf) -> skeleton joint 1
  // ("Root/Pelvis/Spine"); local index 1 = "Root" -> joint 0. The leaf
  // tokens miss the path-keyed lookup and exercise the fallback scan.
  assert(mesh.skin->joint_indices.size() == 4);
  assert(mesh.skin->joint_indices[0] == 1);
  assert(mesh.skin->joint_indices[1] == 0);
  assert(mesh.skin->joint_indices[2] == 1);
  assert(mesh.skin->joint_indices[3] == 0);

  std::cout << "  Skeleton joint remap leaf tokens: PASSED\n";
}

// Regression: ReleaseStaticGeometryArraysForPrim (retain_geometry = false)
// must drop STATIC geometry arrays from the composed stage, but must skip
// TIME-SAMPLED properties (release_static_array_value guard) so animated
// sources survive conversion intact.
void TestGeometryReleaseSkipsAnimatedArrays() {
  std::cout << "Testing geometry release vs time-sampled arrays...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
    startTimeCode = 0
    endTimeCode = 1
)

def Xform "World"
{
    def Mesh "Static"
    {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
    }
    def Mesh "Animated"
    {
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        point3f[] points.timeSamples = {
            0: [(0, 0, 0), (1, 0, 0), (0, 1, 0)],
            1: [(1, 1, 1), (2, 1, 1), (1, 2, 1)]
        }
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        double3 xformOp:translate.timeSamples = {
            0: (0, 0, 0),
            1: (5, 0, 0)
        }
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  if (!lr.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: "
              << lr.error_summary << ")\n";
    return;
  }

  ConverterConfig config;
  config.mesh.retain_geometry = false;
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(lr.stage);
  assert(result.success);
  assert(result.scene.meshes.size() == 2);
  assert(result.scene.animations.size() == 1);

  // Conversion itself already ran on the authored data.
  int animated_mesh_id = -1;
  int static_mesh_id = -1;
  for (int32_t id = 0; id < static_cast<int32_t>(result.scene.meshes.size());
       ++id) {
    const std::string& path =
        result.scene.meshes[static_cast<size_t>(id)].prim_path;
    if (path == "/World/Animated") animated_mesh_id = id;
    if (path == "/World/Static") static_mesh_id = id;
  }
  assert(animated_mesh_id >= 0 && static_mesh_id >= 0);

  // retain_geometry = false also releases the OUTPUT mesh geometry
  // (ReleaseMeshGeometry -- metadata-only/proxy contract), so the scene
  // meshes carry no points or triangles.
  assert(result.scene.meshes[static_cast<size_t>(animated_mesh_id)]
             .point_count() == 0 &&
         "proxy meshes carry no retained points");
  assert(result.scene.meshes[static_cast<size_t>(static_mesh_id)]
             .point_count() == 0);

  // Retained conversion accepts immutable Stage snapshots and must not hide a
  // destructive cache eviction behind its const API. Eviction is explicit;
  // ConvertToSink performs the same operation incrementally on a mutable
  // Stage.
  {
    auto prim = lr.stage.GetPrimAtPath("/World/Static");
    assert(prim.IsValid());
    assert(prim.GetPropertyValue("points") != nullptr &&
           "retained conversion must not mutate its source Stage");
  }
  lr.stage.ReleaseStaticGeometryArrays(1);

  // Static mesh: source arrays released from the composed stage.
  {
    auto prim = lr.stage.GetPrimAtPath("/World/Static");
    assert(prim.IsValid());
    assert(prim.GetPropertyValue("points") == nullptr &&
           "static points must be released");
    assert(prim.GetPropertyValue("faceVertexCounts") == nullptr);
  }

  // Animated mesh: time-sampled arrays retained, animation intact.
  {
    auto prim = lr.stage.GetPrimAtPath("/World/Animated");
    assert(prim.IsValid());
    assert(prim.HasTimeSamples("points") &&
           "time-sampled points must NOT be released");
    assert(prim.HasTimeSamples("xformOp:translate"));
    assert(prim.GetPropertyValue("faceVertexCounts") == nullptr &&
           "static topology of the animated mesh may be released");
    const AnimationClip& clip = result.scene.animations[0];
    assert(clip.prim_path == "/World/Animated");
    bool saw_translation = false;
    for (const AnimationChannel& ch : clip.channels) {
      if (ch.target_path == AnimationChannel::TargetPath::Translation) {
        saw_translation = true;
        assert(ch.keyframes.size() == 2);
        assert(std::fabs(ch.keyframes[1].value.x - 5.0f) < 0.001f);
      }
    }
    assert(saw_translation && "animated translate must be baked");
  }

  std::cout << "  Geometry release vs animated arrays: PASSED\n";
}

// Regression: ConvertAnimation iterates TimeSampleStorage's raw (time,
// value_offset) pairs directly (it is sorted with last-wins upsert). A USDA
// file whose timeSamples keys are authored OUT OF ORDER must still produce
// monotonically increasing AnimationClip keyframes.
void TestAnimationKeyframeOrdering() {
  std::cout << "Testing animation keyframe ordering...\n";

  const char* usda = R"(#usda 1.0
def Xform "Root"
{
    double3 xformOp:translate.timeSamples = {
        4: (40, 0, 0),
        1: (10, 0, 0),
        3: (30, 0, 0),
        1: (11, 0, 0)
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  if (!lr.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: "
              << lr.error_summary << ")\n";
    return;
  }

  ConverterConfig config;
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(lr.stage);
  assert(result.success);
  assert(result.scene.animations.size() == 1);
  const AnimationClip& clip = result.scene.animations[0];
  assert(clip.prim_path == "/Root");

  bool saw_translation = false;
  for (const AnimationChannel& ch : clip.channels) {
    if (ch.target_path != AnimationChannel::TargetPath::Translation) continue;
    saw_translation = true;
    // Sorted, deduplicated (last-wins): 1 (11), 3 (30), 4 (40).
    assert(ch.keyframes.size() == 3);
    for (size_t i = 1; i < ch.keyframes.size(); ++i) {
      assert(ch.keyframes[i].time > ch.keyframes[i - 1].time &&
             "keyframes must be in ascending time order");
    }
    assert(std::fabs(ch.keyframes[0].time - 1.0) < 1e-9);
    assert(std::fabs(ch.keyframes[0].value.x - 11.0f) < 0.001f);
    assert(std::fabs(ch.keyframes[1].time - 3.0) < 1e-9);
    assert(std::fabs(ch.keyframes[1].value.x - 30.0f) < 0.001f);
    assert(std::fabs(ch.keyframes[2].time - 4.0) < 1e-9);
    assert(std::fabs(ch.keyframes[2].value.x - 40.0f) < 0.001f);
  }
  assert(saw_translation);

  std::cout << "  Animation keyframe ordering: PASSED\n";
}

// 2026-07 audit UsdSkel cluster: quat-array slerp between keys, indexed skin
// primvar expansion, token-array joints/blendShapes in GetSkelAnimationData,
// and count/width validation warnings.
void TestUsdSkelClusterFixes() {
  std::cout << "Testing UsdSkel cluster fixes...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
    startTimeCode = 1
    endTimeCode = 3
)

def Xform "World"
{
    def SkelRoot "Char"
    {
        rel skel:animationSource = </World/Char/Anim>

        # 2 joints but only 1 bindTransform: must WARN (silent identity
        # fill collapses the limb with no hint why).
        def Skeleton "Skel"
        {
            uniform token[] joints = ["Root", "Root/Spine"]
            uniform matrix4d[] bindTransforms = [
                ((1,0,0,0),(0,1,0,0),(0,0,1,0),(0,0,0,1))
            ]
        }

        # 2 declared blendShapes but 3 weights per sample: must WARN.
        # rotations: identity -> 90deg about X; t=2 must SLERP to 45deg.
        def SkelAnimation "Anim"
        {
            uniform token[] joints = ["Root", "Root/Spine"]
            uniform token[] blendShapes = ["smile", "frown"]
            float[] blendShapeWeights.timeSamples = {
                1: [0, 0, 0],
                3: [1, 1, 1],
            }
            quatf[] rotations.timeSamples = {
                1: [(1, 0, 0, 0), (1, 0, 0, 0)],
                3: [(0.70710678, 0.70710678, 0, 0), (1, 0, 0, 0)],
            }
        }

        # Indexed skin primvars: values are 2 ELEMENTS of elementSize 2;
        # :indices addresses elements, expansion is per-group.
        def Mesh "Body"
        {
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
            rel skel:skeleton = </World/Char/Skel>
            int[] primvars:skel:jointIndices = [0, 1, 1, 0] (elementSize = 2)
            int[] primvars:skel:jointIndices:indices = [0, 1, 0, 1]
            float[] primvars:skel:jointWeights = [0.75, 0.25, 0.5, 0.5] (elementSize = 2)
            int[] primvars:skel:jointWeights:indices = [0, 1, 0, 1]
        }
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  if (!lr.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: " << lr.error_summary
              << ")\n";
    return;
  }

  // Quat ARRAY slerp between keys (was: empty/held). Internal quat lane
  // order is real-first (w, x, y, z); midpoint of identity->90degX is 45degX.
  {
    UsdPrim anim = lr.stage.GetPrimAtPath("/World/Char/Anim");
    assert(anim.IsValid());
    Value mid = anim.GetInterpolatedValue("rotations", 2.0);
    const std::vector<float>* lanes = mid.as_float_array();
    assert(lanes && lanes->size() == 8);
    assert(std::fabs((*lanes)[0] - 0.9238795f) < 1e-4f);  // cos(22.5deg)
    assert(std::fabs((*lanes)[1] - 0.3826834f) < 1e-4f);  // sin(22.5deg)
    assert(std::fabs((*lanes)[2]) < 1e-6f);
    assert(std::fabs((*lanes)[3]) < 1e-6f);
    // Second element stays identity.
    assert(std::fabs((*lanes)[4] - 1.0f) < 1e-4f);
    // The slerped value keeps its array element type.
    assert(mid.type_id() == TypeId::Quatf);
  }

  // GetSkelAnimationData: token[] joints/blendShapes must populate (the old
  // reader only handled scalar string/token authoring).
  {
    UsdPrim anim = lr.stage.GetPrimAtPath("/World/Char/Anim");
    ::lightusd::next::SkelAnimationData data;
    assert(::lightusd::next::GetSkelAnimationData(lr.stage, anim, &data, 1.0));
    assert(data.joints.size() == 2);
    assert(data.joints[0] == "Root");
    assert(data.joints[1] == "Root/Spine");
    assert(data.blendShapes.size() == 2);
    assert(data.blendShapes[0] == "smile");
  }

  // Indexed skin primvars expand per elementSize group.
  {
    UsdPrim mesh = lr.stage.GetPrimAtPath("/World/Char/Body");
    SkinBindingInfo sb;
    assert(GetSkinBinding(mesh, &sb));
    const std::vector<int32_t> want_idx = {0, 1, 1, 0, 0, 1, 1, 0};
    assert(sb.joint_indices == want_idx);
    assert(sb.joint_weights.size() == 8);
    assert(std::fabs(sb.joint_weights[0] - 0.75f) < 1e-6f);
    assert(std::fabs(sb.joint_weights[2] - 0.5f) < 1e-6f);
    assert(std::fabs(sb.joint_weights[4] - 0.75f) < 1e-6f);
  }

  // Validation warnings from the converter.
  {
    ConverterConfig config;
    RenderSceneConverter converter(config);
    ConvertResult result = converter.Convert(lr.stage);
    assert(result.success);
    bool warned_bind = false, warned_weights = false;
    for (const std::string& w : result.warnings) {
      if (w.find("bindTransforms") != std::string::npos) warned_bind = true;
      if (w.find("blendShapeWeights") != std::string::npos) {
        warned_weights = true;
      }
    }
    assert(warned_bind);
    assert(warned_weights);
  }

  std::cout << "  UsdSkel cluster fixes: PASSED\n";
}


// Material-driven UV primvar promotion: a texture sampling a
// UsdPrimvarReader with varname "uv" (not "st") must surface that primvar
// as texcoords_0.
void TestMaterialUVPrimvarPromotion() {
  std::cout << "Testing material-driven UV primvar promotion...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Mesh "Quad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        texCoord2f[] primvars:uv = [(0, 0), (0.25, 0), (0.25, 0.5), (0, 0.5)] (
            interpolation = "vertex"
        )
        rel material:binding = </World/Mat>
    }

    def Material "Mat"
    {
        token outputs:surface.connect = </World/Mat/PS.outputs:surface>

        def Shader "PS"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/Mat/Tex.outputs:rgb>
            token outputs:surface
        }

        def Shader "Tex"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @wood.png@
            float2 inputs:st.connect = </World/Mat/Reader.outputs:result>
            float3 outputs:rgb
        }

        def Shader "Reader"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            token inputs:varname = "uv"
            float2 outputs:result
        }
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  if (!lr.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: " << lr.error_summary << ")\n";
    return;
  }

  ConverterConfig config;
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(lr.stage);
  assert(result.success);
  assert(result.scene.meshes.size() == 1);
  const RenderMesh& mesh = result.scene.meshes[0];

  // The texture records the authored varname...
  assert(!result.scene.textures.empty());
  assert(result.scene.textures[0].uv_primvar == "uv");

  // ...and the "uv" primvar is promoted into texcoords_0.
  assert(mesh.texcoords_0.size() == 8);
  assert(std::fabs(mesh.texcoords_0[2] - 0.25f) < 0.001f);
  assert(std::fabs(mesh.texcoords_0[5] - 0.5f) < 0.001f);
  // The promoted primvar no longer duplicates in the generic channel.
  for (const VertexAttribute& attr : mesh.primvars) {
    assert(attr.name != "uv");
  }

  std::cout << "  Material UV primvar promotion: PASSED\n";
}


// Materials P1 fixes: dangling leaf binding must not shadow a valid
// ancestor binding; inputs:varname authored as a connection resolves;
// a texture behind a compute node (input-side connection) still resolves;
// transmissive OpenPBR marks the blend path.
void TestMaterialParityFixes() {
  std::cout << "Testing material parity fixes...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    rel material:binding = </World/GoodMat>

    def Mesh "Quad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        texCoord2f[] primvars:st = [(0, 0), (0.5, 0), (0.5, 0.5), (0, 0.5)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:customUV = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        rel material:binding = </World/MissingMat>
    }

    def Mesh "Glass"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 2), (1, 0, 2), (1, 1, 2), (0, 1, 2)]
        rel material:binding = </World/GlassMat>
    }

    def Material "GoodMat"
    {
        token inputs:frame:stPrimvarName = "customUV"
        token outputs:surface.connect = </World/GoodMat/PS.outputs:surface>

        def Shader "PS"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/GoodMat/Convert.outputs:out>
            token outputs:surface
        }

        def Shader "Convert"
        {
            uniform token info:id = "ND_convert_color4_color3"
            color4f inputs:in.connect = </World/GoodMat/Tex.outputs:rgba>
            color3f outputs:out
        }

        def Shader "Tex"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @albedo.png@
            float2 inputs:st.connect = </World/GoodMat/Reader.outputs:result>
            float4 outputs:rgba
        }

        def Shader "Reader"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            token inputs:varname.connect = </World/GoodMat.inputs:frame:stPrimvarName>
            float2 outputs:result
        }
    }

    def Material "GlassMat"
    {
        token outputs:surface.connect = </World/GlassMat/PBR.outputs:out>

        def Shader "PBR"
        {
            uniform token info:id = "ND_open_pbr_surface_surfaceshader"
            float inputs:transmission_weight = 1.0
            token outputs:out
        }
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  if (!lr.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: " << lr.error_summary << ")\n";
    return;
  }

  ConverterConfig config;
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(lr.stage);
  assert(result.success);

  // Dangling </World/MissingMat> on the mesh must not shadow the valid
  // ancestor binding.
  int quad = -1;
  int glass = -1;
  for (size_t mi = 0; mi < result.scene.meshes.size(); ++mi) {
    if (result.scene.meshes[mi].prim_path == "/World/Quad") quad = static_cast<int>(mi);
    if (result.scene.meshes[mi].prim_path == "/World/Glass") glass = static_cast<int>(mi);
  }
  assert(quad >= 0 && glass >= 0);
  const RenderMesh& quad_mesh = result.scene.meshes[static_cast<size_t>(quad)];
  assert(quad_mesh.material_id >= 0);
  assert(result.scene.materials[static_cast<size_t>(quad_mesh.material_id)]
             .prim_path == "/World/GoodMat");

  // The texture behind the ND_convert node resolves, with the varname
  // followed through the connection to the material interface attribute.
  assert(!result.scene.textures.empty());
  bool found_texture = false;
  for (const RenderTexture& tex : result.scene.textures) {
    if (tex.asset_path == "albedo.png") {
      found_texture = true;
      assert(tex.uv_primvar == "customUV");
    }
  }
  assert(found_texture);

  // ...and the customUV primvar is promoted into texcoords_0.
  assert(quad_mesh.texcoords_0.size() == 8);
  assert(quad_mesh.texcoords_0_name == "customUV");
  // The displaced conventional set remains available as the secondary UVs.
  assert(quad_mesh.texcoords_1.size() == 8);
  assert(quad_mesh.texcoords_1_name == "st");

  // Transmissive OpenPBR takes the blend path.
  const RenderMesh& glass_mesh = result.scene.meshes[static_cast<size_t>(glass)];
  assert(glass_mesh.material_id >= 0);
  const RenderMaterial& glass_mat =
      result.scene.materials[static_cast<size_t>(glass_mesh.material_id)];
  assert(glass_mat.alpha_mode == RenderMaterial::AlphaMode::Blend);

  std::cout << "  Material parity fixes: PASSED\n";
}


// Regressions for the pre-push review round: ND_mix input priority,
// factor-input skip, dangling purpose-specific binding fall-through, and
// the jointIndices/jointWeights desync warning.
void TestAuditReviewFixes() {
  std::cout << "Testing audit review fixes...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Mesh "MixQuad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        rel material:binding = </World/MixMat>
    }

    def Mesh "MaskQuad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)]
        rel material:binding = </World/MaskMat>
    }

    def Mesh "PurposeQuad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 2), (1, 0, 2), (1, 1, 2), (0, 1, 2)]
        rel material:binding:preview = </World/DoesNotExist>
        rel material:binding = </World/PlainMat>
    }

    def Mesh "DesyncQuad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 3), (1, 0, 3), (1, 1, 3), (0, 1, 3)]
        rel skel:skeleton = </World/Skel>
        int[] primvars:skel:jointIndices = [0, 0, 0, 0] (elementSize = 1)
        float[] primvars:skel:jointWeights = [1] (elementSize = 1)
        int[] primvars:skel:jointWeights:indices = [9]
    }

    def Skeleton "Skel"
    {
        uniform token[] joints = ["Root"]
    }

    def Material "MixMat"
    {
        token outputs:surface.connect = </World/MixMat/PS.outputs:surface>

        def Shader "PS"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/MixMat/Mix.outputs:out>
            token outputs:surface
        }

        # bg authored BEFORE fg (what alphabetized crate flattening
        # produces): the fallback must still follow fg.
        def Shader "Mix"
        {
            uniform token info:id = "ND_mix_color3"
            color3f inputs:bg.connect = </World/MixMat/TexB.outputs:rgb>
            color3f inputs:fg.connect = </World/MixMat/TexA.outputs:rgb>
            color3f outputs:out
        }

        def Shader "TexA"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @base.png@
            float3 outputs:rgb
        }

        def Shader "TexB"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @dirt.png@
            float3 outputs:rgb
        }
    }

    def Material "MaskMat"
    {
        token outputs:surface.connect = </World/MaskMat/PS.outputs:surface>

        def Shader "PS"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/MaskMat/Mix.outputs:out>
            token outputs:surface
        }

        # Only the mix FACTOR is connected: the mask must never be bound as
        # the base color texture.
        def Shader "Mix"
        {
            uniform token info:id = "ND_mix_color3"
            color3f inputs:fg = (1, 0, 0)
            color3f inputs:bg = (0, 1, 0)
            float inputs:mix.connect = </World/MaskMat/MaskTex.outputs:r>
            color3f outputs:out
        }

        def Shader "MaskTex"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @mask.png@
            float outputs:r
        }
    }

    def Material "PlainMat"
    {
        token outputs:surface.connect = </World/PlainMat/PS.outputs:surface>

        def Shader "PS"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.2, 0.4, 0.6)
            token outputs:surface
        }
    }
}
)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  if (!lr.success) {
    std::cout << "  SKIPPED (failed to parse test USDA: " << lr.error_summary
              << ")\n";
    return;
  }

  ConverterConfig config;
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(lr.stage);
  assert(result.success);

  // ND_mix: the fg texture wins even though bg is authored first.
  bool has_base = false, has_dirt = false, has_mask = false;
  for (const RenderTexture& tex : result.scene.textures) {
    if (tex.asset_path == "base.png") has_base = true;
    if (tex.asset_path == "dirt.png") has_dirt = true;
    if (tex.asset_path == "mask.png") has_mask = true;
  }
  assert(has_base && "fg texture must be followed through ND_mix");
  assert(!has_dirt && "bg texture must not shadow fg");
  // Factor-only connection: the blend mask must not become a base color.
  assert(!has_mask && "a mix FACTOR input must never bind as a texture");

  // Dangling material:binding:preview falls through to the valid
  // all-purpose binding on the same prim.
  int purpose = -1, desync = -1;
  for (size_t mi = 0; mi < result.scene.meshes.size(); ++mi) {
    if (result.scene.meshes[mi].prim_path == "/World/PurposeQuad")
      purpose = static_cast<int>(mi);
    if (result.scene.meshes[mi].prim_path == "/World/DesyncQuad")
      desync = static_cast<int>(mi);
  }
  assert(purpose >= 0 && desync >= 0);
  const RenderMesh& pq = result.scene.meshes[static_cast<size_t>(purpose)];
  assert(pq.material_id >= 0);
  assert(result.scene.materials[static_cast<size_t>(pq.material_id)]
             .prim_path == "/World/PlainMat");

  // Desynced jointIndices/jointWeights (a malformed :indices left the pair
  // at different sizes): mesh renders unskinned WITH a warning.
  assert(!result.scene.meshes[static_cast<size_t>(desync)].skin);
  bool warned = false;
  for (const std::string& w : result.warnings) {
    if (w.find("Mismatched skin jointIndices/jointWeights") !=
        std::string::npos) {
      warned = true;
    }
  }
  assert(warned);

  // Hostile indexed skin primvar: indices x elementSize requesting ~1e9
  // lanes must NOT be expanded (this TU builds without exceptions, so an
  // unbounded reserve would abort the process). The authored arrays are
  // kept as-is instead.
  {
    std::string hostile =
        "#usda 1.0\n"
        "def Mesh \"M\"\n"
        "{\n"
        "    int[] primvars:skel:jointIndices = [";
    for (int i = 0; i < 1000; ++i) hostile += (i ? ",0" : "0");
    hostile +=
        "] (elementSize = 1000)\n"
        "    int[] primvars:skel:jointIndices:indices = [";
    for (int i = 0; i < 300000; ++i) hostile += (i ? ",0" : "0");
    hostile +=
        "]\n"
        "    float[] primvars:skel:jointWeights = [1] (elementSize = 1)\n"
        "}\n";
    LoadResult hr = LoadUSDAFromString(hostile.c_str(), hostile.size());
    assert(hr.success);
    UsdPrim m = hr.stage.GetPrimAtPath("/M");
    assert(m.IsValid());
    SkinBindingInfo sb;
    GetSkinBinding(m, &sb);
    // Kept authored (1000 entries), not expanded to 3e8.
    assert(sb.joint_indices.size() == 1000);
  }

  std::cout << "  Audit review fixes: PASSED\n";
}

// P2 audit fixes: blendshape sparse OOB parallel-array drop, weightless
// in-between skip, hole/subset face remap across topology sanitize, spec
// `constant` primvar-interpolation default (with size inference), quad
// shorter-diagonal split, earcut winding vs projected ring orientation,
// analytic prim axis as rotation (not mirror), displayOpacity channel,
// default material for unbound geometry, chained UsdTransform2d composition,
// displacement/volume terminal metadata.
void TestP2AuditFixes() {
  std::cout << "Testing P2 audit fixes...\n";

  const char* usda = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    # Sparse blendshape with an out-of-range point index (9) and one
    # in-between without an authored weight.
    def Mesh "Sparse"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        uniform token[] skel:blendShapes = ["sparse"]
        prepend rel skel:blendShapeTargets = </World/Sparse/BS>

        def BlendShape "BS"
        {
            uniform vector3f[] offsets = [(1, 0, 0), (2, 0, 0), (3, 0, 0)]
            uniform vector3f[] normalOffsets = [(0, 1, 0), (0, 2, 0), (0, 3, 0)]
            uniform int[] pointIndices = [0, 9, 2]
            uniform vector3f[] inbetweens:half = [(10, 0, 0), (20, 0, 0), (30, 0, 0)] (
                weight = 0.5
            )
            uniform vector3f[] inbetweens:noweight = [(0, 0, 1), (0, 0, 2), (0, 0, 3)]
        }
    }

    # Face 1 has an out-of-range vertex index and is dropped by sanitize;
    # the GeomSubset references AUTHORED face 2, which becomes face 1.
    def Mesh "SubsetRemap"
    {
        int[] faceVertexCounts = [4, 3, 3]
        int[] faceVertexIndices = [0, 1, 2, 3, 0, 1, 9, 0, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        rel material:binding = </World/MatA>

        def GeomSubset "S"
        {
            uniform token elementType = "face"
            uniform token familyName = "materialBind"
            int[] indices = [2]
            rel material:binding = </World/MatB>
        }
    }

    # Authored face 0 is dropped by sanitize; the authored hole face 2
    # becomes face 1.
    def Mesh "HoleRemap"
    {
        int[] faceVertexCounts = [3, 3, 3]
        int[] faceVertexIndices = [0, 1, 9, 0, 1, 2, 0, 2, 3]
        int[] holeIndices = [2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    }

    # Primvars WITHOUT authored interpolation: per-point arrays infer
    # vertex; single elements keep the spec default (constant).
    def Mesh "InterpInfer"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        color3f[] primvars:displayColor = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 1)]
        float[] primvars:displayOpacity = [0.1, 0.2, 0.3, 0.4]
        float[] primvars:myconst = [7.5]
    }

    # Non-square quad: diagonal 1-3 (len^2 = 2) is shorter than 0-2
    # (len^2 = 10) and must be the split edge.
    def Mesh "Kite"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 0), (2, 0, 0), (3, 1, 0), (1, 1, 0)]
    }

    # Concave pentagons handled by earcut: winding of the emitted triangles
    # must follow the authored ring winding for BOTH +z and -z dominant
    # normals (the old unconditional reverse flipped one of them).
    def Mesh "PentPosZ"
    {
        int[] faceVertexCounts = [5]
        int[] faceVertexIndices = [0, 1, 2, 3, 4]
        point3f[] points = [(0, 0, 0), (2, 0, 0), (2, 2, 0), (1, 1, 0), (0, 2, 0)]
    }

    def Mesh "PentNegZ"
    {
        int[] faceVertexCounts = [5]
        int[] faceVertexIndices = [4, 3, 2, 1, 0]
        point3f[] points = [(0, 0, 0), (2, 0, 0), (2, 2, 0), (1, 1, 0), (0, 2, 0)]
    }

    # y-dominant projection (the earcut drop-axis pair (x,z) mirrors the
    # ring): winding must still follow the authored ring.
    def Mesh "PentPosY"
    {
        int[] faceVertexCounts = [5]
        int[] faceVertexIndices = [0, 1, 2, 3, 4]
        point3f[] points = [(0, 0, 0), (0, 0, -2), (2, 0, -2), (1, 0, -1), (2, 0, 0)]
    }

    # Analytic prims: the axis transform must be a proper rotation, not a
    # mirror (a mirror turns the mesh inside out).
    def Capsule "CapX"
    {
        token axis = "X"
    }
    def Capsule "CapZ"
    {
    }
    def Cylinder "CylX"
    {
        token axis = "X"
    }
    def Sphere "BallY"
    {
    }

    def Material "MatA"
    {
        token outputs:surface.connect = </World/MatA/PS.outputs:surface>
        def Shader "PS"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (1, 0, 0)
            token outputs:surface
        }
    }

    def Material "MatB"
    {
        token outputs:surface.connect = </World/MatB/PS.outputs:surface>
        def Shader "PS"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0, 1, 0)
            token outputs:surface
        }
    }

    # Chained UsdTransform2d: Outer(translate) o Inner(scale+translate)
    # must COMPOSE (the old code overwrote fields per hop).
    def Mesh "ChainQuad"
    {
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0, 1, 2, 3]
        point3f[] points = [(0, 0, 5), (1, 0, 5), (1, 1, 5), (0, 1, 5)]
        rel material:binding = </World/ChainMat>
    }

    def Material "ChainMat"
    {
        token outputs:surface.connect = </World/ChainMat/PS.outputs:surface>

        def Shader "PS"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/ChainMat/Tex.outputs:rgb>
            token outputs:surface
        }

        def Shader "Tex"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @chain.png@
            float2 inputs:st.connect = </World/ChainMat/Outer.outputs:result>
            float3 outputs:rgb
        }

        def Shader "Outer"
        {
            uniform token info:id = "UsdTransform2d"
            float2 inputs:in.connect = </World/ChainMat/Inner.outputs:result>
            float2 inputs:translation = (0.1, 0)
            float2 outputs:result
        }

        def Shader "Inner"
        {
            uniform token info:id = "UsdTransform2d"
            float2 inputs:in.connect = </World/ChainMat/Reader.outputs:result>
            float2 inputs:scale = (2, 2)
            float2 inputs:translation = (0.2, 0.3)
            float2 outputs:result
        }

        def Shader "Reader"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            token inputs:varname = "st"
            float2 outputs:result
        }
    }

    # Displacement/volume terminals recorded as metadata.
    def Material "DispMat"
    {
        token outputs:surface.connect = </World/DispMat/PS.outputs:surface>
        token outputs:displacement.connect = </World/DispMat/Disp.outputs:displacement>
        token outputs:volume.connect = </World/DispMat/Vol.outputs:volume>

        def Shader "PS"
        {
            uniform token info:id = "UsdPreviewSurface"
            token outputs:surface
        }
        def Shader "Disp"
        {
            uniform token info:id = "UsdPreviewSurface"
            token outputs:displacement
        }
        def Shader "Vol"
        {
            uniform token info:id = "UsdPreviewSurface"
            float inputs:density.connect = </World/DispMat/Density.outputs:out>
            float inputs:emission = 2.0
            token outputs:volume
        }
        def Shader "Density"
        {
            uniform token info:id = "ND_constant_float"
            float inputs:value = 0.75
            float outputs:out
        }
    }
}

)";

  LoadResult lr = LoadUSDAFromString(usda, std::strlen(usda));
  assert(lr.success);

  ConverterConfig config;
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(lr.stage);
  assert(result.success);
  RenderScene& scene = result.scene;

  auto find_mesh = [&scene](const char* path) -> RenderMesh& {
    auto it = scene.mesh_by_path.find(path);
    assert(it != scene.mesh_by_path.end());
    return scene.meshes[static_cast<size_t>(it->second)];
  };

  // --- Item 1 + 4: sparse blendshape parallel arrays + weightless
  // in-between.
  {
    RenderMesh& m = find_mesh("/World/Sparse");
    assert(m.blend_shapes.size() == 1);
    const RenderMesh::BlendShape& bs = m.blend_shapes[0];
    // The OOB entry (pointIndices[1] == 9) drops from EVERY parallel array.
    assert(bs.point_indices == std::vector<uint32_t>({0, 2}));
    assert(bs.point_offsets.size() == 6);
    assert(std::fabs(bs.point_offsets[0] - 1.0f) < 1e-6f);
    assert(std::fabs(bs.point_offsets[3] - 3.0f) < 1e-6f);
    assert(bs.normal_offsets.size() == 6);
    assert(std::fabs(bs.normal_offsets[1] - 1.0f) < 1e-6f);
    assert(std::fabs(bs.normal_offsets[4] - 3.0f) < 1e-6f);
    // The weightless in-between is skipped (0.0 collides with the base
    // shape); the weighted one is kept and filtered consistently.
    assert(bs.inbetweens.size() == 1);
    assert(bs.inbetweens[0].name == "half");
    assert(std::fabs(bs.inbetweens[0].weight - 0.5f) < 1e-6f);
    assert(bs.inbetweens[0].point_offsets.size() == 6);
    assert(std::fabs(bs.inbetweens[0].point_offsets[0] - 10.0f) < 1e-6f);
    assert(std::fabs(bs.inbetweens[0].point_offsets[3] - 30.0f) < 1e-6f);
  }

  // --- Item 2: GeomSubset indices survive sanitize via the face remap.
  {
    RenderMesh& m = find_mesh("/World/SubsetRemap");
    assert(m.sanitize_dropped_faces == 1);
    assert(m.face_count() == 2);  // quad + valid tri
    // Authored subset face 2 -> post-sanitize face 1 -> triangle range
    // [2, 3) (the quad becomes triangles 0-1).
    assert(m.material_subsets.size() == 1);
    assert(m.material_subsets[0].face_start == 2);
    assert(m.material_subsets[0].face_count == 1);
    assert(m.material_subsets[0].material_id >= 0);
    assert(m.material_subsets[0].material_id != m.material_id);
    assert(scene.materials[static_cast<size_t>(
               m.material_subsets[0].material_id)].prim_path == "/World/MatB");
  }

  // --- Item 2: holeIndices survive sanitize via the face remap.
  {
    RenderMesh& m = find_mesh("/World/HoleRemap");
    assert(m.sanitize_dropped_faces == 1);
    assert(m.face_count() == 2);
    assert(m.hole_faces == std::vector<uint32_t>({1}));
    // Only the non-hole surviving triangle is emitted.
    assert(m.triangulated_indices.size() == 3);
    assert(m.triangulated_indices[0] == 0 && m.triangulated_indices[1] == 1 &&
           m.triangulated_indices[2] == 2);
    // Unbound geometry keeps material_id == -1 with the default config.
    assert(m.material_id == -1);
  }

  // --- Item 3 + 8: unauthored interpolation inference + displayOpacity
  // channel.
  {
    RenderMesh& m = find_mesh("/World/InterpInfer");
    assert(m.colors.size() == 12);
    assert(m.colors_interp == Interpolation::Vertex);
    assert(m.has_opacities());
    assert(m.opacities.size() == 4);
    assert(m.opacities_interp == Interpolation::Vertex);
    assert(std::fabs(m.opacities[2] - 0.3f) < 1e-6f);
    bool saw_const = false;
    for (const VertexAttribute& pv : m.primvars) {
      if (pv.name == "myconst") {
        saw_const = true;
        // 1 element, unauthored interpolation: USD spec default (constant).
        assert(pv.interpolation == Interpolation::Constant);
      }
    }
    assert(saw_const);
  }

  // --- Item 5: quad split along the SHORTER diagonal (1-3 here).
  {
    RenderMesh& m = find_mesh("/World/Kite");
    assert(m.triangulated_indices.size() == 6);
    assert(m.triangulated_indices[0] == 0 && m.triangulated_indices[1] == 1 &&
           m.triangulated_indices[2] == 3);
    assert(m.triangulated_indices[3] == 1 && m.triangulated_indices[4] == 2 &&
           m.triangulated_indices[5] == 3);
  }

  // --- Item 6: earcut triangles follow the authored ring winding.
  {
    auto check_winding = [&](const char* path) {
      RenderMesh& m = find_mesh(path);
      assert(m.face_count() == 1);
      assert(m.triangulated_indices.size() == 9);  // pentagon -> 3 triangles
      // Newell normal of the authored ring.
      double nx = 0, ny = 0, nz = 0;
      const size_t nv = m.face_vertex_indices.size();
      for (size_t i = 0; i < nv; ++i) {
        const size_t a = size_t(m.face_vertex_indices[i]) * 3;
        const size_t b = size_t(m.face_vertex_indices[(i + 1) % nv]) * 3;
        nx += (double(m.points[a + 1]) - m.points[b + 1]) *
              (double(m.points[a + 2]) + m.points[b + 2]);
        ny += (double(m.points[a + 2]) - m.points[b + 2]) *
              (double(m.points[a]) + m.points[b]);
        nz += (double(m.points[a]) - m.points[b]) *
              (double(m.points[a + 1]) + m.points[b + 1]);
      }
      for (size_t t = 0; t < m.triangulated_indices.size(); t += 3) {
        const size_t ia = size_t(m.triangulated_indices[t]) * 3;
        const size_t ib = size_t(m.triangulated_indices[t + 1]) * 3;
        const size_t ic = size_t(m.triangulated_indices[t + 2]) * 3;
        const double e1x = double(m.points[ib]) - m.points[ia];
        const double e1y = double(m.points[ib + 1]) - m.points[ia + 1];
        const double e1z = double(m.points[ib + 2]) - m.points[ia + 2];
        const double e2x = double(m.points[ic]) - m.points[ia];
        const double e2y = double(m.points[ic + 1]) - m.points[ia + 1];
        const double e2z = double(m.points[ic + 2]) - m.points[ia + 2];
        const double tx = e1y * e2z - e1z * e2y;
        const double ty = e1z * e2x - e1x * e2z;
        const double tz = e1x * e2y - e1y * e2x;
        // Triangle normal must point the same way as the face normal.
        assert(nx * tx + ny * ty + nz * tz > 1e-9);
      }
    };
    check_winding("/World/PentPosZ");
    check_winding("/World/PentNegZ");
    check_winding("/World/PentPosY");
  }

  // --- Item 7: analytic prim axis is a rotation; meshes stay outward
  // (positive signed volume) for every axis.
  {
    auto signed_volume = [&](const char* path) -> double {
      RenderMesh& m = find_mesh(path);
      assert(m.is_triangulated);
      double vol = 0.0;
      for (size_t t = 0; t < m.triangulated_indices.size(); t += 3) {
        const size_t ia = size_t(m.triangulated_indices[t]) * 3;
        const size_t ib = size_t(m.triangulated_indices[t + 1]) * 3;
        const size_t ic = size_t(m.triangulated_indices[t + 2]) * 3;
        const double ax = m.points[ia], ay = m.points[ia + 1],
                     az = m.points[ia + 2];
        const double bx = m.points[ib], by = m.points[ib + 1],
                     bz = m.points[ib + 2];
        const double cx = m.points[ic], cy = m.points[ic + 1],
                     cz = m.points[ic + 2];
        vol += (ax * (by * cz - bz * cy) + ay * (bz * cx - bx * cz) +
                az * (bx * cy - by * cx)) /
               6.0;
      }
      return vol;
    };
    // Baseline: the generators themselves are outward (+Y sphere).
    assert(signed_volume("/World/BallY") > 0.01);
    // Match legacy's subdivision-4 sphere (10 * 4^4 + 2 vertices).
    assert(find_mesh("/World/BallY").point_count() == 2562);
    // A mirror would flip these negative.
    assert(signed_volume("/World/CapX") > 0.01);
    assert(signed_volume("/World/CapZ") > 0.01);
    assert(signed_volume("/World/CylX") > 0.01);
    // The rotated capsule/cylinder axes really lie along the authored axis.
    RenderMesh& capx = find_mesh("/World/CapX");
    assert((capx.bbox_max.x - capx.bbox_min.x) >
           (capx.bbox_max.y - capx.bbox_min.y) + 0.5f);
    RenderMesh& capz = find_mesh("/World/CapZ");
    assert((capz.bbox_max.z - capz.bbox_min.z) >
           (capz.bbox_max.y - capz.bbox_min.y) + 0.5f);
  }

  // --- Item 10: chained UsdTransform2d composes.
  // Outer(translate (0.1,0)) o Inner(scale (2,2), translate (0.2,0.3)):
  // uv' = 2*uv + (0.3, 0.3).
  {
    bool saw_chain_tex = false;
    for (const RenderTexture& tex : scene.textures) {
      if (tex.asset_path != "chain.png") continue;
      saw_chain_tex = true;
      assert(std::fabs(tex.scale.x - 2.0f) < 1e-5f);
      assert(std::fabs(tex.scale.y - 2.0f) < 1e-5f);
      assert(std::fabs(tex.offset.x - 0.3f) < 1e-5f);
      assert(std::fabs(tex.offset.y - 0.3f) < 1e-5f);
      assert(std::fabs(tex.rotation) < 1e-5f);
      assert(tex.uv_primvar == "st");
    }
    assert(saw_chain_tex);
  }

  // --- Item 11: displacement/volume terminal metadata.
  {
    auto it = scene.material_by_path.find("/World/DispMat");
    assert(it != scene.material_by_path.end());
    const RenderMaterial& mat =
        scene.materials[static_cast<size_t>(it->second)];
    assert(mat.has_displacement);
    assert(mat.displacement_shader_path == "/World/DispMat/Disp");
    assert(mat.has_volume);
    assert(mat.volume_shader_path == "/World/DispMat/Vol");
    assert(!mat.volume_nodegraph_json.empty());
    assert(mat.volume_nodegraph_json.find("volume_density") != std::string::npos);
    // volume emission nodes vary by MaterialX version — presence not asserted
    assert(mat.volume_nodegraph_json.find("volume") != std::string::npos);
    auto ita = scene.material_by_path.find("/World/MatA");
    assert(ita != scene.material_by_path.end());
    assert(!scene.materials[static_cast<size_t>(ita->second)].has_displacement);
  }

  // --- Item 9: default material for unbound geometry (config-gated).
  {
    ConverterConfig def_config;
    def_config.material.assign_default_material = true;
    RenderSceneConverter def_converter(def_config);
    ConvertResult def_result = def_converter.Convert(lr.stage);
    assert(def_result.success);
    RenderScene& def_scene = def_result.scene;
    auto it = def_scene.mesh_by_path.find("/World/HoleRemap");
    assert(it != def_scene.mesh_by_path.end());
    const RenderMesh& unbound =
        def_scene.meshes[static_cast<size_t>(it->second)];
    assert(unbound.material_id >= 0);
    const RenderMaterial& dm =
        def_scene.materials[static_cast<size_t>(unbound.material_id)];
    assert(dm.name == "defaultMaterial");
    assert(dm.prim_path == "/__lightusd_default_material__");
    assert(dm.shader_type == RenderMaterial::ShaderType::PreviewSurface);
    assert(dm.preview_surface);
    // Bound geometry keeps its authored material.
    auto bit = def_scene.mesh_by_path.find("/World/SubsetRemap");
    assert(bit != def_scene.mesh_by_path.end());
    const RenderMesh& bound =
        def_scene.meshes[static_cast<size_t>(bit->second)];
    assert(bound.material_id >= 0);
    assert(def_scene.materials[static_cast<size_t>(bound.material_id)]
               .prim_path == "/World/MatA");
  }

  std::cout << "  P2 audit fixes: PASSED\n";
}

void TestRenderColorManagement() {
  std::cout << "Testing RenderSettings color management...\n";
  const char* usda = R"USD(#usda 1.0
(
    defaultPrim = "World"
    renderSettingsPrimPath = "/World/settings"
)
def Xform "World" (
    prepend apiSchemas = ["ColorSpaceDefinitionAPI:studio_ap0"]
)
{
    uniform token colorSpaceDefinition:studio_ap0:name = "studio_ap0"
    float2 colorSpaceDefinition:studio_ap0:redChroma = (0.7348552434, 0.2642253252)
    float2 colorSpaceDefinition:studio_ap0:greenChroma = (-0.0061709125, 1.0113149590)
    float2 colorSpaceDefinition:studio_ap0:blueChroma = (0.0159675593, -0.0642355031)
    float2 colorSpaceDefinition:studio_ap0:whitePoint = (0.3127, 0.3290)
    float colorSpaceDefinition:studio_ap0:gamma = 1
    float colorSpaceDefinition:studio_ap0:linearBias = 0
    def RenderSettings "settings"
    {
        uniform token renderingColorSpace = "lin_ap1_scene"
    }
    def Material "Mat"
    {
        token outputs:surface.connect = </World/Mat/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.25, 0.5, 0.75) (
                colorSpace = "srgb_rec709_scene"
            )
            color3f inputs:diffuseColor.timeSamples = {
                1: (0.1, 0.2, 0.3),
                2: (0.9, 0.6, 0.1),
            }
            color3f inputs:emissiveColor = (0.25, 0.5, 0.75) (
                colorSpace = "raw"
            )
            token outputs:surface
        }
    }
    def Material "TextureMat"
    {
        token outputs:surface.connect = </World/TextureMat/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/TextureMat/Tex.outputs:rgb>
            token outputs:surface
        }
        def Shader "Tex"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @missing-wide-gamut.png@ (
                colorSpace = "studio_ap0"
            )
            token inputs:sourceColorSpace = "sRGB"
            float3 outputs:rgb
        }
    }
    def Material "MtlxTextureMat" (
        prepend apiSchemas = ["MaterialXConfigAPI"]
    )
    {
        string config:mtlx:version = "1.39"
        string config:mtlx:colorspace = "studio_ap0"
        token outputs:mtlx:surface.connect = </World/MtlxTextureMat/Surface.outputs:out>
        def Shader "Surface"
        {
            uniform token info:id = "ND_open_pbr_surface_surfaceshader"
            color3f inputs:base_color.connect = </World/MtlxTextureMat/Image.outputs:out>
            color3f inputs:emission_color.connect = </World/MtlxTextureMat/Constant.outputs:out>
            float inputs:emission_luminance = 1
            color3f inputs:specular_color.connect = </World/MtlxTextureMat/ApiImage.outputs:out>
            color3f inputs:coat_color.connect = </World/MtlxTextureMat/Override.outputs:out>
            float inputs:coat = 1
            color3f inputs:fuzz_color.connect = </World/MtlxTextureMat/Mix.outputs:out>
            float inputs:fuzz_weight = 1
            float inputs:fuzz_roughness = 0.42
            token outputs:out
        }
        def Shader "Image"
        {
            uniform token info:id = "ND_image_color3"
            asset inputs:file = @missing-wide-gamut.png@
            color3f outputs:out
        }
        def Shader "Constant"
        {
            uniform token info:id = "ND_constant_color3"
            color3f inputs:value = (0.1, 0.2, 0.3)
            color3f outputs:out
        }
        def Shader "Override"
        {
            uniform token info:id = "ND_constant_color3"
            color3f inputs:value = (0.2, 0.3, 0.4) (
                colorSpace = "lin_rec709_scene"
            )
            color3f outputs:out
        }
        def Shader "Encoded"
        {
            uniform token info:id = "ND_constant_color3"
            color3f inputs:value = (0.5, 0, 0) (
                colorSpace = "srgb_rec709_scene"
            )
            color3f outputs:out
        }
        def Shader "Linear"
        {
            uniform token info:id = "ND_constant_color3"
            color3f inputs:value = (0, 0.5, 0) (
                colorSpace = "lin_rec709_scene"
            )
            color3f outputs:out
        }
        def Shader "Mix"
        {
            uniform token info:id = "ND_mix_color3"
            color3f inputs:bg.connect = </World/MtlxTextureMat/Encoded.outputs:out>
            color3f inputs:fg.connect = </World/MtlxTextureMat/Linear.outputs:out>
            float inputs:mix = 0.5
            color3f outputs:out
        }
        def Shader "ApiImage" (
            prepend apiSchemas = ["ColorSpaceAPI"]
        )
        {
            uniform token colorSpace:name = "lin_rec709_scene"
            uniform token info:id = "ND_image_color3"
            asset inputs:file = @missing-inherited.png@
            color3f outputs:out
        }
    }
    def Material "HsvMat"
    {
        token outputs:mtlx:surface.connect = </World/HsvMat/Surface.outputs:out>
        def Shader "Surface"
        {
            uniform token info:id = "ND_open_pbr_surface_surfaceshader"
            color3f inputs:base_color.connect = </World/HsvMat/Hsv.outputs:out>
            token outputs:out
        }
        def Shader "Hsv"
        {
            uniform token info:id = "ND_hsvadjust_color3"
            color3f inputs:in = (0.9, 0.18, 0.42) (
                colorSpace = "raw"
            )
            float3 inputs:amount = (0, 1, 1)
            color3f outputs:out
        }
    }
    def Xform "Alt" (
        prepend apiSchemas = ["ColorSpaceDefinitionAPI:studio_ap0"]
    )
    {
        uniform token colorSpaceDefinition:studio_ap0:name = "studio_ap0"
        float2 colorSpaceDefinition:studio_ap0:redChroma = (0.64, 0.33)
        float2 colorSpaceDefinition:studio_ap0:greenChroma = (0.30, 0.60)
        float2 colorSpaceDefinition:studio_ap0:blueChroma = (0.15, 0.06)
        float2 colorSpaceDefinition:studio_ap0:whitePoint = (0.3127, 0.3290)
        float colorSpaceDefinition:studio_ap0:gamma = 1
        float colorSpaceDefinition:studio_ap0:linearBias = 0
        def Material "TextureMat"
        {
            token outputs:surface.connect = </World/Alt/TextureMat/Surface.outputs:surface>
            def Shader "Surface"
            {
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor.connect = </World/Alt/TextureMat/Tex.outputs:rgb>
                token outputs:surface
            }
            def Shader "Tex"
            {
                uniform token info:id = "UsdUVTexture"
                asset inputs:file = @missing-wide-gamut.png@ (
                    colorSpace = "studio_ap0"
                )
                float3 outputs:rgb
            }
        }
    }
}
)USD";
  LoadResult loaded = LoadUSDAFromString(usda, std::strlen(usda));
  assert(loaded.success);
  ConverterConfig converter_config;
  converter_config.time_code = 1.5;
  RenderSceneConverter converter(converter_config);
  ConvertResult result = converter.Convert(loaded.stage);
  assert(result.success);
  assert(result.scene.render_settings_path == "/World/settings");
  assert(result.scene.working_color_space == "lin_ap1_scene");
  assert(result.scene.materials.size() == 5);
  const RenderMaterial* material_ptr = nullptr;
  const RenderMaterial* mtlx_material_ptr = nullptr;
  const RenderMaterial* hsv_material_ptr = nullptr;
  for (const RenderMaterial& candidate : result.scene.materials) {
    if (candidate.prim_path == "/World/Mat") material_ptr = &candidate;
    if (candidate.prim_path == "/World/MtlxTextureMat") {
      mtlx_material_ptr = &candidate;
    }
    if (candidate.prim_path == "/World/HsvMat") {
      hsv_material_ptr = &candidate;
    }
  }
  assert(material_ptr);
  assert(mtlx_material_ptr);
  assert(hsv_material_ptr);
  assert(hsv_material_ptr->openpbr);
  const Float4& hsv_identity = hsv_material_ptr->openpbr->base_color.value;
  assert(std::fabs(hsv_identity.x - 0.9f) < 1.0e-6f);
  assert(std::fabs(hsv_identity.y - 0.18f) < 1.0e-6f);
  assert(std::fabs(hsv_identity.z - 0.42f) < 1.0e-6f);
  assert(mtlx_material_ptr->openpbr);
  assert(mtlx_material_ptr->openpbr->base_color.is_texture());
  assert(mtlx_material_ptr->openpbr->specular_color.is_texture());
  assert(std::fabs(mtlx_material_ptr->openpbr->sheen_weight.value.x - 1.0f) <
         1.0e-6f);
  assert(std::fabs(mtlx_material_ptr->openpbr->sheen_roughness.value.x -
                   0.42f) < 1.0e-6f);
  assert(mtlx_material_ptr->mtlx_config.authored);
  assert(mtlx_material_ptr->mtlx_config.version == "1.39");
  assert(mtlx_material_ptr->mtlx_config.colorspace == "studio_ap0");
  const Float4& mtlx_emission =
      mtlx_material_ptr->openpbr->emission_color.value;
  const float mtlx_emission_display[3] = {
      result.scene.working_to_display_linear[0] * mtlx_emission.x +
          result.scene.working_to_display_linear[1] * mtlx_emission.y +
          result.scene.working_to_display_linear[2] * mtlx_emission.z,
      result.scene.working_to_display_linear[3] * mtlx_emission.x +
          result.scene.working_to_display_linear[4] * mtlx_emission.y +
          result.scene.working_to_display_linear[5] * mtlx_emission.z,
      result.scene.working_to_display_linear[6] * mtlx_emission.x +
          result.scene.working_to_display_linear[7] * mtlx_emission.y +
          result.scene.working_to_display_linear[8] * mtlx_emission.z};
  ::lightusd::color::ColorTransform mtlx_to_display;
  assert(::lightusd::next::color_management::BuildColorTransform(
      loaded.stage.GetPrimAtPath("/World/MtlxTextureMat/Constant"),
      "studio_ap0", "lin_rec709_scene", &mtlx_to_display));
  float expected_mtlx_emission[3] = {0.1f, 0.2f, 0.3f};
  ::lightusd::color::TransformRGB(
      mtlx_to_display, expected_mtlx_emission);
  for (int channel = 0; channel < 3; ++channel) {
    assert(std::fabs(mtlx_emission_display[channel] -
                     expected_mtlx_emission[channel]) < 3.0e-4f);
  }
  const Float4& explicit_coat = mtlx_material_ptr->openpbr->coat_color.value;
  const float explicit_coat_display[3] = {
      result.scene.working_to_display_linear[0] * explicit_coat.x +
          result.scene.working_to_display_linear[1] * explicit_coat.y +
          result.scene.working_to_display_linear[2] * explicit_coat.z,
      result.scene.working_to_display_linear[3] * explicit_coat.x +
          result.scene.working_to_display_linear[4] * explicit_coat.y +
          result.scene.working_to_display_linear[5] * explicit_coat.z,
      result.scene.working_to_display_linear[6] * explicit_coat.x +
          result.scene.working_to_display_linear[7] * explicit_coat.y +
          result.scene.working_to_display_linear[8] * explicit_coat.z};
  const float expected_explicit_coat[3] = {0.2f, 0.3f, 0.4f};
  for (int channel = 0; channel < 3; ++channel) {
    assert(std::fabs(explicit_coat_display[channel] -
                     expected_explicit_coat[channel]) < 3.0e-4f);
  }
  const Float4& mixed_sheen = mtlx_material_ptr->openpbr->sheen_color.value;
  const float mixed_sheen_display[3] = {
      result.scene.working_to_display_linear[0] * mixed_sheen.x +
          result.scene.working_to_display_linear[1] * mixed_sheen.y +
          result.scene.working_to_display_linear[2] * mixed_sheen.z,
      result.scene.working_to_display_linear[3] * mixed_sheen.x +
          result.scene.working_to_display_linear[4] * mixed_sheen.y +
          result.scene.working_to_display_linear[5] * mixed_sheen.z,
      result.scene.working_to_display_linear[6] * mixed_sheen.x +
          result.scene.working_to_display_linear[7] * mixed_sheen.y +
          result.scene.working_to_display_linear[8] * mixed_sheen.z};
  assert(std::fabs(mixed_sheen_display[0] - 0.1070205f) < 3.0e-4f);
  assert(std::fabs(mixed_sheen_display[1] - 0.25f) < 3.0e-4f);
  assert(std::fabs(mixed_sheen_display[2]) < 3.0e-4f);
  const RenderMaterial& material = *material_ptr;
  assert(material.preview_surface);
  const Float4& ap1 = material.preview_surface->diffuse_color.value;
  const float display[3] = {
      result.scene.working_to_display_linear[0] * ap1.x +
          result.scene.working_to_display_linear[1] * ap1.y +
          result.scene.working_to_display_linear[2] * ap1.z,
      result.scene.working_to_display_linear[3] * ap1.x +
          result.scene.working_to_display_linear[4] * ap1.y +
          result.scene.working_to_display_linear[5] * ap1.z,
      result.scene.working_to_display_linear[6] * ap1.x +
          result.scene.working_to_display_linear[7] * ap1.y +
          result.scene.working_to_display_linear[8] * ap1.z};
  // Interpolate the authored sRGB samples first: midpoint = (0.5, 0.4, 0.2),
  // then convert that evaluated value to the selected linear working space.
  assert(std::fabs(display[0] - 0.214041f) < 2.0e-4f);
  assert(std::fabs(display[1] - 0.132868f) < 2.0e-4f);
  assert(std::fabs(display[2] - 0.0331048f) < 2.0e-4f);
  const Float4& raw = material.preview_surface->emissive_color.value;
  assert(std::fabs(raw.x - 0.25f) < 1.0e-7f);
  assert(std::fabs(raw.y - 0.5f) < 1.0e-7f);
  assert(std::fabs(raw.z - 0.75f) < 1.0e-7f);
  assert(result.scene.textures.size() == 4);
  // Asset colorSpace metadata wins over inputs:sourceColorSpace and carries
  // the selected render working space to native/web texture consumers.
  const RenderTexture* ap0_texture = nullptr;
  const RenderTexture* scoped_rec709_texture = nullptr;
  const RenderTexture* inherited_rec709_texture = nullptr;
  size_t ap0_texture_count = 0;
  for (const RenderTexture& texture : result.scene.textures) {
    assert(texture.target_color_space == "lin_ap1_scene");
    assert(texture.color_transform_valid);
    assert(!texture.source_color_is_data);
    assert(std::fabs(texture.source_gamma - 1.0f) < 1.0e-7f);
    if (texture.source_color_space == "lin_rec709_scene") {
      assert(texture.color_transform_bypass);
      inherited_rec709_texture = &texture;
      continue;
    }
    assert(!texture.color_transform_bypass);
    assert(texture.source_color_space == "studio_ap0");
    if (texture.source_to_display_linear[0] > 2.0f) {
      ap0_texture = &texture;
      ++ap0_texture_count;
    } else {
      scoped_rec709_texture = &texture;
    }
  }
  assert(ap0_texture);
  assert(scoped_rec709_texture);
  assert(inherited_rec709_texture);
  assert(ap0_texture_count == 2);
  assert(std::fabs(ap0_texture->source_to_display_linear[0] -
                   2.521686f) < 2.0e-4f);
  assert(std::fabs(scoped_rec709_texture->source_to_display_linear[0] -
                   1.0f) < 2.0e-4f);
  assert(std::fabs(inherited_rec709_texture->source_to_display_linear[0] -
                   1.0f) < 2.0e-4f);
  std::cout << "  RenderSettings color management: PASSED\n";
}

class RecordingSceneUpdateSink final : public SceneUpdateSink {
 public:
  bool BeginUpdate(uint64_t base, uint64_t next, bool full) override {
    base_revision = base;
    new_revision = next;
    full_resync = full;
    mesh_upserts = 0;
    removes = 0;
    mesh_removes = 0;
    return true;
  }
  bool Remove(const RemovedRenderResource& removed) override {
    ++removes;
    if (removed.kind == RenderResourceKind::Mesh) ++mesh_removes;
    return true;
  }
  bool UpsertMesh(RenderId id, const RenderMesh& mesh) override {
    ++mesh_upserts;
    mesh_ids[mesh.prim_path] = id;
    if (!mesh.points.empty()) last_mesh_x = mesh.points[0];
    return true;
  }
  bool EndUpdate() override { return true; }

  uint64_t base_revision = 0;
  uint64_t new_revision = 0;
  bool full_resync = false;
  size_t mesh_upserts = 0;
  size_t removes = 0;
  size_t mesh_removes = 0;
  float last_mesh_x = 0.0f;
  std::map<std::string, RenderId> mesh_ids;
};

void TestIncrementalRenderSession() {
  std::cout << "Testing incremental RenderSession...\n";
  auto source = [](float x) {
    std::string text = "#usda 1.0\ndef Mesh \"M\" {\n";
    text += "    int[] faceVertexCounts = [3]\n";
    text += "    int[] faceVertexIndices = [0, 1, 2]\n";
    text += "    point3f[] points = [(" + std::to_string(x);
    text += ", 0, 0), (1, 0, 0), (0, 1, 0)]\n}\n";
    return text;
  };

  LoadResult first = LoadUSDAFromString(source(0.0f));
  assert(first.success);
  StageSnapshot first_snapshot;
  first_snapshot.revision = 1;
  first_snapshot.stage.reset(new Stage(std::move(first.stage)));

  RecordingSceneUpdateSink sink;
  RenderSession render_session;
  RenderUpdateResult initial =
      render_session.Initialize(first_snapshot, &sink);
  if (!initial) std::cerr << "RenderSession init failed: " << initial.error << "\n";
  assert(initial);
  assert(initial.full_resync);
  assert(initial.converted_resource_count > 0);
  assert(initial.converted_scene_bytes > 0);
  assert(sink.full_resync);
  assert(sink.mesh_upserts == 1);
  const RenderId mesh_id = sink.mesh_ids.at("/M");
  assert(std::fabs(sink.last_mesh_x) < 1.0e-6f);

  LoadResult second = LoadUSDAFromString(source(2.0f));
  assert(second.success);
  StageSnapshot second_snapshot;
  second_snapshot.revision = 2;
  second_snapshot.stage.reset(new Stage(std::move(second.stage)));
  StageChangeSet changes;
  changes.base_revision = 1;
  changes.new_revision = 2;
  PrimChange mesh_change;
  mesh_change.path = Path("/M");
  mesh_change.flags = StageChangeFlag::Topology;
  mesh_change.properties.push_back("points");
  changes.prims.push_back(std::move(mesh_change));
  RenderUpdateResult update =
      render_session.Apply(second_snapshot, changes, &sink);
  assert(update);
  assert(!update.full_resync);
  assert(update.converted_resource_count > 0);
  assert(!sink.full_resync);
  assert(sink.mesh_upserts == 1);
  assert(sink.mesh_ids.at("/M") == mesh_id);
  assert(std::fabs(sink.last_mesh_x - 2.0f) < 1.0e-6f);
  assert(sink.removes == 0);
  assert(render_session.revision() == second_snapshot.revision);

  LoadResult third = LoadUSDAFromString("#usda 1.0\n");
  assert(third.success);
  StageSnapshot third_snapshot;
  third_snapshot.revision = 3;
  third_snapshot.stage.reset(new Stage(std::move(third.stage)));
  StageChangeSet removal;
  removal.base_revision = 2;
  removal.new_revision = 3;
  PrimChange removed_mesh;
  removed_mesh.path = Path("/M");
  removed_mesh.flags = StageChangeFlag::Resync;
  removal.prims.push_back(std::move(removed_mesh));
  RenderUpdateResult removed =
      render_session.Apply(third_snapshot, removal, &sink);
  assert(removed);
  assert(!sink.full_resync);
  assert(sink.mesh_upserts == 0);
  assert(sink.mesh_removes == 1);
  assert(sink.removes >= 1);
  assert(removed.remove_count == sink.removes);
  assert(render_session.revision() == 3);

  StageSnapshot no_op_snapshot;
  no_op_snapshot.revision = 4;
  no_op_snapshot.stage = third_snapshot.stage;
  StageChangeSet no_op;
  no_op.base_revision = 3;
  no_op.new_revision = 4;
  RenderUpdateResult no_op_update =
      render_session.Apply(no_op_snapshot, no_op, &sink);
  assert(no_op_update);
  assert(!sink.full_resync);
  assert(no_op_update.upsert_count == 0);
  assert(no_op_update.remove_count == 0);
  assert(no_op_update.converted_resource_count == 0);
  assert(no_op_update.converted_scene_bytes == 0);
  assert(sink.mesh_upserts == 0);
  assert(sink.removes == 0);
  assert(render_session.revision() == 4);
  std::cout << "  incremental RenderSession: PASSED\n";
}

void TestPtexMaterialInterfaceAsset() {
  std::cout << "Testing Ptex material-interface asset forwarding...\n";
  const char* usda = "#usda 1.0\ndef Xform \"World\"\n{\n"
                    "    def Material \"Mat\"\n"
                    "    {\n"
                    "        asset inputs:surfaceMap = @maps/surface.ptx@\n"
                    "        token outputs:surface.connect = </World/Mat/Surface.outputs:surface>\n"
                    "        def Shader \"Surface\"\n"
                    "        {\n"
                    "            uniform token info:id = \"UsdPreviewSurface\"\n"
                    "            color3f inputs:diffuseColor.connect = </World/Mat/Ptex.outputs:resultRGB>\n"
                    "            token outputs:surface\n"
                    "        }\n"
                    "        def Shader \"Ptex\"\n"
                    "        {\n"
                    "            uniform token info:id = \"HwPtexTexture\"\n"
                    "            asset inputs:file.connect = </World/Mat.inputs:surfaceMap>\n"
                    "            color3f outputs:resultRGB\n"
                    "        }\n"
                    "    }\n"
                    "    def Mesh \"Quad\"\n"
                    "    {\n"
                    "        point3f[] points = [(0,0,0), (1,0,0), (1,1,0), (0,1,0)]\n"
                    "        int[] faceVertexCounts = [4]\n"
                    "        int[] faceVertexIndices = [0,1,2,3]\n"
                    "        rel material:binding = </World/Mat>\n"
                    "    }\n"
                    "}\n";

  LoadResult loaded = LoadUSDAFromString(usda, std::strlen(usda));
  assert(loaded.success);
  ConverterConfig config;
  config.material.load_textures = false;
  RenderSceneConverter converter(config);
  ConvertResult converted = converter.Convert(loaded.stage);
  assert(converted.success);
  assert(converted.scene.textures.size() == 1);
  assert(converted.scene.textures[0].asset_path == "maps/surface.ptx");
  assert(converted.scene.images[0].resolved_path == "maps/surface.ptx");
  std::cout << "  Ptex material-interface asset forwarding: PASSED\n";
}

void TestSurfaceUnlitMaterialXConversion() {
  std::cout << "Testing MaterialX surface_unlit conversion...\n";
  const char* usda = R"(#usda 1.0
def Scope "World" {
  def Material "Unlit" {
    token outputs:mtlx:surface.connect = </World/Unlit/Surface.outputs:out>
    def Shader "Surface" {
      uniform token info:id = "ND_surface_unlit_surfaceshader"
      float inputs:emission = 2.25
      color3f inputs:emission_color.connect = </World/Unlit/Color.outputs:out>
      float inputs:transmission = 0.35
      color3f inputs:transmission_color = (0.2, 0.4, 0.8)
      color3f inputs:normal = (0.1, 0.2, 0.9)
      color3f inputs:opacity = (0.6, 0.7, 0.8)
      token outputs:out
    }
    def Shader "Color" {
      uniform token info:id = "ND_multiply_color3"
      color3f inputs:in1 = (0.5, 0.25, 0.125)
      color3f inputs:in2 = (0.8, 0.8, 0.8)
      color3f outputs:out
    }
  }
}
)";
  LoadResult loaded = LoadUSDAFromString(usda, std::strlen(usda));
  assert(loaded.success);

  ConverterConfig config;
  config.material.load_textures = false;
  RenderSceneConverter converter(config);
  ConvertResult result = converter.Convert(loaded.stage);
  assert(result.success);
  const int32_t material_id = result.scene.material_by_path.at("/World/Unlit");
  const RenderMaterial& material =
      result.scene.materials.at(static_cast<size_t>(material_id));
  assert(material.shader_type == RenderMaterial::ShaderType::OpenPBR);
  assert(material.openpbr);
  assert(!material.default_fallback);
  assert(std::fabs(material.openpbr->base_weight.value.x) < 1.0e-6f);
  assert(std::fabs(material.openpbr->specular_weight.value.x) < 1.0e-6f);
  assert(std::fabs(material.openpbr->emission_luminance.value.x - 2.25f) <
         1.0e-6f);
  assert(std::fabs(material.openpbr->transmission_weight.value.x - 0.35f) <
         1.0e-6f);
  assert(std::fabs(material.openpbr->transmission_color.value.z - 0.8f) <
         1.0e-6f);
  assert(std::fabs(material.openpbr->normal.value.x - 0.1f) < 1.0e-6f);
  assert(std::fabs(material.openpbr->normal.value.y - 0.2f) < 1.0e-6f);
  assert(std::fabs(material.openpbr->normal.value.z - 0.9f) < 1.0e-6f);
  assert(std::fabs(material.openpbr->opacity.value.x - 0.6f) < 1.0e-6f);
  assert(material.alpha_mode == RenderMaterial::AlphaMode::Blend);
  assert(material.openpbr->nodegraph_json.find("ND_multiply_color3") !=
         std::string::npos);
  assert(material.openpbr->nodegraph_json.find("emission_color") !=
         std::string::npos);
  std::cout << "  MaterialX surface_unlit conversion: PASSED\n";
}

void TestParallelMaterialTextureRemapAndCallbackPolicy() {
  std::cout << "Testing parallel material texture remap/callback policy...\n";
  const char* usda = R"(#usda 1.0
def Xform "World" {
  def Material "First" {
    token outputs:surface.connect = </World/First/S.outputs:surface>
    def Shader "S" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor.connect = </World/First/T.outputs:rgb>
      token outputs:surface
    }
    def Shader "T" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @first.png@
      color3f outputs:rgb
    }
  }
  def Material "Second" {
    token outputs:mtlx:surface.connect = </World/Second/S.outputs:surface>
    def Shader "S" {
      uniform token info:id = "ND_open_pbr_surface_surfaceshader"
      color3f inputs:transmission_color.connect = </World/Second/T.outputs:rgb>
      token outputs:surface
    }
    def Shader "T" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @second.png@
      color3f outputs:rgb
    }
  }
  def Material "Fallback" {
    token outputs:surface.connect = </World/Fallback/U.outputs:surface>
    def Shader "U" {
      uniform token info:id = "UnsupportedSurface"
      float inputs:future_lobe.connect = </World/Fallback/T.outputs:r>
      token outputs:surface
    }
    def Shader "T" {
      uniform token info:id = "UsdUVTexture"
      asset inputs:file = @fallback.png@
      float outputs:r
    }
  }
}
)";
  LoadResult loaded = LoadUSDAFromString(usda, std::strlen(usda));
  assert(loaded.success);

  ConverterConfig parallel;
  parallel.material.load_textures = false;
  parallel.execution.max_threads = 4;
  parallel.execution.callback_concurrency = CallbackConcurrency::Concurrent;
  RenderSceneConverter converter(parallel);
  ConvertResult result = converter.Convert(loaded.stage);
  assert(result.success);
  assert(result.scene.images.size() == 3);
  const RenderMaterial& second = result.scene.materials.at(
      static_cast<size_t>(result.scene.material_by_path.at("/World/Second")));
  assert(second.openpbr);
  const int32_t transmission_id = second.openpbr->transmission_color.texture_id;
  assert(transmission_id >= 0);
  const RenderTexture& transmission =
      result.scene.textures.at(static_cast<size_t>(transmission_id));
  assert(result.scene.images.at(static_cast<size_t>(transmission.image_id))
             .resolved_path == "second.png");

  const RenderMaterial& fallback = result.scene.materials.at(
      static_cast<size_t>(result.scene.material_by_path.at("/World/Fallback")));
  const RetainedMaterialParam* future = nullptr;
  for (const RetainedMaterialParam& param : fallback.retained_params) {
    if (param.name == "future_lobe") future = &param;
  }
  assert(future && future->value.texture_id >= 0);
  const RenderTexture& future_texture = result.scene.textures.at(
      static_cast<size_t>(future->value.texture_id));
  assert(result.scene.images.at(static_cast<size_t>(future_texture.image_id))
             .resolved_path == "fallback.png");

  std::atomic<int> active{0};
  std::atomic<int> max_active{0};
  ConverterConfig serialized = parallel;
  serialized.material.load_textures = true;
  serialized.execution.callback_concurrency = CallbackConcurrency::Serialized;
  serialized.material.custom_texture_loader =
      [&](const std::string& path, TextureImage* image) {
        const int now = active.fetch_add(1) + 1;
        int observed = max_active.load();
        while (now > observed &&
               !max_active.compare_exchange_weak(observed, now)) {
        }
        for (int i = 0; i < 1000; ++i) std::this_thread::yield();
        image->resolved_path = path;
        active.fetch_sub(1);
        return true;
      };
  RenderSceneConverter serialized_converter(serialized);
  ConvertResult serialized_result = serialized_converter.Convert(loaded.stage);
  assert(serialized_result.success);
  assert(max_active.load() == 1);

  ConverterConfig hardened = MakeHardenedConverterConfig(size_t(64) << 20);
  assert(hardened.execution.max_threads == 1);
  assert(hardened.execution.max_in_flight_bytes == (size_t(64) << 20));
  assert(hardened.max_render_records != 0);
  std::cout << "  parallel material texture remap/callback policy: PASSED\n";
}

int main() {
  std::cout << "=== Tydra Next Unit Tests ===\n\n";

  // ChunkedArray tests
  TestChunkedArrayBasic();
  TestChunkedArrayShrinkToFit();
  TestChunkedArrayAllocFailure();
  TestChunkedArrayLarge();
  TestChunkedArrayStraddlingReads();
  TestChunkedArrayShareCow();
  TestChunkedArrayShareCowBudgetFailure();
  TestChunkedArrayBudgetTracking();
  TestChunkedArrayStorageEdges();
  TestChunkedArrayAppend();
  TestChunkedArrayIterator();
  TestRenderDataSafety();
  TestTextureBudgetLease();

  std::cout << "\n";

  // Run before fixture-heavy tests so the global name table only holds
  // registered names (maximizes intern-miss coverage).
  TestNameTableFreezeStability();

  std::cout << "\n";

  // RenderData tests
  TestRenderMesh();
  TestRenderScene();

  std::cout << "\n";

  // Scene Access tests
  TestSceneAccess();
  TestDeepSceneAccess();
  TestRenderExtract();

  std::cout << "\n";

  // Converter tests
  TestRenderConverter();
  TestRenderConverterHalfGaussian();
  TestRenderConverterMaterials();
  TestRenderConverterNestedMaterialXGraph();
  TestRenderConverterUnrealSurfaceFallback();
  TestRenderConverterUnrealSourceAssetFallback();
  TestRenderConverterInheritedSkelBinding();
  TestRenderConverterPointInstancerWarnings();
  TestRenderConverterPointInstancerIndexVisibility();
  TestRenderConverterPointInstancerInvalidArrays();
  TestRenderConverterPointInstancerDuplicateMeshMetadata();
  TestMaterialXUtilities();
  TestExternalMaterialXImageGraph();
  TestAudit2026_07();
  TestAudit2026_07_Gaps();
  TestPhysicsAnnotations();
  TestRenderConverterCurves();
  TestConverterMemoryBudget();
  TestLightLinkCollectionRanges();
  TestUsdzEntryMatching();
  TestTriangulateOutOfRangeIndices();
  TestLargeMeshTangents();
  TestValueClipBaking();
  TestAnimationExtractionGate();
  TestMeshParityCleanups();
  TestHalfPrecisionXformOps();
  TestCompoundRotationTransformParity();
  TestMultiSkeletonJointRemap();
  TestSkeletonJointRemapLeafTokens();
  TestGeometryReleaseSkipsAnimatedArrays();
  TestAnimationKeyframeOrdering();
  TestLightLinkingAndDomeFormat();
  TestStandardSurfaceNoRecursion();
  TestUsdSkelParityFixes();
  TestUsdSkelClusterFixes();
  TestMaterialUVPrimvarPromotion();
  TestMaterialParityFixes();
  TestAuditReviewFixes();
  TestP2AuditFixes();
  TestLegacyParityExtraction();
  TestRenderColorManagement();
  TestIncrementalRenderSession();
  TestPtexMaterialInterfaceAsset();
  TestSurfaceUnlitMaterialXConversion();
  TestParallelMaterialTextureRemapAndCallbackPolicy();

  std::cout << "\n=== All Tydra Next tests PASSED ===\n";
  return 0;
}
