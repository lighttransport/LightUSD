// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// AOUSD supplemental value_resolution: sampled-value oracle.
//
// Translates the expected resolved values from the supplemental corpus's
// value_resolution/tests/test_value_resolution.py (release_dec2025) into
// assertions against next's core resolver (AttributeEval + value clips).
// The corpus load-only check lives in run-aousd-supplemental.py; this binary
// asserts the actual resolved SAMPLE VALUES (bracketing, interpolation,
// LVRPS clip strength, clip time mappings, clip-set ordering).
//
// Usage: test_aousd_value_resolution <supplemental-root>
//   (or AOUSD_CORE_SUPPLEMENTAL_ROOT in the environment)
// Exits 77 (ctest SKIP_RETURN_CODE) when the corpus is unavailable.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/stat.h>

#include "next/eval/attribute-eval.hh"
#include "next/load-usd.hh"
#include "next/pcp/cache.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/stage/stage.hh"

using namespace lightusd::next;

namespace {

bool DirExists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

struct Case {
  std::string dir;  // absolute case directory
  Stage stage;
  std::shared_ptr<ValueClipStageCache> clip_cache;

  bool Open(const std::string& assets_root, const std::string& name) {
    dir = assets_root + "/" + name;
    AssetResolver resolver;
    resolver.SetWorkingDirectory(dir);
    pcp::CompositionOptions opts;
    std::string warn, err;
    if (!pcp::ComposeStageFromFile(dir + "/entry.usd", resolver, &stage, opts,
                                   &warn, &err)) {
      std::fprintf(stderr, "FAIL: cannot compose %s/entry.usd: %s\n",
                   name.c_str(), err.c_str());
      return false;
    }
    clip_cache = std::make_shared<ValueClipStageCache>();
    return true;
  }

  EvalOptions Options(TimeQuery time,
                      TimeInterpolation interp = TimeInterpolation::Linear) {
    EvalOptions opts;
    opts.time = time;
    opts.interp = interp;
    const std::string base = dir;
    opts.clip_stage_loader = [base](const std::string& asset, Stage* out,
                                    std::string* warn, std::string* err) {
      std::string path = asset;
      if (!path.empty() && !AssetResolver::IsAbsolutePath(path)) {
        // Anchor authored-relative clip/manifest paths at the case directory.
        if (path.compare(0, 2, "./") == 0) path = path.substr(2);
        path = base + "/" + path;
      }
      return LoadUSD(path, out, LoadUSDOptions(), warn, err);
    };
    opts.clip_stage_cache = clip_cache;
    return opts;
  }

  EvalResult Eval(const std::string& prim_path, const std::string& attr,
                  const EvalOptions& opts) {
    UsdPrim prim = stage.GetPrimAtPath(prim_path);
    assert(prim.IsValid());
    AttributeEval eval(&stage);
    return eval.EvalWith(prim, attr, opts);
  }
};

double AsDouble(const EvalResult& r) {
  assert(r.success);
  if (const float* f = r.value.as_float()) return *f;
  if (const double* d = r.value.as_double()) return *d;
  assert(false && "resolved value is not float/double");
  return 0.0;
}

bool Near(double a, double b, double eps = 1e-6) {
  return std::fabs(a - b) < eps;
}

#define CHECK_VALUE(c, prim, attr, time, expected)                         \
  do {                                                                     \
    EvalResult r = (c).Eval(prim, attr, (c).Options(TimeQuery(time)));     \
    if (!r.success || !Near(AsDouble(r), expected)) {                      \
      std::fprintf(stderr,                                                 \
                   "FAIL %s%s at t=%g: expected %g got %s%g (%s)\n", prim, \
                   attr, double(time), double(expected),                   \
                   r.success ? "" : "<no value> ", r.success ? AsDouble(r) \
                                                             : 0.0,        \
                   r.error.c_str());                                       \
      return false;                                                        \
    }                                                                      \
  } while (0)

// test_default: authored default beats timeSamples for a DEFAULT-time query.
bool TestDefault(const std::string& assets_root) {
  std::printf("value_resolution/default...\n");
  Case c;
  if (!c.Open(assets_root, "default")) return false;
  EvalResult r = c.Eval("/Root", "root", c.Options(TimeQuery::Default()));
  if (!r.success || !Near(AsDouble(r), 2.0) || r.from_time_sample) {
    std::fprintf(stderr, "FAIL default-time /Root.root: expected 2.0\n");
    return false;
  }
  return true;
}

// test_timesamples: bracketing, linear interpolation, out-of-range clamping,
// default-time fallback process, and Held interpolation.
bool TestTimeSamples(const std::string& assets_root) {
  std::printf("value_resolution/timesamples...\n");
  Case c;
  if (!c.Open(assets_root, "timesamples")) return false;
  // Samples: {1: 5, 30: 10, 40: 15}
  CHECK_VALUE(c, "/Root", "root", 1.0, 5.0);
  CHECK_VALUE(c, "/Root", "root", 40.0, 15.0);
  {
    EvalResult r = c.Eval("/Root", "root", c.Options(TimeQuery(15.0)));
    const double v = AsDouble(r);
    if (!(v > 5.0 && v < 10.0)) {
      std::fprintf(stderr, "FAIL /Root.root t=15: expected (5,10) got %g\n", v);
      return false;
    }
  }
  // Out of range clamps to the boundary samples.
  CHECK_VALUE(c, "/Root", "root", 60.0, 15.0);
  CHECK_VALUE(c, "/Root", "root", 0.5, 5.0);
  // Default-time query on a sampled-only attribute resolves through the
  // FALLBACK process (never through the samples).
  {
    EvalResult r = c.Eval("/Root", "root", c.Options(TimeQuery::Default()));
    if (r.from_time_sample || r.from_default ||
        (r.success && !r.from_schema_fallback)) {
      std::fprintf(stderr,
                   "FAIL default-time /Root.root: expected fallback process\n");
      return false;
    }
  }
  // Held interpolation.
  {
    EvalResult r = c.Eval("/Root", "root",
                          c.Options(TimeQuery(15.0), TimeInterpolation::Held));
    if (!Near(AsDouble(r), 5.0)) {
      std::fprintf(stderr, "FAIL held t=15: expected 5 got %g\n", AsDouble(r));
      return false;
    }
    r = c.Eval("/Root", "root",
               c.Options(TimeQuery(30.0), TimeInterpolation::Held));
    if (!Near(AsDouble(r), 10.0)) {
      std::fprintf(stderr, "FAIL held t=30: expected 10 got %g\n", AsDouble(r));
      return false;
    }
  }
  return true;
}

// test_clip_timings: `times` mapping with a jump discontinuity
// [(0,10),(20,20),(20,10),(40,20)] and out-of-range stage times.
bool TestClipTimings(const std::string& assets_root) {
  std::printf("value_resolution/clip_timings...\n");
  Case c;
  if (!c.Open(assets_root, "clip_timings")) return false;
  CHECK_VALUE(c, "/Model", "size", 0.0, 10.0);
  CHECK_VALUE(c, "/Model", "size", 15.0, 17.5);
  CHECK_VALUE(c, "/Model", "size", 30.0, 15.0);
  CHECK_VALUE(c, "/Model", "size", 40.0, 20.0);
  // Out of the authored times range: boundary stage time -> clip sample clamp.
  CHECK_VALUE(c, "/Model", "size", 50.0, 25.0);
  CHECK_VALUE(c, "/Model", "size", -1.0, 0.0);
  return true;
}

// test_clip_basic: clips beat the reference layer's samples/default (LVRPS).
bool TestClipBasic(const std::string& assets_root) {
  std::printf("value_resolution/clip_basic...\n");
  Case c;
  if (!c.Open(assets_root, "clip_basic")) return false;
  CHECK_VALUE(c, "/Model", "size", 0.0, 0.0);
  CHECK_VALUE(c, "/Model", "size", 5.0, 5.0);
  return true;
}

// test_clip_advanced: local (root layer stack) opinions beat clips; opinions
// from the weaker reference lose to clips.
bool TestClipAdvanced(const std::string& assets_root) {
  std::printf("value_resolution/clip_advanced...\n");
  Case c;
  if (!c.Open(assets_root, "clip_advanced")) return false;
  // Local samples win over the clip's negative values.
  CHECK_VALUE(c, "/Model", "local", 0.0, 0.0);
  CHECK_VALUE(c, "/Model", "local", 5.0, 5.0);
  CHECK_VALUE(c, "/Model", "local", 10.0, 10.0);
  CHECK_VALUE(c, "/Model", "local", 15.0, 15.0);
  CHECK_VALUE(c, "/Model", "local", 20.0, 20.0);
  CHECK_VALUE(c, "/Model", "local", 25.0, 20.0);  // clamp past last sample
  // The `ref` opinions live in the weaker referenced layer: clips win.
  CHECK_VALUE(c, "/Model", "ref", 0.0, 0.0);
  CHECK_VALUE(c, "/Model", "ref", 5.0, -5.0);
  CHECK_VALUE(c, "/Model", "ref", 10.0, -10.0);
  CHECK_VALUE(c, "/Model", "ref", 20.0, -20.0);
  CHECK_VALUE(c, "/Model", "ref", 25.0, -25.0);
  CHECK_VALUE(c, "/Model", "ref", 30.0, -25.0);  // out of clip range: clamp
  return true;
}

// test_clip_sets: clip sets resolve in NAME order (clip_a before clip_b),
// independent of authored dictionary order.
bool TestClipSets(const std::string& assets_root) {
  std::printf("value_resolution/clip_sets...\n");
  Case c;
  if (!c.Open(assets_root, "clip_sets")) return false;
  CHECK_VALUE(c, "/DefaultOrderTest", "attr", 0.0, 10.0);
  CHECK_VALUE(c, "/DefaultOrderTest", "attr", 1.0, 20.0);
  CHECK_VALUE(c, "/DefaultOrderTest", "attr", 2.0, 30.0);
  return true;
}

// test_clip_multi: two clips with active [(0,0),(16,1)] and a times jump
// [(0,0),(16,16),(16,0),(32,16)]; safe_step(16) still samples clip 1.
bool TestClipMulti(const std::string& assets_root) {
  std::printf("value_resolution/clip_multi...\n");
  Case c;
  if (!c.Open(assets_root, "clip_multi")) return false;
  const double kSafeStep = 1e-9;  // corpus timeCode.safe_step epsilon
  CHECK_VALUE(c, "/Model_1", "size", 5.0, -5.0);
  CHECK_VALUE(c, "/Model_1", "size", 10.0, -10.0);
  CHECK_VALUE(c, "/Model_1", "size", 16.0 - kSafeStep, -15.0);
  CHECK_VALUE(c, "/Model_1", "size", 16.0, -23.0);
  CHECK_VALUE(c, "/Model_1", "size", 19.0, -23.0);
  CHECK_VALUE(c, "/Model_1", "size", 22.0, -26.0);
  CHECK_VALUE(c, "/Model_1", "size", 25.0, -29.0);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string root;
  if (argc > 1) {
    root = argv[1];
  } else if (const char* env = std::getenv("AOUSD_CORE_SUPPLEMENTAL_ROOT")) {
    root = env;
  }
  const std::string assets_root = root + "/value_resolution/tests/assets";
  if (root.empty() || !DirExists(assets_root)) {
    std::printf(
        "SKIP: AOUSD supplemental corpus not found (pass the suite root as "
        "argv[1] or set AOUSD_CORE_SUPPLEMENTAL_ROOT)\n");
    return 77;
  }

  bool ok = true;
  ok &= TestDefault(assets_root);
  ok &= TestTimeSamples(assets_root);
  ok &= TestClipTimings(assets_root);
  ok &= TestClipBasic(assets_root);
  ok &= TestClipAdvanced(assets_root);
  ok &= TestClipSets(assets_root);
  ok &= TestClipMulti(assets_root);
  if (!ok) {
    std::fprintf(stderr, "AOUSD value-resolution oracle FAILED.\n");
    return 1;
  }
  std::printf("All AOUSD value-resolution oracle tests passed.\n");
  return 0;
}
