// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// USDC structural-limit plumbing.
//
//   A. The FIELDSETS index table has its own bound (max_fieldset_indices),
//      decoupled from max_fields: a flattened production scene has more
//      fieldset indices than fields (Island: 13.4M indices, 9.2M fields,
//      which the old shared max_fields bound of ~10.5M rejected on read-back).
//   B. CompositionOptions::usdc_limits / LayerLoadOptions::usdc_limits reach
//      every crate layer load made by pcp (a referenced .usdc and the root).

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "next/load-usd.hh"
#include "next/pcp/cache.hh"
#include "next/pcp/layer-registry.hh"
#include "next/reader/usda-reader.hh"
#include "next/reader/usdc-reader.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/stage/stage.hh"

using namespace lightusd::next;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::cerr << "FAIL: " << (msg) << "  (line " << __LINE__ << ")\n";  \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

std::string MakeTempDir() {
  std::string base = "/tmp/lightusd_crate_limits_" + std::to_string(getpid());
  std::string rm = "rm -rf '" + base + "'";
  if (std::system(rm.c_str()) != 0) { /* ignore */ }
  if (::mkdir(base.c_str(), 0755) != 0 && errno != EEXIST) {
    std::cerr << "FAIL: could not create temp dir " << base << "\n";
    ++g_failures;
  }
  return base;
}

void RemoveDir(const std::string &dir) {
  std::string rm = "rm -rf '" + dir + "'";
  if (std::system(rm.c_str()) != 0) { /* ignore */ }
}

// Prims whose attributes share names and values, so FIELDS dedups to a handful
// of entries while each spec's fieldset still lists every field: fieldset
// indices outnumber fields, like a large flattened scene.
std::string SourceUSDA() {
  std::string s = "#usda 1.0\n(\n    defaultPrim = \"Root\"\n)\n\n";
  s += "def Xform \"Root\"\n{\n";
  for (int i = 0; i < 64; ++i) {
    s += "    def Mesh \"M" + std::to_string(i) + "\"\n    {\n";
    s += "        float a = 1\n        float b = 2\n        float c = 3\n";
    s += "        int d = 4\n        token e = \"x\"\n";
    s += "    }\n";
  }
  s += "}\n";
  return s;
}

bool LoadsWith(const std::string &path, const CrateLimits &limits,
               std::string *errors = nullptr) {
  USDCLoadOptions opts;
  static_cast<CrateLimits &>(opts.crate_options) = limits;
  USDCLoadResult r = LoadUSDCFromFile(path, opts);
  if (errors) {
    *errors = r.error_summary;
    for (const auto &e : r.errors) *errors += e.message + "\n";
  }
  return r.success;
}

// Smallest value of `field` (in [1, hi]) that still loads.
size_t MinLoadable(const std::string &path, size_t CrateLimits::*field,
                   size_t hi) {
  size_t lo = 1;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    CrateLimits l;
    l.*field = mid;
    if (LoadsWith(path, l)) hi = mid; else lo = mid + 1;
  }
  return lo;
}

}  // namespace

int main() {
  const std::string dir = MakeTempDir();
  const std::string usdc = dir + "/ref.usdc";

  {
    LoadResult src = LoadUSDAFromString(SourceUSDA());
    CHECK(src.success, "source USDA must parse: " + src.error_summary);
    std::string err;
    CHECK(WriteUSDC(src.stage, usdc, &err), "WriteUSDC failed: " + err);
  }

  // A. Separate fieldset-index bound.
  CHECK(LoadsWith(usdc, CrateLimits{}), "default limits must load");
  const size_t min_fields = MinLoadable(usdc, &CrateLimits::max_fields, 1u << 20);
  const size_t min_fsi =
      MinLoadable(usdc, &CrateLimits::max_fieldset_indices, 1u << 20);
  std::cout << "fields=" << min_fields << " fieldset indices=" << min_fsi << "\n";
  CHECK(min_fsi > min_fields,
        "fixture must have more fieldset indices than fields");
  {
    // The Island shape: max_fields below the fieldset-index count must not
    // reject the FIELDSETS table any more.
    CrateLimits l;
    l.max_fields = min_fields;
    CHECK(LoadsWith(usdc, l),
          "fieldset indices above max_fields must load (separate bound)");
  }
  {
    CrateLimits l;
    l.max_fieldset_indices = min_fsi - 1;
    std::string errors;
    CHECK(!LoadsWith(usdc, l, &errors),
          "max_fieldset_indices below the table size must reject");
    CHECK(errors.find("Too many fieldset indices") != std::string::npos,
          "rejection must name the fieldset-index bound: " + errors);
  }

  // B. pcp forwards usdc_limits to crate layer loads.
  {
    pcp::LayerLoadOptions o;
    std::string warn, err;
    CHECK(pcp::LoadLayerFromFile(usdc, &warn, &err, o) != nullptr,
          "LoadLayerFromFile with default limits must load: " + err);
    o.usdc_limits.max_fieldset_indices = min_fsi - 1;
    warn.clear();
    err.clear();
    CHECK(pcp::LoadLayerFromFile(usdc, &warn, &err, o) == nullptr,
          "LoadLayerFromFile must apply LayerLoadOptions::usdc_limits");
  }
  {
    const std::string root = dir + "/root.usda";
    std::ofstream(root) << "#usda 1.0\n\ndef \"A\" (\n"
                           "    references = @./ref.usdc@</Root>\n)\n{\n}\n";

    auto compose = [&](const pcp::CompositionOptions &co, Stage *stage) {
      AssetResolver resolver;
      std::string warn, err;
      return pcp::ComposeStageFromFile(root, resolver, stage, co, &warn, &err);
    };
    pcp::CompositionOptions co;
    Stage ok_stage;
    CHECK(compose(co, &ok_stage), "compose with default limits must succeed");
    CHECK(ok_stage.GetPrimAtPath(std::string("/A/M0")).IsValid(),
          "referenced USDC content must compose in with default limits");

    co.usdc_limits.max_fieldset_indices = min_fsi - 1;
    Stage limited;
    compose(co, &limited);  // the reference is dropped (a warning), not fatal
    CHECK(!limited.GetPrimAtPath(std::string("/A/M0")).IsValid(),
          "CompositionOptions::usdc_limits must reach the referenced USDC load");

    // Root-layer USDC load goes through the same options.
    Stage root_limited;
    AssetResolver resolver;
    std::string warn, err;
    CHECK(!pcp::ComposeStageFromFile(usdc, resolver, &root_limited, co, &warn,
                                     &err),
          "CompositionOptions::usdc_limits must reach the root USDC load");
  }

  RemoveDir(dir);
  if (g_failures) {
    std::cerr << g_failures << " failure(s)\n";
    return 1;
  }
  std::cout << "test_crate_limits: OK\n";
  return 0;
}
