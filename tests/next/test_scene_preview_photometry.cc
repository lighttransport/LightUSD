// SPDX-License-Identifier: Apache-2.0
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "tydra/scene-preview-photometry.hh"
namespace td = lightusd::tydra;
namespace fs = std::filesystem;
int main() {
  std::string error;
  const auto check = [&](bool ok, const char* message) {
    if (!ok) std::cerr << message << ": " << error << '\n';
    return ok;
  };
  const std::string constant =
      "IESNA:LM-63-2002\r\n[TEST] Synthetic constant\r\nTILT=NONE\r\n1 -1 2 2 "
      "1 1 2 0 0 0\r\n.5 1 0\r\n0 180\r\n0\r\n4 4\r\n";
  td::PreviewIesProfile profile;
  if (!check(td::ParsePreviewIes(constant, &profile, &error),
             "parse constant") ||
      !check(std::abs(profile.mean_candela - 4) < 1e-5f,
             "integrated isotropic power") ||
      !check(profile.sample({0, 0, -1}) == 4 &&
                 profile.sample({1, 0, 0}, 0, true) == 1,
             "constant and normalized intensity"))
    return 1;
  const std::string beam =
      "IESNA:LM-63-2002\nTILT=NONE\n1 -1 1 3 1 1 2 0 0 0\n1 1 0\n0 45 90\n0\n8 "
      "4 0\n";
  if (!check(td::ParsePreviewIes(beam, &profile, &error), "parse beam") ||
      !check(std::abs(profile.mean_candela - 4 * (1 - 2 / 3.141592653589793)) <
                 1e-5f,
             "exact piecewise-linear beam power") ||
      !check(std::abs(profile.sample({0, .70710678f, -.70710678f}) - 4) < 1e-4f,
             "vertical interpolation") ||
      !check(profile.sample({0, .70710678f, -.70710678f}, .5f) < 1e-4f &&
                 profile.sample({0, 0, 1}) == 0,
             "angle scale and missing hemisphere"))
    return 1;
  const std::string narrow =
      "IESNA:LM-63-2019\nTILT=NONE\n1 -1 1 2 1 1 2 0 0 0\n1 1 0\n0 .0001\n0\n1 "
      "0\n";
  if (!check(td::ParsePreviewIes(narrow, &profile, &error) &&
                 profile.mean_candela > 0,
             "narrow-beam normalization does not underflow"))
    return 1;
  for (unsigned type : {2u, 3u}) {
    const auto text = "IESNA:LM-63-2002\nTILT=NONE\n1 -1 1 2 2 " +
                      std::to_string(type) +
                      " 2 0 0 0\n1 1 0\n-90 90\n-90 90\n3 3 3 3\n";
    if (!check(td::ParsePreviewIes(text, &profile, &error), "parse type A/B") ||
        !check(std::abs(profile.mean_candela - 1.5f) < 1e-5f &&
                   profile.sample({0, 0, -1}) == 3 &&
                   profile.sample({0, 0, 1}) == 0,
               "type A/B hemisphere and power"))
      return 1;
  }
  auto tilt = constant;
  tilt.replace(tilt.find("TILT=NONE"), 9,
               "TILT=INCLUDE\n1 3\n0 90 180\n1 .5 .25");
  if (!check(td::ParsePreviewIes(tilt, &profile, &error), "included tilt") ||
      !check(profile.sample({0, 0, -1}, 0, false, 90) == 2 &&
                 std::abs(profile.tiltMultiplier(45) - .75f) < 1e-6f,
             "tilt interpolation"))
    return 1;
  auto external = constant;
  external.replace(external.find("TILT=NONE"), 9, "TILT=lamp.tilt");
  if (!check(td::ParsePreviewIes(
                 external, &profile, &error,
                 [](const std::string& name, std::string* text, std::string*) {
                   *text = "1 2 0 90 1 .5";
                   return name == "lamp.tilt";
                 }),
             "external tilt") ||
      !check(profile.sample({0, 0, -1}, 0, false, 90) == 2,
             "external tilt factor"))
    return 1;
  auto quadrant = constant;
  quadrant.replace(quadrant.find("2 1 1 2"), 7, "2 2 1 2");
  quadrant.replace(quadrant.find("0\r\n4 4"), 6, "0 90\r\n4 4 8 8");
  if (!check(td::ParsePreviewIes(quadrant, &profile, &error),
             "quadrant symmetry") ||
      !check(std::abs(profile.sample({1, 1, 0}) - profile.sample({-1, 1, 0})) <
                 1e-5f,
             "quadrant reflection"))
    return 1;
  for (const auto* invalid :
       {"", "TILT=INCLUDE\n1 99999999", "TILT=NONE\n1 -1 1 9999999 1 1",
        "TILT=NONE\n1 -1 nan"})
    if (!check(!td::ParsePreviewIes(invalid, &profile, &error),
               "reject invalid input"))
      return 1;
  for (const auto* numbers : {"0 0", "180 0", "0 nan"}) {
    auto malformed = constant;
    malformed.replace(malformed.find("0 180"), 5, numbers);
    if (!check(!td::ParsePreviewIes(malformed, &profile, &error),
               "reject unordered/nonfinite angles"))
      return 1;
  }
  const fs::path directory =
      fs::temp_directory_path() /
      ("lightusd-photometry-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(directory);
  {
    std::ofstream file(directory / "lamp.ies");
    file << external;
    std::ofstream t(directory / "lamp.tilt");
    t << "1 2 0 90 1 .5";
  }
  std::vector<std::string> dependencies;
  if (!check(td::LoadPreviewIes((directory / "lamp.ies").string(), &profile,
                                &error, &dependencies) &&
                 dependencies.size() == 2,
             "tilt dependencies") ||
      !check(td::WritePreviewIes((directory / "portable.ies").string(), profile,
                                 &error),
             "portable writer"))
    return 1;
  fs::remove(directory / "lamp.tilt");
  td::PreviewIesProfile portable;
  if (!check(td::LoadPreviewIes((directory / "portable.ies").string(),
                                &portable, &error) &&
                 portable.candela == profile.candela &&
                 portable.tilt_factors == profile.tilt_factors,
             "portable IES retains external tilt"))
    return 1;
  fs::remove_all(directory);
  std::cout
      << "PASS IES parsing, symmetry, power, shaping, tilt and portability\n";
}
