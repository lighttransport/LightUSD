// SPDX-License-Identifier: Apache-2.0
// Bounded LM-63 parser and photometric coordinate evaluation. The format is
// described by ANSI/IES LM-63; this implementation does not embed third-party
// parser code. See doc/preview-lighting-goal.md for reference documentation.
#include "core/native-io.hh"
#include "scene-preview-photometry.hh"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace lightusd {
namespace tydra {
namespace {
constexpr double pi = 3.14159265358979323846, radians = pi / 180;
constexpr size_t max_bytes = 4u << 20, max_angles = 4096, max_values = 1u << 20;
bool Error(std::string* error, const char* message) {
  if (error) *error = message;
  return false;
}
std::string_view Trim(std::string_view text) {
  const auto white = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (!text.empty() && white(text.front())) text.remove_prefix(1);
  while (!text.empty() && white(text.back())) text.remove_suffix(1);
  return text;
}
class Numbers {
 public:
  explicit Numbers(std::string_view source) : source_(source) {}
  bool get(float* output) {
    while (!source_.empty() &&
           (source_.front() == ' ' || source_.front() == '\t' ||
            source_.front() == '\r' || source_.front() == '\n' ||
            source_.front() == ','))
      source_.remove_prefix(1);
    size_t count = 0;
    while (count < source_.size()) {
      const char c = source_[count];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') break;
      if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' ||
            c == 'e' || c == 'E'))
        return false;
      if (++count > 128) return false;
    }
    if (!count) return false;
    const std::string token(source_.substr(0, count));
    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(token.c_str(), &end);
    if (errno == ERANGE || end != token.c_str() + token.size() ||
        !std::isfinite(value))
      return false;
    source_.remove_prefix(count);
    *output = value;
    return true;
  }
  bool integer(size_t maximum, unsigned* output) {
    float value = 0;
    if (!get(&value) || value < 0 || value > maximum ||
        std::floor(value) != value)
      return false;
    *output = static_cast<unsigned>(value);
    return true;
  }
  bool empty() {
    float value = 0;
    if (get(&value)) return false;
    return Trim(source_).empty();
  }

 private:
  std::string_view source_;
};
bool Angles(Numbers& numbers, size_t count, float minimum, float maximum,
            std::vector<float>* out) {
  out->resize(count);
  for (size_t i = 0; i < count; ++i)
    if (!numbers.get(&(*out)[i]) || (*out)[i] < minimum ||
        (*out)[i] > maximum || (i && (*out)[i] <= (*out)[i - 1]))
      return false;
  return true;
}
bool Tilt(Numbers& numbers, PreviewIesProfile* profile) {
  unsigned count = 0;
  if (!numbers.integer(3, &profile->tilt_geometry) ||
      profile->tilt_geometry == 0 || !numbers.integer(max_angles, &count) ||
      count == 0 || !Angles(numbers, count, 0, 180, &profile->tilt_angles))
    return false;
  profile->tilt_factors.resize(count);
  for (auto& factor : profile->tilt_factors)
    if (!numbers.get(&factor) || factor < 0) return false;
  return true;
}
float Interpolate(const std::vector<float>& x, const std::vector<float>& values,
                  float position) {
  if (x.empty()) return 1;
  if (position <= x.front()) return values.front();
  if (position >= x.back()) return values.back();
  const size_t upper = static_cast<size_t>(
                   std::upper_bound(x.begin(), x.end(), position) - x.begin()),
               lower = upper - 1;
  const float t = (position - x[lower]) / (x[upper] - x[lower]);
  return values[lower] * (1 - t) + values[upper] * t;
}
// Exact integrals of the two linear interpolation bases against 1/sin/cos.
std::array<double, 2> Integrals(double a, double b, unsigned weight) {
  const double d = b - a;
  if (weight == 0) return {d * .5, d * .5};
  const double half = d * .5, mid = (a + b) * .5;
  const double total =
      2 * (weight == 1 ? std::sin(mid) : std::cos(mid)) * std::sin(half);
  const double cubic =
      std::abs(half) < .001
          ? half * half * half *
                (1. / 3 - half * half / 30 + half * half * half * half / 840)
          : std::sin(half) - half * std::cos(half);
  const double moment =
      2 * (weight == 1 ? std::cos(mid) : -std::sin(mid)) * cubic / d;
  return {std::max(0., total * .5 - moment), std::max(0., total * .5 + moment)};
}
double ProfileMean(const PreviewIesProfile& p) {
  const size_t nv = p.vertical.size(), nh = p.horizontal.size();
  double integral = 0;
  const unsigned vw = p.type == 1   ? 1
                      : p.type == 3 ? 2
                                    : 0,
                 hw = p.type == 2 ? 2 : 0;
  double symmetry = 1;
  if (p.type == 1 && nh > 1) {
    if (p.horizontal.front() == 0 && p.horizontal.back() == 90)
      symmetry = 4;
    else if ((p.horizontal.front() == 0 && p.horizontal.back() == 180) ||
             (p.horizontal.front() == 90 && p.horizontal.back() == 270))
      symmetry = 2;
  }
  if (p.type != 1) {
    if (nv > 1 && p.vertical.front() == 0) symmetry *= 2;
    if (nh > 1 && p.horizontal.front() == 0) symmetry *= 2;
  }
  const bool periodic = p.type == 1 && nh > 1 && symmetry == 1;
  const size_t vc = nv == 1 ? 1 : nv - 1,
               hc = nh == 1 ? 1
                            : (periodic && p.horizontal.back() -
                                                   p.horizontal.front() <
                                               359.999f
                                   ? nh
                                   : nh - 1);
  for (size_t h = 0; h < hc; ++h)
    for (size_t v = 0; v < vc; ++v) {
      std::array<double, 2> vi, hi;
      if (nv == 1)
        vi = p.type == 2 ? std::array<double, 2>{pi * .5, pi * .5}
                         : std::array<double, 2>{1, 1};
      else
        vi =
            Integrals(p.vertical[v] * radians, p.vertical[v + 1] * radians, vw);
      if (nh == 1)
        hi = p.type == 1   ? std::array<double, 2>{pi, pi}
             : p.type == 2 ? std::array<double, 2>{1, 1}
                           : std::array<double, 2>{pi * .5, pi * .5};
      else
        hi = Integrals(
            p.horizontal[h] * radians,
            (h + 1 == nh ? p.horizontal.front() + 360 : p.horizontal[h + 1]) *
                radians,
            hw);
      for (size_t j = 0; j < 2; ++j)
        for (size_t k = 0; k < 2; ++k) {
          const size_t row = nh == 1 ? 0 : (h + j) % nh,
                       col = nv == 1 ? 0 : v + k;
          integral += p.candela[row * nv + col] * hi[j] * vi[k];
        }
    }
  return integral * symmetry / (4 * pi);
}
float Table(const PreviewIesProfile& p, float vertical, float horizontal) {
  const size_t nv = p.vertical.size(), nh = p.horizontal.size();
  if (!nv || !nh || p.candela.size() != nv * nh) return 0;
  if (p.type == 1) {
    horizontal = std::fmod(horizontal, 360.f);
    if (horizontal < 0) horizontal += 360;
    if (nh > 1 && p.horizontal.front() == 0 && p.horizontal.back() == 90) {
      horizontal = std::fmod(horizontal, 180.f);
      if (horizontal > 90) horizontal = 180 - horizontal;
    } else if (nh > 1 && p.horizontal.front() == 0 &&
               p.horizontal.back() == 180) {
      if (horizontal > 180) horizontal = 360 - horizontal;
    } else if (nh > 1 && p.horizontal.front() == 90 &&
               p.horizontal.back() == 270) {
      if (horizontal < 90)
        horizontal = 180 - horizontal;
      else if (horizontal > 270)
        horizontal = 540 - horizontal;
    } else if (nh > 1 && horizontal < p.horizontal.front())
      horizontal += 360;
  } else {
    if (p.vertical.front() == 0) vertical = std::abs(vertical);
    if (p.horizontal.front() == 0) horizontal = std::abs(horizontal);
    if (std::abs(vertical) > 90.0001f || std::abs(horizontal) > 90.0001f)
      return 0;
  }
  if (nv > 1 && (vertical < p.vertical.front() || vertical > p.vertical.back()))
    return 0;
  size_t v0 = 0, v1 = 0, h0 = 0, h1 = 0;
  float fv = 0, fh = 0;
  if (nv > 1) {
    v1 = std::min(nv - 1, static_cast<size_t>(
                              std::upper_bound(p.vertical.begin(),
                                               p.vertical.end(), vertical) -
                              p.vertical.begin()));
    v0 = v1 ? v1 - 1 : 0;
    fv = v0 == v1
             ? 0
             : (vertical - p.vertical[v0]) / (p.vertical[v1] - p.vertical[v0]);
  }
  if (nh > 1) {
    if (horizontal > p.horizontal.back() && p.type == 1) {
      h0 = nh - 1;
      h1 = 0;
      fh = (horizontal - p.horizontal.back()) /
           (p.horizontal.front() + 360 - p.horizontal.back());
    } else {
      if (horizontal < p.horizontal.front() || horizontal > p.horizontal.back())
        return 0;
      h1 = std::min(
          nh - 1,
          static_cast<size_t>(std::upper_bound(p.horizontal.begin(),
                                               p.horizontal.end(), horizontal) -
                              p.horizontal.begin()));
      h0 = h1 ? h1 - 1 : 0;
      fh = h0 == h1 ? 0
                    : (horizontal - p.horizontal[h0]) /
                          (p.horizontal[h1] - p.horizontal[h0]);
    }
  }
  const float a = p.candela[h0 * nv + v0] * (1 - fv) +
                  p.candela[h0 * nv + v1] * fv,
              b = p.candela[h1 * nv + v0] * (1 - fv) +
                  p.candela[h1 * nv + v1] * fv;
  return a * (1 - fh) + b * fh;
}
bool Read(const std::string& path, std::string* text, std::string* error) {
  if (!::lightusd::NativeIOAvailable(error)) return false;

  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return Error(error, "cannot open IES/TILT asset");
  const auto size = file.tellg();
  if (size < 0 || static_cast<uint64_t>(size) > max_bytes)
    return Error(error, "IES/TILT asset exceeds 4 MiB limit");
  text->resize(static_cast<size_t>(size));
  file.seekg(0);
  if (size && !file.read(text->data(), size))
    return Error(error, "cannot read IES/TILT asset");
  return true;
}
}  // namespace
float PreviewIesProfile::tiltMultiplier(float degrees) const {
  return Interpolate(tilt_angles, tilt_factors,
                     std::max(0.f, std::min(180.f, degrees)));
}
float PreviewIesProfile::sample(std::array<float, 3> d, float angle_scale,
                                bool normalize, float tilt_degrees) const {
  double length = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] +
                            double(d[2]) * d[2]);
  if (!std::isfinite(length) || length == 0 || !std::isfinite(angle_scale) ||
      !std::isfinite(tilt_degrees))
    return 0;
  for (auto& value : d) value = static_cast<float>(value / length);
  double theta = std::acos(std::max(-1.f, std::min(1.f, -d[2]))),
         phi = std::atan2(d[1], d[0]);
  if (angle_scale > 0)
    theta /= angle_scale;
  else if (angle_scale < 0)
    theta = (theta - pi) / (-angle_scale) + pi;
  if (theta < 0 || theta > pi) return 0;
  d = {float(std::sin(theta) * std::cos(phi)),
       float(std::sin(theta) * std::sin(phi)), float(-std::cos(theta))};
  const float v =
      type == 1 ? float(theta / radians)
      : type == 2
          ? float(std::atan2(d[1], -d[2]) / radians)
          : float(std::asin(std::max(-1.f, std::min(1.f, d[1]))) / radians);
  const float h =
      type == 1 ? float(phi / radians)
      : type == 2
          ? float(std::asin(std::max(-1.f, std::min(1.f, d[0]))) / radians)
          : float(std::atan2(d[0], -d[2]) / radians);
  const float value = Table(*this, v, h) * tiltMultiplier(tilt_degrees);
  return normalize ? (mean_candela > 0 ? value / mean_candela : 0) : value;
}
bool ParsePreviewIes(std::string_view text, PreviewIesProfile* output,
                     std::string* error, const PreviewIesTiltLoader& external) {
  if (!output || text.size() > max_bytes)
    return Error(error, "invalid or oversized IES input");
  if (text.size() >= 3 &&
      text.substr(0, 3) == std::string_view("\xef\xbb\xbf", 3))
    text.remove_prefix(3);
  PreviewIesProfile result;
  bool old = true, found = false;
  std::string tilt;
  while (!text.empty()) {
    const size_t end = text.find('\n');
    auto line = Trim(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{}
                                         : text.substr(end + 1);
    if (line.find("LM-63-1995") != std::string_view::npos ||
        line.find("LM-63-2002") != std::string_view::npos ||
        line.find("LM-63-2019") != std::string_view::npos)
      old = false;
    if (line.size() >= 4 &&
        (line.substr(0, 4) == "TILT" || line.substr(0, 4) == "Tilt" ||
         line.substr(0, 4) == "tilt")) {
      const auto equal = line.find('=');
      if (equal == std::string_view::npos)
        return Error(error, "invalid IES TILT header");
      tilt = std::string(Trim(line.substr(equal + 1)));
      found = true;
      break;
    }
  }
  if (!found || tilt.empty() || tilt.size() > 4096)
    return Error(error, "IES TILT header missing or invalid");
  Numbers numbers(text);
  if (tilt == "INCLUDE") {
    if (!Tilt(numbers, &result))
      return Error(error, "invalid included IES TILT data");
  } else if (tilt != "NONE") {
    if (tilt.size() > 1 && tilt.front() == '"' && tilt.back() == '"')
      tilt = tilt.substr(1, tilt.size() - 2);
    std::string value;
    if (!external || !external(tilt, &value, error) || value.size() > max_bytes)
      return Error(error, "cannot load external IES TILT data");
    Numbers n(value);
    if (!Tilt(n, &result) || !n.empty())
      return Error(error, "invalid external IES TILT data");
  }
  unsigned lamps = 0, nv = 0, nh = 0, units = 0;
  float lumens = 0, multiplier = 0, dimensions[3], ballast = 0, future = 0,
        watts = 0;
  if (!numbers.integer(1000000, &lamps) || !lamps || !numbers.get(&lumens) ||
      (lumens < 0 && lumens != -1) || !numbers.get(&multiplier) ||
      multiplier < 0 || !numbers.integer(max_angles, &nv) || !nv ||
      !numbers.integer(max_angles, &nh) || !nh ||
      size_t(nv) * nh > max_values || !numbers.integer(3, &result.type) ||
      !result.type || !numbers.integer(2, &units) || !units)
    return Error(error, "invalid IES photometric header");
  for (auto& value : dimensions)
    if (!numbers.get(&value)) return Error(error, "invalid IES dimensions");
  if (!numbers.get(&ballast) || ballast < 0 || !numbers.get(&future) ||
      future < 0 || !numbers.get(&watts) || watts < 0)
    return Error(error, "invalid IES factors");
  if (!Angles(numbers, nv, result.type == 1 ? 0.f : -90.f,
              result.type == 1 ? 180.f : 90.f, &result.vertical) ||
      !Angles(numbers, nh, result.type == 1 ? 0.f : -90.f,
              result.type == 1 ? 360.f : 90.f, &result.horizontal))
    return Error(error, "invalid or unordered IES angles");
  if (result.type == 1 && nh > 1) {
    const float a = result.horizontal.front(), b = result.horizontal.back();
    if (!((a == 0 && (b == 90 || b == 180 || b > 180)) ||
          (a == 90 && b == 270)))
      return Error(error, "unsupported IES horizontal symmetry");
  }
  const double scale = double(multiplier) * ballast * (old ? future : 1);
  result.candela.resize(size_t(nv) * nh);
  for (auto& value : result.candela) {
    if (!numbers.get(&value) || value < 0 ||
        value * scale > std::numeric_limits<float>::max())
      return Error(error, "invalid IES candela data");
    value = static_cast<float>(value * scale);
  }
  if (!numbers.empty()) return Error(error, "unexpected trailing IES data");
  const double mean = ProfileMean(result);
  if (!std::isfinite(mean) || mean > std::numeric_limits<float>::max())
    return Error(error, "IES power exceeds float range");
  result.mean_candela = static_cast<float>(mean);
  *output = std::move(result);
  return true;
}
bool LoadPreviewIes(const std::string& path, PreviewIesProfile* profile,
                    std::string* error,
                    std::vector<std::string>* dependencies) {
  std::string text;
  if (!Read(path, &text, error)) return false;
  if (dependencies) dependencies->push_back(path);
  return ParsePreviewIes(
      text, profile, error,
      [&](const std::string& name, std::string* data, std::string* e) {
        const auto file = (std::filesystem::path(path).parent_path() / name)
                              .lexically_normal()
                              .string();
        if (!Read(file, data, e)) return false;
        if (dependencies) dependencies->push_back(file);
        return true;
      });
}
bool WritePreviewIes(const std::string& path, const PreviewIesProfile& p,
                     std::string* error) {
  if (!::lightusd::NativeIOAvailable(error)) return false;

  if (p.vertical.empty() || p.horizontal.empty() ||
      p.candela.size() != p.vertical.size() * p.horizontal.size())
    return Error(error, "cannot write incomplete IES profile");
  std::ofstream file(path, std::ios::binary);
  file << std::setprecision(9)
       << "IESNA:LM-63-2002\n[TEST] LightUSD portable photometry\nTILT="
       << (p.tilt_angles.empty() ? "NONE\n" : "INCLUDE\n");
  if (!p.tilt_angles.empty()) {
    file << p.tilt_geometry << ' ' << p.tilt_angles.size() << '\n';
    for (auto value : p.tilt_angles) file << value << ' ';
    file << '\n';
    for (auto value : p.tilt_factors) file << value << ' ';
    file << '\n';
  }
  file << "1 -1 1 " << p.vertical.size() << ' ' << p.horizontal.size() << ' '
       << p.type << " 2 0 0 0\n1 1 0\n";
  for (auto value : p.vertical) file << value << ' ';
  file << '\n';
  for (auto value : p.horizontal) file << value << ' ';
  file << '\n';
  for (size_t h = 0; h < p.horizontal.size(); ++h) {
    for (size_t v = 0; v < p.vertical.size(); ++v)
      file << p.candela[h * p.vertical.size() + v] << ' ';
    file << '\n';
  }
  if (!file) return Error(error, "cannot write portable IES profile");
  return true;
}
}  // namespace tydra
}  // namespace lightusd
