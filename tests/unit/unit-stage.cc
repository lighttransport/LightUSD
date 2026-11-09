#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-stage.h"
#include "core/prim.hh"
#include "core/path.hh"
#include "lightusd.hh"
#include "usdGeom.hh"
#include "usda-writer.hh"
#include "stage.hh"
#include "timesamples.hh"
#include "mmap-array-ref.hh"

#include <cstdint>
#include <vector>

#if defined(LIGHTUSD_ENABLE_THREAD)
#include <atomic>
#include <thread>
#endif

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

// Helper to build a test hierarchy:
// /Root
//   /Root/Child1
//     /Root/Child1/GrandChild1
//     /Root/Child1/GrandChild2
//   /Root/Child2
//     /Root/Child2/GrandChild3
static Stage build_test_stage() {
  Stage stage;

  // Create root prim
  Xform root_xform;
  root_xform.name = "Root";
  Prim root_prim("Root", root_xform);

  // Create Child1 with grandchildren
  Xform child1_xform;
  child1_xform.name = "Child1";
  Prim child1("Child1", child1_xform);

  Xform gc1_xform;
  gc1_xform.name = "GrandChild1";
  Prim grandchild1("GrandChild1", gc1_xform);
  child1.add_child(std::move(grandchild1));

  Xform gc2_xform;
  gc2_xform.name = "GrandChild2";
  Prim grandchild2("GrandChild2", gc2_xform);
  child1.add_child(std::move(grandchild2));

  root_prim.add_child(std::move(child1));

  // Create Child2 with grandchild
  Xform child2_xform;
  child2_xform.name = "Child2";
  Prim child2("Child2", child2_xform);

  Xform gc3_xform;
  gc3_xform.name = "GrandChild3";
  Prim grandchild3("GrandChild3", gc3_xform);
  child2.add_child(std::move(grandchild3));

  root_prim.add_child(std::move(child2));

  stage.add_root_prim(std::move(root_prim));

  return stage;
}

void stage_get_prim_at_path_test(void) {
  Stage stage = build_test_stage();

  // Test finding root prim
  {
    Path path("/Root", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(result.has_value());
    if (result) {
      TEST_CHECK(result.value()->element_name() == "Root");
    }
  }

  // Test finding first level child
  {
    Path path("/Root/Child1", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(result.has_value());
    if (result) {
      TEST_CHECK(result.value()->element_name() == "Child1");
    }
  }

  // Test finding second level child (grandchild)
  {
    Path path("/Root/Child1/GrandChild1", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(result.has_value());
    if (result) {
      TEST_CHECK(result.value()->element_name() == "GrandChild1");
    }
  }

  {
    Path path("/Root/Child1/GrandChild2", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(result.has_value());
    if (result) {
      TEST_CHECK(result.value()->element_name() == "GrandChild2");
    }
  }

  {
    Path path("/Root/Child2/GrandChild3", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(result.has_value());
    if (result) {
      TEST_CHECK(result.value()->element_name() == "GrandChild3");
    }
  }

  // Test non-existent paths
  {
    Path path("/NonExistent", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(!result.has_value());
  }

  {
    Path path("/Root/NonExistent", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(!result.has_value());
  }

  {
    Path path("/Root/Child1/NonExistent", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(!result.has_value());
  }

  // Test invalid path (root only)
  {
    Path path("/", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(!result.has_value());
  }

  // Test caching - second lookup should use cache
  {
    Path path("/Root/Child1/GrandChild1", "");
    auto result1 = stage.GetPrimAtPath(path);
    auto result2 = stage.GetPrimAtPath(path);
    TEST_CHECK(result1.has_value());
    TEST_CHECK(result2.has_value());
    if (result1 && result2) {
      TEST_CHECK(result1.value() == result2.value());  // Same pointer
    }
  }
}

void stage_get_prim_from_relative_path_test(void) {
  Stage stage = build_test_stage();
  TEST_CHECK(stage.compute_absolute_prim_path_and_assign_prim_id(true));

  auto root_result = stage.GetPrimAtPath(Path("/Root", ""));
  TEST_CHECK(root_result.has_value());
  if (!root_result) return;

  auto child1_result = stage.GetPrimAtPath(Path("/Root/Child1", ""));
  TEST_CHECK(child1_result.has_value());
  if (!child1_result) return;

  {
    auto result =
        stage.GetPrimFromRelativePath(*root_result.value(), Path("Child1", ""));
    TEST_CHECK(result.has_value());
    if (result) {
      TEST_CHECK(result.value()->element_name() == "Child1");
    }
  }

  {
    auto result = stage.GetPrimFromRelativePath(*root_result.value(),
                                                Path("Child1/GrandChild2", ""));
    TEST_CHECK(result.has_value());
    if (result) {
      TEST_CHECK(result.value()->element_name() == "GrandChild2");
    }
  }

  {
    auto result = stage.GetPrimFromRelativePath(
        *child1_result.value(), Path("../Child2/GrandChild3", ""));
    TEST_CHECK(result.has_value());
    if (result) {
      TEST_CHECK(result.value()->element_name() == "GrandChild3");
    }
  }

  {
    auto result =
        stage.GetPrimFromRelativePath(*root_result.value(), Path("Missing", ""));
    TEST_CHECK(!result.has_value());
  }

  {
    auto result = stage.GetPrimFromRelativePath(*root_result.value(),
                                                Path("Child1", "visibility"));
    TEST_CHECK(!result.has_value());
  }
}

void stage_find_prim_by_id_test(void) {
  Stage stage = build_test_stage();

  // Assign prim IDs
  TEST_CHECK(stage.compute_absolute_prim_path_and_assign_prim_id(true));

  // Collect prim IDs first
  std::vector<int64_t> prim_ids;
  std::vector<std::string> prim_names;

  // Get root prim ID
  {
    Path path("/Root", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(result.has_value());
    if (result) {
      prim_ids.push_back(result.value()->prim_id());
      prim_names.push_back(result.value()->element_name());
    }
  }

  // Get child prim IDs
  {
    Path path("/Root/Child1", "");
    auto result = stage.GetPrimAtPath(path);
    if (result) {
      prim_ids.push_back(result.value()->prim_id());
      prim_names.push_back(result.value()->element_name());
    }
  }

  {
    Path path("/Root/Child1/GrandChild1", "");
    auto result = stage.GetPrimAtPath(path);
    if (result) {
      prim_ids.push_back(result.value()->prim_id());
      prim_names.push_back(result.value()->element_name());
    }
  }

  {
    Path path("/Root/Child2/GrandChild3", "");
    auto result = stage.GetPrimAtPath(path);
    if (result) {
      prim_ids.push_back(result.value()->prim_id());
      prim_names.push_back(result.value()->element_name());
    }
  }

  // Now test find_prim_by_prim_id
  for (size_t i = 0; i < prim_ids.size(); i++) {
    const Prim *found_prim = nullptr;
    std::string err;
    bool found = stage.find_prim_by_prim_id(
        static_cast<uint64_t>(prim_ids[i]), found_prim, &err);

    TEST_CHECK(found);
    if (found && found_prim) {
      TEST_CHECK(found_prim->element_name() == prim_names[i]);
      TEST_CHECK(found_prim->prim_id() == prim_ids[i]);
    }
  }

  // Test non-existent prim ID
  {
    const Prim *found_prim = nullptr;
    std::string err;
    bool found = stage.find_prim_by_prim_id(999999, found_prim, &err);
    TEST_CHECK(!found);
  }

  // Test invalid prim ID (0)
  {
    const Prim *found_prim = nullptr;
    std::string err;
    bool found = stage.find_prim_by_prim_id(0, found_prim, &err);
    TEST_CHECK(!found);
    TEST_CHECK(!err.empty());
  }

  // Mutable lookup reports the same invalid-ID diagnostic.
  {
    Prim *found_prim = nullptr;
    std::string err;
    bool found = stage.find_prim_by_prim_id(0, found_prim, &err);
    TEST_CHECK(!found);
    TEST_CHECK(!err.empty());
  }

  // Test caching - second lookup should use cache
  if (!prim_ids.empty()) {
    const Prim *found1 = nullptr;
    const Prim *found2 = nullptr;
    std::string err;

    bool ok1 = stage.find_prim_by_prim_id(
        static_cast<uint64_t>(prim_ids[0]), found1, &err);
    bool ok2 = stage.find_prim_by_prim_id(
        static_cast<uint64_t>(prim_ids[0]), found2, &err);

    TEST_CHECK(ok1 && ok2);
    TEST_CHECK(found1 == found2);  // Same pointer from cache
  }
}

void stage_adopt_mmap_buffer_lifetime_test(void) {
  std::vector<uint8_t> bytes = {0, 1, 2, 3, 4, 5, 6, 7};

  Stage stage;
  stage.set_mmap_source(MMapDataSource(bytes.data() + 2, 4));

  MMapArrayRef ref;
  ref.byte_offset = 1;
  ref.element_count = 2;
  ref.element_size = sizeof(uint8_t);

  MMapArrayTable table;
  table.add("/Mesh", "points", ref);
  stage.set_mmap_table(std::move(table));

  TEST_CHECK(stage.adopt_mmap_buffer(std::move(bytes)));
  TEST_CHECK(stage.has_mmap_zero_copy());

  const MMapArrayRef *stored_ref = stage.mmap_table()->find("/Mesh", "points");
  TEST_CHECK(stored_ref != nullptr);
  if (stored_ref) {
    const uint8_t *ptr = stage.mmap_source()->get_ptr<uint8_t>(*stored_ref);
    TEST_CHECK(ptr != nullptr);
    if (ptr) {
      TEST_CHECK(ptr[0] == 3);
      TEST_CHECK(ptr[1] == 4);
    }
  }

  stage.clear_mmap_data();
  TEST_CHECK(!stage.has_mmap_zero_copy());
}

void stage_add_root_prim_test(void) {
  // Start with empty stage
  Stage stage;
  TEST_CHECK(stage.root_prims().size() == 0);

  // Add first root prim
  {
    Xform xf;
    xf.name = "First";
    Prim prim("First", xf);
    bool ok = stage.add_root_prim(std::move(prim), false);
    TEST_CHECK(ok);
    TEST_CHECK(stage.root_prims().size() == 1);
    TEST_CHECK(stage.root_prims()[0].element_name() == "First");
  }

  // Add second root prim with different name
  {
    Xform xf;
    xf.name = "Second";
    Prim prim("Second", xf);
    bool ok = stage.add_root_prim(std::move(prim), false);
    TEST_CHECK(ok);
    TEST_CHECK(stage.root_prims().size() == 2);
    TEST_CHECK(stage.root_prims()[1].element_name() == "Second");
  }

  // Adding prim with same name and rename_prim_name=true should succeed
  {
    Xform xf;
    xf.name = "First";
    Prim prim("First", xf);
    bool ok = stage.add_root_prim(std::move(prim), true);
    TEST_CHECK(ok);
    TEST_CHECK(stage.root_prims().size() == 3);
    // The name should have been renamed to avoid collision
    TEST_MSG("Third prim name after rename: %s",
             stage.root_prims()[2].element_name().c_str());
  }
}

void stage_replace_root_prim_test(void) {
  Stage stage;

  // Add an Xform root prim named "A"
  {
    Xform xf;
    xf.name = "A";
    Prim prim("A", xf);
    stage.add_root_prim(std::move(prim), false);
  }
  TEST_CHECK(stage.root_prims().size() == 1);
  TEST_CHECK(stage.root_prims()[0].type_name() == "Xform");

  // Replace "A" with a GeomMesh prim
  {
    GeomMesh mesh;
    mesh.name = "A";
    Prim prim("A", mesh);
    bool ok = stage.replace_root_prim("A", std::move(prim));
    TEST_CHECK(ok);
    TEST_CHECK(stage.root_prims().size() == 1);
    TEST_CHECK(stage.root_prims()[0].type_name() == "Mesh");
    TEST_CHECK(stage.root_prims()[0].element_name() == "A");
  }

  // replace_root_prim for non-existent name should add it
  {
    Xform xf;
    xf.name = "NonExistent";
    Prim prim("NonExistent", xf);
    bool ok = stage.replace_root_prim("NonExistent", std::move(prim));
    TEST_CHECK(ok);
    TEST_CHECK(stage.root_prims().size() == 2);
  }

  // replace_root_prim with empty name should fail
  {
    Xform xf;
    Prim prim("Dummy", xf);
    bool ok = stage.replace_root_prim("", std::move(prim));
    TEST_CHECK(!ok);
  }
}

void stage_export_to_string_test(void) {
  Stage stage;

  // Add an Xform root prim
  Xform xf;
  xf.name = "MyXform";
  Prim prim("MyXform", xf);
  stage.add_root_prim(std::move(prim), false);

  // Export to string
  std::string usda = stage.ExportToString();
  TEST_CHECK(!usda.empty());
  TEST_CHECK(usda.find("#usda 1.0") != std::string::npos);
  TEST_MSG("USDA output contains header");
  TEST_CHECK(usda.find("MyXform") != std::string::npos);
  TEST_MSG("USDA output contains prim name");

  // Round-trip: re-parse the USDA string
  Stage stage2;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "", &stage2, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());
    return;
  }
  TEST_CHECK(stage2.root_prims().size() == 1);
  TEST_CHECK(stage2.root_prims()[0].element_name() == "MyXform");
}

void stage_commit_prim_id_test(void) {
  Stage stage;

  // Build a small hierarchy: /Root/Child
  Xform root_xf;
  root_xf.name = "Root";
  Prim root_prim("Root", root_xf);

  Xform child_xf;
  child_xf.name = "Child";
  Prim child_prim("Child", child_xf);
  root_prim.add_child(std::move(child_prim));

  stage.add_root_prim(std::move(root_prim), false);

  // commit() assigns prim IDs
  bool ok = stage.commit();
  TEST_CHECK(ok);

  // Verify Root prim has a valid prim_id
  {
    Path path("/Root", "");
    auto result = stage.GetPrimAtPath(path);
    TEST_CHECK(result.has_value());
    if (result) {
      int64_t id = result.value()->prim_id();
      TEST_CHECK(id > 0);
      TEST_MSG("Root prim_id = %lld", (long long)id);
    }
  }

  // Verify Child prim has a valid and different prim_id
  {
    Path root_path("/Root", "");
    Path child_path("/Root/Child", "");
    auto root_result = stage.GetPrimAtPath(root_path);
    auto child_result = stage.GetPrimAtPath(child_path);
    TEST_CHECK(root_result.has_value());
    TEST_CHECK(child_result.has_value());
    if (root_result && child_result) {
      int64_t root_id = root_result.value()->prim_id();
      int64_t child_id = child_result.value()->prim_id();
      TEST_CHECK(child_id > 0);
      TEST_CHECK(root_id != child_id);
      TEST_MSG("Root prim_id = %lld, Child prim_id = %lld",
               (long long)root_id, (long long)child_id);
    }
  }
}

void stage_metas_test(void) {
  Stage stage;

  // Set stage metadata
  stage.metas().defaultPrim = lightusd::legacy::value::token("Root");
  stage.metas().upAxis = Axis::Y;
  stage.metas().metersPerUnit = 0.01;

  // Verify locally
  TEST_CHECK(stage.metas().defaultPrim.str() == "Root");
  TEST_CHECK(stage.metas().upAxis.get_value() == Axis::Y);
  TEST_CHECK(stage.metas().metersPerUnit.get_value() == 0.01);

  // Add a root prim so we have valid USDA
  Xform xf;
  xf.name = "Root";
  Prim prim("Root", xf);
  stage.add_root_prim(std::move(prim), false);

  // Export and round-trip
  std::string usda = stage.ExportToString();
  TEST_CHECK(!usda.empty());

  Stage stage2;
  std::string warn, err;
  bool ok = LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "", &stage2, &warn, &err);
  TEST_CHECK(ok);
  if (!ok) {
    TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());
    return;
  }

  // Check re-parsed metas
  TEST_CHECK(stage2.metas().defaultPrim.str() == "Root");
  TEST_CHECK(stage2.metas().upAxis.get_value() == Axis::Y);

  // Check metersPerUnit with tolerance for floating point
  double mpu = stage2.metas().metersPerUnit.get_value();
  TEST_CHECK(mpu > 0.009 && mpu < 0.011);
  TEST_MSG("Re-parsed metersPerUnit = %f", mpu);
}

void stage_memory_estimation_test(void) {
  Stage stage = build_test_stage();
  size_t mem = stage.estimate_memory_usage();
  TEST_CHECK(mem > 0);
  TEST_MSG("Memory usage estimate: %zu bytes", mem);
}

void stage_empty_test(void) {
  Stage stage;

  // Empty stage should have no root prims
  TEST_CHECK(stage.root_prims().empty());

  // ExportToString should still produce valid USDA with header
  std::string usda = stage.ExportToString();
  TEST_CHECK(!usda.empty());
  TEST_CHECK(usda.find("#usda 1.0") != std::string::npos);
  TEST_MSG("Empty stage USDA: %.80s...", usda.c_str());
}

void stage_nested_hierarchy_test(void) {
  Stage stage;

  // Build 4-level hierarchy: /Root/A/B/C
  Xform xf_c;
  xf_c.name = "C";
  Prim prim_c("C", xf_c);

  Xform xf_b;
  xf_b.name = "B";
  Prim prim_b("B", xf_b);
  prim_b.add_child(std::move(prim_c));

  Xform xf_a;
  xf_a.name = "A";
  Prim prim_a("A", xf_a);
  prim_a.add_child(std::move(prim_b));

  Xform xf_root;
  xf_root.name = "Root";
  Prim prim_root("Root", xf_root);
  prim_root.add_child(std::move(prim_a));

  stage.add_root_prim(std::move(prim_root), false);
  bool ok = stage.commit();
  TEST_CHECK(ok);

  // Verify all 4 levels are reachable via GetPrimAtPath
  {
    Path p("/Root", "");
    auto r = stage.GetPrimAtPath(p);
    TEST_CHECK(r.has_value());
    if (r) {
      TEST_CHECK(r.value()->element_name() == "Root");
    }
  }

  {
    Path p("/Root/A", "");
    auto r = stage.GetPrimAtPath(p);
    TEST_CHECK(r.has_value());
    if (r) {
      TEST_CHECK(r.value()->element_name() == "A");
    }
  }

  {
    Path p("/Root/A/B", "");
    auto r = stage.GetPrimAtPath(p);
    TEST_CHECK(r.has_value());
    if (r) {
      TEST_CHECK(r.value()->element_name() == "B");
    }
  }

  {
    Path p("/Root/A/B/C", "");
    auto r = stage.GetPrimAtPath(p);
    TEST_CHECK(r.has_value());
    if (r) {
      TEST_CHECK(r.value()->element_name() == "C");
    }
  }

  // Non-existent deep path
  {
    Path p("/Root/A/B/C/D", "");
    auto r = stage.GetPrimAtPath(p);
    TEST_CHECK(!r.has_value());
  }
}

// Shares ONE Stage across many threads, all calling the const read API
// find_prim_at_path() concurrently. This exercises the lazy lookup-cache
// (clear-on-dirty + insert) under contention. Must be ThreadSanitizer-clean
// (the cache read/write is guarded by Stage::_cache_mu).
void stage_concurrent_find_prim_test(void) {
#if defined(LIGHTUSD_ENABLE_THREAD)
  Stage stage = build_test_stage();

  const std::vector<std::string> hit_paths = {
      "/Root",
      "/Root/Child1",
      "/Root/Child1/GrandChild1",
      "/Root/Child1/GrandChild2",
      "/Root/Child2",
      "/Root/Child2/GrandChild3"};
  const std::string miss_path = "/Root/DoesNotExist";

  const int kThreads = 8;
  const int kIters = 2000;
  std::atomic<bool> ok{true};

  auto worker = [&]() {
    for (int it = 0; it < kIters; ++it) {
      for (const auto &sp : hit_paths) {
        const Prim *prim = nullptr;
        std::string err;
        bool found = stage.find_prim_at_path(Path(sp, ""), prim, &err);
        if (!found || prim == nullptr) {
          ok.store(false);
        }
      }
      {
        const Prim *prim = nullptr;
        std::string err;
        bool found = stage.find_prim_at_path(Path(miss_path, ""), prim, &err);
        if (found) {
          ok.store(false);
        }
      }
    }
  };

  std::vector<std::thread> ts;
  ts.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    ts.emplace_back(worker);
  }
  for (auto &t : ts) {
    t.join();
  }

  TEST_CHECK(ok.load());
#else
  // Threads disabled: the const read API is covered by the other Stage tests.
  TEST_CHECK(true);
#endif
}

// N threads each parse the SAME in-memory USDA buffer (read-only) into their
// OWN Stage, concurrently. This exercises process-wide parse state — notably the
// ParserProfiler singleton, which used to mutate a shared std::map on every
// parse. The buffer contains an out-of-order timeSamples block so the parse-time
// TimeSamples finalize (sort) runs too. Must be ThreadSanitizer-clean.
void stage_concurrent_parse_test(void) {
#if defined(LIGHTUSD_ENABLE_THREAD)
  const std::string usda =
      "#usda 1.0\n"
      "def Xform \"Root\"\n"
      "{\n"
      "    double val.timeSamples = {\n"
      "        2: 2.0,\n"
      "        0: 0.0,\n"
      "        1: 1.0,\n"
      "    }\n"
      "    def Xform \"Child\"\n"
      "    {\n"
      "    }\n"
      "}\n";

  const int kThreads = 8;
  std::atomic<int> ok_count{0};

  auto worker = [&]() {
    Stage stage;
    std::string warn, err;
    bool ok = lightusd::legacy::LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
        /* base_dir */ "", &stage, &warn, &err);
    if (ok) {
      ok_count.fetch_add(1);
    }
  };

  std::vector<std::thread> ts;
  ts.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    ts.emplace_back(worker);
  }
  for (auto &t : ts) {
    t.join();
  }

  TEST_CHECK(ok_count.load() == kThreads);
#else
  TEST_CHECK(true);
#endif
}

// A finalized (update()-sorted) TimeSamples must be safe to read from many
// threads: the const accessors (get()/size()) must not mutate it. Build with
// out-of-order samples, finalize once (as the parsers now do), then read
// concurrently. Must be ThreadSanitizer-clean.
void stage_concurrent_timesamples_read_test(void) {
#if defined(LIGHTUSD_ENABLE_THREAD)
  lightusd::legacy::value::TimeSamples ts;
  // Add out of time order so update() actually sorts.
  ts.add_sample<double>(2.0, 20.0);
  ts.add_sample<double>(0.0, 0.0);
  ts.add_sample<double>(1.0, 10.0);
  ts.add_sample<double>(3.0, 30.0);

  // Finalize once, single-threaded (mirrors what the parsers do at load). This
  // sorts unified storage but intentionally leaves _samples unmaterialized, so
  // the threads below race on the first get_samples() materialization — which
  // must be internally guarded.
  ts.update();
  TEST_CHECK(ts.size() == 4);

  const int kThreads = 8;
  const int kIters = 5000;
  std::atomic<bool> ok{true};
  // Start barrier so all threads hit the cold first get_samples() (the lazy
  // materialization) at the same instant. NOTE: this validates functional
  // correctness under concurrency; the acutest binary's in-process TSan does not
  // reliably flag data races (see doc/datarace.md) — the authoritative TSan
  // check for this fix is the standalone harness documented there.
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};

  auto worker = [&]() {
    ready.fetch_add(1);
    while (!go.load(std::memory_order_acquire)) {
      // spin until released
    }
    for (int it = 0; it < kIters; ++it) {
      if (ts.size() != 4) {
        ok.store(false);
      }
      double v = 0.0;
      // Binary scalar fast path.
      if (ts.get<double>(&v, 1.0)) {
        if (v != 10.0) {
          ok.store(false);
        }
      }
      // Generic-samples path: this lazily materializes `_samples` from unified
      // storage on first call (the const-read mutation being guarded). Exercise
      // it concurrently so the materialization race would surface under TSan.
      const std::vector<lightusd::legacy::value::TimeSamples::Sample> &samples = ts.get_samples();
      if (samples.size() != 4) {
        ok.store(false);
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back(worker);
  }
  // Release all workers once they're all spun up.
  while (ready.load() < kThreads) {
  }
  go.store(true, std::memory_order_release);
  for (auto &t : threads) {
    t.join();
  }

  TEST_CHECK(ok.load());
#else
  TEST_CHECK(true);
#endif
}
