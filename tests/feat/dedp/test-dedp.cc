#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "crate-writer.hh"
#include "lightusd.hh"
#include "usdGeom.hh"
#include "value-types.hh"

namespace {

constexpr const char *kUsdaFixture = "tests/usda/dedup-global-001.usda";
constexpr const char *kOpenUsdUsdcFixture = "tests/usdc/dedup-global-001.usdc";

int ProcessId() {
#if defined(_WIN32)
  return _getpid();
#else
  return getpid();
#endif
}

std::string TempUsdcPath(const char *label) {
  const char *tmpdir = std::getenv("TMPDIR");
  if (!tmpdir || !tmpdir[0]) {
    tmpdir = std::getenv("TEMP");
  }
  if (!tmpdir || !tmpdir[0]) {
#if defined(_WIN32)
    tmpdir = ".";
#else
    tmpdir = "/tmp";
#endif
  }
  return std::string(tmpdir) + "/lightusd_dedp_" + label + "_" +
         std::to_string(ProcessId()) + ".usdc";
}

size_t FileSize(const std::string &filename) {
  std::ifstream ifs(filename, std::ios::binary | std::ios::ate);
  if (!ifs.is_open()) {
    return 0;
  }
  return static_cast<size_t>(ifs.tellg());
}

bool LoadStage(const std::string &filename, lightusd::legacy::Stage *stage) {
  std::string warn;
  std::string err;
  bool ok = lightusd::legacy::LoadUSDFromFile(filename, stage, &warn, &err);
  if (!warn.empty()) {
    std::cerr << "warning while loading " << filename << ": " << warn << "\n";
  }
  if (!ok) {
    std::cerr << "failed to load " << filename << ": " << err << "\n";
  }
  return ok;
}

bool WriteStageUsdc(const lightusd::legacy::Stage &stage, const std::string &filename,
                    bool dedup) {
  lightusd::legacy::experimental::CrateWriter writer(filename);
  lightusd::legacy::experimental::CrateWriter::Options opts;
  opts.enable_deduplication = dedup;
  opts.enable_compression = false;
  writer.SetOptions(opts);

  std::string err;
  if (!writer.Open(&err)) {
    std::cerr << "CrateWriter::Open failed: " << err << "\n";
    return false;
  }
  if (!writer.ConvertStageToSpecs(stage, &err)) {
    std::cerr << "ConvertStageToSpecs failed: " << err << "\n";
    writer.Close();
    return false;
  }
  if (!writer.Finalize(&err)) {
    std::cerr << "CrateWriter::Finalize failed: " << err << "\n";
    writer.Close();
    return false;
  }
  writer.Close();
  return true;
}

const lightusd::legacy::Attribute *FindXformProperty(const lightusd::legacy::Stage &stage,
                                             const std::string &name) {
  const lightusd::legacy::Prim *prim = nullptr;
  if (!stage.find_prim_at_path(lightusd::legacy::Path("/DedupRoot", ""), prim) ||
      !prim) {
    return nullptr;
  }

  const lightusd::legacy::Xform *xform = prim->as<lightusd::legacy::Xform>();
  if (!xform) {
    return nullptr;
  }

  auto it = xform->props.find(name);
  if (it == xform->props.end() || !it->second.is_attribute()) {
    return nullptr;
  }
  return &it->second.get_attribute();
}

bool NearlyEqual(double a, double b) {
  return std::abs(a - b) < 1.0e-12;
}

bool CheckLoadedDedupScene(const lightusd::legacy::Stage &stage,
                           const std::string &label) {
  const lightusd::legacy::Attribute *scalar = FindXformProperty(stage, "scalarA");
  if (!scalar) {
    std::cerr << label << ": missing scalarA\n";
    return false;
  }
  auto scalar_value = scalar->get_value<double>();
  if (!scalar_value || !NearlyEqual(scalar_value.value(), 42.25)) {
    std::cerr << label << ": unexpected scalarA value\n";
    return false;
  }

  const lightusd::legacy::Attribute *vector = FindXformProperty(stage, "vectorA");
  if (!vector) {
    std::cerr << label << ": missing vectorA\n";
    return false;
  }
  auto vector_value = vector->get_value<lightusd::legacy::value::double3>();
  if (!vector_value || !NearlyEqual(vector_value.value()[0], 1.25) ||
      !NearlyEqual(vector_value.value()[1], 2.5) ||
      !NearlyEqual(vector_value.value()[2], 3.75)) {
    std::cerr << label << ": unexpected vectorA value\n";
    return false;
  }

  const lightusd::legacy::Attribute *array =
      FindXformProperty(stage, "vectorArrayA");
  if (!array) {
    std::cerr << label << ": missing vectorArrayA\n";
    return false;
  }
  auto array_value =
      array->get_value<std::vector<lightusd::legacy::value::double3>>();
  if (!array_value || array_value.value().size() != 4 ||
      !NearlyEqual(array_value.value()[3][0], 10.0) ||
      !NearlyEqual(array_value.value()[3][1], 11.0) ||
      !NearlyEqual(array_value.value()[3][2], 12.0)) {
    std::cerr << label << ": unexpected vectorArrayA value\n";
    return false;
  }

  const lightusd::legacy::Attribute *anim = FindXformProperty(stage, "animA");
  if (!anim || !anim->has_timesamples()) {
    std::cerr << label << ": missing animA timeSamples\n";
    return false;
  }
  const lightusd::legacy::value::TimeSamples &ts = anim->get_var().ts_raw();
  const std::vector<double> &times = ts.get_times();
  if (times.size() != 8 || !NearlyEqual(times.front(), 0.0) ||
      !NearlyEqual(times.back(), 7.0)) {
    std::cerr << label << ": unexpected animA sample times\n";
    return false;
  }

  lightusd::legacy::value::double3 sample{};
  if (!ts.get(&sample, 7.0) || !NearlyEqual(sample[0], 7.0) ||
      !NearlyEqual(sample[1], 8.0) || !NearlyEqual(sample[2], 9.0)) {
    std::cerr << label << ": unexpected animA sample value\n";
    return false;
  }

  return true;
}

bool TestLightUsdcWriteDedup() {
  lightusd::legacy::Stage stage;
  if (!LoadStage(kUsdaFixture, &stage)) {
    return false;
  }
  if (!CheckLoadedDedupScene(stage, "USDA fixture")) {
    return false;
  }

  const std::string dedup_path = TempUsdcPath("dedup_on");
  const std::string no_dedup_path = TempUsdcPath("dedup_off");

  if (!WriteStageUsdc(stage, dedup_path, true) ||
      !WriteStageUsdc(stage, no_dedup_path, false)) {
    std::remove(dedup_path.c_str());
    std::remove(no_dedup_path.c_str());
    return false;
  }

  const size_t dedup_size = FileSize(dedup_path);
  const size_t no_dedup_size = FileSize(no_dedup_path);
  std::cout << "LightUSD USDC sizes: dedup=" << dedup_size
            << " no_dedup=" << no_dedup_size << "\n";
  if (dedup_size == 0 || no_dedup_size == 0 || dedup_size >= no_dedup_size) {
    std::cerr << "expected LightUSD dedup USDC to be smaller\n";
    std::remove(dedup_path.c_str());
    std::remove(no_dedup_path.c_str());
    return false;
  }

  const size_t openusd_size = FileSize(kOpenUsdUsdcFixture);
  if (openusd_size == 0) {
    std::cerr << "missing OpenUSD-generated USDC fixture\n";
    std::remove(dedup_path.c_str());
    std::remove(no_dedup_path.c_str());
    return false;
  }
  const size_t delta = (dedup_size > openusd_size)
      ? (dedup_size - openusd_size)
      : (openusd_size - dedup_size);
  const size_t tolerance = 128;
  if (delta > tolerance) {
    std::cerr << "LightUSD dedup USDC size drifted from OpenUSD: lightusd="
              << dedup_size << " openusd=" << openusd_size
              << " delta=" << delta << "\n";
    std::remove(dedup_path.c_str());
    std::remove(no_dedup_path.c_str());
    return false;
  }

  lightusd::legacy::Stage reloaded;
  const bool reload_ok = LoadStage(dedup_path, &reloaded) &&
                         CheckLoadedDedupScene(reloaded, "LightUSD dedup USDC");
  std::remove(dedup_path.c_str());
  std::remove(no_dedup_path.c_str());
  return reload_ok;
}

bool TestOpenUsdUsdcReadDedup() {
  lightusd::legacy::Stage usda_stage;
  if (!LoadStage(kUsdaFixture, &usda_stage)) {
    return false;
  }

  const std::string no_dedup_path = TempUsdcPath("openusd_compare_no_dedup");
  if (!WriteStageUsdc(usda_stage, no_dedup_path, false)) {
    std::remove(no_dedup_path.c_str());
    return false;
  }

  const size_t openusd_size = FileSize(kOpenUsdUsdcFixture);
  const size_t no_dedup_size = FileSize(no_dedup_path);
  std::cout << "OpenUSD fixture size=" << openusd_size
            << " LightUSD no_dedup=" << no_dedup_size << "\n";
  std::remove(no_dedup_path.c_str());

  if (openusd_size == 0 || no_dedup_size == 0 ||
      openusd_size >= no_dedup_size) {
    std::cerr << "expected OpenUSD-generated USDC fixture to be deduplicated\n";
    return false;
  }

  lightusd::legacy::Stage openusd_stage;
  if (!LoadStage(kOpenUsdUsdcFixture, &openusd_stage)) {
    return false;
  }
  return CheckLoadedDedupScene(openusd_stage, "OpenUSD dedup USDC");
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestLightUsdcWriteDedup() && ok;
  ok = TestOpenUsdUsdcReadDedup() && ok;
  if (!ok) {
    return 1;
  }
  std::cout << "dedp feature tests passed\n";
  return 0;
}
