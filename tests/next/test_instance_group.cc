// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP instancing group-assignment performance + correctness
//
// Guards pcp::AssignPrototype (RegisterInstance) against an O(N^2) regression:
// a single prototype group with N instanceable members used to cost O(N^2)
// string compares (a linear std::find over the growing group vector per
// instance). A forest-style scene (tens of thousands of instances of one
// asset) is the real-world shape: build N instanceable prims all referencing
// the same </Model>, BuildStage, and check the grouping + wall time.

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "next/layer/layer.hh"
#include "next/layer/asset-anchor.hh"
#include "next/pcp/cache.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/stage/stage.hh"
#include "next/types/value.hh"

using namespace lightusd::next;

static int g_failures = 0;
#define IG_CHECK(cond, msg)                                                 \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::fprintf(stderr, "IG_CHECK FAILED: %s @ %s:%d\n", msg,            \
                   __FILE__, static_cast<int>(__LINE__));                    \
      ++g_failures;                                                         \
    }                                                                       \
  } while (0)

// A root layer with N instanceable prims (/Inst_<i>), each referencing the
// shared </Model> (Mesh) and flagged instanceable. They all resolve to one
// instance key -> one prototype group of N members.
static std::shared_ptr<Layer> BuildForestLayer(int n_instances) {
  Layer layer;
  LayerBuilder lb(layer);

  // Shared prototype content.
  lb.begin_prim("Model", "Mesh");
  lb.add_property("size", Value::MakeFloat3(1.0f, 2.0f, 3.0f));
  lb.end_prim();  // Model

  // N instances of it.
  for (int i = 0; i < n_instances; ++i) {
    const std::string name = "Inst_" + std::to_string(i);
    lb.begin_prim(name, "");
    lb.current()->meta().references.push_back("</Model>");
    lb.current()->meta().instanceable = true;
    lb.end_prim();
  }

  lb.finalize();
  return std::make_shared<Layer>(std::move(layer));
}

static void test_large_instance_group() {
  const int kInstances = 40000;
  const auto t0 = std::chrono::steady_clock::now();

  AssetResolver resolver;
  auto root = BuildForestLayer(kInstances);

  auto opened = pcp::Cache::Open(resolver, root);
  IG_CHECK(opened.has_value(), "Cache::Open failed");
  pcp::Cache cache = std::move(*opened);

  lightusd::next::Stage stage;
  std::string warn, err;
  const auto t1 = std::chrono::steady_clock::now();
  IG_CHECK(cache.BuildStage(&stage, &warn, &err), "BuildStage failed");
  const auto t_build = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t1).count();
  const auto t_total = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();

  // Correctness: every instance collapses to ONE prototype group.
  IG_CHECK(cache.PrototypeCount() == 1, "expected exactly one prototype group");
  const std::vector<Path> protos = cache.GetPrototypePaths();
  IG_CHECK(protos.size() == 1, "expected one prototype path");
  const Path &proto = protos.front();
  const std::vector<Path> members = cache.GetInstancesForPrototype(proto);
  IG_CHECK(static_cast<int>(members.size()) == kInstances,
           "group did not collect all N members");
  // The prototype is its own group's first member; every other is an instance.
  IG_CHECK(!cache.IsInstance(proto), "prototype must not be an instance");
  IG_CHECK(cache.IsInstance(Path("/Inst_1")), "member must be an instance");

  // Regression gate: the O(N) build is a small fraction of this bound, while an
  // O(N^2) regression at N=40000 (~1.6e9 compares) is many times over it.
  IG_CHECK(t_build < 6000, "BuildStage too slow (O(N^2) regression?)");

  std::cout << "  large instance group: N=" << kInstances
            << " BuildStage=" << t_build << "ms total=" << t_total << "ms"
            << std::endl;
  std::cout << "  OK" << std::endl;
}

int main() {
  test_large_instance_group();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d IG_CHECK(s) failed\n", g_failures);
    return 1;
  }
  return 0;
}
