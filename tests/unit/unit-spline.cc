#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-spline.h"
#include "lightusd.hh"
#include "tydra/attribute-eval.hh"
#include "spline-binary.hh"
#include "primvar.hh"
#include "usdc-writer.hh"

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

static bool LoadStage(const std::string &usda, Stage *stage) {
  std::string warn, err;
  bool ok = lightusd::legacy::LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(), "mem.usda",
      stage, &warn, &err);
  if (!ok) {
    TEST_MSG("USDA parse failed: %s", err.c_str());
  }
  return ok;
}

static bool EvalF(Stage &stage, const char *primpath, const char *attr,
                  double t, float *out) {
  auto pr = stage.GetPrimAtPath(Path(primpath, ""));
  if (!pr || !pr.value()) return false;
  lightusd::legacy::tydra::TerminalAttributeValue tav;
  std::string err;
  if (!lightusd::legacy::tydra::EvaluateAttribute(stage, *pr.value(), attr, &tav, &err, t)) {
    return false;
  }
  const float *f = tav.as<float>();
  if (!f) return false;
  *out = *f;
  return true;
}

void spline_usda_eval_test(void) {
  const std::string usda =
      "#usda 1.0\n"
      "\n"
      "def Xform \"W\"\n"
      "{\n"
      "    float lin.spline = {\n"
      "        0: 0; post linear,\n"
      "        10: 10,\n"
      "    }\n"
      "    float hld.spline = {\n"
      "        0: 0; post held,\n"
      "        10: 10,\n"
      "    }\n"
      "    float bez.spline = {\n"
      "        bezier,\n"
      "        0: 0; post curve (3.333, 1),\n"
      "        10: 10; pre (3.333, 1),\n"
      "    }\n"
      "}\n";

  Stage stage;
  TEST_CHECK(LoadStage(usda, &stage));

  float v = 0.0f;

  // Linear interpolation between (0,0) and (10,10).
  TEST_CHECK(EvalF(stage, "/W", "lin", 0.0, &v) && std::fabs(v - 0.0f) < 1e-3f);
  TEST_CHECK(EvalF(stage, "/W", "lin", 5.0, &v) && std::fabs(v - 5.0f) < 1e-3f);
  TEST_CHECK(EvalF(stage, "/W", "lin", 10.0, &v) && std::fabs(v - 10.0f) < 1e-3f);

  // Held-extrapolation outside the knot range (default extrapolation).
  TEST_CHECK(EvalF(stage, "/W", "lin", -5.0, &v) && std::fabs(v - 0.0f) < 1e-3f);
  TEST_CHECK(EvalF(stage, "/W", "lin", 15.0, &v) && std::fabs(v - 10.0f) < 1e-3f);

  // Held interpolation: holds the previous knot's value across the segment.
  TEST_CHECK(EvalF(stage, "/W", "hld", 5.0, &v) && std::fabs(v - 0.0f) < 1e-3f);
  TEST_CHECK(EvalF(stage, "/W", "hld", 9.99, &v) && std::fabs(v - 0.0f) < 1e-3f);
  TEST_CHECK(EvalF(stage, "/W", "hld", 10.0, &v) && std::fabs(v - 10.0f) < 1e-3f);

  // Bezier with unit slope tangents (1/3 width) is monotonic and ~linear.
  float b2 = 0, b5 = 0, b8 = 0;
  TEST_CHECK(EvalF(stage, "/W", "bez", 2.0, &b2));
  TEST_CHECK(EvalF(stage, "/W", "bez", 5.0, &b5));
  TEST_CHECK(EvalF(stage, "/W", "bez", 8.0, &b8));
  TEST_CHECK(b2 < b5 && b5 < b8);          // monotonic increasing
  TEST_CHECK(b5 > 0.0f && b5 < 10.0f);     // within range
  TEST_CHECK(std::fabs(b5 - 5.0f) < 1.0f); // near the midpoint
}

void spline_binary_roundtrip_test(void) {
  using SD = primvar::PrimVar::SplineData;
  using SK = primvar::PrimVar::SplineKnotData;

  // float bezier spline with two knots, tangents, a dual-valued knot, and
  // sloped post-extrapolation.
  SD sd;
  sd.curveType = 0;            // bezier
  sd.preExtrapolation = 1;     // held
  sd.postExtrapolation = 3;    // sloped
  sd.postExtrapolationSlope = 0.5;
  {
    SK k0;
    k0.time = 0.0;
    k0.val = lightusd::legacy::value::Value(0.0f);
    k0.interpolationMode = 3;  // curve
    k0.postTangentWidth = 1.0;
    k0.postTangentSlope = 2.0;
    sd.knots.push_back(k0);

    SK k1;
    k1.time = 10.0;
    k1.val = lightusd::legacy::value::Value(10.0f);
    k1.preValue = lightusd::legacy::value::Value(9.0f);
    k1.hasDualValue = true;
    k1.interpolationMode = 3;
    k1.preTangentWidth = 1.0;
    k1.preTangentSlope = 2.0;
    sd.knots.push_back(k1);
  }

  std::vector<uint8_t> blob;
  std::string err;
  TEST_CHECK_(EncodeSplineToBinary(sd, &blob, &err), "encode failed: %s",
              err.c_str());

  SD out;
  TEST_CHECK_(DecodeSplineFromBinary(blob.data(), blob.size(), &out, &err),
              "decode failed: %s", err.c_str());

  TEST_CHECK(out.curveType == 0);
  TEST_CHECK(out.preExtrapolation == 1);
  TEST_CHECK(out.postExtrapolation == 3);
  TEST_CHECK(std::fabs(out.postExtrapolationSlope - 0.5) < 1e-12);
  TEST_CHECK(out.knots.size() == 2);
  if (out.knots.size() == 2) {
    TEST_CHECK(std::fabs(out.knots[0].time - 0.0) < 1e-12);
    auto v0 = out.knots[0].val.get_value<float>();
    TEST_CHECK(v0 && std::fabs(v0.value() - 0.0f) < 1e-6f);
    TEST_CHECK(out.knots[0].interpolationMode == 3);
    TEST_CHECK(std::fabs(out.knots[0].postTangentSlope - 2.0) < 1e-6);
    TEST_CHECK(out.knots[1].hasDualValue);
    auto pv = out.knots[1].preValue.get_value<float>();
    TEST_CHECK(pv && std::fabs(pv.value() - 9.0f) < 1e-6f);
  }

  // double spline with inner loops, hermite, round-trips.
  SD sd2;
  sd2.curveType = 1;  // hermite
  sd2.hasLoop = true;
  sd2.loopProtoStart = 1.0;
  sd2.loopProtoEnd = 5.0;
  sd2.loopNumPreLoops = 2;
  sd2.loopNumPostLoops = 3;
  sd2.loopValueOffset = 1.5;
  {
    SK k;
    k.time = 1.0;
    k.val = lightusd::legacy::value::Value(3.0);  // double
    k.interpolationMode = 2;    // linear
    sd2.knots.push_back(k);
  }
  std::vector<uint8_t> blob2;
  TEST_CHECK(EncodeSplineToBinary(sd2, &blob2, &err));
  SD out2;
  TEST_CHECK(DecodeSplineFromBinary(blob2.data(), blob2.size(), &out2, &err));
  TEST_CHECK(out2.curveType == 1);
  TEST_CHECK(out2.hasLoop);
  TEST_CHECK(out2.loopNumPreLoops == 2 && out2.loopNumPostLoops == 3);
  TEST_CHECK(std::fabs(out2.loopValueOffset - 1.5) < 1e-12);
  TEST_CHECK(out2.knots.size() == 1);
  if (out2.knots.size() == 1) {
    auto d = out2.knots[0].val.get_value<double>();
    TEST_CHECK(d && std::fabs(d.value() - 3.0) < 1e-12);
  }
}

void spline_binary_rejects_huge_knot_count_test(void) {
  // Header says version 1, double-valued bezier spline, followed by a forged
  // knot count and no knot payload. Decode must reject before reserving.
  std::vector<uint8_t> blob;
  blob.push_back(uint8_t(1 | (1 << 4)));  // version=1, descriptor=double
  blob.push_back(0);                      // no extrapolation, no loops
  const uint32_t count = (std::numeric_limits<uint32_t>::max)();
  const size_t off = blob.size();
  blob.resize(off + sizeof(count));
  std::memcpy(blob.data() + off, &count, sizeof(count));

  primvar::PrimVar::SplineData out;
  std::string err;
  TEST_CHECK(!DecodeSplineFromBinary(blob.data(), blob.size(), &out, &err));
  TEST_CHECK(err.find("knot count") != std::string::npos);
  TEST_CHECK(out.knots.empty());
}

void spline_crate_roundtrip_test(void) {
  // Author a spline in USDA, save to USDC (exercises crate-59 write), reload the
  // USDC (exercises crate-59 read), and confirm evaluation matches.
  const std::string usda =
      "#usda 1.0\n"
      "\n"
      "def Xform \"W\"\n"
      "{\n"
      "    float lin.spline = {\n"
      "        0: 0; post linear,\n"
      "        10: 10,\n"
      "    }\n"
      "    float bez.spline = {\n"
      "        bezier,\n"
      "        0: 0; post curve (3.333, 1),\n"
      "        10: 10; pre (3.333, 1),\n"
      "    }\n"
      "}\n";

  Stage stage;
  TEST_CHECK(LoadStage(usda, &stage));

  std::vector<uint8_t> usdc;
  std::string w, e;
  bool sret = lightusd::legacy::usdc::SaveAsUSDCToMemory(stage, &usdc, &w, &e);
  TEST_CHECK_(sret, "SaveAsUSDCToMemory failed: %s", e.c_str());
  if (!sret) return;
  TEST_CHECK(!usdc.empty());

  // Boot header byte 9 = version minor. A TsSpline value must bump the emitted
  // crate version to >= 0.12.0.
  TEST_CHECK(usdc.size() > 10);
  TEST_CHECK_(usdc[9] >= 12, "crate version minor = %d, expected >= 12", usdc[9]);

  Stage stage2;
  std::string w2, e2;
  bool lret = lightusd::legacy::LoadUSDCFromMemory(usdc.data(), usdc.size(), "mem.usdc",
                                           &stage2, &w2, &e2);
  TEST_CHECK_(lret, "LoadUSDCFromMemory failed: %s", e2.c_str());
  if (!lret) return;

  float v = 0.0f;
  TEST_CHECK(EvalF(stage2, "/W", "lin", 0.0, &v) && std::fabs(v - 0.0f) < 1e-3f);
  TEST_CHECK(EvalF(stage2, "/W", "lin", 5.0, &v) && std::fabs(v - 5.0f) < 1e-3f);
  TEST_CHECK(EvalF(stage2, "/W", "lin", 10.0, &v) &&
             std::fabs(v - 10.0f) < 1e-3f);

  float b5 = 0.0f;
  TEST_CHECK(EvalF(stage2, "/W", "bez", 5.0, &b5));
  TEST_CHECK(b5 > 0.0f && b5 < 10.0f);  // bezier stays in range after roundtrip
}

void spline_usda_roundtrip_test(void) {
  // Parse a spline, export to USDA, and confirm the `.spline` block re-emits
  // and re-parses with the same evaluated values.
  const std::string usda =
      "#usda 1.0\n"
      "\n"
      "def Xform \"W\"\n"
      "{\n"
      "    double s.spline = {\n"
      "        0: 0; post linear,\n"
      "        4: 8,\n"
      "    }\n"
      "}\n";

  Stage stage;
  TEST_CHECK(LoadStage(usda, &stage));

  std::string exported = stage.ExportToString();
  TEST_CHECK_(exported.find(".spline") != std::string::npos,
              "exported USDA missing .spline. Output:\n%s", exported.c_str());

  // Re-parse and check the value still interpolates (linear: t=2 -> 4).
  Stage stage2;
  std::string warn, err;
  bool ok = lightusd::legacy::LoadUSDFromMemory(
      reinterpret_cast<const uint8_t *>(exported.data()), exported.size(),
      "mem2.usda", &stage2, &warn, &err);
  TEST_CHECK_(ok, "re-parse of exported spline failed: %s\n%s", err.c_str(),
              exported.c_str());
  if (!ok) return;

  auto pr = stage2.GetPrimAtPath(Path("/W", ""));
  TEST_CHECK(bool(pr));
  if (pr && pr.value()) {
    lightusd::legacy::tydra::TerminalAttributeValue tav;
    std::string e2;
    if (lightusd::legacy::tydra::EvaluateAttribute(stage2, *pr.value(), "s", &tav, &e2, 2.0)) {
      const double *d = tav.as<double>();
      TEST_CHECK(d && std::fabs(*d - 4.0) < 1e-6);
    } else {
      TEST_CHECK_(false, "eval after roundtrip failed: %s", e2.c_str());
    }
  }
}

// ---------------------------------------------------------------------------
// Per-segment interpolation modes (held / linear / curve / none) + dual value.
// ---------------------------------------------------------------------------
void spline_interpolation_modes_test(void) {
  const std::string usda =
      "#usda 1.0\n"
      "\n"
      "def Xform \"W\"\n"
      "{\n"
      "    float held.spline = { 0: 0; post held, 10: 100, }\n"
      "    float lin.spline = { 0: 0; post linear, 10: 100, }\n"
      "    float none.spline = { 0: 0; post none, 10: 100, }\n"
      "    float multi.spline = { 0: 0; post linear, 5: 50; post linear, 10: 0, }\n"
      "}\n";
  Stage stage;
  TEST_CHECK(LoadStage(usda, &stage));

  float v = 0.0f;
  // held: holds k0 across the segment
  TEST_CHECK(EvalF(stage, "/W", "held", 5.0, &v) && std::fabs(v - 0.0f) < 1e-3f);
  TEST_CHECK(EvalF(stage, "/W", "held", 10.0, &v) && std::fabs(v - 100.0f) < 1e-3f);

  // linear: midpoint == average
  TEST_CHECK(EvalF(stage, "/W", "lin", 5.0, &v) && std::fabs(v - 50.0f) < 1e-3f);
  TEST_CHECK(EvalF(stage, "/W", "lin", 2.5, &v) && std::fabs(v - 25.0f) < 1e-3f);

  // none: value-block in the segment -> evaluator returns no value, falls back
  // to nearest authored knot. Either way it is one of the knot values.
  TEST_CHECK(EvalF(stage, "/W", "none", 5.0, &v));
  TEST_CHECK(std::fabs(v - 0.0f) < 1e-3f || std::fabs(v - 100.0f) < 1e-3f);

  // multi-knot piecewise linear: peak at t=5, back down.
  TEST_CHECK(EvalF(stage, "/W", "multi", 2.5, &v) && std::fabs(v - 25.0f) < 1e-3f);
  TEST_CHECK(EvalF(stage, "/W", "multi", 5.0, &v) && std::fabs(v - 50.0f) < 1e-3f);
  TEST_CHECK(EvalF(stage, "/W", "multi", 7.5, &v) && std::fabs(v - 25.0f) < 1e-3f);
}

// ---------------------------------------------------------------------------
// Extrapolation modes (held / sloped / none).
// ---------------------------------------------------------------------------
void spline_extrapolation_modes_test(void) {
  const std::string usda =
      "#usda 1.0\n"
      "\n"
      "def Xform \"W\"\n"
      "{\n"
      "    double held.spline = { pre: held, post: held, 0: 0; post linear, 10: 10, }\n"
      "    double slope.spline = { pre: sloped(2.0), post: sloped(2.0), 0: 0; post linear, 10: 10, }\n"
      "    double none.spline = { pre: none, post: none, 0: 0; post linear, 10: 10, }\n"
      "}\n";
  Stage stage;
  TEST_CHECK(LoadStage(usda, &stage));

  auto evalD = [&](const char *attr, double t, double *out) -> bool {
    auto pr = stage.GetPrimAtPath(Path("/W", ""));
    if (!pr || !pr.value()) return false;
    lightusd::legacy::tydra::TerminalAttributeValue tav;
    std::string e;
    if (!lightusd::legacy::tydra::EvaluateAttribute(stage, *pr.value(), attr, &tav, &e, t)) return false;
    const double *d = tav.as<double>();
    if (!d) return false;
    *out = *d;
    return true;
  };

  double v = 0.0;
  // held extrapolation clamps to edge knot values.
  TEST_CHECK(evalD("held", -5.0, &v) && std::fabs(v - 0.0) < 1e-9);
  TEST_CHECK(evalD("held", 15.0, &v) && std::fabs(v - 10.0) < 1e-9);

  // sloped extrapolation projects with the given slope.
  TEST_CHECK(evalD("slope", -5.0, &v) && std::fabs(v - (0.0 + 2.0 * -5.0)) < 1e-9);
  TEST_CHECK(evalD("slope", 15.0, &v) && std::fabs(v - (10.0 + 2.0 * 5.0)) < 1e-9);

  // none extrapolation: interior still works.
  TEST_CHECK(evalD("none", 5.0, &v) && std::fabs(v - 5.0) < 1e-9);
}

// ---------------------------------------------------------------------------
// Binary codec: value types + features round-trip (encode -> decode).
// ---------------------------------------------------------------------------
void spline_binary_types_test(void) {
  using SD = primvar::PrimVar::SplineData;
  using SK = primvar::PrimVar::SplineKnotData;

  // half-typed spline.
  {
    SD sd;
    sd.curveType = 0;
    SK k0; k0.time = 0.0; k0.val = lightusd::legacy::value::Value(lightusd::legacy::value::float_to_half_full(1.5f));
    k0.interpolationMode = 2;  // linear
    sd.knots.push_back(k0);
    SK k1; k1.time = 4.0; k1.val = lightusd::legacy::value::Value(lightusd::legacy::value::float_to_half_full(3.5f));
    sd.knots.push_back(k1);

    std::vector<uint8_t> blob;
    std::string err;
    TEST_CHECK(EncodeSplineToBinary(sd, &blob, &err));
    SD out;
    TEST_CHECK(DecodeSplineFromBinary(blob.data(), blob.size(), &out, &err));
    TEST_CHECK(out.knots.size() == 2);
    if (out.knots.size() == 2) {
      auto h = out.knots[0].val.get_value<lightusd::legacy::value::half>();
      TEST_CHECK(h && std::fabs(lightusd::legacy::value::half_to_float(h.value()) - 1.5f) < 1e-2f);
      TEST_CHECK(out.knots[0].interpolationMode == 2);
    }
  }

  // hermite curve + all extrapolation enum values round-trip.
  for (int mode = 0; mode <= 6; mode++) {
    SD sd;
    sd.curveType = 1;  // hermite
    sd.preExtrapolation = mode;
    sd.postExtrapolation = mode;
    sd.preExtrapolationSlope = 1.25;
    sd.postExtrapolationSlope = -0.75;
    if (mode >= 4) {  // loop modes
      sd.hasLoop = true;
      sd.loopProtoStart = 0.0;
      sd.loopProtoEnd = 2.0;
      sd.loopNumPreLoops = 1;
      sd.loopNumPostLoops = 1;
      sd.loopValueOffset = 0.5;
    }
    SK k; k.time = 0.0; k.val = lightusd::legacy::value::Value(1.0); k.interpolationMode = 3;
    k.postTangentSlope = 2.0;
    sd.knots.push_back(k);

    std::vector<uint8_t> blob;
    std::string err;
    TEST_CHECK_(EncodeSplineToBinary(sd, &blob, &err), "encode mode %d: %s",
                mode, err.c_str());
    SD out;
    TEST_CHECK(DecodeSplineFromBinary(blob.data(), blob.size(), &out, &err));
    TEST_CHECK(out.curveType == 1);
    TEST_CHECK(out.preExtrapolation == mode);
    TEST_CHECK(out.postExtrapolation == mode);
    if (mode >= 4) {
      TEST_CHECK(out.hasLoop);
      TEST_CHECK(out.loopNumPreLoops == 1 && out.loopNumPostLoops == 1);
    }
    TEST_CHECK(out.knots.size() == 1);
    if (!out.knots.empty()) {
      TEST_CHECK(std::fabs(out.knots[0].postTangentSlope - 2.0) < 1e-9);
    }
  }

  // Empty spline (no knots) encodes/decodes without error.
  {
    SD sd;
    std::vector<uint8_t> blob;
    std::string err;
    TEST_CHECK(EncodeSplineToBinary(sd, &blob, &err));
    SD out;
    TEST_CHECK(DecodeSplineFromBinary(blob.data(), blob.size(), &out, &err));
    TEST_CHECK(out.knots.empty());
  }

  // Truncated blob fails gracefully (no crash).
  {
    std::vector<uint8_t> truncated = {0x01};  // partial header
    SD out;
    std::string err;
    TEST_CHECK(!DecodeSplineFromBinary(truncated.data(), truncated.size(), &out, &err));
  }
}

// ---------------------------------------------------------------------------
// Cross-tool: read an OpenUSD-authored crate (type 59) and evaluate a spline.
// Fixture generated by `usdcat` (crate 0.12.0).
// ---------------------------------------------------------------------------
namespace {
std::string SplineFixture(const std::string &rel) {
  if (const char *root = std::getenv("LIGHTUSD_TEST_FIXTURE_DIR")) {
    if (*root != '\0') {
      const std::string candidate = std::string(root) + "/" + rel;
      std::ifstream f(candidate, std::ios::binary);
      if (f.good()) return candidate;
    }
  }

  const char *prefixes[] = {"", "../", "../../"};
  for (const char *p : prefixes) {
    std::string cand = std::string(p) + rel;
    std::ifstream f(cand, std::ios::binary);
    if (f.good()) return cand;
  }
  return rel;
}
}  // namespace

void spline_openusd_crate_read_test(void) {
  const std::string path =
      SplineFixture("tests/unit/fixtures/openusd/spline_openusd.usdc");
  std::ifstream probe(path, std::ios::binary);
  if (!probe.good()) {
    TEST_MSG("fixture not found: %s (skipping)", path.c_str());
    return;
  }

  Stage stage;
  std::string warn, err;
  bool ok = lightusd::legacy::LoadUSDCFromFile(path, &stage, &warn, &err);
  TEST_CHECK_(ok, "LoadUSDCFromFile(%s) failed: %s", path.c_str(), err.c_str());
  if (!ok) return;

  // linear spline: { 0:0; post linear, 10:100 } -> 50 at t=5.
  float v = 0.0f;
  TEST_CHECK(EvalF(stage, "/Splines", "linear", 5.0, &v) &&
             std::fabs(v - 50.0f) < 1e-2f);

  // bezier spline stays in range.
  float b = 0.0f;
  TEST_CHECK(EvalF(stage, "/Splines", "bezier", 5.0, &b));
  TEST_CHECK(b > 0.0f && b < 10.0f);

  // held spline (double-typed) holds the first knot across the segment.
  {
    auto pr = stage.GetPrimAtPath(Path("/Splines", ""));
    TEST_CHECK(bool(pr));
    if (pr && pr.value()) {
      lightusd::legacy::tydra::TerminalAttributeValue tav;
      std::string e2;
      bool ev =
          lightusd::legacy::tydra::EvaluateAttribute(stage, *pr.value(), "held", &tav, &e2, 5.0);
      TEST_CHECK_(ev, "held eval failed: %s", e2.c_str());
      const double *d = tav.as<double>();
      TEST_CHECK(d && std::fabs(*d - 0.0) < 1e-9);
    }
  }
}

void spline_tangent_algorithm_crate_013_test(void) {
  // Direct codec check: a knot with a non-None tangent algorithm forces spline
  // binary version 2 (extra per-knot algorithmByte), which is the sole trigger
  // for crate version 0.13.0.
  using SD = primvar::PrimVar::SplineData;
  {
    SD sd;
    sd.curveType = 0;  // bezier
    primvar::PrimVar::SplineKnotData k0;
    k0.time = 0.0;
    k0.val = lightusd::legacy::value::Value(0.0f);
    k0.postTangentAlgorithm = 2;  // AutoEase
    sd.knots.push_back(k0);
    primvar::PrimVar::SplineKnotData k1;
    k1.time = 10.0;
    k1.val = lightusd::legacy::value::Value(10.0f);
    k1.preTangentAlgorithm = 2;  // AutoEase
    sd.knots.push_back(k1);

    TEST_CHECK(SplineBinaryFormatVersion(sd) == 2);

    std::vector<uint8_t> blob;
    std::string e;
    TEST_CHECK_(EncodeSplineToBinary(sd, &blob, &e), "encode failed: %s",
                e.c_str());
    // Header byte 1 low nibble = binary format version.
    TEST_CHECK(!blob.empty());
    TEST_CHECK_((blob[0] & 0x0f) == 2, "spline binary version = %d, want 2",
                blob[0] & 0x0f);

    SD decoded;
    TEST_CHECK(DecodeSplineFromBinary(blob.data(), blob.size(), &decoded, &e));
    TEST_CHECK(decoded.knots.size() == 2);
    if (decoded.knots.size() == 2) {
      TEST_CHECK(decoded.knots[0].postTangentAlgorithm == 2);
      TEST_CHECK(decoded.knots[1].preTangentAlgorithm == 2);
    }

    // A plain spline (no algorithms) must stay version 1.
    SD plain;
    plain.knots.push_back(k0);
    plain.knots[0].postTangentAlgorithm = 0;
    TEST_CHECK(SplineBinaryFormatVersion(plain) == 1);
  }

  // End-to-end: USDA with an autoEase tangent must write a crate 0.13.0 file
  // and round-trip the algorithm back through USDA.
  const std::string usda =
      "#usda 1.0\n"
      "\n"
      "def Xform \"W\"\n"
      "{\n"
      "    float bez.spline = {\n"
      "        bezier,\n"
      "        0: 0; post curve (3.333, 1, autoEase),\n"
      "        10: 10; pre (3.333, 1, autoEase),\n"
      "    }\n"
      "}\n";

  Stage stage;
  TEST_CHECK(LoadStage(usda, &stage));

  std::vector<uint8_t> usdc;
  std::string w, e;
  bool sret = lightusd::legacy::usdc::SaveAsUSDCToMemory(stage, &usdc, &w, &e);
  TEST_CHECK_(sret, "SaveAsUSDCToMemory failed: %s", e.c_str());
  if (!sret) return;

  // A spline tangent algorithm must bump the emitted crate version to >= 0.13.0.
  TEST_CHECK(usdc.size() > 10);
  TEST_CHECK_(usdc[9] >= 13, "crate version minor = %d, expected >= 13",
              usdc[9]);

  Stage stage2;
  std::string w2, e2;
  bool lret = lightusd::legacy::LoadUSDCFromMemory(usdc.data(), usdc.size(), "mem.usdc",
                                           &stage2, &w2, &e2);
  TEST_CHECK_(lret, "LoadUSDCFromMemory failed: %s", e2.c_str());
  if (!lret) return;

  // The decoded spline must retain the autoEase algorithm.
  std::string exported = stage2.ExportToString();
  TEST_CHECK(exported.find("autoEase") != std::string::npos);
}
