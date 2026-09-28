// Native MJCF/URDF-style physics scene to USD exporter.
//
// This example currently implements a native MJCF path for MuJoCo menagerie
// scenes. It parses XML with pugixml, bakes STL/OBJ and simple MJCF primitive
// geometry into the same compact JSON contract used by the JS/WASM demo, then
// reuses tydra::ConvertURDFJsonToUSDStage.

#include "jsonhpp/nlohmann/json.hpp"
#include "pugixml.hpp"
// Built for the next product with LIGHTUSD_URDF_TO_USD_NEXT (next converter
// and writers); otherwise for the legacy product.
#if defined(LIGHTUSD_URDF_TO_USD_NEXT)
#include "next/load-usd.hh"
#include "next/stage/stage.hh"
#include "next/writer/usdc-writer.hh"
#include "next/writer/usdz-writer.hh"
#include "tydra/next/urdf-to-usd.hh"
#else
#include "lightusd.hh"
#include "tydra/urdf-to-usd.hh"
#include "usda-writer.hh"
#include "usdc-writer.hh"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Local (file-scope) PNG/image decoder for <asset><hfield file="...png">.
// STB_IMAGE_STATIC keeps the implementation private to this TU so it cannot
// collide with any copy linked into liblightusd_static.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

// Use the vendored ghc::filesystem (a portable std::filesystem replacement, the
// same one lightusd::io uses internally) so this example does not depend on
// std::filesystem. This file relies on the fs::path value type pervasively, so
// it keeps the filesystem API rather than the string-based io-util helpers.
// ghc/filesystem.hpp pulls in <windows.h> on Windows; keep it from defining the
// min/max macros (which would break std::numeric_limits<>::max() below) and trim
// the surface area.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "filesystem/include/ghc/filesystem.hpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace fs = ghc::filesystem;

namespace {

struct Options {
  std::string input_filename;
  std::string output_filename;
  std::string format = "usda";
  std::string input_format = "auto";
  std::string up_axis = "Z";
  std::string dump_json_filename;
  bool allow_missing = false;
  bool tessellate_collision_shapes = false;
  bool verbose = false;
};

struct MeshAsset {
  fs::path path;
  std::array<double, 3> scale{{1.0, 1.0, 1.0}};
  std::array<double, 3> refpos{{0.0, 0.0, 0.0}};   // <mesh refpos="...">
  std::array<double, 4> refquat{{1.0, 0.0, 0.0, 0.0}};  // <mesh refquat="..."> wxyz
  bool has_refpos = false;
  bool has_refquat = false;
  bool smoothnormal = false;  // <mesh smoothnormal>; MuJoCo default false
};

struct MeshData {
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<float> uvs;  // 2 per vertex (st), parallel to positions; empty if none
  std::vector<int32_t> indices;
};

// <asset><hfield>: a row-major nrow x ncol grid of elevations normalized to
// [0,1], plus size = (radius_x, radius_y, elevation_z, base_z).
struct HFieldAsset {
  int nrow{0};
  int ncol{0};
  std::array<double, 4> size{{1.0, 1.0, 1.0, 0.1}};
  std::vector<float> data;  // nrow*ncol, normalized [0,1], row 0 = -y edge
};

struct MeshPayload {
  std::string name;
  std::vector<double> matrix;
  MeshData mesh;
  nlohmann::json shape;
};

struct Stats {
  size_t links{0};
  size_t joints{0};
  size_t visuals{0};
  size_t collisions{0};
  size_t actuators{0};
  size_t tendons{0};
  size_t equalities{0};
  size_t contact_excludes{0};
  size_t sites{0};
  size_t keyframes{0};
  size_t materials{0};
  size_t sensors{0};
  size_t contact_pairs{0};
};

// Resolved attribute map (attr name -> value) for MuJoCo <default> classes.
using AttrMap = std::map<std::string, std::string>;

struct Defaults {
  std::map<std::string, AttrMap> geom;   // class name -> merged geom attrs
  std::map<std::string, AttrMap> joint;  // class name -> merged joint attrs
  AttrMap root_geom;                      // unnamed (top-level) default
  AttrMap root_joint;
};

struct Context {
  // <compiler angle="...">: degrees by default; radians otherwise.
  double angle_to_rad = 0.017453292519943295;  // pi/180
  std::string eulerseq = "xyz";                 // <compiler eulerseq="...">
  Defaults defaults;
};

void PrintHelp() {
  std::cout << R"(Native URDF/MJCF -> USD Physics + MuJoCo + Newton exporter

Usage:
  urdf-to-usd <scene.xml> --input-format mjcf --format usdc -o /tmp/robot.usdc

Options:
  --input-format <fmt>  Input format: auto, mjcf (default: auto)
  --format <fmt>        Output format: usda, usdc, usdz, all (default: usda)
  -o, --output <path>   Output file path, or base path when --format all
  --up-axis <axis>      Export up axis: Z or Y (default: Z)
  --dump-json <path>    Write the intermediate converter JSON payload
  --allow-missing       Skip unsupported or missing mesh assets
  --tessellate-collision-shapes
                       Tessellate primitive collision geoms to meshes.
                       Default: export primitive collisions as USD native shapes.
  -v, --verbose         Print details
  -h, --help            Show this help
)";
}

bool HasPrefix(const std::string &s, const std::string &prefix) {
  return s.rfind(prefix, 0) == 0;
}

std::string ToLower(std::string s) {
  for (char &c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

bool ParseArgs(int argc, char **argv, Options *opts, std::string *err) {
  for (int i = 1; i < argc; i++) {
    const std::string arg(argv[i]);
    auto need_value = [&](const std::string &name) -> std::string {
      if (i + 1 >= argc) {
        if (err) *err = name + " requires a value.";
        return std::string();
      }
      return std::string(argv[++i]);
    };

    if (arg == "-h" || arg == "--help") {
      PrintHelp();
      std::exit(EXIT_SUCCESS);
    } else if (arg == "--input-format") {
      opts->input_format = ToLower(need_value(arg));
    } else if (arg == "--format") {
      opts->format = ToLower(need_value(arg));
    } else if (arg == "-o" || arg == "--output") {
      opts->output_filename = need_value(arg);
    } else if (arg == "--up-axis") {
      opts->up_axis = need_value(arg);
      std::transform(opts->up_axis.begin(), opts->up_axis.end(),
                     opts->up_axis.begin(), [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                     });
    } else if (arg == "--dump-json") {
      opts->dump_json_filename = need_value(arg);
    } else if (arg == "--allow-missing") {
      opts->allow_missing = true;
    } else if (arg == "--tessellate-collision-shapes") {
      opts->tessellate_collision_shapes = true;
    } else if (arg == "-v" || arg == "--verbose") {
      opts->verbose = true;
    } else if (!HasPrefix(arg, "-") && opts->input_filename.empty()) {
      opts->input_filename = arg;
    } else {
      if (err) *err = "Unknown argument: " + arg;
      return false;
    }

    if (err && !err->empty()) return false;
  }

  const std::set<std::string> formats{"usda", "usdc", "usdz", "all"};
  if (!formats.count(opts->format)) {
    if (err) *err = "--format must be one of: usda, usdc, usdz, all.";
    return false;
  }
  const std::set<std::string> input_formats{"auto", "mjcf"};
  if (!input_formats.count(opts->input_format)) {
    if (err) *err = "--input-format must be one of: auto, mjcf.";
    return false;
  }
  if (opts->up_axis != "Z" && opts->up_axis != "Y") {
    if (err) *err = "--up-axis must be Z or Y.";
    return false;
  }
  if (opts->input_filename.empty()) {
    if (err) *err = "Input file is required.";
    return false;
  }
  return true;
}

bool ParseInt(const std::string &text, int *out) {
  if (text.empty()) return false;
  char *end = nullptr;
  const long v = std::strtol(text.c_str(), &end, 10);
  if (!end || *end != '\0' || v < std::numeric_limits<int>::min() ||
      v > std::numeric_limits<int>::max()) {
    return false;
  }
  *out = static_cast<int>(v);
  return true;
}

bool ReadFile(const fs::path &filename, std::string *text, std::string *err) {
  fs::ifstream ifs(filename, std::ios::binary);
  if (!ifs) {
    if (err) *err = "Failed to open: " + filename.string();
    return false;
  }
  std::ostringstream ss;
  ss << ifs.rdbuf();
  (*text) = ss.str();
  return true;
}

bool WriteFile(const fs::path &filename, const std::vector<uint8_t> &bytes,
               std::string *err) {
  fs::ofstream ofs(filename, std::ios::binary);
  if (!ofs) {
    if (err) *err = "Failed to open output: " + filename.string();
    return false;
  }
  ofs.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return bool(ofs);
}

std::vector<double> ParseDoubles(const std::string &text) {
  std::vector<double> values;
  std::stringstream ss(text);
  double v = 0.0;
  while (ss >> v) values.push_back(v);
  return values;
}

std::array<double, 3> ParseDouble3(const std::string &text,
                                   std::array<double, 3> fallback) {
  const std::vector<double> values = ParseDoubles(text);
  if (values.size() >= 3) {
    return {{values[0], values[1], values[2]}};
  }
  return fallback;
}

double ParseDoubleAttr(const pugi::xml_node &node,
                       const std::string &name, double fallback) {
  if (!node || !node.attribute(name.c_str())) return fallback;
  char *end = nullptr;
  const std::string s = node.attribute(name.c_str()).as_string();
  const double v = std::strtod(s.c_str(), &end);
  return (end && end != s.c_str()) ? v : fallback;
}

bool HasAttr(const pugi::xml_node &node, const std::string &name) {
  return node && node.attribute(name.c_str());
}

std::string Attr(const pugi::xml_node &node, const std::string &name,
                 const std::string &fallback = std::string()) {
  if (!node) return fallback;
  return node.attribute(name.c_str()).as_string(fallback.c_str());
}

std::vector<pugi::xml_node> Children(
    const pugi::xml_node &node, const std::string &name) {
  if (!node) return {};
  std::vector<pugi::xml_node> out;
  for (pugi::xml_node child : node.children(name.c_str())) {
    out.push_back(child);
  }
  return out;
}

pugi::xml_node Child(const pugi::xml_node &node,
                                 const std::string &name) {
  if (!node) return pugi::xml_node();
  return node.child(name.c_str());
}

std::string StripMujocoRoot(const std::string &xml) {
  std::string out = xml;
  const size_t decl = out.find("<?xml");
  if (decl != std::string::npos) {
    const size_t end = out.find("?>", decl);
    if (end != std::string::npos) out.erase(decl, end + 2 - decl);
  }

  const size_t open = out.find("<mujoco");
  const size_t close = out.rfind("</mujoco>");
  if (open == std::string::npos || close == std::string::npos) return out;
  const size_t open_end = out.find('>', open);
  if (open_end == std::string::npos || open_end >= close) return out;
  return out.substr(open_end + 1, close - open_end - 1);
}

std::string AttributeFromTag(const std::string &tag, const std::string &name) {
  const std::string needle = name + "=";
  size_t pos = tag.find(needle);
  if (pos == std::string::npos) return std::string();
  pos += needle.size();
  if (pos >= tag.size()) return std::string();
  const char quote = tag[pos];
  if (quote != '"' && quote != '\'') return std::string();
  const size_t end = tag.find(quote, pos + 1);
  if (end == std::string::npos) return std::string();
  return tag.substr(pos + 1, end - pos - 1);
}

bool ExpandIncludes(const std::string &xml, const fs::path &base_dir,
                    std::set<fs::path> *seen, std::string *expanded,
                    std::string *err) {
  std::string out;
  size_t cursor = 0;
  while (true) {
    const size_t pos = xml.find("<include", cursor);
    if (pos == std::string::npos) {
      out += xml.substr(cursor);
      break;
    }
    out += xml.substr(cursor, pos - cursor);
    const size_t end = xml.find('>', pos);
    if (end == std::string::npos) {
      if (err) *err = "Malformed <include> element.";
      return false;
    }
    const std::string tag = xml.substr(pos, end - pos + 1);
    const std::string file = AttributeFromTag(tag, "file");
    if (!file.empty()) {
      std::error_code wc_ec;
      const fs::path include_path = fs::weakly_canonical(base_dir / file, wc_ec);
      if (seen->count(include_path)) {
        if (err) *err = "Recursive include: " + include_path.string();
        return false;
      }
      std::string child;
      if (!ReadFile(include_path, &child, err)) return false;
      seen->insert(include_path);
      std::string child_expanded;
      if (!ExpandIncludes(StripMujocoRoot(child), include_path.parent_path(),
                          seen, &child_expanded, err)) {
        return false;
      }
      seen->erase(include_path);
      out += child_expanded;
    }
    cursor = end + 1;
  }
  (*expanded) = std::move(out);
  return true;
}

// Quaternions are stored MuJoCo-style: {w, x, y, z}.
using Quat = std::array<double, 4>;

Quat QuatNormalize(const Quat &q) {
  const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (n < 1.0e-12) return {{1.0, 0.0, 0.0, 0.0}};
  return {{q[0] / n, q[1] / n, q[2] / n, q[3] / n}};
}

// Hamilton product a * b ({w,x,y,z}).
Quat QuatMul(const Quat &a, const Quat &b) {
  return {{a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
           a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
           a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
           a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]}};
}

Quat QuatFromAxisAngle(const std::array<double, 3> &axis, double angle) {
  const double len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] +
                               axis[2] * axis[2]);
  if (len < 1.0e-12) return {{1.0, 0.0, 0.0, 0.0}};
  const double s = std::sin(angle * 0.5) / len;
  return {{std::cos(angle * 0.5), axis[0] * s, axis[1] * s, axis[2] * s}};
}

// Rotation that maps unit vector `from` onto unit vector `to`.
Quat QuatFromTwoVecs(const std::array<double, 3> &from,
                     const std::array<double, 3> &to) {
  auto norm = [](std::array<double, 3> v) -> std::array<double, 3> {
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n < 1.0e-12) return {{0.0, 0.0, 1.0}};
    return {{v[0] / n, v[1] / n, v[2] / n}};
  };
  const std::array<double, 3> a = norm(from);
  const std::array<double, 3> b = norm(to);
  const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  if (dot > 0.999999) return {{1.0, 0.0, 0.0, 0.0}};
  if (dot < -0.999999) {
    // Antiparallel: rotate pi about any axis orthogonal to `a`.
    const std::array<double, 3> ortho =
        std::abs(a[0]) < 0.9 ? std::array<double, 3>{{1.0, 0.0, 0.0}}
                             : std::array<double, 3>{{0.0, 1.0, 0.0}};
    const std::array<double, 3> axis{{a[1] * ortho[2] - a[2] * ortho[1],
                                      a[2] * ortho[0] - a[0] * ortho[2],
                                      a[0] * ortho[1] - a[1] * ortho[0]}};
    return QuatFromAxisAngle(axis, 3.14159265358979323846);
  }
  const std::array<double, 3> axis{{a[1] * b[2] - a[2] * b[1],
                                    a[2] * b[0] - a[0] * b[2],
                                    a[0] * b[1] - a[1] * b[0]}};
  return QuatNormalize({{1.0 + dot, axis[0], axis[1], axis[2]}});
}

// Quaternion from a rotation matrix given as its three column basis vectors.
Quat QuatFromBasis(const std::array<double, 3> &cx,
                   const std::array<double, 3> &cy,
                   const std::array<double, 3> &cz) {
  const double m00 = cx[0], m10 = cx[1], m20 = cx[2];
  const double m01 = cy[0], m11 = cy[1], m21 = cy[2];
  const double m02 = cz[0], m12 = cz[1], m22 = cz[2];
  const double trace = m00 + m11 + m22;
  Quat q{{1.0, 0.0, 0.0, 0.0}};
  if (trace > 0.0) {
    const double s = 0.5 / std::sqrt(trace + 1.0);
    q = {{0.25 / s, (m21 - m12) * s, (m02 - m20) * s, (m10 - m01) * s}};
  } else if (m00 > m11 && m00 > m22) {
    const double s = 2.0 * std::sqrt(1.0 + m00 - m11 - m22);
    q = {{(m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s}};
  } else if (m11 > m22) {
    const double s = 2.0 * std::sqrt(1.0 + m11 - m00 - m22);
    q = {{(m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s}};
  } else {
    const double s = 2.0 * std::sqrt(1.0 + m22 - m00 - m11);
    q = {{(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s}};
  }
  return QuatNormalize(q);
}

// Resolve a MuJoCo orientation specifier (quat / axisangle / euler / xyaxes /
// zaxis) into a {w,x,y,z} quaternion, honoring <compiler angle/eulerseq>.
Quat OrientationQuat(const std::string &quat_str,
                     const std::string &axisangle_str,
                     const std::string &euler_str,
                     const std::string &xyaxes_str,
                     const std::string &zaxis_str, const Context &ctx) {
  const std::vector<double> q = ParseDoubles(quat_str);
  if (q.size() >= 4) return QuatNormalize({{q[0], q[1], q[2], q[3]}});

  const std::vector<double> aa = ParseDoubles(axisangle_str);
  if (aa.size() >= 4) {
    return QuatFromAxisAngle({{aa[0], aa[1], aa[2]}}, aa[3] * ctx.angle_to_rad);
  }

  const std::vector<double> xy = ParseDoubles(xyaxes_str);
  if (xy.size() >= 6) {
    auto norm = [](std::array<double, 3> v) -> std::array<double, 3> {
      const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      if (n < 1.0e-12) return {{1.0, 0.0, 0.0}};
      return {{v[0] / n, v[1] / n, v[2] / n}};
    };
    const std::array<double, 3> x = norm({{xy[0], xy[1], xy[2]}});
    std::array<double, 3> y{{xy[3], xy[4], xy[5]}};
    const double d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    y = norm({{y[0] - d * x[0], y[1] - d * x[1], y[2] - d * x[2]}});
    const std::array<double, 3> z{{x[1] * y[2] - x[2] * y[1],
                                   x[2] * y[0] - x[0] * y[2],
                                   x[0] * y[1] - x[1] * y[0]}};
    return QuatFromBasis(x, y, z);
  }

  const std::vector<double> za = ParseDoubles(zaxis_str);
  if (za.size() >= 3) {
    return QuatFromTwoVecs({{0.0, 0.0, 1.0}}, {{za[0], za[1], za[2]}});
  }

  const std::vector<double> e = ParseDoubles(euler_str);
  if (e.size() >= 3) {
    Quat out{{1.0, 0.0, 0.0, 0.0}};
    for (size_t i = 0; i < 3 && i < ctx.eulerseq.size(); i++) {
      const char c = ctx.eulerseq[i];
      const char lower =
          static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::array<double, 3> axis{{0.0, 0.0, 0.0}};
      if (lower == 'x') axis = {{1.0, 0.0, 0.0}};
      else if (lower == 'y') axis = {{0.0, 1.0, 0.0}};
      else if (lower == 'z') axis = {{0.0, 0.0, 1.0}};
      else continue;
      const Quat qi = QuatFromAxisAngle(axis, e[i] * ctx.angle_to_rad);
      // Lowercase = intrinsic (post-multiply); uppercase = extrinsic (pre).
      out = std::islower(static_cast<unsigned char>(c)) ? QuatMul(out, qi)
                                                        : QuatMul(qi, out);
    }
    return QuatNormalize(out);
  }

  return {{1.0, 0.0, 0.0, 0.0}};
}

// Three.js Matrix4.elements (column-major) flat array from pos + quat.
std::vector<double> MatrixFromPosQuat(const std::array<double, 3> &pos,
                                      const Quat &q) {
  const double w = q[0];
  const double x = q[1];
  const double y = q[2];
  const double z = q[3];
  const double x2 = x + x;
  const double y2 = y + y;
  const double z2 = z + z;
  const double xx = x * x2;
  const double xy = x * y2;
  const double xz = x * z2;
  const double yy = y * y2;
  const double yz = y * z2;
  const double zz = z * z2;
  const double wx = w * x2;
  const double wy = w * y2;
  const double wz = w * z2;
  return {
      1.0 - (yy + zz), xy + wz,         xz - wy,         0.0,
      xy - wz,         1.0 - (xx + zz), yz + wx,         0.0,
      xz + wy,         yz - wx,         1.0 - (xx + yy), 0.0,
      pos[0],          pos[1],          pos[2],          1.0};
}

// Pose matrix for a body/geom node, reading any MuJoCo orientation specifier
// directly off the node (orientation is authored on the element, not in
// <default> classes).
std::vector<double> PoseMatrix(const pugi::xml_node &node, const Context &ctx) {
  const std::array<double, 3> pos =
      ParseDouble3(node ? Attr(node, "pos") : "", {{0.0, 0.0, 0.0}});
  const Quat q =
      node ? OrientationQuat(Attr(node, "quat"), Attr(node, "axisangle"),
                             Attr(node, "euler"), Attr(node, "xyaxes"),
                             Attr(node, "zaxis"), ctx)
           : Quat{{1.0, 0.0, 0.0, 0.0}};
  return MatrixFromPosQuat(pos, q);
}

// Column-major (Three.js Matrix4.elements) product a * b: index = col*4 + row.
// As a transform this applies `b` first, then `a` (a*(b*v)) -- so the rightmost
// argument is the innermost/local transform, matching Three.js Matrix4.multiply.
std::vector<double> MultiplyMatrix(const std::vector<double> &a,
                                   const std::vector<double> &b) {
  std::vector<double> out(16, 0.0);
  for (size_t c = 0; c < 4; c++) {
    for (size_t r = 0; r < 4; r++) {
      double s = 0.0;
      for (size_t k = 0; k < 4; k++) {
        s += a[k * 4 + r] * b[c * 4 + k];
      }
      out[c * 4 + r] = s;
    }
  }
  return out;
}

// 4x4 identity in the column-major flat layout used throughout.
std::vector<double> IdentityMatrix() {
  return {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
          0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
}

std::vector<double> ScaleMatrix(const std::array<double, 3> &scale) {
  return {scale[0], 0.0,      0.0,      0.0, 0.0, scale[1], 0.0,      0.0,
          0.0,      0.0,      scale[2], 0.0, 0.0, 0.0,      0.0,      1.0};
}

std::vector<double> TranslationMatrix(const std::array<double, 3> &t) {
  return {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
          0.0, 0.0, 1.0, 0.0, t[0], t[1], t[2], 1.0};
}

// Mesh frame transform from MJCF <mesh refpos/refquat>: vertices are first
// translated by -refpos, then rotated by conjugate(refquat). Returned as
// R(conj(refquat)) * T(-refpos) so it can left-compose with the geom matrix.
std::vector<double> MeshRefMatrix(const MeshAsset &asset) {
  std::vector<double> m = {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                           0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
  if (asset.has_refquat) {
    const Quat conj{{asset.refquat[0], -asset.refquat[1], -asset.refquat[2],
                     -asset.refquat[3]}};
    m = MatrixFromPosQuat({{0.0, 0.0, 0.0}}, QuatNormalize(conj));
  }
  if (asset.has_refpos) {
    m = MultiplyMatrix(m, TranslationMatrix({{-asset.refpos[0], -asset.refpos[1],
                                              -asset.refpos[2]}}));
  }
  return m;
}

void AppendTri(MeshData *mesh, const std::array<float, 3> &a,
               const std::array<float, 3> &b,
               const std::array<float, 3> &c) {
  const int32_t base = static_cast<int32_t>(mesh->positions.size() / 3);
  const std::array<std::array<float, 3>, 3> tri{{a, b, c}};
  for (size_t i = 0; i < tri.size(); i++) {
    const auto &v = tri[i];
    mesh->positions.push_back(v[0]);
    mesh->positions.push_back(v[1]);
    mesh->positions.push_back(v[2]);
    mesh->indices.push_back(base + static_cast<int32_t>(i));
  }
}

MeshData MakeBoxMesh(const std::array<double, 3> &half) {
  MeshData mesh;
  const float x = static_cast<float>(std::max(half[0], 1.0e-6));
  const float y = static_cast<float>(std::max(half[1], 1.0e-6));
  const float z = static_cast<float>(std::max(half[2], 1.0e-6));
  const std::array<std::array<float, 3>, 8> v{{
      {{-x, -y, -z}}, {{x, -y, -z}}, {{x, y, -z}}, {{-x, y, -z}},
      {{-x, -y, z}},  {{x, -y, z}},  {{x, y, z}},  {{-x, y, z}},
  }};
  const int faces[12][3] = {{0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6},
                            {0, 4, 5}, {0, 5, 1}, {1, 5, 6}, {1, 6, 2},
                            {2, 6, 7}, {2, 7, 3}, {3, 7, 4}, {3, 4, 0}};
  for (const auto &f : faces) AppendTri(&mesh, v[f[0]], v[f[1]], v[f[2]]);
  return mesh;
}

MeshData MakeSphereMesh(double radius, int slices = 16, int stacks = 8) {
  MeshData mesh;
  const float r = static_cast<float>(std::max(radius, 1.0e-6));
  std::vector<std::array<float, 3>> verts;
  for (int iy = 0; iy <= stacks; iy++) {
    const double v = double(iy) / double(stacks);
    const double phi = v * 3.14159265358979323846;
    for (int ix = 0; ix <= slices; ix++) {
      const double u = double(ix) / double(slices);
      const double theta = u * 2.0 * 3.14159265358979323846;
      verts.push_back({{static_cast<float>(r * std::sin(phi) * std::cos(theta)),
                        static_cast<float>(r * std::sin(phi) * std::sin(theta)),
                        static_cast<float>(r * std::cos(phi))}});
    }
  }
  for (int iy = 0; iy < stacks; iy++) {
    for (int ix = 0; ix < slices; ix++) {
      const int a = iy * (slices + 1) + ix;
      const int b = a + 1;
      const int c = a + slices + 1;
      const int d = c + 1;
      AppendTri(&mesh, verts[a], verts[c], verts[b]);
      AppendTri(&mesh, verts[b], verts[c], verts[d]);
    }
  }
  return mesh;
}

MeshData MakeCylinderMesh(double radius, double half_length, int slices = 20) {
  MeshData mesh;
  const float r = static_cast<float>(std::max(radius, 1.0e-6));
  const float h = static_cast<float>(std::max(half_length, 1.0e-6));
  const std::array<float, 3> top{{0.0f, 0.0f, h}};
  const std::array<float, 3> bottom{{0.0f, 0.0f, -h}};
  for (int i = 0; i < slices; i++) {
    const double a0 = 2.0 * 3.14159265358979323846 * double(i) / double(slices);
    const double a1 = 2.0 * 3.14159265358979323846 * double(i + 1) / double(slices);
    const std::array<float, 3> p0{{static_cast<float>(r * std::cos(a0)),
                                   static_cast<float>(r * std::sin(a0)), -h}};
    const std::array<float, 3> p1{{static_cast<float>(r * std::cos(a1)),
                                   static_cast<float>(r * std::sin(a1)), -h}};
    const std::array<float, 3> p2{{p0[0], p0[1], h}};
    const std::array<float, 3> p3{{p1[0], p1[1], h}};
    AppendTri(&mesh, p0, p2, p1);
    AppendTri(&mesh, p1, p2, p3);
    AppendTri(&mesh, bottom, p1, p0);
    AppendTri(&mesh, top, p2, p3);
  }
  return mesh;
}

float ReadFloatLE(const std::vector<uint8_t> &bytes, size_t offset) {
  float v = 0.0f;
  std::memcpy(&v, bytes.data() + offset, sizeof(float));
  return v;
}

// MuJoCo removes repeated STL vertices and computes one normal per vertex:
// face normals weighted by area, and (unless smoothnormal) a face whose normal
// deviates from the vertex average by more than acos(0.8) is left out so
// sharp edges stay sharp. Reproduce that so the USD mesh is indexed (an STL
// is a triangle soup, ~6x the unique positions) and shades like MuJoCo.
void ComputeMujocoVertexNormals(MeshData *mesh, bool smoothnormal) {
  const size_t nv = mesh->positions.size() / 3;
  const size_t nf = mesh->indices.size() / 3;
  std::vector<double> acc(nv * 3, 0.0), face(nf * 3, 0.0);
  auto P = [&](int32_t i, int c) { return double(mesh->positions[size_t(i) * 3 + c]); };
  for (size_t f = 0; f < nf; ++f) {
    const int32_t a = mesh->indices[f * 3], b = mesh->indices[f * 3 + 1],
                  c = mesh->indices[f * 3 + 2];
    const double e1[3] = {P(b, 0) - P(a, 0), P(b, 1) - P(a, 1), P(b, 2) - P(a, 2)};
    const double e2[3] = {P(c, 0) - P(a, 0), P(c, 1) - P(a, 1), P(c, 2) - P(a, 2)};
    const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                         e1[0] * e2[1] - e1[1] * e2[0]};  // length = 2 * area
    for (int k = 0; k < 3; ++k) face[f * 3 + k] = n[k];
    for (int32_t v : {a, b, c}) {
      for (int k = 0; k < 3; ++k) acc[size_t(v) * 3 + k] += n[k];
    }
  }
  auto normalized = [](const double *v, double out[3]) {
    const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int k = 0; k < 3; ++k) out[k] = len > 0.0 ? v[k] / len : 0.0;
    return len > 0.0;
  };
  std::vector<double> sum = acc;
  if (!smoothnormal) {
    for (size_t f = 0; f < nf; ++f) {
      double uf[3];
      if (!normalized(&face[f * 3], uf)) continue;
      for (int j = 0; j < 3; ++j) {
        const size_t v = size_t(mesh->indices[f * 3 + j]);
        double avg[3];
        if (!normalized(&acc[v * 3], avg)) continue;
        if (avg[0] * uf[0] + avg[1] * uf[1] + avg[2] * uf[2] < 0.8) {
          for (int k = 0; k < 3; ++k) sum[v * 3 + k] -= face[f * 3 + k];
        }
      }
    }
  }
  mesh->normals.assign(nv * 3, 0.0f);
  for (size_t v = 0; v < nv; ++v) {
    double n[3];
    // Every face was excluded (a spike vertex): fall back to the full average.
    if (!normalized(&sum[v * 3], n)) normalized(&acc[v * 3], n);
    for (int k = 0; k < 3; ++k) mesh->normals[v * 3 + k] = float(n[k]);
  }
}

bool LoadBinarySTL(const fs::path &filename, bool smoothnormal, MeshData *mesh,
                   std::string *err) {
  fs::ifstream ifs(filename, std::ios::binary);
  if (!ifs) {
    if (err) *err = "Failed to open STL: " + filename.string();
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
  if (bytes.size() < 84) {
    if (err) *err = "STL is too small: " + filename.string();
    return false;
  }
  uint32_t tri_count = 0;
  std::memcpy(&tri_count, bytes.data() + 80, sizeof(uint32_t));
  const uint64_t expected = 84ull + uint64_t(tri_count) * 50ull;
  if (expected != bytes.size()) {
    if (err) *err = "Only binary STL with exact triangle count is supported: " + filename.string();
    return false;
  }

  // Weld bit-identical positions (MuJoCo's repeated-vertex removal).
  struct Key {
    uint32_t x, y, z;
    bool operator<(const Key &o) const {
      return x != o.x ? x < o.x : (y != o.y ? y < o.y : z < o.z);
    }
  };
  std::map<Key, int32_t> welded;
  mesh->positions.clear();
  mesh->indices.clear();
  mesh->indices.reserve(size_t(tri_count) * 3);
  for (uint32_t i = 0; i < tri_count; i++) {
    const size_t base = 84ull + size_t(i) * 50ull;
    for (size_t v = 0; v < 3; v++) {
      const size_t vo = base + 12 + v * 12;
      Key key;
      std::memcpy(&key.x, bytes.data() + vo + 0, 4);
      std::memcpy(&key.y, bytes.data() + vo + 4, 4);
      std::memcpy(&key.z, bytes.data() + vo + 8, 4);
      auto found = welded.find(key);
      if (found == welded.end()) {
        const int32_t index = static_cast<int32_t>(mesh->positions.size() / 3);
        found = welded.emplace(key, index).first;
        mesh->positions.push_back(ReadFloatLE(bytes, vo + 0));
        mesh->positions.push_back(ReadFloatLE(bytes, vo + 4));
        mesh->positions.push_back(ReadFloatLE(bytes, vo + 8));
      }
      mesh->indices.push_back(found->second);
    }
  }
  ComputeMujocoVertexNormals(mesh, smoothnormal);
  return true;
}

bool LoadOBJ(const fs::path &filename, MeshData *mesh, std::string *err) {
  fs::ifstream ifs(filename);
  if (!ifs) {
    if (err) *err = "Failed to open OBJ: " + filename.string();
    return false;
  }

  std::vector<std::array<float, 3>> vertices;
  std::vector<std::array<float, 2>> texcoords;  // vt (u,v)
  std::map<std::pair<int, int>, int32_t> corner_index;  // (v, vt) -> output vertex
  bool all_have_uv = true;  // drop UVs unless every face-vertex has a vt index
  std::string line;
  while (std::getline(ifs, line)) {
    std::stringstream ss(line);
    std::string tag;
    ss >> tag;
    if (tag == "v") {
      std::array<float, 3> v{{0.0f, 0.0f, 0.0f}};
      ss >> v[0] >> v[1] >> v[2];
      vertices.push_back(v);
    } else if (tag == "vt") {
      std::array<float, 2> t{{0.0f, 0.0f}};
      ss >> t[0] >> t[1];
      texcoords.push_back(t);
    } else if (tag == "f") {
      std::vector<int> face;      // vertex indices
      std::vector<int> face_uv;   // parallel vt indices (-1 if absent)
      std::string tok;
      while (ss >> tok) {
        const size_t slash = tok.find('/');
        int idx_val = 0;
        if (!ParseInt(slash == std::string::npos ? tok : tok.substr(0, slash), &idx_val)) {
          std::cerr << "Invalid face vertex index: " << tok << "\n";
          return false;
        }
        face.push_back(idx_val > 0 ? idx_val - 1 : static_cast<int>(vertices.size()) + idx_val);
        // OBJ face token: v/vt/vn (vt optional). Parse the vt field.
        int vt = -1;
        if (slash != std::string::npos) {
          const size_t slash2 = tok.find('/', slash + 1);
          const std::string vt_str = tok.substr(
              slash + 1, slash2 == std::string::npos ? std::string::npos : slash2 - slash - 1);
          int vt_val = 0;
          if (!vt_str.empty() && ParseInt(vt_str, &vt_val)) {
            vt = vt_val > 0 ? vt_val - 1 : static_cast<int>(texcoords.size()) + vt_val;
          }
        }
        if (vt < 0) all_have_uv = false;
        face_uv.push_back(vt);
      }
      for (size_t i = 1; i + 1 < face.size(); i++) {
        const int tri[3] = {face[0], face[i], face[i + 1]};
        const int triuv[3] = {face_uv[0], face_uv[i], face_uv[i + 1]};
        bool valid = true;
        for (int k = 0; k < 3; k++) {
          valid = valid && tri[k] >= 0 && size_t(tri[k]) < vertices.size();
        }
        if (!valid) continue;
        for (int k = 0; k < 3; k++) {
          // Keep the OBJ's own indexing: one output vertex per (v, vt) pair.
          const int ti = (triuv[k] >= 0 && size_t(triuv[k]) < texcoords.size())
                             ? triuv[k] : -1;
          const std::pair<int, int> key{tri[k], ti};
          auto found = corner_index.find(key);
          if (found == corner_index.end()) {
            const auto &v = vertices[size_t(tri[k])];
            found = corner_index
                        .emplace(key, static_cast<int32_t>(mesh->positions.size() / 3))
                        .first;
            mesh->positions.push_back(v[0]);
            mesh->positions.push_back(v[1]);
            mesh->positions.push_back(v[2]);
            mesh->uvs.push_back(ti >= 0 ? texcoords[size_t(ti)][0] : 0.0f);
            mesh->uvs.push_back(ti >= 0 ? texcoords[size_t(ti)][1] : 0.0f);
          }
          mesh->indices.push_back(found->second);
        }
      }
    }
  }
  if (!all_have_uv || texcoords.empty()) mesh->uvs.clear();
  return !mesh->positions.empty();
}

bool LoadMeshFile(const fs::path &filename, bool smoothnormal, MeshData *mesh,
                  std::string *err) {
  const std::string ext = ToLower(filename.extension().string());
  if (ext == ".stl") return LoadBinarySTL(filename, smoothnormal, mesh, err);
  if (ext == ".obj") return LoadOBJ(filename, mesh, err);
  if (err) *err = "Unsupported native mesh extension: " + ext;
  return false;
}

nlohmann::json MeshPayloadToJson(const MeshPayload &payload) {
  if (payload.shape.is_object()) {
    return {
        {"name", payload.name},
        {"matrix", payload.matrix},
        {"shape", payload.shape}};
  }
  nlohmann::json geometry = {{"positions", payload.mesh.positions},
                             {"normals", payload.mesh.normals},
                             {"indices", payload.mesh.indices}};
  if (!payload.mesh.uvs.empty()) geometry["uvs"] = payload.mesh.uvs;
  return {{"name", payload.name},
          {"matrix", payload.matrix},
          {"geometry", std::move(geometry)}};
}

// --- MJCF <default> class resolution ---------------------------------------
//
// MuJoCo geoms/joints inherit attributes from a <default> class tree. The
// applicable class is the element's own `class`, else the enclosing body's
// `childclass`, else the unnamed/"main" default. Classes inherit from their
// parent <default>. We resolve an element's effective value as: own attribute
// wins, otherwise the resolved class default, otherwise a fallback.

std::string Eff(const pugi::xml_node &node, const AttrMap &cls,
                const std::string &name, const std::string &fallback = "") {
  if (node && node.attribute(name.c_str())) {
    return node.attribute(name.c_str()).as_string();
  }
  const auto it = cls.find(name);
  return it != cls.end() ? it->second : fallback;
}

bool HasEff(const pugi::xml_node &node, const AttrMap &cls,
            const std::string &name) {
  return (node && node.attribute(name.c_str())) || cls.count(name) > 0;
}

double EffDouble(const pugi::xml_node &node, const AttrMap &cls,
                 const std::string &name, double fallback) {
  if (!HasEff(node, cls, name)) return fallback;
  const std::string s = Eff(node, cls, name);
  char *end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  return (end && end != s.c_str()) ? v : fallback;
}

void WalkDefault(const pugi::xml_node &node, const AttrMap &parent_geom,
                 const AttrMap &parent_joint, Defaults *d) {
  AttrMap g = parent_geom;
  AttrMap j = parent_joint;
  if (const auto gn = Child(node, "geom")) {
    for (const auto &a : gn.attributes()) g[a.name()] = a.value();
  }
  if (const auto jn = Child(node, "joint")) {
    for (const auto &a : jn.attributes()) j[a.name()] = a.value();
  }
  const std::string cls = Attr(node, "class");
  if (cls.empty() || cls == "main") {
    d->root_geom = g;
    d->root_joint = j;
  }
  if (!cls.empty()) {
    d->geom[cls] = g;
    d->joint[cls] = j;
  }
  for (const auto &child : Children(node, "default")) {
    WalkDefault(child, g, j, d);
  }
}

Defaults ParseDefaults(const pugi::xml_node &root) {
  Defaults d;
  for (const auto &def : Children(root, "default")) {
    WalkDefault(def, AttrMap{}, AttrMap{}, &d);
  }
  return d;
}

const AttrMap &ResolveGeomClass(const pugi::xml_node &geom_node,
                                const Defaults &defaults,
                                const std::string &childclass) {
  std::string cls = Attr(geom_node, "class");
  if (cls.empty()) cls = childclass;
  if (!cls.empty()) {
    const auto it = defaults.geom.find(cls);
    if (it != defaults.geom.end()) return it->second;
  }
  return defaults.root_geom;
}

const AttrMap &ResolveJointClass(const pugi::xml_node &joint_node,
                                 const Defaults &defaults,
                                 const std::string &childclass) {
  std::string cls = joint_node ? Attr(joint_node, "class") : std::string();
  if (cls.empty()) cls = childclass;
  if (!cls.empty()) {
    const auto it = defaults.joint.find(cls);
    if (it != defaults.joint.end()) return it->second;
  }
  return defaults.root_joint;
}

void AddGeomPhysicsAttrs(const pugi::xml_node &geom_node, const AttrMap &cls,
                         nlohmann::json *geom_json) {
  if (!geom_json) return;
  if (HasEff(geom_node, cls, "group")) {
    (*geom_json)["group"] =
        static_cast<int32_t>(EffDouble(geom_node, cls, "group", 3.0));
  }
  if (HasEff(geom_node, cls, "condim")) {
    (*geom_json)["condim"] =
        static_cast<int32_t>(EffDouble(geom_node, cls, "condim", 3.0));
  }
  if (HasEff(geom_node, cls, "margin")) {
    (*geom_json)["mjc"]["margin"] = EffDouble(geom_node, cls, "margin", 0.0);
  }
  if (HasEff(geom_node, cls, "solmix")) {
    (*geom_json)["mjc"]["solmix"] = EffDouble(geom_node, cls, "solmix", 1.0);
  }
}

// Basename -> path index of mesh files under `root_dir`. Used as a last-resort
// fallback when a <mesh file="..."> path does not resolve against meshdir --
// e.g. MuJoCo scenes whose assets are referenced relative to an included
// file's directory (the include flattening here drops that provenance).
// Names that occur in more than one location are dropped to avoid ambiguity.
std::map<std::string, fs::path> BuildMeshIndex(const fs::path &root_dir) {
  std::map<std::string, fs::path> index;
  std::set<std::string> ambiguous;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root_dir, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string ext = ToLower(it->path().extension().string());
    if (ext != ".stl" && ext != ".obj" && ext != ".msh") continue;
    const std::string key = ToLower(it->path().filename().string());
    if (index.count(key)) {
      ambiguous.insert(key);
    } else {
      index[key] = it->path();
    }
  }
  for (const auto &a : ambiguous) index.erase(a);
  return index;
}

fs::path ResolveMeshPath(const fs::path &base_dir, const fs::path &mesh_dir,
                         const std::string &file,
                         const std::map<std::string, fs::path> &index) {
  std::error_code ec;
  const std::array<fs::path, 3> candidates{{mesh_dir / file,
                                            base_dir / "assets" / file,
                                            base_dir / file}};
  for (const auto &c : candidates) {
    if (fs::exists(c, ec)) return c;
  }
  const auto it = index.find(ToLower(fs::path(file).filename().string()));
  if (it != index.end()) return it->second;
  return mesh_dir / file;  // primary candidate; errors later with a clear msg
}

std::map<std::string, MeshAsset> CollectMujocoAssets(
    const pugi::xml_node &root, const fs::path &base_dir) {
  std::map<std::string, MeshAsset> assets;
  const auto compiler = Child(root, "compiler");
  // MuJoCo <compiler meshdir> takes precedence; assetdir is the shared
  // default for meshdir/texturedir when meshdir is absent.
  std::string dir_attr = compiler ? Attr(compiler, "meshdir") : std::string();
  if (dir_attr.empty() && compiler) dir_attr = Attr(compiler, "assetdir");
  const fs::path mesh_dir = dir_attr.empty() ? base_dir : base_dir / dir_attr;
  const std::map<std::string, fs::path> index = BuildMeshIndex(base_dir);

  for (const auto &asset_node : Children(root, "asset")) {
    for (const auto &mesh_node : Children(asset_node, "mesh")) {
      const std::string file = Attr(mesh_node, "file");
      if (file.empty()) continue;
      std::string name = Attr(mesh_node, "name");
      if (name.empty()) name = fs::path(file).stem().string();
      MeshAsset asset;
      asset.path = ResolveMeshPath(base_dir, mesh_dir, file, index);
      asset.scale =
          ParseDouble3(Attr(mesh_node, "scale"), {{1.0, 1.0, 1.0}});
      if (HasAttr(mesh_node, "refpos")) {
        asset.refpos = ParseDouble3(Attr(mesh_node, "refpos"), {{0, 0, 0}});
        asset.has_refpos = true;
      }
      asset.smoothnormal = Attr(mesh_node, "smoothnormal") == "true";
      const std::vector<double> rq = ParseDoubles(Attr(mesh_node, "refquat"));
      if (rq.size() >= 4) {
        asset.refquat = {{rq[0], rq[1], rq[2], rq[3]}};
        asset.has_refquat = true;
      }
      assets[name] = asset;
    }
  }
  return assets;
}

// Normalize raw elevation samples to [0,1] the way MuJoCo's compiler does:
// subtract the min, divide by the range (constant fields collapse to 0).
void NormalizeHField(std::vector<float> *data) {
  if (data->empty()) return;
  float emin = (*data)[0], emax = (*data)[0];
  for (float v : *data) { emin = std::min(emin, v); emax = std::max(emax, v); }
  const float range = emax - emin;
  for (float &v : *data) {
    v -= emin;
    if (range > 1e-9f) v /= range;
  }
}

// Collect <asset><hfield> declarations. File-based fields decode a (grayscale)
// PNG with rows reversed (MuJoCo stores row 0 at the -y edge); inline fields
// read nrow/ncol/elevation. All are normalized to [0,1].
std::map<std::string, HFieldAsset> CollectMujocoHFields(
    const pugi::xml_node &root, const fs::path &base_dir) {
  std::map<std::string, HFieldAsset> hfields;
  const auto compiler = Child(root, "compiler");
  std::string dir_attr = compiler ? Attr(compiler, "meshdir") : std::string();
  if (dir_attr.empty() && compiler) dir_attr = Attr(compiler, "assetdir");
  const fs::path mesh_dir = dir_attr.empty() ? base_dir : base_dir / dir_attr;
  const std::map<std::string, fs::path> index = BuildMeshIndex(base_dir);

  for (const auto &asset_node : Children(root, "asset")) {
    for (const auto &hf_node : Children(asset_node, "hfield")) {
      std::string name = Attr(hf_node, "name");
      const std::string file = Attr(hf_node, "file");
      if (name.empty()) {
        name = file.empty() ? std::string() : fs::path(file).stem().string();
      }
      if (name.empty()) continue;
      HFieldAsset hf;
      const std::vector<double> sz = ParseDoubles(Attr(hf_node, "size"));
      for (size_t i = 0; i < 4 && i < sz.size(); ++i) hf.size[i] = sz[i];

      if (!file.empty()) {
        const fs::path path =
            ResolveMeshPath(base_dir, mesh_dir, file, index);
        int w = 0, h = 0, comp = 0;
        unsigned char *img =
            stbi_load(path.string().c_str(), &w, &h, &comp, 1);  // force grey
        if (img && w > 0 && h > 0) {
          hf.ncol = w;
          hf.nrow = h;
          hf.data.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
          for (int r = 0; r < h; ++r) {
            for (int c = 0; c < w; ++c) {
              // reverse rows: data row 0 is the bottom (-y) image row
              hf.data[static_cast<size_t>(r) * w + c] =
                  static_cast<float>(img[c + (h - 1 - r) * w]);
            }
          }
        }
        if (img) stbi_image_free(img);
      } else {
        hf.nrow = static_cast<int>(ParseDoubleAttr(hf_node, "nrow", 0.0));
        hf.ncol = static_cast<int>(ParseDoubleAttr(hf_node, "ncol", 0.0));
        const std::vector<double> elev = ParseDoubles(Attr(hf_node, "elevation"));
        if (hf.nrow > 0 && hf.ncol > 0 &&
            elev.size() == static_cast<size_t>(hf.nrow) * hf.ncol) {
          hf.data.assign(elev.begin(), elev.end());
        }
      }
      if (hf.nrow > 0 && hf.ncol > 0 && !hf.data.empty()) {
        NormalizeHField(&hf.data);
        hfields[name] = std::move(hf);
      }
    }
  }
  return hfields;
}

// Tessellate a heightfield's top surface into a triangle mesh in the field's
// local frame: x spans [-rx,rx] across ncol columns, y spans [-ry,ry] across
// nrow rows, z = normalized_elevation * elevation_z. Per-vertex normals come
// from the local height gradient (smooth shading).
MeshData TessellateHField(const HFieldAsset &hf) {
  MeshData mesh;
  const int nr = hf.nrow, nc = hf.ncol;
  if (nr < 2 || nc < 2) return mesh;
  const double rx = hf.size[0], ry = hf.size[1], ez = hf.size[2];
  const double dx = (2.0 * rx) / (nc - 1);
  const double dy = (2.0 * ry) / (nr - 1);
  auto H = [&](int r, int c) -> double {
    r = std::max(0, std::min(nr - 1, r));
    c = std::max(0, std::min(nc - 1, c));
    return static_cast<double>(hf.data[static_cast<size_t>(r) * nc + c]) * ez;
  };
  mesh.positions.reserve(static_cast<size_t>(nr) * nc * 3);
  mesh.normals.reserve(static_cast<size_t>(nr) * nc * 3);
  for (int r = 0; r < nr; ++r) {
    for (int c = 0; c < nc; ++c) {
      mesh.positions.push_back(static_cast<float>(-rx + c * dx));
      mesh.positions.push_back(static_cast<float>(-ry + r * dy));
      mesh.positions.push_back(static_cast<float>(H(r, c)));
      // Normal from the height-field gradient. Use the ACTUAL neighbor span so
      // the border ring is a correct one-sided difference (interior: 2 cells;
      // edges: 1 cell) rather than a half-magnitude central difference.
      const int cl = std::max(0, c - 1), cr = std::min(nc - 1, c + 1);
      const int rb = std::max(0, r - 1), rt = std::min(nr - 1, r + 1);
      const double hx = (H(r, cr) - H(r, cl)) / ((cr - cl) * dx);
      const double hy = (H(rt, c) - H(rb, c)) / ((rt - rb) * dy);
      double nx = -hx, ny = -hy, nz = 1.0;
      const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
      if (len > 1e-12) { nx /= len; ny /= len; nz /= len; }
      mesh.normals.push_back(static_cast<float>(nx));
      mesh.normals.push_back(static_cast<float>(ny));
      mesh.normals.push_back(static_cast<float>(nz));
    }
  }
  mesh.indices.reserve(static_cast<size_t>(nr - 1) * (nc - 1) * 6);
  for (int r = 0; r < nr - 1; ++r) {
    for (int c = 0; c < nc - 1; ++c) {
      const int32_t v00 = r * nc + c;
      const int32_t v01 = r * nc + (c + 1);
      const int32_t v10 = (r + 1) * nc + c;
      const int32_t v11 = (r + 1) * nc + (c + 1);
      // two CCW triangles (viewed from +z)
      mesh.indices.push_back(v00);
      mesh.indices.push_back(v01);
      mesh.indices.push_back(v11);
      mesh.indices.push_back(v00);
      mesh.indices.push_back(v11);
      mesh.indices.push_back(v10);
    }
  }
  return mesh;
}

bool BuildGeomPayload(const pugi::xml_node &geom_node, const AttrMap &cls,
                      const std::string &cls_name,
                      const std::map<std::string, MeshAsset> &assets,
                      const std::map<std::string, HFieldAsset> &hfields,
                      const Options &opts, const Context &ctx,
                      const std::vector<double> &body_world,
                      MeshPayload *payload, bool *is_visual, std::string *err) {
  const std::string type =
      HasEff(geom_node, cls, "type") ? Eff(geom_node, cls, "type") :
      HasEff(geom_node, cls, "mesh") ? "mesh" : "sphere";
  payload->name = Attr(geom_node, "name");
  if (payload->name.empty()) payload->name = Eff(geom_node, cls, "mesh", "geom");
  // Bake the accumulated body-chain world transform into the geom matrix: the
  // USD converter places every link Xform at identity, so each geom must carry
  // its full world placement (body_world * geom-local pose). The geom pose may
  // be inherited from a <default> class (e.g. allegro fingertip geoms get
  // pos="0 0 0.0267" from class="fingertip_visual"), so resolve via Eff.
  const std::array<double, 3> geom_pos =
      ParseDouble3(Eff(geom_node, cls, "pos"), {{0.0, 0.0, 0.0}});
  const Quat geom_quat = OrientationQuat(
      Eff(geom_node, cls, "quat"), Eff(geom_node, cls, "axisangle"),
      Eff(geom_node, cls, "euler"), Eff(geom_node, cls, "xyaxes"),
      Eff(geom_node, cls, "zaxis"), ctx);
  payload->matrix =
      MultiplyMatrix(body_world, MatrixFromPosQuat(geom_pos, geom_quat));

  // Classification mirrors the JS converter: a resolved geom group (own or
  // inherited from <default>) wins -- groups 0-2 are visible, 3-5 are
  // collision. With no group anywhere, fall back to the class name and then
  // MuJoCo's contype/conaffinity==0 (visual-only) convention.
  if (HasEff(geom_node, cls, "group")) {
    (*is_visual) =
        static_cast<int>(EffDouble(geom_node, cls, "group", 3.0)) < 3;
  } else {
    const std::string lname = ToLower(cls_name);
    if (lname.find("collision") != std::string::npos ||
        lname.find("collider") != std::string::npos) {
      (*is_visual) = false;
    } else if (Eff(geom_node, cls, "contype") == "0" &&
               Eff(geom_node, cls, "conaffinity") == "0") {
      (*is_visual) = true;
    } else {
      (*is_visual) = true;  // default group 0 is visible
    }
  }

  if (type == "mesh") {
    const std::string mesh_name = Eff(geom_node, cls, "mesh");
    const auto it = assets.find(mesh_name);
    if (it == assets.end()) {
      if (err) *err = "Missing MJCF mesh asset: " + mesh_name;
      return false;
    }
    if (!LoadMeshFile(it->second.path, it->second.smoothnormal, &payload->mesh, err))
      return false;

    const std::array<double, 3> scale =
        ParseDouble3(Eff(geom_node, cls, "scale"), it->second.scale);
    // G * refframe(refpos/refquat) * scale, matching the JS mesh handling.
    payload->matrix = MultiplyMatrix(payload->matrix, MeshRefMatrix(it->second));
    payload->matrix = MultiplyMatrix(payload->matrix, ScaleMatrix(scale));
  } else if (type == "hfield") {
    // <geom type="hfield" hfield="name"> -> tessellated GeomMesh (top surface).
    const std::string hf_name = Eff(geom_node, cls, "hfield");
    const auto it = hfields.find(hf_name);
    if (it == hfields.end()) {
      if (err) *err = "Missing MJCF hfield asset: " + hf_name;
      return false;
    }
    payload->mesh = TessellateHField(it->second);
    if (payload->mesh.positions.empty()) {
      if (err) *err = "Empty/invalid hfield asset: " + hf_name;
      return false;
    }
  } else if (type == "box") {
    const auto half = ParseDouble3(Eff(geom_node, cls, "size"),
                                   {{0.05, 0.05, 0.05}});
    if (!(*is_visual) && !opts.tessellate_collision_shapes) {
      payload->shape = {{"type", "box"}};
      payload->matrix = MultiplyMatrix(payload->matrix, ScaleMatrix(half));
    } else {
      payload->mesh = MakeBoxMesh(half);
    }
  } else if (type == "sphere") {
    const auto size = ParseDoubles(Eff(geom_node, cls, "size"));
    const double radius = size.empty() ? 0.05 : size[0];
    if (!(*is_visual) && !opts.tessellate_collision_shapes) {
      payload->shape = {{"type", "sphere"}, {"radius", radius}};
    } else {
      payload->mesh = MakeSphereMesh(radius);
    }
  } else if (type == "ellipsoid") {
    const auto radii = ParseDouble3(Eff(geom_node, cls, "size"),
                                    {{0.05, 0.05, 0.05}});
    if (!(*is_visual) && !opts.tessellate_collision_shapes) {
      payload->shape = {{"type", "sphere"}, {"radius", 1.0}};
      payload->matrix = MultiplyMatrix(payload->matrix, ScaleMatrix(radii));
    } else {
      payload->mesh = MakeSphereMesh(1.0);
      payload->matrix = MultiplyMatrix(payload->matrix, ScaleMatrix(radii));
    }
  } else if (type == "cylinder" || type == "capsule") {
    const auto size = ParseDoubles(Eff(geom_node, cls, "size"));
    const double radius = size.empty() ? 0.05 : size[0];
    double half_length = size.size() >= 2 ? size[1] : radius;
    const auto fromto = ParseDoubles(Eff(geom_node, cls, "fromto"));
    if (fromto.size() >= 6) {
      const std::array<double, 3> dir{{fromto[3] - fromto[0],
                                       fromto[4] - fromto[1],
                                       fromto[5] - fromto[2]}};
      half_length =
          0.5 * std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
      // fromto fully specifies the geom frame (midpoint + Z aligned to dir),
      // overriding the geom's own pos/quat; still placed in body world space.
      const std::array<double, 3> center{{0.5 * (fromto[0] + fromto[3]),
                                          0.5 * (fromto[1] + fromto[4]),
                                          0.5 * (fromto[2] + fromto[5])}};
      payload->matrix = MultiplyMatrix(
          body_world,
          MatrixFromPosQuat(center, QuatFromTwoVecs({{0.0, 0.0, 1.0}}, dir)));
    }
    if (!(*is_visual) && !opts.tessellate_collision_shapes) {
      payload->shape = {{"type", type},
                        {"radius", radius},
                        {"height", half_length * 2.0},
                        {"axis", "Z"}};
    } else {
      payload->mesh = MakeCylinderMesh(radius, half_length);
    }
  } else if (type == "plane") {
    const auto size = ParseDoubles(Eff(geom_node, cls, "size"));
    const double sx = size.size() >= 1 && size[0] > 0.0 ? size[0] : 1.0;
    const double sy = size.size() >= 2 && size[1] > 0.0 ? size[1] : 1.0;
    const double sz = size.size() >= 3 && size[2] > 0.0 ? size[2] : 0.001;
    if (!(*is_visual) && !opts.tessellate_collision_shapes) {
      payload->shape = {{"type", "plane"},
                        {"width", sx * 2.0},
                        {"length", sy * 2.0},
                        {"axis", "Z"}};
    } else {
      payload->mesh = MakeBoxMesh({{sx, sy, sz}});
    }
  } else {
    if (err) *err = "Unsupported native MJCF geom type: " + type;
    return false;
  }
  return true;
}

std::string AxisToken(const std::array<double, 3> &axis) {
  const double ax = std::abs(axis[0]);
  const double ay = std::abs(axis[1]);
  const double az = std::abs(axis[2]);
  if (ay >= ax && ay >= az) return "Y";
  if (az >= ax && az >= ay) return "Z";
  return "X";
}

nlohmann::json InertialToJson(const pugi::xml_node &body_node) {
  nlohmann::json inertial = nlohmann::json::object();
  const auto inertial_node = Child(body_node, "inertial");
  if (!inertial_node) return inertial;
  inertial["mass"] = ParseDoubleAttr(inertial_node, "mass", 0.0);
  const auto com =
      ParseDouble3(Attr(inertial_node, "pos"), {{0.0, 0.0, 0.0}});
  inertial["centerOfMass"] = {com[0], com[1], com[2]};
  // MJCF inertia: <inertial fullinertia="Ixx Iyy Izz Ixy Ixz Iyz"> (full
  // symmetric tensor) or the more common <inertial diaginertia="Ixx Iyy Izz">.
  const std::vector<double> full =
      ParseDoubles(Attr(inertial_node, "fullinertia"));
  if (full.size() >= 6) {
    // Carry all 6 components so the converter can diagonalize them into
    // diagonalInertia (eigenvalues) + principalAxes (eigenvector quaternion).
    // Keep a diagonal fallback for consumers that ignore fullInertia.
    inertial["fullInertia"] = {full[0], full[1], full[2],
                               full[3], full[4], full[5]};
    inertial["diagonalInertia"] = {full[0], full[1], full[2]};
  } else if (full.size() >= 3) {
    inertial["diagonalInertia"] = {full[0], full[1], full[2]};
  } else {
    const std::vector<double> diag =
        ParseDoubles(Attr(inertial_node, "diaginertia"));
    if (diag.size() >= 3) {
      inertial["diagonalInertia"] = {diag[0], diag[1], diag[2]};
    }
  }
  return inertial;
}

// Build one joint JSON entry from an explicit <joint> node (or a null node for
// a fixed connection), wiring parent_name -> child_name with the given origin
// frame. `origin_matrix` (16 elems) and `origin_pos` are the joint frame; in a
// multi-DOF chain only the first joint carries the body offset (the rest are at
// identity). A null joint_node yields a PhysicsFixedJoint.
void AddJointFromNode(const pugi::xml_node &joint_node, const AttrMap &jcls,
                      const Context &ctx, const std::string &parent_name,
                      const std::string &child_name,
                      const std::vector<double> &origin_matrix,
                      const std::array<double, 3> &origin_pos,
                      nlohmann::json *joints) {
  nlohmann::json joint = nlohmann::json::object();
  joint["name"] = joint_node ? Attr(joint_node, "name", parent_name + "_to_" + child_name)
                             : parent_name + "_to_" + child_name + "_fixed";
  const std::string mj_type =
      joint_node ? Eff(joint_node, jcls, "type", "hinge") : "fixed";
  // MuJoCo ball (3-DOF rotation) -> USD PhysicsSphericalJoint. Matches the JS
  // parsers (web/js/cli/urdf-to-usd.js, web/js/urdf.js); the USD converter
  // already handles "spherical". (free/floating base is still emitted as
  // "fixed" here — handled separately as the articulation root.)
  joint["type"] = (mj_type == "hinge") ? "revolute" :
                  (mj_type == "slide") ? "prismatic" :
                  (mj_type == "ball")  ? "spherical" : "fixed";
  joint["parent"] = parent_name;
  joint["child"] = child_name;
  const auto axis = ParseDouble3(joint_node ? Eff(joint_node, jcls, "axis") : "",
                                 {{0.0, 0.0, 1.0}});
  joint["axis"] = {axis[0], axis[1], axis[2]};
  joint["axisToken"] = AxisToken(axis);
  joint["origin"] = {origin_pos[0], origin_pos[1], origin_pos[2]};
  joint["originMatrix"] = origin_matrix;

  if (joint_node) {
    const std::vector<double> range = ParseDoubles(Eff(joint_node, jcls, "range"));
    if (range.size() >= 2) {
      // The USD converter expects revolute limits in radians (it re-converts to
      // degrees). MJCF hinge ranges are in the compiler's angle unit (degrees by
      // default), so convert them; slide ranges are meters and pass through.
      const double scale = (mj_type == "hinge") ? ctx.angle_to_rad : 1.0;
      joint["limit"] = {{"lower", range[0] * scale}, {"upper", range[1] * scale}};
    }
    nlohmann::json dynamics = nlohmann::json::object();
    if (HasEff(joint_node, jcls, "damping")) {
      dynamics["damping"] = EffDouble(joint_node, jcls, "damping", 0.0);
    }
    if (HasEff(joint_node, jcls, "frictionloss")) {
      dynamics["friction"] = EffDouble(joint_node, jcls, "frictionloss", 0.0);
    }
    // MJCF <joint stiffness=> and <joint armature=> — propagate so the
    // USD-side converter can author physxLimit:*:stiffness and
    // physxJoint:armature alongside the canonical mjc:* fallbacks.
    if (HasEff(joint_node, jcls, "stiffness")) {
      dynamics["stiffness"] = EffDouble(joint_node, jcls, "stiffness", 0.0);
    }
    if (HasEff(joint_node, jcls, "armature")) {
      dynamics["armature"] = EffDouble(joint_node, jcls, "armature", 0.0);
    }
    joint["dynamics"] = dynamics;
    // MJCF <joint ref="..."> is the rest-angle (degrees for hinge under the
    // default <compiler angle="degree">, meters for slide). Forward it as a
    // generic `initPosition` (radians for hinge) so the C++ converter can
    // emit state:{angular,linear}:physics:position.
    if (HasEff(joint_node, jcls, "ref")) {
      double ref = EffDouble(joint_node, jcls, "ref", 0.0);
      if (mj_type == "hinge") ref *= ctx.angle_to_rad;
      joint["initPosition"] = ref;
    }
  }

  joints->push_back(joint);
}

void AddMujocoActuatorsJson(const pugi::xml_node &root,
                            nlohmann::json *actuators,
                            nlohmann::json *mjc_actuators, Stats *stats) {
  if (!actuators) return;
  const auto actuator_root = Child(root, "actuator");
  if (!actuator_root) return;

  for (const auto &act_node : actuator_root.children()) {
    const std::string type = act_node.name();
    const std::string joint = Attr(act_node, "joint");
    const std::string tendon = Attr(act_node, "tendon");
    const std::string site = Attr(act_node, "site");
    const std::string body = Attr(act_node, "body");

    // Muscle/general/adhesion/cylinder/intvelocity/damper, or any tendon-/site-/
    // body-targeted actuator -> MjcActuator (preserves the MuJoCo gain/bias/
    // lengthrange params; e.g. ms_human_700 muscles, flybody adhesion). Plain
    // joint-targeted position/velocity/motor actuators keep the Newton PD path.
    const bool is_mjc = (type == "muscle" || type == "general" ||
                         type == "adhesion" || type == "cylinder" ||
                         type == "intvelocity" || type == "damper" ||
                         type == "plugin" ||
                         !tendon.empty() || !site.empty() || !body.empty());
    if (is_mjc && mjc_actuators) {
      const std::string fallback_target =
          !joint.empty() ? joint
                         : (!tendon.empty() ? tendon
                                            : (!site.empty() ? site : body));
      nlohmann::json a = nlohmann::json::object();
      a["name"] = Attr(act_node, "name", type + "_" + fallback_target);
      a["actuatorType"] = type;
      if (!joint.empty()) a["targetJoint"] = joint;
      if (!tendon.empty()) a["targetTendon"] = tendon;
      if (!site.empty()) a["targetSite"] = site;
      if (!body.empty()) a["targetBody"] = body;
      const auto gainprm = ParseDoubles(Attr(act_node, "gainprm"));
      if (!gainprm.empty()) a["gainPrm"] = gainprm;
      else if (HasAttr(act_node, "gain"))  // <adhesion gain=..> scalar
        a["gainPrm"] = {ParseDoubleAttr(act_node, "gain", 0.0)};
      const auto biasprm = ParseDoubles(Attr(act_node, "biasprm"));
      if (!biasprm.empty()) a["biasPrm"] = biasprm;
      const auto lengthrange = ParseDoubles(Attr(act_node, "lengthrange"));
      if (lengthrange.size() >= 2)
        a["lengthRange"] = {lengthrange[0], lengthrange[1]};
      const auto ctrlrange = ParseDoubles(Attr(act_node, "ctrlrange"));
      if (ctrlrange.size() >= 2) a["ctrlRange"] = {ctrlrange[0], ctrlrange[1]};
      const auto forcerange = ParseDoubles(Attr(act_node, "forcerange"));
      if (forcerange.size() >= 2)
        a["forceRange"] = {forcerange[0], forcerange[1]};
      const auto gear = ParseDoubles(Attr(act_node, "gear"));
      if (!gear.empty()) a["gear"] = gear;
      if (HasAttr(act_node, "dyntype")) a["dynType"] = Attr(act_node, "dyntype");
      if (HasAttr(act_node, "gaintype")) a["gainType"] = Attr(act_node, "gaintype");
      if (HasAttr(act_node, "biastype")) a["biasType"] = Attr(act_node, "biastype");
      // Engine-plugin actuator (<plugin plugin=".." instance="..">): preserve the
      // plugin id + referenced <extension> instance name.
      if (HasAttr(act_node, "plugin")) a["plugin"] = Attr(act_node, "plugin");
      if (HasAttr(act_node, "instance")) a["instance"] = Attr(act_node, "instance");
      mjc_actuators->push_back(std::move(a));
      if (stats) stats->actuators++;
      continue;
    }

    if (joint.empty()) {
      continue;
    }
    nlohmann::json act = nlohmann::json::object();
    act["name"] = Attr(act_node, "name", type + "_" + joint);
    act["joint"] = joint;
    act["control"] = "pd";

    if (HasAttr(act_node, "kp")) {
      act["kp"] = ParseDoubleAttr(act_node, "kp", 0.0);
    } else {
      const auto gain = ParseDoubles(Attr(act_node, "gainprm"));
      if (!gain.empty()) {
        act["kp"] = gain[0];
      }
    }
    if (HasAttr(act_node, "kv")) {
      act["kd"] = ParseDoubleAttr(act_node, "kv", 0.0);
    } else {
      const auto bias = ParseDoubles(Attr(act_node, "biasprm"));
      if (bias.size() >= 3) {
        act["kd"] = std::abs(bias[2]);
      }
    }
    const auto force_range = ParseDoubles(Attr(act_node, "forcerange"));
    if (force_range.size() >= 2) {
      act["maxEffort"] = std::max(std::abs(force_range[0]),
                                  std::abs(force_range[1]));
    }
    const auto ctrl_range = ParseDoubles(Attr(act_node, "ctrlrange"));
    if (type == "motor" && ctrl_range.size() >= 2) {
      act["constEffort"] = std::max(std::abs(ctrl_range[0]),
                                    std::abs(ctrl_range[1]));
    }
    if (HasAttr(act_node, "delay")) {
      act["delaySteps"] = static_cast<int32_t>(
          ParseDoubleAttr(act_node, "delay", 1.0));
    }
    actuators->push_back(std::move(act));
    if (stats) stats->actuators++;
  }
}

// MJCF <tendon><fixed> -> JSON tendon entries consumed by AddMjcTendonFromJson.
// A fixed tendon constrains a linear combination of joint coordinates; we carry
// the {joint, coef} list plus the spring/damper/range scalars. Spatial tendons
// (sites/pulleys) aren't representable without sites yet -> warn and skip.
void AddMujocoTendonsJson(const pugi::xml_node &root, const Context &ctx,
                          nlohmann::json *tendons, Stats *stats) {
  if (!tendons) return;
  const auto tendon_root = Child(root, "tendon");
  if (!tendon_root) return;
  for (const auto &t_node : tendon_root.children()) {
    const std::string kind = t_node.name();  // "fixed" or "spatial"
    if (kind != "fixed" && kind != "spatial") continue;
    nlohmann::json tendon = nlohmann::json::object();
    tendon["name"] = Attr(t_node, "name", "tendon_" + std::to_string(stats ? stats->tendons : 0));
    tendon["type"] = kind;
    if (kind == "fixed") {
      // Fixed tendon: linear combination of joint coordinates.
      nlohmann::json jlist = nlohmann::json::array();
      for (const auto &j_node : Children(t_node, "joint")) {
        const std::string jn = Attr(j_node, "joint");
        if (jn.empty()) continue;
        nlohmann::json je = nlohmann::json::object();
        je["joint"] = jn;
        je["coef"] = ParseDoubleAttr(j_node, "coef", 1.0);
        jlist.push_back(std::move(je));
      }
      if (jlist.empty()) continue;
      tendon["joints"] = std::move(jlist);
    } else {
      // Spatial tendon (muscle path): ordered <site>/<geom sidesite=...> waypoints.
      nlohmann::json waypoints = nlohmann::json::array();
      for (const auto &w : t_node.children()) {
        const std::string wn = w.name();
        if (wn == "site" && HasAttr(w, "site")) {
          waypoints.push_back({{"site", Attr(w, "site")}});
        } else if (wn == "geom") {
          nlohmann::json wp = {{"geom", Attr(w, "geom")}};
          if (HasAttr(w, "sidesite")) wp["sidesite"] = Attr(w, "sidesite");
          waypoints.push_back(std::move(wp));
        } else if (wn == "pulley" && HasAttr(w, "divisor")) {
          waypoints.push_back({{"pulley", ParseDoubleAttr(w, "divisor", 1.0)}});
        }
      }
      if (waypoints.size() < 2) continue;
      tendon["path"] = std::move(waypoints);
      if (HasAttr(t_node, "width"))
        tendon["width"] = ParseDoubleAttr(t_node, "width", 0.003);
      const auto rgba = ParseDoubles(Attr(t_node, "rgba"));
      if (rgba.size() >= 4) tendon["rgba"] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    }
    if (HasAttr(t_node, "stiffness"))
      tendon["stiffness"] = ParseDoubleAttr(t_node, "stiffness", 0.0);
    if (HasAttr(t_node, "damping"))
      tendon["damping"] = ParseDoubleAttr(t_node, "damping", 0.0);
    if (HasAttr(t_node, "frictionloss"))
      tendon["frictionloss"] = ParseDoubleAttr(t_node, "frictionloss", 0.0);
    if (HasAttr(t_node, "margin"))
      tendon["margin"] = ParseDoubleAttr(t_node, "margin", 0.0);
    const auto range = ParseDoubles(Attr(t_node, "range"));
    if (range.size() >= 2) tendon["range"] = {range[0], range[1]};
    const auto springlen = ParseDoubles(Attr(t_node, "springlength"));
    if (!springlen.empty()) tendon["springlength"] = springlen;
    tendons->push_back(std::move(tendon));
    if (stats) stats->tendons++;
  }
  (void)ctx;
}

// MJCF <site> (in bodies) -> JSON site entries with a baked world transform,
// consumed by the converter to emit MjcSite marker prims. Sites are the routing
// points for spatial (muscle) tendons. Recurses the body tree.
void CollectMujocoSitesJson(const pugi::xml_node &body_node,
                            const std::vector<double> &parent_world,
                            const Context &ctx, nlohmann::json *sites,
                            Stats *stats) {
  const std::vector<double> body_world =
      MultiplyMatrix(parent_world, PoseMatrix(body_node, ctx));
  for (const auto &s_node : Children(body_node, "site")) {
    const std::string name = Attr(s_node, "name");
    if (name.empty()) continue;
    const std::vector<double> site_world =
        MultiplyMatrix(body_world, PoseMatrix(s_node, ctx));
    nlohmann::json site = {{"name", name}, {"matrix", site_world}};
    if (HasAttr(s_node, "group")) site["group"] = std::atoi(Attr(s_node, "group").c_str());
    const auto size = ParseDoubles(Attr(s_node, "size"));
    if (!size.empty()) site["size"] = size[0];
    sites->push_back(std::move(site));
    if (stats) stats->sites++;
  }
  for (const auto &child : Children(body_node, "body")) {
    CollectMujocoSitesJson(child, body_world, ctx, sites, stats);
  }
}

bool MjcBoolAttr(const std::string &v);  // defined below

// MJCF <sensor> children -> JSON sensor entries consumed by the converter's
// AddMjcSensorFromJson -> MjcSensor prim. The sensor kind is the element name;
// the measured object is captured via objtype/objname (frame sensors carry them
// explicitly, others attach via site/joint/actuator/tendon/body/geom).
void AddMujocoSensorsJson(const pugi::xml_node &root, nlohmann::json *sensors,
                          Stats *stats) {
  if (!sensors) return;
  const auto sensor_root = Child(root, "sensor");
  if (!sensor_root) return;
  for (const auto &s_node : sensor_root.children()) {
    const std::string type = s_node.name();
    if (type.empty()) continue;
    nlohmann::json s = {{"type", type}};
    s["name"] = Attr(s_node, "name", type + "_" + std::to_string(sensors->size()));
    if (HasAttr(s_node, "objtype")) {
      s["objtype"] = Attr(s_node, "objtype");
      s["objname"] = Attr(s_node, "objname");
    } else {
      // Infer objtype from the attachment attribute the sensor type uses.
      for (const char *k : {"site", "joint", "actuator", "tendon", "body", "geom"}) {
        if (HasAttr(s_node, k)) { s["objtype"] = k; s["objname"] = Attr(s_node, k); break; }
      }
    }
    if (HasAttr(s_node, "reftype")) s["reftype"] = Attr(s_node, "reftype");
    if (HasAttr(s_node, "refname")) s["refname"] = Attr(s_node, "refname");
    if (HasAttr(s_node, "group")) s["group"] = static_cast<int>(ParseDoubleAttr(s_node, "group", 0.0));
    if (HasAttr(s_node, "cutoff")) s["cutoff"] = ParseDoubleAttr(s_node, "cutoff", 0.0);
    if (HasAttr(s_node, "noise")) s["noise"] = ParseDoubleAttr(s_node, "noise", 0.0);
    const auto user = ParseDoubles(Attr(s_node, "user"));
    if (!user.empty()) s["user"] = user;
    sensors->push_back(std::move(s));
    if (stats) stats->sensors++;
  }
}

// MJCF <custom><numeric|text> -> JSON consumed by the converter to preserve
// model metadata / MJX compile knobs (e.g. max_contact_points) as mjc:custom:*.
void AddMujocoCustomJson(const pugi::xml_node &root, nlohmann::json *custom) {
  if (!custom) return;
  const auto custom_root = Child(root, "custom");
  if (!custom_root) return;
  nlohmann::json numerics = nlohmann::json::array();
  for (const auto &n : Children(custom_root, "numeric")) {
    const std::string name = Attr(n, "name");
    if (name.empty()) continue;
    numerics.push_back({{"name", name}, {"data", ParseDoubles(Attr(n, "data"))}});
  }
  nlohmann::json texts = nlohmann::json::array();
  for (const auto &t : Children(custom_root, "text")) {
    const std::string name = Attr(t, "name");
    if (name.empty()) continue;
    texts.push_back({{"name", name}, {"data", Attr(t, "data")}});
  }
  if (!numerics.empty()) (*custom)["numeric"] = std::move(numerics);
  if (!texts.empty()) (*custom)["text"] = std::move(texts);
}

// <extension><plugin plugin="..."><instance name="..."><config key= value=>
// -> JSON plugin-instance declarations. The instance config (e.g. a PID's
// kp/ki/kd) is what an <actuator><plugin instance=".."> references.
void AddMujocoPluginsJson(const pugi::xml_node &root, nlohmann::json *plugins) {
  if (!plugins) return;
  const auto extension = Child(root, "extension");
  if (!extension) return;
  for (const auto &plugin_node : Children(extension, "plugin")) {
    const std::string plugin_id = Attr(plugin_node, "plugin");
    for (const auto &inst : Children(plugin_node, "instance")) {
      const std::string inst_name = Attr(inst, "name");
      if (inst_name.empty()) continue;
      nlohmann::json p = nlohmann::json::object();
      p["instance"] = inst_name;
      if (!plugin_id.empty()) p["plugin"] = plugin_id;
      nlohmann::json config = nlohmann::json::object();
      for (const auto &cfg : Children(inst, "config")) {
        const std::string key = Attr(cfg, "key");
        if (key.empty()) continue;
        config[key] = Attr(cfg, "value");
      }
      if (!config.empty()) p["config"] = std::move(config);
      plugins->push_back(std::move(p));
    }
  }
}

// MJCF <light>/<camera> (in worldbody or nested bodies) -> JSON with a baked
// world matrix, consumed by the converter's AddLightFromJson/AddCameraFromJson.
// `frame_world` is the world transform of `frame_node`'s frame.
void CollectMujocoLightsCamerasJson(const pugi::xml_node &frame_node,
                                    const std::vector<double> &frame_world,
                                    const Context &ctx, nlohmann::json *lights,
                                    nlohmann::json *cameras) {
  for (const auto &l : Children(frame_node, "light")) {
    nlohmann::json light = nlohmann::json::object();
    light["name"] = Attr(l, "name", "light");
    std::string type = Attr(l, "type");
    if (type.empty())
      type = MjcBoolAttr(Attr(l, "directional", "false")) ? "directional" : "spot";
    light["type"] = type;
    light["matrix"] = MultiplyMatrix(frame_world, PoseMatrix(l, ctx));
    const auto dir = ParseDouble3(Attr(l, "dir"), {{0.0, 0.0, -1.0}});
    light["dir"] = {dir[0], dir[1], dir[2]};
    const auto diffuse = ParseDoubles(Attr(l, "diffuse"));
    if (diffuse.size() >= 3) light["color"] = {diffuse[0], diffuse[1], diffuse[2]};
    if (HasAttr(l, "castshadow"))
      light["castshadow"] = MjcBoolAttr(Attr(l, "castshadow"));
    if (HasAttr(l, "cutoff")) light["cutoff"] = ParseDoubleAttr(l, "cutoff", 45.0);
    lights->push_back(std::move(light));
  }
  for (const auto &c : Children(frame_node, "camera")) {
    nlohmann::json cam = nlohmann::json::object();
    cam["name"] = Attr(c, "name", "camera");
    cam["matrix"] = MultiplyMatrix(frame_world, PoseMatrix(c, ctx));
    if (HasAttr(c, "fovy")) cam["fovy"] = ParseDoubleAttr(c, "fovy", 45.0);
    const std::string proj = Attr(c, "projection");
    if (proj == "orthographic" || MjcBoolAttr(Attr(c, "orthographic", "false")))
      cam["orthographic"] = true;
    cameras->push_back(std::move(cam));
  }
  for (const auto &child : Children(frame_node, "body")) {
    const std::vector<double> body_world =
        MultiplyMatrix(frame_world, PoseMatrix(child, ctx));
    CollectMujocoLightsCamerasJson(child, body_world, ctx, lights, cameras);
  }
}

// MJCF <equality> -> JSON entries consumed by AddMjcEqualityFromJson.
void AddMujocoEqualityJson(const pugi::xml_node &root, nlohmann::json *equalities,
                           Stats *stats) {
  if (!equalities) return;
  const auto eq_root = Child(root, "equality");
  if (!eq_root) return;
  for (const auto &e_node : eq_root.children()) {
    const std::string kind = e_node.name();  // connect | weld | joint
    if (kind != "connect" && kind != "weld" && kind != "joint") continue;
    nlohmann::json eq = nlohmann::json::object();
    eq["name"] = Attr(e_node, "name", kind + "_" +
                      std::to_string(stats ? stats->equalities : 0));
    eq["type"] = kind;
    if (kind == "joint") {
      eq["joint1"] = Attr(e_node, "joint1");
      if (HasAttr(e_node, "joint2")) eq["joint2"] = Attr(e_node, "joint2");
      const auto poly = ParseDoubles(Attr(e_node, "polycoef"));
      if (!poly.empty()) eq["polycoef"] = poly;
    } else {
      eq["body1"] = Attr(e_node, "body1");
      if (HasAttr(e_node, "body2")) eq["body2"] = Attr(e_node, "body2");
      const auto anchor = ParseDoubles(Attr(e_node, "anchor"));
      if (!anchor.empty()) eq["anchor"] = anchor;
      if (kind == "weld" && HasAttr(e_node, "torquescale"))
        eq["torquescale"] = ParseDoubleAttr(e_node, "torquescale", 1.0);
    }
    const auto solref = ParseDoubles(Attr(e_node, "solref"));
    if (!solref.empty()) eq["solref"] = solref;
    const auto solimp = ParseDoubles(Attr(e_node, "solimp"));
    if (!solimp.empty()) eq["solimp"] = solimp;
    equalities->push_back(std::move(eq));
    if (stats) stats->equalities++;
  }
}

// MJCF <contact><exclude body1 body2> -> filteredPairs entries (maps to
// PhysicsFilteredPairsAPI in the converter, which already handles this array).
void AddMujocoContactExcludesJson(const pugi::xml_node &root,
                                  nlohmann::json *filtered_pairs, Stats *stats) {
  if (!filtered_pairs) return;
  const auto contact_root = Child(root, "contact");
  if (!contact_root) return;
  for (const auto &c_node : Children(contact_root, "exclude")) {
    const std::string b1 = Attr(c_node, "body1");
    const std::string b2 = Attr(c_node, "body2");
    if (b1.empty() || b2.empty()) continue;
    filtered_pairs->push_back({{"body1", b1}, {"body2", b2}});
    if (stats) stats->contact_excludes++;
  }
}

// MJCF <contact><pair> -> JSON entries consumed by AddContactPairFromJson. An
// explicit collision pair between two geoms with its own friction/solver params.
void AddMujocoContactPairsJson(const pugi::xml_node &root,
                               nlohmann::json *pairs, Stats *stats) {
  if (!pairs) return;
  const auto contact_root = Child(root, "contact");
  if (!contact_root) return;
  for (const auto &p : Children(contact_root, "pair")) {
    const std::string g1 = Attr(p, "geom1");
    const std::string g2 = Attr(p, "geom2");
    if (g1.empty() || g2.empty()) continue;
    nlohmann::json pr = {{"geom1", g1}, {"geom2", g2}};
    pr["name"] = Attr(p, "name", "pair_" + std::to_string(pairs->size()));
    if (HasAttr(p, "condim")) pr["condim"] = static_cast<int>(ParseDoubleAttr(p, "condim", 3.0));
    if (HasAttr(p, "margin")) pr["margin"] = ParseDoubleAttr(p, "margin", 0.0);
    if (HasAttr(p, "gap")) pr["gap"] = ParseDoubleAttr(p, "gap", 0.0);
    const auto friction = ParseDoubles(Attr(p, "friction"));
    if (!friction.empty()) pr["friction"] = friction;
    const auto solref = ParseDoubles(Attr(p, "solref"));
    if (!solref.empty()) pr["solref"] = solref;
    const auto solimp = ParseDoubles(Attr(p, "solimp"));
    if (!solimp.empty()) pr["solimp"] = solimp;
    pairs->push_back(std::move(pr));
    if (stats) stats->contact_pairs++;
  }
}

// Normalize a MuJoCo boolean/flag attribute: <flag x="enable|disable"> and
// <compiler x="true|false"> both occur; map enable/true/1 -> true.
bool MjcBoolAttr(const std::string &v) {
  return v == "true" || v == "enable" || v == "1";
}

// Read the full <option>/<option><flag>/<compiler> attribute set into a
// `mjcScene` JSON block (MJCF attr names) consumed by the converter's
// ApplyMjcSceneOptions -> MjcSceneAPI. (The MjcSceneAPI schema + USDC
// round-trip already support all of these; only timestep was emitted before.)
nlohmann::json BuildMjcSceneJson(const pugi::xml_node &root) {
  nlohmann::json ms = nlohmann::json::object();
  if (const auto option = Child(root, "option")) {
    nlohmann::json opt = nlohmann::json::object();
    auto num = [&](const char *k) { if (HasAttr(option, k)) opt[k] = ParseDoubleAttr(option, k, 0.0); };
    auto inum = [&](const char *k) { if (HasAttr(option, k)) opt[k] = static_cast<int>(ParseDoubleAttr(option, k, 0.0)); };
    auto tok = [&](const char *k) { if (HasAttr(option, k)) opt[k] = Attr(option, k); };
    auto vec = [&](const char *k) { auto v = ParseDoubles(Attr(option, k)); if (!v.empty()) opt[k] = v; };
    num("timestep"); num("impratio"); num("density"); num("viscosity"); num("o_margin");
    num("tolerance"); num("ls_tolerance"); num("noslip_tolerance"); num("ccd_tolerance");
    inum("iterations"); inum("ls_iterations"); inum("noslip_iterations");
    inum("ccd_iterations"); inum("sdf_iterations"); inum("sdf_initpoints");
    tok("integrator"); tok("cone"); tok("jacobian"); tok("solver");
    vec("wind"); vec("magnetic"); vec("o_solref"); vec("o_solimp"); vec("o_friction");
    if (!opt.empty()) ms["option"] = std::move(opt);
    if (const auto flag = Child(option, "flag")) {
      nlohmann::json fl = nlohmann::json::object();
      static const char *kFlags[] = {
          "constraint", "equality", "frictionloss", "limit", "contact",
          "gravity", "clampctrl", "warmstart", "filterparent", "actuation",
          "refsafe", "sensor", "midphase", "nativeccd", "eulerdamp",
          "autoreset", "island", "override", "energy", "fwdinv",
          "invdiscrete", "multiccd"};
      for (const char *k : kFlags)
        if (HasAttr(flag, k)) fl[k] = MjcBoolAttr(Attr(flag, k));
      if (!fl.empty()) ms["flag"] = std::move(fl);
    }
  }
  if (const auto compiler = Child(root, "compiler")) {
    nlohmann::json comp = nlohmann::json::object();
    auto num = [&](const char *k) { if (HasAttr(compiler, k)) comp[k] = ParseDoubleAttr(compiler, k, 0.0); };
    auto tok = [&](const char *k) { if (HasAttr(compiler, k)) comp[k] = Attr(compiler, k); };
    auto boolean = [&](const char *k) { if (HasAttr(compiler, k)) comp[k] = MjcBoolAttr(Attr(compiler, k)); };
    boolean("autolimits"); num("boundmass"); num("boundinertia"); num("settotalmass");
    boolean("usethread"); boolean("balanceinertia"); tok("angle"); boolean("fitaabb");
    boolean("fusestatic"); tok("inertiafromgeom"); boolean("alignfree"); boolean("saveinertial");
    const auto igr = ParseDoubles(Attr(compiler, "inertiagrouprange"));
    if (igr.size() >= 2) {
      comp["inertiagrouprange_min"] = static_cast<int>(igr[0]);
      comp["inertiagrouprange_max"] = static_cast<int>(igr[1]);
    }
    if (!comp.empty()) ms["compiler"] = std::move(comp);
  }
  return ms;
}

// MJCF <asset><material> -> JSON material entries consumed by the converter's
// AddMaterialFromJson -> UsdShade Material (UsdPreviewSurface). Color/PBR-scalar
// only; texture maps are a documented follow-on.
void AddMujocoMaterialsJson(const pugi::xml_node &root, const fs::path &base_dir,
                            nlohmann::json *materials, Stats *stats) {
  if (!materials) return;
  const auto asset = Child(root, "asset");
  if (!asset) return;
  // Resolve <texture name file=...> to an absolute path so a material's
  // `texture` becomes a UsdUVTexture in the converter. Builtin (file-less)
  // textures are skipped (no image to reference).
  std::string tdir;
  if (const auto compiler = Child(root, "compiler")) {
    tdir = Attr(compiler, "texturedir");
    if (tdir.empty()) tdir = Attr(compiler, "assetdir");
  }
  const fs::path tex_dir = tdir.empty() ? base_dir : base_dir / tdir;
  const auto index = BuildMeshIndex(base_dir);
  std::map<std::string, std::string> tex_files;
  for (const auto &t_node : Children(asset, "texture")) {
    const std::string tname = Attr(t_node, "name");
    const std::string tfile = Attr(t_node, "file");
    if (tname.empty() || tfile.empty()) continue;
    tex_files[tname] = ResolveMeshPath(base_dir, tex_dir, tfile, index).string();
  }
  for (const auto &m_node : Children(asset, "material")) {
    const std::string name = Attr(m_node, "name");
    if (name.empty()) continue;
    nlohmann::json mat = {{"name", name}};
    const auto rgba = ParseDoubles(Attr(m_node, "rgba"));
    if (rgba.size() >= 4) mat["rgba"] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    if (HasAttr(m_node, "metallic")) mat["metallic"] = ParseDoubleAttr(m_node, "metallic", 0.0);
    if (HasAttr(m_node, "roughness")) mat["roughness"] = ParseDoubleAttr(m_node, "roughness", 0.5);
    if (HasAttr(m_node, "specular")) mat["specular"] = ParseDoubleAttr(m_node, "specular", 0.0);
    if (HasAttr(m_node, "emission")) mat["emission"] = ParseDoubleAttr(m_node, "emission", 0.0);
    if (HasAttr(m_node, "reflectance")) mat["reflectance"] = ParseDoubleAttr(m_node, "reflectance", 0.0);
    const std::string tref = Attr(m_node, "texture");
    if (!tref.empty()) {
      const auto it = tex_files.find(tref);
      if (it != tex_files.end()) {
        // Emit a portable, source-relative reference (e.g. "assets/foo.png")
        // for inputs:file so the exported usda/usdc opens beside its assets
        // instead of carrying a machine-specific absolute path. Keep the
        // absolute source in `texture_abs` (ignored by the converter) so usdz
        // export can embed the bytes into the package.
        std::error_code ec;
        const fs::path abs_tex = fs::absolute(it->second, ec);
        const fs::path rel = fs::relative(abs_tex, base_dir, ec);
        const std::string rel_s = rel.generic_string();
        mat["texture"] = (ec || rel_s.empty() || rel_s.rfind("..", 0) == 0)
                             ? abs_tex.filename().string()
                             : rel_s;
        mat["texture_abs"] = abs_tex.string();
      }
    }
    materials->push_back(std::move(mat));
    if (stats) stats->materials++;
  }
}

// MJCF <keyframe><key qpos/qvel/act/ctrl/mpos/mquat> -> JSON keyframe entries
// consumed by the converter's AddMjcKeyframeFromJson -> MjcKeyframe prim.
void AddMujocoKeyframesJson(const pugi::xml_node &root, nlohmann::json *keyframes,
                            Stats *stats) {
  if (!keyframes) return;
  const auto kf_root = Child(root, "keyframe");
  if (!kf_root) return;
  for (const auto &key : Children(kf_root, "key")) {
    nlohmann::json k = nlohmann::json::object();
    k["name"] = Attr(key, "name", "key_" + std::to_string(keyframes->size()));
    auto arr = [&](const char *attr) {
      auto v = ParseDoubles(Attr(key, attr));
      if (!v.empty()) k[attr] = v;
    };
    arr("qpos"); arr("qvel"); arr("act"); arr("ctrl"); arr("mpos"); arr("mquat");
    if (HasAttr(key, "time")) k["time"] = ParseDoubleAttr(key, "time", 0.0);
    keyframes->push_back(std::move(k));
    if (stats) stats->keyframes++;
  }
}

// Bake one <geom> into a link's "visuals"/"collisions" arrays. Shared by the
// per-body traversal and the worldbody-level (static) geom collection.
bool AppendGeomToLink(const pugi::xml_node &geom_node, const std::string &cc,
                      const std::map<std::string, MeshAsset> &assets,
                      const std::map<std::string, HFieldAsset> &hfields,
                      const Options &opts, const Context &ctx,
                      const std::vector<double> &body_world,
                      nlohmann::json *link, Stats *stats, std::string *err,
                      bool dual_collider = false) {
  const AttrMap &cls = ResolveGeomClass(geom_node, ctx.defaults, cc);
  std::string cls_name = Attr(geom_node, "class");
  if (cls_name.empty()) cls_name = cc;
  MeshPayload payload;
  bool visual = true;
  if (!BuildGeomPayload(geom_node, cls, cls_name, assets, hfields, opts, ctx,
                        body_world, &payload, &visual, err)) {
    return false;
  }
  if (visual) {
    nlohmann::json visual_json = MeshPayloadToJson(payload);
    AddGeomPhysicsAttrs(geom_node, cls, &visual_json);
    const std::string mat = Eff(geom_node, cls, "material");
    if (!mat.empty()) visual_json["material"] = mat;
    (*link)["visuals"].push_back(std::move(visual_json));
    if (stats) stats->visuals++;
    // A world-fixed geom that is visible AND a collider (MuJoCo default
    // contype=conaffinity=1; non-collider only when BOTH are 0) is ALSO emitted
    // as a USD collider so floors/ground/hfield actually collide. The owning
    // link is static, so use an exact triangle-mesh collider (`none`) rather
    // than a convex hull, which would flatten terrain. Robot bodies keep the
    // group-based visual/collision split (they ship dedicated collision geoms).
    if (dual_collider) {
      const int contype =
          static_cast<int>(EffDouble(geom_node, cls, "contype", 1.0));
      const int conaffinity =
          static_cast<int>(EffDouble(geom_node, cls, "conaffinity", 1.0));
      if (contype != 0 || conaffinity != 0) {
        nlohmann::json col = MeshPayloadToJson(payload);
        AddGeomPhysicsAttrs(geom_node, cls, &col);
        col["approximation"] = "none";
        (*link)["collisions"].push_back(std::move(col));
        if (stats) stats->collisions++;
      }
    }
  } else {
    nlohmann::json col = MeshPayloadToJson(payload);
    AddGeomPhysicsAttrs(geom_node, cls, &col);
    // Default approximation `convexHull` matches
    // `src/tydra/urdf-to-usd.cc::AddCollisionAPIs` and the
    // mujoco-usd-converter convention (one Mesh per geom +
    // `UsdPhysicsMeshCollisionAPI` with hull approximation).
    col["approximation"] = "convexHull";
    (*link)["collisions"].push_back(std::move(col));
    if (stats) stats->collisions++;
  }
  return true;
}

// Geoms that are direct children of <worldbody> (e.g. a ground/floor plane or
// heightfield) are world-fixed and belong to no body. Collect them onto a
// single static root link named "world" so they still convert. Accumulates
// across every <worldbody> block (post-<include> merge). World-fixed colliders
// are emitted as both a render mesh and a (triangle) collider via dual_collider.
bool AddWorldbodyGeomsLink(const std::vector<pugi::xml_node> &worldbodies,
                           const std::map<std::string, MeshAsset> &assets,
                           const std::map<std::string, HFieldAsset> &hfields,
                           const Options &opts, const Context &ctx,
                           nlohmann::json *links, Stats *stats,
                           std::string *err) {
  nlohmann::json link = {{"name", "world"},
                         {"static", true},
                         {"inertial", {{"mass", 0.0}}},
                         {"visuals", nlohmann::json::array()},
                         {"collisions", nlohmann::json::array()}};
  for (const auto &worldbody : worldbodies) {
    for (const auto &geom_node : Children(worldbody, "geom")) {
      std::string gerr;
      if (!AppendGeomToLink(geom_node, "", assets, hfields, opts, ctx,
                            IdentityMatrix(), &link, stats, &gerr,
                            /*dual_collider=*/true)) {
        // Match the per-body geom path: a build failure is fatal unless the
        // caller opted into --allow-missing.
        if (!opts.allow_missing) {
          if (err) *err = gerr.empty() ? "worldbody geom failed" : gerr;
          return false;
        }
        std::cerr << "WARN: " << (gerr.empty() ? "worldbody geom skipped" : gerr)
                  << "\n";
      }
    }
  }
  if (link["visuals"].empty() && link["collisions"].empty()) return true;
  links->push_back(std::move(link));
  if (stats) stats->links++;
  return true;
}

bool VisitMujocoBody(const pugi::xml_node &body_node,
                     const std::string &parent_name,
                     const std::string &childclass,
                     const std::vector<double> &parent_world,
                     const std::map<std::string, MeshAsset> &assets,
                     const std::map<std::string, HFieldAsset> &hfields,
                     const Options &opts, const Context &ctx,
                     nlohmann::json *links, nlohmann::json *joints,
                     Stats *stats, std::string *err) {
  // A body's own `childclass` becomes the default class for its descendant
  // geoms/joints/bodies, unless overridden deeper.
  const std::string cc = HasAttr(body_node, "childclass")
                             ? Attr(body_node, "childclass")
                             : childclass;
  // Accumulate the body-chain world transform: this body's frame relative to
  // its parent, composed onto the parent's world frame.
  const std::vector<double> body_world =
      MultiplyMatrix(parent_world, PoseMatrix(body_node, ctx));
  const std::string body_name =
      Attr(body_node, "name", "body_" + std::to_string(stats->links));
  // A <freejoint> (or <joint type="free">) makes this body a 6-DOF floating
  // base: it is NOT joined to its parent. Mark it so the converter emits a free
  // articulation root (vs a fixed base, which has no such joint).
  bool has_free = static_cast<bool>(Child(body_node, "freejoint"));
  if (!has_free) {
    for (const auto &jn : Children(body_node, "joint")) {
      if (ToLower(Attr(jn, "type")) == "free") { has_free = true; break; }
    }
  }
  nlohmann::json link = {
      {"name", body_name},
      {"inertial", InertialToJson(body_node)},
      {"visuals", nlohmann::json::array()},
      {"collisions", nlohmann::json::array()}};
  if (has_free) link["floating"] = true;
  // MuJoCo mocap body (driven externally; not simulated). Flag it so the USD
  // link records mjc:mocap.
  if (MjcBoolAttr(Attr(body_node, "mocap", "false"))) link["mocap"] = true;

  for (const auto &geom_node : Children(body_node, "geom")) {
    if (!AppendGeomToLink(geom_node, cc, assets, hfields, opts, ctx, body_world,
                          &link, stats, err)) {
      if (opts.allow_missing) {
        std::cerr << "WARN: " << (err ? *err : "mesh skipped") << "\n";
        if (err) err->clear();
        continue;
      }
      return false;
    }
  }

  links->push_back(std::move(link));
  stats->links++;
  if (!parent_name.empty() && !has_free) {
    const std::vector<double> body_matrix = PoseMatrix(body_node, ctx);
    const std::array<double, 3> body_pos =
        ParseDouble3(Attr(body_node, "pos"), {{0.0, 0.0, 0.0}});
    const std::vector<double> identity = IdentityMatrix();
    const std::array<double, 3> zero_pos = {{0.0, 0.0, 0.0}};
    const std::vector<pugi::xml_node> joint_nodes =
        Children(body_node, "joint");

    if (joint_nodes.size() <= 1) {
      // 0 joints -> fixed connection; 1 joint -> single DOF (the common case).
      const pugi::xml_node jn =
          joint_nodes.empty() ? pugi::xml_node() : joint_nodes[0];
      const AttrMap &jcls = ResolveJointClass(jn, ctx.defaults, cc);
      AddJointFromNode(jn, jcls, ctx, parent_name, body_name, body_matrix,
                       body_pos, joints);
      stats->joints++;
    } else {
      // MuJoCo allows multiple <joint> per body (e.g. ball+slide, or a stack of
      // hinges) — they compose in sequence. Represent the N DOFs as a chain of
      // (N-1) massless intermediate link Xforms joined by single-DOF joints:
      //   parent --j0--> dof_1 --j1--> ... --j(N-1)--> body
      // Only the first joint carries the body offset; intermediates sit at the
      // body frame (identity). Geometry is world-baked on `body`, so the empty
      // intermediate links add no visible geometry.
      std::string prev = parent_name;
      for (size_t k = 0; k < joint_nodes.size(); k++) {
        const bool last = (k + 1 == joint_nodes.size());
        const std::string child =
            last ? body_name : (body_name + "__mjcdof_" + std::to_string(k + 1));
        if (!last) {
          // Emit a massless intermediate link (no geometry, no mass).
          nlohmann::json dof_link = {{"name", child},
                                     {"inertial", {{"mass", 0.0}}},
                                     {"visuals", nlohmann::json::array()},
                                     {"collisions", nlohmann::json::array()}};
          links->push_back(std::move(dof_link));
          stats->links++;
        }
        const AttrMap &jcls =
            ResolveJointClass(joint_nodes[k], ctx.defaults, cc);
        AddJointFromNode(joint_nodes[k], jcls, ctx, prev, child,
                         (k == 0) ? body_matrix : identity,
                         (k == 0) ? body_pos : zero_pos, joints);
        stats->joints++;
        prev = child;
      }
    }
  }

  for (const auto &child_body : Children(body_node, "body")) {
    if (!VisitMujocoBody(child_body, body_name, cc, body_world, assets, hfields,
                         opts, ctx, links, joints, stats, err)) {
      return false;
    }
  }
  return true;
}

// --- MJCF <attach> sub-model composition -----------------------------------
// `<asset><model name file>` declares a child model; `<attach model body
// prefix>` grafts that child's body subtree at the attach point, prefixing
// every name + reference. We expand it at the XML level (after include
// expansion) so the rest of the pipeline sees one flat model. Used by
// iit_softfoot/scene.xml (the only menagerie model using <attach>).

// Attributes whose value is a name or a reference to one (prefixed on attach).
const std::set<std::string> &AttachRefAttrs() {
  static const std::set<std::string> s = {
      "name", "class", "childclass", "material", "mesh", "hfield", "texture",
      "joint", "joint1", "joint2", "site", "site1", "site2", "refsite",
      "sidesite", "tendon", "tendon1", "tendon2", "body", "body1", "body2",
      "geom", "geom1", "geom2", "objname"};
  return s;
}

void PrefixAttachSubtree(pugi::xml_node node, const std::string &prefix,
                         bool rad_to_deg) {
  for (pugi::xml_attribute a : node.attributes()) {
    if (AttachRefAttrs().count(a.name())) {
      const std::string v = a.value();
      if (!v.empty()) a.set_value((prefix + v).c_str());
    }
  }
  if (rad_to_deg) {
    // The child model authored angles in radians but the merged model is parsed
    // in degrees; convert orientation angles so geometry stays correct. (Joint
    // range/ref are left as-is — they affect limits, not the rest pose.)
    const double k = 57.295779513082323;  // 180/pi
    if (pugi::xml_attribute e = node.attribute("euler")) {
      const auto v = ParseDoubles(e.value());
      if (v.size() == 3) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%.10g %.10g %.10g",
                      v[0] * k, v[1] * k, v[2] * k);
        e.set_value(buf);
      }
    }
    if (pugi::xml_attribute aa = node.attribute("axisangle")) {
      const auto v = ParseDoubles(aa.value());
      if (v.size() == 4) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%.10g %.10g %.10g %.10g", v[0], v[1],
                      v[2], v[3] * k);
        aa.set_value(buf);
      }
    }
  }
  for (pugi::xml_node c : node.children()) {
    PrefixAttachSubtree(c, prefix, rad_to_deg);
  }
}

void CollectAttachNodes(pugi::xml_node n, std::vector<pugi::xml_node> *out) {
  for (pugi::xml_node c : n.children()) {
    if (std::string(c.name()) == "attach") out->push_back(c);
    CollectAttachNodes(c, out);
  }
}

pugi::xml_node FindBodyByName(pugi::xml_node n, const std::string &name) {
  for (pugi::xml_node b : n.children("body")) {
    if (std::string(b.attribute("name").as_string()) == name) return b;
    if (pugi::xml_node r = FindBodyByName(b, name)) return r;
  }
  return pugi::xml_node();
}

bool ExpandAttachments(pugi::xml_node root, const fs::path &base_dir,
                       std::string *err) {
  std::map<std::string, fs::path> models;
  for (pugi::xml_node asset : root.children("asset")) {
    for (pugi::xml_node m : asset.children("model")) {
      const std::string name = m.attribute("name").as_string();
      const std::string file = m.attribute("file").as_string();
      if (!name.empty() && !file.empty()) {
        std::error_code wc_ec;
        models[name] = fs::weakly_canonical(base_dir / file, wc_ec);
      }
    }
  }
  std::vector<pugi::xml_node> attaches;
  CollectAttachNodes(root, &attaches);
  if (attaches.empty()) return true;

  bool parent_radian = false;
  if (pugi::xml_node comp = root.child("compiler")) {
    parent_radian = ToLower(comp.attribute("angle").as_string("degree")) == "radian";
  }

  for (pugi::xml_node attach : attaches) {
    const std::string model_name = attach.attribute("model").as_string();
    const std::string body_name = attach.attribute("body").as_string();
    const std::string prefix = attach.attribute("prefix").as_string();
    const auto it = models.find(model_name);
    if (it == models.end()) {
      std::cerr << "WARN: <attach> references unknown model `" << model_name
                << "`\n";
      continue;
    }
    std::string child_xml;
    if (!ReadFile(it->second, &child_xml, err)) return false;
    std::set<fs::path> seen;
    std::string child_expanded;
    if (!ExpandIncludes(child_xml, it->second.parent_path(), &seen,
                        &child_expanded, err)) {
      return false;
    }
    pugi::xml_document child_doc;
    if (!child_doc.load_string(child_expanded.c_str())) {
      if (err) *err = "Failed to parse attached model: " + it->second.string();
      return false;
    }
    pugi::xml_node child_root = child_doc.child("mujoco");
    if (!child_root) continue;

    const fs::path child_dir = it->second.parent_path();
    std::string child_meshdir;
    bool child_radian = false;
    if (pugi::xml_node cc = child_root.child("compiler")) {
      child_meshdir = cc.attribute("meshdir").as_string();
      if (child_meshdir.empty()) child_meshdir = cc.attribute("assetdir").as_string();
      child_radian = ToLower(cc.attribute("angle").as_string("degree")) == "radian";
    }
    const bool rad_to_deg = child_radian && !parent_radian;

    // Make the child's mesh/hfield file paths absolute so they still resolve
    // after the assets are merged into the (differently-rooted) parent model.
    for (pugi::xml_node asset : child_root.children("asset")) {
      for (const char *tag : {"mesh", "hfield", "skin"}) {
        for (pugi::xml_node a : asset.children(tag)) {
          pugi::xml_attribute f = a.attribute("file");
          if (f && !std::string(f.value()).empty() &&
              fs::path(f.value()).is_relative()) {
            std::error_code wc_ec;
            const fs::path abs = fs::weakly_canonical(
                child_dir / child_meshdir / f.value(), wc_ec);
            f.set_value(abs.string().c_str());
          }
        }
      }
    }

    PrefixAttachSubtree(child_root, prefix, rad_to_deg);

    pugi::xml_node found;
    for (pugi::xml_node wb : child_root.children("worldbody")) {
      found = FindBodyByName(wb, prefix + body_name);
      if (found) break;
    }
    if (!found) {
      std::cerr << "WARN: <attach> body `" << body_name
                << "` not found in model `" << model_name << "`\n";
      continue;
    }

    pugi::xml_node parent = attach.parent();
    parent.insert_copy_before(found, attach);
    parent.remove_child(attach);

    // Merge the child's model-level sections (prefixed) into the parent.
    for (const char *sec : {"default", "asset", "tendon", "actuator", "sensor",
                            "equality", "contact", "keyframe"}) {
      pugi::xml_node child_sec = child_root.child(sec);
      if (!child_sec) continue;
      pugi::xml_node main_sec = root.child(sec);
      if (!main_sec) main_sec = root.append_child(sec);
      for (pugi::xml_node c : child_sec.children()) {
        if (std::string(c.name()) == "model") continue;  // don't recurse models
        main_sec.append_copy(c);
      }
    }
  }
  return true;
}

// --- MJCF <frame> flattening -----------------------------------------------
// A <frame> is a pure coordinate transform applied to all its direct children
// (it is NOT a body). MuJoCo 3 uses it to group/offset geoms/bodies. We dissolve
// each frame by composing its transform into every child's pose and moving the
// children up to the frame's parent, so the rest of the pipeline never sees a
// <frame>. Used by apptronik_apollo (frames wrap finger-mesh geoms).

void SetNodeAttr(pugi::xml_node n, const char *name, const char *val) {
  pugi::xml_attribute a = n.attribute(name);
  if (a) a.set_value(val); else n.append_attribute(name).set_value(val);
}

// Column-major 4x4 -> translation + quaternion {w,x,y,z}.
void DecomposePosQuat(const std::vector<double> &m, std::array<double, 3> *pos,
                      Quat *q) {
  *pos = {{m[12], m[13], m[14]}};
  const double trace = m[0] + m[5] + m[10];
  double w, x, y, z;
  if (trace > 0.0) {
    const double S = 2.0 * std::sqrt(trace + 1.0);
    w = 0.25 * S; x = (m[6] - m[9]) / S; y = (m[8] - m[2]) / S; z = (m[1] - m[4]) / S;
  } else if (m[0] > m[5] && m[0] > m[10]) {
    const double S = 2.0 * std::sqrt(1.0 + m[0] - m[5] - m[10]);
    w = (m[6] - m[9]) / S; x = 0.25 * S; y = (m[4] + m[1]) / S; z = (m[8] + m[2]) / S;
  } else if (m[5] > m[10]) {
    const double S = 2.0 * std::sqrt(1.0 + m[5] - m[0] - m[10]);
    w = (m[8] - m[2]) / S; x = (m[4] + m[1]) / S; y = 0.25 * S; z = (m[9] + m[6]) / S;
  } else {
    const double S = 2.0 * std::sqrt(1.0 + m[10] - m[0] - m[5]);
    w = (m[1] - m[4]) / S; x = (m[8] + m[2]) / S; y = (m[9] + m[6]) / S; z = 0.25 * S;
  }
  *q = QuatNormalize({{w, x, y, z}});
}

void FlattenFramesIn(pugi::xml_node parent, const Context &ctx) {
  // Bottom-up: flatten frames nested inside children before dissolving the
  // frames directly under `parent`.
  for (pugi::xml_node c : parent.children()) FlattenFramesIn(c, ctx);

  pugi::xml_node child = parent.first_child();
  while (child) {
    pugi::xml_node next = child.next_sibling();
    if (std::string(child.name()) == "frame") {
      const std::array<double, 3> fpos = ParseDouble3(Attr(child, "pos"), {{0, 0, 0}});
      const Quat fq = OrientationQuat(Attr(child, "quat"), Attr(child, "axisangle"),
                                      Attr(child, "euler"), Attr(child, "xyaxes"),
                                      Attr(child, "zaxis"), ctx);
      const std::vector<double> fm = MatrixFromPosQuat(fpos, fq);
      const std::string fcc = Attr(child, "childclass");
      for (pugi::xml_node gc : child.children()) {
        const std::array<double, 3> cpos = ParseDouble3(Attr(gc, "pos"), {{0, 0, 0}});
        const Quat cq = OrientationQuat(Attr(gc, "quat"), Attr(gc, "axisangle"),
                                        Attr(gc, "euler"), Attr(gc, "xyaxes"),
                                        Attr(gc, "zaxis"), ctx);
        const std::vector<double> cm = MultiplyMatrix(fm, MatrixFromPosQuat(cpos, cq));
        std::array<double, 3> npos;
        Quat nq;
        DecomposePosQuat(cm, &npos, &nq);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%.10g %.10g %.10g", npos[0], npos[1], npos[2]);
        SetNodeAttr(gc, "pos", buf);
        std::snprintf(buf, sizeof(buf), "%.12g %.12g %.12g %.12g", nq[0], nq[1], nq[2], nq[3]);
        SetNodeAttr(gc, "quat", buf);
        gc.remove_attribute("euler");
        gc.remove_attribute("axisangle");
        gc.remove_attribute("xyaxes");
        gc.remove_attribute("zaxis");
        // A capsule/cylinder fromto is in the frame's coordinates too.
        if (pugi::xml_attribute ft = gc.attribute("fromto")) {
          const auto v = ParseDoubles(ft.value());
          if (v.size() == 6) {
            auto tp = [&](double a, double b, double c) {
              return std::array<double, 3>{{fm[0] * a + fm[4] * b + fm[8] * c + fm[12],
                                            fm[1] * a + fm[5] * b + fm[9] * c + fm[13],
                                            fm[2] * a + fm[6] * b + fm[10] * c + fm[14]}};
            };
            const auto p0 = tp(v[0], v[1], v[2]);
            const auto p1 = tp(v[3], v[4], v[5]);
            std::snprintf(buf, sizeof(buf), "%.10g %.10g %.10g %.10g %.10g %.10g",
                          p0[0], p0[1], p0[2], p1[0], p1[1], p1[2]);
            SetNodeAttr(gc, "fromto", buf);
          }
        }
        // Propagate the frame's childclass to children that don't set one.
        if (!fcc.empty() && !gc.attribute("childclass") && !gc.attribute("class")) {
          SetNodeAttr(gc, "childclass", fcc.c_str());
        }
        parent.insert_copy_before(gc, child);
      }
      parent.remove_child(child);
    }
    child = next;
  }
}

bool BuildMujocoPayload(const std::string &xml, const fs::path &input_filename,
                        const Options &opts, nlohmann::json *payload,
                        Stats *stats, std::string *err) {
  std::set<fs::path> seen;
  std::string expanded;
  if (!ExpandIncludes(xml, input_filename.parent_path(), &seen, &expanded, err)) {
    return false;
  }

  pugi::xml_document doc;
  const pugi::xml_parse_result parse_result = doc.load_string(expanded.c_str());
  if (!parse_result) {
    if (err) *err = std::string("MJCF XML parse failed: ") + parse_result.description();
    return false;
  }
  const auto root = doc.child("mujoco");
  if (!root) {
    if (err) *err = "Expected <mujoco> root.";
    return false;
  }

  // Expand <attach> sub-models (graft + prefix child body subtrees) before any
  // semantic parsing, so assets/defaults/tendons/etc. see the merged model.
  if (!ExpandAttachments(root, input_filename.parent_path(), err)) {
    return false;
  }

  const auto assets = CollectMujocoAssets(root, input_filename.parent_path());
  const auto hfields = CollectMujocoHFields(root, input_filename.parent_path());
  // MuJoCo merges every <worldbody> block (the local one plus any pulled in via
  // <include>) into a single world; visit them all, not just the first.
  const auto worldbodies = Children(root, "worldbody");
  if (worldbodies.empty()) {
    if (err) *err = "MJCF has no <worldbody>.";
    return false;
  }

  // <compiler angle/eulerseq> + <default> tree drive orientation units and
  // class inheritance for the whole scene.
  Context ctx;
  if (const auto compiler = Child(root, "compiler")) {
    if (ToLower(Attr(compiler, "angle", "degree")) == "radian") {
      ctx.angle_to_rad = 1.0;
    }
    const std::string eseq = Attr(compiler, "eulerseq");
    if (!eseq.empty()) ctx.eulerseq = eseq;
  }
  ctx.defaults = ParseDefaults(root);

  // Dissolve <frame> grouping transforms now that the angle/eulerseq context is
  // known, so body/geom/site traversal never has to look inside a <frame>.
  for (const auto &worldbody : Children(root, "worldbody")) {
    FlattenFramesIn(worldbody, ctx);
  }

  nlohmann::json links = nlohmann::json::array();
  nlohmann::json joints = nlohmann::json::array();
  nlohmann::json actuators = nlohmann::json::array();
  nlohmann::json tendons = nlohmann::json::array();
  nlohmann::json equalities = nlohmann::json::array();
  nlohmann::json filtered_pairs = nlohmann::json::array();
  nlohmann::json sites = nlohmann::json::array();
  nlohmann::json mjc_actuators = nlohmann::json::array();
  nlohmann::json keyframes = nlohmann::json::array();
  nlohmann::json lights = nlohmann::json::array();
  nlohmann::json cameras = nlohmann::json::array();
  nlohmann::json materials = nlohmann::json::array();
  nlohmann::json sensors = nlohmann::json::array();
  nlohmann::json contact_pairs = nlohmann::json::array();
  for (const auto &worldbody : worldbodies) {
    for (const auto &body : Children(worldbody, "body")) {
      if (!VisitMujocoBody(body, "", "", IdentityMatrix(), assets, hfields, opts,
                           ctx, &links, &joints, stats, err)) {
        return false;
      }
      CollectMujocoSitesJson(body, IdentityMatrix(), ctx, &sites, stats);
    }
    CollectMujocoLightsCamerasJson(worldbody, IdentityMatrix(), ctx, &lights,
                                   &cameras);
  }
  // World-fixed geoms living directly under <worldbody> (floor/ground/hfield).
  if (!AddWorldbodyGeomsLink(worldbodies, assets, hfields, opts, ctx, &links,
                             stats, err)) {
    return false;
  }
  AddMujocoActuatorsJson(root, &actuators, &mjc_actuators, stats);
  AddMujocoTendonsJson(root, ctx, &tendons, stats);
  AddMujocoEqualityJson(root, &equalities, stats);
  AddMujocoContactExcludesJson(root, &filtered_pairs, stats);
  AddMujocoKeyframesJson(root, &keyframes, stats);
  AddMujocoMaterialsJson(root, input_filename.parent_path(), &materials, stats);
  AddMujocoSensorsJson(root, &sensors, stats);
  AddMujocoContactPairsJson(root, &contact_pairs, stats);
  nlohmann::json custom = nlohmann::json::object();
  AddMujocoCustomJson(root, &custom);
  nlohmann::json plugins = nlohmann::json::array();
  AddMujocoPluginsJson(root, &plugins);

  (*payload) = {
      {"name", Attr(root, "model", input_filename.stem().string())},
      {"upAxis", opts.up_axis},
      {"sourceFormat", "mjcf"},
      {"gravity", opts.up_axis == "Z" ? nlohmann::json::array({0, 0, -1})
                                       : nlohmann::json::array({0, -1, 0})},
      {"links", std::move(links)},
      {"joints", std::move(joints)},
      {"actuators", std::move(actuators)}};
  if (!tendons.empty()) (*payload)["tendons"] = std::move(tendons);
  if (!equalities.empty()) (*payload)["equalities"] = std::move(equalities);
  if (!filtered_pairs.empty())
    (*payload)["filteredPairs"] = std::move(filtered_pairs);
  if (!sites.empty()) (*payload)["sites"] = std::move(sites);
  if (!mjc_actuators.empty()) (*payload)["mjcActuators"] = std::move(mjc_actuators);
  nlohmann::json mjc_scene = BuildMjcSceneJson(root);
  if (!mjc_scene.empty()) (*payload)["mjcScene"] = std::move(mjc_scene);
  if (!keyframes.empty()) (*payload)["keyframes"] = std::move(keyframes);
  if (!lights.empty()) (*payload)["lights"] = std::move(lights);
  if (!cameras.empty()) (*payload)["cameras"] = std::move(cameras);
  if (!materials.empty()) (*payload)["materials"] = std::move(materials);
  if (!sensors.empty()) (*payload)["sensors"] = std::move(sensors);
  if (!contact_pairs.empty()) (*payload)["contactPairs"] = std::move(contact_pairs);
  if (!custom.empty()) (*payload)["custom"] = std::move(custom);
  if (!plugins.empty()) (*payload)["plugins"] = std::move(plugins);
  const auto option = Child(root, "option");
  if (option && HasAttr(option, "timestep")) {
    (*payload)["timestep"] = ParseDoubleAttr(option, "timestep", 0.0);
  }
  return true;
}

fs::path OutputPath(const Options &opts, const std::string &format) {
  if (!opts.output_filename.empty()) {
    fs::path out(opts.output_filename);
    if (opts.format == "all") {
      out.replace_extension("." + format);
      return out;
    }
    if (out.extension().empty()) out.replace_extension("." + format);
    return out;
  }
  fs::path out(opts.input_filename);
  out.replace_extension("." + format);
  return out;
}

#if defined(LIGHTUSD_URDF_TO_USD_NEXT)
using OutputStage = lightusd::next::Stage;

bool SaveStage(const OutputStage &stage, const fs::path &filename,
               const std::string &format,
               const std::map<std::string, std::vector<uint8_t>> &assets,
               std::string *err) {
  if (format == "usda") return lightusd::next::WriteUSDA(stage, filename.string(), err);
  if (format == "usdc") return lightusd::next::WriteUSDC(stage, filename.string(), err);
  // Embed referenced textures so the .usdz is self-contained; the archive
  // names match the (relative) inputs:file references in the stage.
  std::vector<uint8_t> usdc;
  const lightusd::next::USDCWriteResult crate =
      lightusd::next::WriteUSDCToMemory(usdc, stage);
  if (!crate.success) {
    if (err) *err = crate.error;
    return false;
  }
  std::vector<uint8_t> usdz;
  const lightusd::next::USDZWriteResult zip =
      lightusd::next::WriteUSDZFromUSDCAndAssetsToMemory(usdz, usdc.data(), usdc.size(),
                                                         assets);
  if (!zip.success) {
    if (err) *err = zip.error;
    return false;
  }
  return WriteFile(filename, usdz, err);
}

bool ConvertPayload(const nlohmann::json &payload, OutputStage *stage,
                    std::string *warn, std::string *err) {
  return lightusd::tydra::next::ConvertURDFJsonToUSDStage(payload.dump(), stage,
                                                          warn, err);
}
#else
using OutputStage = lightusd::Stage;

bool SaveStage(const OutputStage &stage, const fs::path &filename,
               const std::string &format,
               const std::map<std::string, std::vector<uint8_t>> &assets,
               std::string *err) {
  std::string warn;
  bool ok = false;
  if (format == "usda") {
    ok = lightusd::usda::SaveAsUSDA(filename.string(), stage, &warn, err);
  } else if (format == "usdc") {
    ok = lightusd::usdc::SaveAsUSDCToFile(filename.string(), stage, &warn, err);
  } else if (format == "usdz") {
    // Embed referenced textures so the .usdz is self-contained; the archive
    // names match the (relative) inputs:file references in the stage.
    ok = lightusd::SaveAsUSDZToFile(filename.string(), stage, assets, &warn, err);
  }
  if (!warn.empty()) std::cerr << "WARN: " << warn << "\n";
  return ok;
}

bool ConvertPayload(const nlohmann::json &payload, OutputStage *stage,
                    std::string *warn, std::string *err) {
  return lightusd::tydra::ConvertURDFJsonToUSDStage(payload.dump(), stage, warn, err);
}
#endif

}  // namespace

int main(int argc, char **argv) {
  Options opts;
  std::string err;
  if (!ParseArgs(argc, argv, &opts, &err)) {
    std::cerr << "urdf-to-usd: " << err << "\n";
    PrintHelp();
    return EXIT_FAILURE;
  }

  std::error_code abs_ec;
  const fs::path input_path = fs::absolute(opts.input_filename, abs_ec);
  std::string xml;
  if (!ReadFile(input_path, &xml, &err)) {
    std::cerr << "urdf-to-usd: " << err << "\n";
    return EXIT_FAILURE;
  }

  const bool is_mjcf = opts.input_format == "mjcf" ||
                       (opts.input_format == "auto" &&
                        xml.find("<mujoco") != std::string::npos);
  if (!is_mjcf) {
    std::cerr << "urdf-to-usd: native example currently supports MJCF XML input.\n";
    return EXIT_FAILURE;
  }

  nlohmann::json payload;
  Stats stats;
  if (!BuildMujocoPayload(xml, input_path, opts, &payload, &stats, &err)) {
    std::cerr << "urdf-to-usd: " << err << "\n";
    return EXIT_FAILURE;
  }

  if (!opts.dump_json_filename.empty()) {
    fs::ofstream ofs(fs::path(opts.dump_json_filename));
    ofs << payload.dump(2) << "\n";
  }

  OutputStage stage;
  std::string warn;
  if (!ConvertPayload(payload, &stage, &warn, &err)) {
    std::cerr << "urdf-to-usd: " << err << "\n";
    return EXIT_FAILURE;
  }
  if (!warn.empty()) std::cerr << "WARN: " << warn << "\n";

  // Gather referenced texture bytes (keyed by their relative archive name) so a
  // usdz output is a self-contained package. usda/usdc reference the texture by
  // the same relative path and resolve it beside the .usd on disk.
  std::map<std::string, std::vector<uint8_t>> tex_assets;
  if (payload.contains("materials")) {
    for (const auto &m : payload["materials"]) {
      if (!m.contains("texture") || !m.contains("texture_abs")) continue;
      const std::string arc = m["texture"].get<std::string>();
      if (arc.empty() || tex_assets.count(arc)) continue;
      std::string bytes;
      std::string rerr;
      if (ReadFile(fs::path(m["texture_abs"].get<std::string>()), &bytes, &rerr)) {
        tex_assets[arc] =
            std::vector<uint8_t>(bytes.begin(), bytes.end());
      } else {
        std::cerr << "WARN: texture not embedded into usdz: " << rerr << "\n";
      }
    }
  }

  const std::vector<std::string> formats =
      opts.format == "all" ? std::vector<std::string>{"usda", "usdc", "usdz"}
                           : std::vector<std::string>{opts.format};
  for (const std::string &format : formats) {
    const fs::path out = OutputPath(opts, format);
    if (!SaveStage(stage, out, format, tex_assets, &err)) {
      std::cerr << "urdf-to-usd: failed to write " << out << ": " << err << "\n";
      return EXIT_FAILURE;
    }
    std::cout << "Wrote " << out << "\n";
  }

  std::cout << "Converted " << payload.value("name", std::string("scene")) << ": "
            << stats.links << " links, " << stats.joints << " joints, "
            << stats.visuals << " visual meshes, " << stats.collisions
            << " collisions, " << stats.actuators << " actuators.\n";
  return EXIT_SUCCESS;
}
