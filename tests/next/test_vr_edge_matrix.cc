// SPDX-License-Identifier: Apache-2.0
// Copyright 2026-Present Light Transport Entertainment Inc.
//
// Value-resolution EDGE-CASE matrix with pxr-baked expectations.
//
// Fixtures live in tests/next/fixtures/vr-edge/<case>/ and the expected
// values in tests/next/generated/vr-edge-expected.inc — both COMMITTED and
// produced by scripts/generate-aousd-vr-edge-cases.py from the PINNED
// OpenUSD 26.05 oracle. Unlike test_aousd_value_resolution (which needs the
// external supplemental corpus), this binary is self-contained and never
// skips.
//
// Usage: test_vr_edge_matrix <repo-root-or-fixture-dir>
//   (defaults to walking upward from cwd until tests/next/fixtures/vr-edge)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "next/eval/attribute-eval.hh"
#include "next/pcp/cache.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/stage/stage.hh"

using namespace lightusd::next;

namespace {

bool DirExists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

struct Expectation {
  const char* case_name;
  const char* prim;
  const char* attr;
  int is_default_time;
  double time;
  int held;
  int kind;  // 0=no value, 1=numeric tuple, 2=string, 3=numeric array
  int num_count;
  std::vector<double> nums;
  const char* str;
};

const std::vector<Expectation> kExpectations = {
#define VR_EDGE_CASE(c, p, a, isdef, t, held, kind, n, s, ...) \
  {c, p, a, isdef, t, held, kind, n, std::vector<double>{__VA_ARGS__}, s},
#include "generated/vr-edge-expected.inc"
#undef VR_EDGE_CASE
};

// Flatten a resolved next Value into doubles. `want` is the expected lane
// count (arrays flatten every element).
bool FlattenNext(const Value& v, size_t want, std::vector<double>* out) {
  out->clear();
  if (v.is_array()) {
    if (const auto* fa = v.as_float_array()) {
      for (float f : *fa) out->push_back(double(f));
      return true;
    }
    if (const auto* da = v.as_double_array()) {
      for (double d : *da) out->push_back(d);
      return true;
    }
    if (const auto* ia = v.as_int_array()) {
      for (int32_t i : *ia) out->push_back(double(i));
      return true;
    }
    if (const auto* i64a = v.as_int64_array()) {
      for (int64_t i : *i64a) out->push_back(double(i));
      return true;
    }
    if (const auto* ua = v.as_uint_array()) {
      for (uint32_t u : *ua) out->push_back(double(u));
      return true;
    }
    return false;
  }
  if (want == 16) {
    if (const double* m = v.as_matrix4d()) {
      out->assign(m, m + 16);
      return true;
    }
    if (const float* mf = v.as_matrix4f()) {
      for (int i = 0; i < 16; ++i) out->push_back(double(mf[i]));
      return true;
    }
    return false;
  }
  // Exact-width accessors first (double-backed values keep full precision).
  if (want == 1) {
    if (const bool* b = v.as_bool()) { out->push_back(*b ? 1.0 : 0.0); return true; }
    if (const uint8_t* uc = v.as_uchar()) { out->push_back(double(*uc)); return true; }
    if (const int32_t* i = v.as_int()) { out->push_back(double(*i)); return true; }
    if (const uint32_t* u = v.as_uint()) { out->push_back(double(*u)); return true; }
    if (const int64_t* i = v.as_int64()) { out->push_back(double(*i)); return true; }
    if (const uint64_t* u = v.as_uint64()) { out->push_back(double(*u)); return true; }
    if (const double* d = v.as_double()) { out->push_back(*d); return true; }
    if (const float* f = v.as_float()) { out->push_back(double(*f)); return true; }
  } else if (want == 2) {
    if (const double* d = v.as_double2()) { out->assign(d, d + 2); return true; }
  } else if (want == 3) {
    if (const double* d = v.as_double3()) { out->assign(d, d + 3); return true; }
  } else if (want == 4) {
    if (const double* d = v.as_double4()) { out->assign(d, d + 4); return true; }
  }
  // Converting fallback: covers half-backed lanes, roles (color/normal/
  // texCoord/quat), and narrowing.
  float lanes[4];
  bool ok = false;
  switch (want) {
    case 1: ok = v.to_float(lanes); break;
    case 2: ok = v.to_float2(lanes); break;
    case 3: ok = v.to_float3(lanes); break;
    case 4: ok = v.to_float4(lanes); break;
    default: break;
  }
  if (!ok) return false;
  for (size_t i = 0; i < want; ++i) out->push_back(double(lanes[i]));
  return true;
}

struct CaseStage {
  Stage stage;
  bool ok = false;
};

}  // namespace

int main(int argc, char** argv) {
  std::string fixtures;
  if (argc > 1) {
    fixtures = argv[1];
    if (DirExists(fixtures + "/tests/next/fixtures/vr-edge")) {
      fixtures += "/tests/next/fixtures/vr-edge";
    }
  } else {
    std::string base = ".";
    for (int up = 0; up < 6; ++up) {
      if (DirExists(base + "/tests/next/fixtures/vr-edge")) {
        fixtures = base + "/tests/next/fixtures/vr-edge";
        break;
      }
      base += "/..";
    }
  }
  if (fixtures.empty() || !DirExists(fixtures)) {
    std::fprintf(stderr,
                 "FAIL: cannot locate tests/next/fixtures/vr-edge "
                 "(pass the repo root as argv[1])\n");
    return 1;
  }

  // Compose each distinct case once.
  std::vector<std::pair<std::string, CaseStage>> stages;
  auto stage_for = [&](const std::string& name) -> CaseStage& {
    for (auto& e : stages) {
      if (e.first == name) return e.second;
    }
    stages.emplace_back(name, CaseStage());
    CaseStage& cs = stages.back().second;
    AssetResolver resolver;
    resolver.SetWorkingDirectory(fixtures + "/" + name);
    pcp::CompositionOptions opts;
    std::string warn, err;
    cs.ok = pcp::ComposeStageFromFile(fixtures + "/" + name + "/root.usda",
                                      resolver, &cs.stage, opts, &warn, &err);
    if (!cs.ok) {
      std::fprintf(stderr, "FAIL: cannot compose %s: %s\n", name.c_str(),
                   err.c_str());
    }
    return cs;
  };

  int failures = 0;
  int checked = 0;
  for (const Expectation& e : kExpectations) {
    CaseStage& cs = stage_for(e.case_name);
    if (!cs.ok) {
      failures++;
      continue;
    }
    UsdPrim prim = cs.stage.GetPrimAtPath(e.prim);
    if (!prim.IsValid()) {
      std::fprintf(stderr, "FAIL %s %s: prim missing\n", e.case_name, e.prim);
      failures++;
      continue;
    }
    EvalOptions opts;
    opts.time = e.is_default_time ? TimeQuery::Default() : TimeQuery(e.time);
    opts.interp =
        e.held ? TimeInterpolation::Held : TimeInterpolation::Linear;
    AttributeEval eval(&cs.stage);
    EvalResult r = eval.EvalWith(prim, e.attr, opts);
    checked++;

    auto fail = [&](const char* what) {
      std::fprintf(stderr, "FAIL %s %s.%s t=%s%g interp=%s: %s (%s)\n",
                   e.case_name, e.prim, e.attr,
                   e.is_default_time ? "default/" : "", e.time,
                   e.held ? "held" : "linear", what, r.error.c_str());
      failures++;
    };

    if (e.kind == 0) {
      // pxr resolved NO value (blocked or absent).
      if (r.success) fail("expected no value, got one");
      continue;
    }
    if (!r.success) {
      fail("expected a value, resolver returned none");
      continue;
    }
    if (e.kind == 2) {
      const std::string* s = r.value.as_string();
      if (!s) s = r.value.as_token();
      if (!s || *s != e.str) fail("string mismatch");
      continue;
    }
    // kind 1 (tuple) or 3 (array): compare doubles.
    std::vector<double> got;
    const size_t want =
        (e.kind == 3) ? size_t(e.num_count) : size_t(e.num_count);
    if (!FlattenNext(r.value, e.kind == 3 ? 0 : want, &got)) {
      fail("cannot flatten resolved value to numbers");
      continue;
    }
    if (got.size() != e.nums.size()) {
      std::fprintf(stderr,
                   "FAIL %s %s.%s: lane count %zu != expected %zu\n",
                   e.case_name, e.prim, e.attr, got.size(), e.nums.size());
      failures++;
      continue;
    }
    // half-backed lanes only carry ~1e-3 of precision.
    const bool is_half = (std::strcmp(e.attr, "h") == 0);
    const double eps = is_half ? 2e-3 : 1e-5;
    for (size_t i = 0; i < got.size(); ++i) {
      if (std::fabs(got[i] - e.nums[i]) > eps) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "lane %zu: got %.9g expected %.9g",
                      i, got[i], e.nums[i]);
        fail(buf);
        break;
      }
    }
  }

  std::printf("vr-edge matrix: %d expectations checked, %d failed\n", checked,
              failures);
  return failures == 0 ? 0 : 1;
}
