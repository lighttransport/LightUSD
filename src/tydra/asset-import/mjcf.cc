// SPDX-License-Identifier: Apache-2.0
// Native XML frontends shared by the CLI and language bindings.
#include <algorithm>
#include <atomic>
#include <charconv>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#include "external/filesystem/include/ghc/filesystem.hpp"
#include "external/pugixml.hpp"
#include "import.hh"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "external/stb_image.h"
namespace fs = ghc::filesystem;
namespace lightusd {
namespace tydra {
namespace import_asset {
namespace {
thread_local Reader* active_reader = nullptr;
thread_local Json* active_materials = nullptr;
thread_local std::string active_world_name = "__world";
// <body> node -> exported link name, so later passes (sites) can name the body
// that owns them even when the body name was generated.
thread_local std::map<const void*, std::string> active_body_names;
// Per-geom mass properties in the owning body's frame, gathered while a body's
// geoms are converted when MuJoCo would infer the body inertia from them.
struct GeomMass {
  double mass = 0;
  std::array<double, 3> com{{0, 0, 0}};
  double inertia[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};  // about com
};
thread_local std::vector<GeomMass>* active_geom_masses = nullptr;
// Bodies authored with stacked joints (non-legacy profiles).
thread_local std::set<std::string>* stacked_joint_bodies = nullptr;
constexpr double kPi = 3.14159265358979323846;
bool IsSupportedFlexcompType(const std::string& type);
std::string ToLower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
struct MeshAsset {
  fs::path path;
  std::array<double, 3> scale{{1.0, 1.0, 1.0}};
  std::array<double, 3> refpos{{0.0, 0.0, 0.0}};  // <mesh refpos="...">
  std::array<double, 4> refquat{
      {1.0, 0.0, 0.0, 0.0}};  // <mesh refquat="..."> wxyz
  bool has_refpos = false;
  bool has_refquat = false;
  bool smoothnormal = false;  // <mesh smoothnormal>; MuJoCo default false
  std::string inertia = "legacy";  // <mesh inertia>: legacy|exact|convex|shell
  int maxhullvert = -1;            // <mesh maxhullvert>; -1 = no limit
};

struct UnusedMeshData {
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<float>
      uvs;  // 2 per vertex (st), parallel to positions; empty if none
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
  Mesh mesh;
  Json shape;
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
  AttrMap root_geom;                     // unnamed (top-level) default
  AttrMap root_joint;
};

struct Context {
  // <compiler angle="...">: degrees by default; radians otherwise.
  double angle_to_rad = 0.017453292519943295;  // pi/180
  std::string eulerseq = "xyz";                // <compiler eulerseq="...">
  bool autolimits = true;                      // <compiler autolimits="...">
  std::string inertiafromgeom = "auto";        // <compiler inertiafromgeom>
  int inertia_group_min = 0, inertia_group_max = 5;  // inertiagrouprange
  Defaults defaults;
  // Geoms named by <spatial><geom geom=...>: wrap surfaces must stay native
  // Sphere/Cylinder prims that tendons can reference.
  std::set<std::string> wrap_geoms;
};

bool ParseInt(const std::string& text, int* out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const long v = std::strtol(text.c_str(), &end, 10);
  if (!end || *end != '\0' || v < std::numeric_limits<int>::min() ||
      v > std::numeric_limits<int>::max()) {
    return false;
  }
  *out = static_cast<int>(v);
  return true;
}

// MJCF integer attributes reach us as doubles. Casting an out-of-range or
// non-finite double to an integer is undefined behaviour, so saturate and warn.
int32_t ToInt32(double value, const char* what) {
  constexpr double lo = std::numeric_limits<int32_t>::min();
  constexpr double hi = std::numeric_limits<int32_t>::max();
  if (std::isfinite(value) && value >= lo && value <= hi)
    return static_cast<int32_t>(value);
  if (active_reader)
    active_reader->Warn(std::string("MJCF integer attribute '") + what +
                            "' is out of range; clamped",
                        "mjcf.integer_range");
  if (!std::isfinite(value)) return value > 0 ? INT32_MAX : (value < 0 ? INT32_MIN : 0);
  return value < lo ? INT32_MIN : INT32_MAX;
}

bool ReadFile(const fs::path& filename, std::string* text, std::string* err) {
  std::vector<uint8_t> data;
  std::string resolved;
  if (!active_reader->Read(filename.string(), active_reader->root_source, &data,
                           &resolved, err))
    return false;
  text->assign(reinterpret_cast<const char*>(data.data()), data.size());
  return true;
}

bool WriteFile(const fs::path& filename, const std::vector<uint8_t>& bytes,
               std::string* err) {
  fs::ofstream ofs(filename, std::ios::binary);
  if (!ofs) {
    if (err) *err = "Failed to open output: " + filename.string();
    return false;
  }
  ofs.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return bool(ofs);
}

std::vector<double> ParseDoubles(const std::string& text) {
  std::vector<double> values;
  std::stringstream ss(text);
  double v = 0.0;
  while (ss >> v) {
    if (!std::isfinite(v)) {
      active_reader->failure = "Non-finite XML number";
      return {};
    }
    values.push_back(v);
  }
  if (!ss.eof()) active_reader->failure = "Invalid XML numeric array: " + text;
  return values;
}

std::array<double, 3> ParseDouble3(const std::string& text,
                                   std::array<double, 3> fallback) {
  const std::vector<double> values = ParseDoubles(text);
  if (values.size() >= 3) {
    return {{values[0], values[1], values[2]}};
  }
  return fallback;
}

double ParseDoubleAttr(const pugi::xml_node& node, const std::string& name,
                       double fallback) {
  if (!node || !node.attribute(name.c_str())) return fallback;
  char* end = nullptr;
  const std::string s = node.attribute(name.c_str()).as_string();
  const double v = std::strtod(s.c_str(), &end);
  if (!end || end == s.c_str() || *end || !std::isfinite(v)) {
    active_reader->failure = "Invalid XML number for " + name;
    return fallback;
  }
  return v;
}

bool HasAttr(const pugi::xml_node& node, const std::string& name) {
  return node && node.attribute(name.c_str());
}

std::string Attr(const pugi::xml_node& node, const std::string& name,
                 const std::string& fallback = std::string()) {
  if (!node) return fallback;
  return node.attribute(name.c_str()).as_string(fallback.c_str());
}

std::vector<pugi::xml_node> Children(const pugi::xml_node& node,
                                     const std::string& name) {
  if (!node) return {};
  std::vector<pugi::xml_node> out;
  for (pugi::xml_node child : node.children(name.c_str())) {
    out.push_back(child);
  }
  return out;
}

pugi::xml_node Child(const pugi::xml_node& node, const std::string& name) {
  if (!node) return pugi::xml_node();
  return node.child(name.c_str());
}

std::string StripMujocoRoot(const std::string& xml) {
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

std::string AttributeFromTag(const std::string& tag, const std::string& name) {
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

bool ValidateXMLDepth(pugi::xml_node root, size_t limit, const std::string& source,
                       std::string* err) {
  // Constant-space DFS also validates skipped/unsupported subtrees. Never
  // recurse into attacker-controlled XML just to enforce its depth bound.
  auto node = root;
  size_t depth = 0;
  for (;;) {
    if (depth > limit) {
      if (err) *err = "XML depth limit exceeded";
      if (active_reader->options.report)
        active_reader->options.report->Diagnostic("error", "import.depth_limit", "XML depth limit exceeded", source);
      return false;
    }
    if (node.first_child()) { node = node.first_child(); ++depth; continue; }
    while (node != root && !node.next_sibling()) { node = node.parent(); --depth; }
    if (node == root) break;
    node = node.next_sibling();
  }
  return true;
}

bool UnsupportedXML(pugi::xml_node node, const std::string& message,
    const char* code, const char* feature, const std::string& source,
    bool approximated = false) {
  std::string data;
  if (active_reader->options.unsupported_policy == UnsupportedPolicy::Preserve) {
    struct Writer : pugi::xml_writer {
      size_t cap;
      std::string text;
      bool valid = true;
      explicit Writer(size_t limit) : cap(limit) {}
      void write(const void* bytes, size_t size) override {
        if (!valid || size > cap - text.size()) { valid = false; return; }
        text.append(static_cast<const char*>(bytes), size);
      }
    } writer((active_reader->options.max_memory - std::min(active_reader->bytes,
                     active_reader->options.max_memory)) / 4);
    node.print(writer, "", pugi::format_raw);
    if (!writer.valid) {
      active_reader->failure = "Unsupported XML preservation memory limit exceeded";
      return false;
    }
    data = std::move(writer.text);
  }
  return active_reader->Unsupported(message, code, feature, source, 0,
      std::move(data), "xml", approximated);
}

bool ExpandIncludes(const std::string& xml, const fs::path& base_dir,
                    std::set<fs::path>* seen, std::string* expanded,
                    std::string* err, const std::string& source) {
  if (seen->size() >= active_reader->options.max_depth) {
    if (err) *err = "MJCF include depth limit exceeded";
    if (active_reader->options.report)
      active_reader->options.report->Diagnostic("error", "import.depth_limit", "MJCF include depth limit exceeded", source);
    return false;
  }
  if (xml.find("<!DOCTYPE") != std::string::npos ||
      xml.find("<!ENTITY") != std::string::npos) {
    if (err) *err = "XML DTD and entity declarations are unsupported";
    return false;
  }
  if (!active_reader->Charge(xml.size() * 8, err)) return false;
  pugi::xml_document doc;
  auto parsed = doc.load_buffer(xml.data(), xml.size());
  if (!parsed) {
    if (err)
      *err = std::string("MJCF XML parse failed: ") + parsed.description();
    return false;
  }
  if (!ValidateXMLDepth(doc, active_reader->options.max_depth, source, err)) return false;
  auto root = doc.document_element();
  auto compiler = root.child("compiler");
  // Keep the declaring anchor until every include has contributed compiler
  // settings. A scene may inherit meshdir from its included robot model.
  for (auto asset : root.children("asset"))
    for (auto item : asset.children()) {
      const auto file = item.attribute("file");
      if (!file || !*file.value() || fs::path(file.value()).is_absolute() ||
          std::string(file.value()).find(':') != std::string::npos)
        continue;
      item.append_attribute("_lightusd_asset_base")
          .set_value(base_dir.generic_string().c_str());
      std::string dir;
      if (std::string(item.name()) == "texture")
        dir = compiler.attribute("texturedir").as_string();
      else if (std::string(item.name()) != "model")
        dir = compiler.attribute("meshdir").as_string();
      if (dir.empty() && std::string(item.name()) != "model")
        dir = compiler.attribute("assetdir").as_string();
      if (!dir.empty())
        item.append_attribute("_lightusd_asset_dir").set_value(dir.c_str());
    }
  std::function<bool(pugi::xml_node, size_t)> visit = [&](pugi::xml_node parent,
                                                          size_t depth) {
    if (depth > active_reader->options.max_depth) {
      if (err) *err = "XML depth limit exceeded";
      if (active_reader->options.report)
        active_reader->options.report->Diagnostic("error", "import.depth_limit", "XML depth limit exceeded", source);
      return false;
    }
    auto node = parent.first_child();
    while (node) {
      auto next = node.next_sibling();
      const std::string tag = node.name();
      // The newton profile converts <flexcomp> into a soft body later.
      const std::string flex_type = ToLower(node.attribute("type").as_string("grid"));
      const bool newton = active_reader->options.profile == "newton";
      const bool soft_body = tag == "flexcomp" && newton && IsSupportedFlexcompType(flex_type);
      // Raw <deformable><flex body=... element=...> becomes a soft body too
      // (CollectMujocoRawFlexesJson); <skin> and other children stay reported.
      const bool raw_flex = tag == "flex" && newton && std::string(parent.name()) == "deformable" &&
                            node.attribute("body") && node.attribute("element");
      const bool flex_container = tag == "deformable" && newton;
      if (!soft_body && !raw_flex && !flex_container &&
          (tag == "flex" || tag == "flexcomp" || tag == "deformable" ||
                         tag == "skin")) {
        if (!UnsupportedXML(node, "Unsupported MJCF element: " + tag,
                            "mjcf.element_unsupported", "mjcf.deformable", source)) {
          if (err) *err = active_reader->failure;
          return false;
        }
      } else if (tag == "include") {
        std::string file = node.attribute("file").as_string();
        if (file.empty()) {
          if (err) *err = "MJCF include has no file";
          return false;
        }
        std::vector<uint8_t> bytes;
        std::string resolved;
        if (!active_reader->Read(file,
                                 (base_dir / "__include_anchor.xml").string(),
                                 &bytes, &resolved, err))
          return false;
        fs::path key(resolved);
        if (!seen->insert(key).second) {
          if (err) *err = "Recursive MJCF include: " + resolved;
          return false;
        }
        std::string child_xml(reinterpret_cast<const char*>(bytes.data()),
                              bytes.size()),
            child_expanded;
        if (!ExpandIncludes(child_xml, key.parent_path(), seen, &child_expanded,
                            err, resolved))
          return false;
        seen->erase(key);
        pugi::xml_document child_doc;
        if (!child_doc.load_buffer(child_expanded.data(),
                                   child_expanded.size())) {
          if (err) *err = "Invalid expanded include";
          return false;
        }
        for (auto c : child_doc.document_element().children())
          parent.insert_copy_before(c, node);
        parent.remove_child(node);
      } else if (!visit(node, depth + 1))
        return false;
      node = next;
    }
    return true;
  };
  if (!visit(root, 0)) return false;
  if (!ValidateXMLDepth(doc, active_reader->options.max_depth, source, err)) return false;
  if (seen->empty()) {
    // MJCF permits repeated singleton sections after textual inclusion. Merge
    // attributes in document order rather than reading only the first block.
    // A scene's compiler meshdir must not hide its robot's angle="radian".
    for (const char* section : {"compiler", "option"}) {
      auto first = root.child(section);
      if (!first) continue;
      for (auto next = first.next_sibling(section); next;) {
        auto following = next.next_sibling(section);
        auto merge_attrs = [](pugi::xml_node dst, pugi::xml_node src) {
          for (auto attr : src.attributes()) {
            auto value = dst.attribute(attr.name());
            if (!value) value = dst.append_attribute(attr.name());
            value.set_value(attr.value());
          }
        };
        merge_attrs(first, next);
        for (auto child : next.children()) {
          auto existing = first.child(child.name());
          if (existing)
            merge_attrs(existing, child);
          else
            first.append_copy(child);
        }
        root.remove_child(next);
        next = following;
      }
    }
    // Container sections concatenate after inclusion. Reading only the first
    // one silently dropped most tendons/actuators in large muscle models.
    for (const char* section :
         {"asset", "actuator", "tendon", "equality", "sensor", "contact",
          "keyframe", "custom", "extension"}) {
      auto first = root.child(section);
      if (!first) continue;
      for (auto next = first.next_sibling(section); next;) {
        auto following = next.next_sibling(section);
        for (auto child : next.children()) first.append_copy(child);
        root.remove_child(next);
        next = following;
      }
    }
    std::string meshdir, texturedir, assetdir;
    for (auto c : root.children("compiler")) {
      if (c.attribute("meshdir")) meshdir = c.attribute("meshdir").value();
      if (c.attribute("texturedir"))
        texturedir = c.attribute("texturedir").value();
      if (c.attribute("assetdir")) assetdir = c.attribute("assetdir").value();
    }
    for (auto asset : root.children("asset"))
      for (auto item : asset.children()) {
        auto base = item.attribute("_lightusd_asset_base");
        if (!base) continue;
        std::string dir = item.attribute("_lightusd_asset_dir").as_string();
        const std::string tag = item.name();
        if (dir.empty() && tag != "model") {
          dir = tag == "texture" ? texturedir : meshdir;
          if (dir.empty()) dir = assetdir;
        }
        fs::path anchor(base.value()), path(item.attribute("file").value());
        fs::path candidate = anchor / dir / path;
        if (!active_reader->options.read && !active_reader->options.open) {
          std::error_code ec;
          if (!fs::exists(candidate, ec) && fs::exists(anchor / path, ec))
            candidate = anchor / path;
        }
        item.attribute("file").set_value(
            candidate.lexically_normal().generic_string().c_str());
        item.remove_attribute("_lightusd_asset_base");
        item.remove_attribute("_lightusd_asset_dir");
      }
  }

  struct Writer : pugi::xml_writer {
    std::string text;
    void write(const void* data, size_t size) override {
      text.append(static_cast<const char*>(data), size);
    }
  } writer;
  doc.save(writer, "", pugi::format_raw);
  *expanded = std::move(writer.text);
  return true;
}

// Quaternions are stored MuJoCo-style: {w, x, y, z}.
using Quat = std::array<double, 4>;

Quat QuatNormalize(const Quat& q) {
  const double n =
      std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (n < 1.0e-12) return {{1.0, 0.0, 0.0, 0.0}};
  return {{q[0] / n, q[1] / n, q[2] / n, q[3] / n}};
}

// Hamilton product a * b ({w,x,y,z}).
Quat QuatMul(const Quat& a, const Quat& b) {
  return {{a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
           a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
           a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
           a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]}};
}

Quat QuatFromAxisAngle(const std::array<double, 3>& axis, double angle) {
  const double len =
      std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  if (len < 1.0e-12) return {{1.0, 0.0, 0.0, 0.0}};
  const double s = std::sin(angle * 0.5) / len;
  return {{std::cos(angle * 0.5), axis[0] * s, axis[1] * s, axis[2] * s}};
}

// Rotation that maps unit vector `from` onto unit vector `to`.
Quat QuatFromTwoVecs(const std::array<double, 3>& from,
                     const std::array<double, 3>& to) {
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
    // Antiparallel: rotate pi about an axis orthogonal to `a`. For +-Z this
    // is X, matching MuJoCo's mju_quatZ2Vec (zaxis/fromto pointing down).
    if (std::abs(a[2]) >= 0.9)
      return QuatFromAxisAngle({{1.0, 0.0, 0.0}}, 3.14159265358979323846);
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
Quat QuatFromBasis(const std::array<double, 3>& cx,
                   const std::array<double, 3>& cy,
                   const std::array<double, 3>& cz) {
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
Quat OrientationQuat(const std::string& quat_str,
                     const std::string& axisangle_str,
                     const std::string& euler_str,
                     const std::string& xyaxes_str,
                     const std::string& zaxis_str, const Context& ctx) {
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
      if (lower == 'x')
        axis = {{1.0, 0.0, 0.0}};
      else if (lower == 'y')
        axis = {{0.0, 1.0, 0.0}};
      else if (lower == 'z')
        axis = {{0.0, 0.0, 1.0}};
      else
        continue;
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
std::vector<double> MatrixFromPosQuat(const std::array<double, 3>& pos,
                                      const Quat& q) {
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
  return {1.0 - (yy + zz), xy + wz,         xz - wy,         0.0,
          xy - wz,         1.0 - (xx + zz), yz + wx,         0.0,
          xz + wy,         yz - wx,         1.0 - (xx + yy), 0.0,
          pos[0],          pos[1],          pos[2],          1.0};
}

// Pose matrix for a body/geom node, reading any MuJoCo orientation specifier
// directly off the node (orientation is authored on the element, not in
// <default> classes).
std::vector<double> PoseMatrix(const pugi::xml_node& node, const Context& ctx) {
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
// argument is the innermost/local transform, matching Three.js
// Matrix4.multiply.
std::vector<double> MultiplyMatrix(const std::vector<double>& a,
                                   const std::vector<double>& b) {
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

std::vector<double> ScaleMatrix(const std::array<double, 3>& scale) {
  return {scale[0], 0.0, 0.0,      0.0, 0.0, scale[1], 0.0, 0.0,
          0.0,      0.0, scale[2], 0.0, 0.0, 0.0,      0.0, 1.0};
}

std::vector<double> TranslationMatrix(const std::array<double, 3>& t) {
  return {1.0, 0.0, 0.0, 0.0, 0.0,  1.0,  0.0,  0.0,
          0.0, 0.0, 1.0, 0.0, t[0], t[1], t[2], 1.0};
}

// Mesh frame transform from MJCF <mesh refpos/refquat>: vertices are first
// translated by -refpos, then rotated by conjugate(refquat). Returned as
// R(conj(refquat)) * T(-refpos) so it can left-compose with the geom matrix.
std::vector<double> MeshRefMatrix(const MeshAsset& asset) {
  std::vector<double> m = {1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                           0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0};
  if (asset.has_refquat) {
    const Quat conj{{asset.refquat[0], -asset.refquat[1], -asset.refquat[2],
                     -asset.refquat[3]}};
    m = MatrixFromPosQuat({{0.0, 0.0, 0.0}}, QuatNormalize(conj));
  }
  if (asset.has_refpos) {
    m = MultiplyMatrix(
        m, TranslationMatrix(
               {{-asset.refpos[0], -asset.refpos[1], -asset.refpos[2]}}));
  }
  return m;
}

void AppendTri(Mesh* mesh, const std::array<float, 3>& a,
               const std::array<float, 3>& b, const std::array<float, 3>& c) {
  const int32_t base = static_cast<int32_t>(mesh->positions.size() / 3);
  const std::array<std::array<float, 3>, 3> tri{{a, b, c}};
  for (size_t i = 0; i < tri.size(); i++) {
    const auto& v = tri[i];
    mesh->positions.push_back(v[0]);
    mesh->positions.push_back(v[1]);
    mesh->positions.push_back(v[2]);
    mesh->indices.push_back(base + static_cast<int32_t>(i));
  }
}

Mesh MakeBoxMesh(const std::array<double, 3>& half) {
  Mesh mesh;
  const float x = static_cast<float>(std::max(half[0], 1.0e-6));
  const float y = static_cast<float>(std::max(half[1], 1.0e-6));
  const float z = static_cast<float>(std::max(half[2], 1.0e-6));
  const std::array<std::array<float, 3>, 8> v{{
      {{-x, -y, -z}},
      {{x, -y, -z}},
      {{x, y, -z}},
      {{-x, y, -z}},
      {{-x, -y, z}},
      {{x, -y, z}},
      {{x, y, z}},
      {{-x, y, z}},
  }};
  const int faces[12][3] = {{0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6},
                            {0, 4, 5}, {0, 5, 1}, {1, 5, 6}, {1, 6, 2},
                            {2, 6, 7}, {2, 7, 3}, {3, 7, 4}, {3, 4, 0}};
  for (const auto& f : faces) AppendTri(&mesh, v[f[0]], v[f[1]], v[f[2]]);
  return mesh;
}

Mesh MakeSphereMesh(double radius, int slices = 16, int stacks = 8) {
  Mesh mesh;
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

// Capsule along Z: the sphere's upper and lower hemispheres shifted by
// +-half_length, joined at the equator (a cylinder side between them).
Mesh MakeCapsuleMesh(double radius, double half_length, int slices = 16, int stacks = 8) {
  Mesh mesh;
  const double r = std::max(radius, 1.0e-6);
  const double h = std::max(half_length, 0.0);
  std::vector<std::array<float, 3>> verts;
  // stacks + 2 rings: the equator is duplicated, once per hemisphere.
  for (int iy = 0; iy <= stacks + 1; iy++) {
    const int k = iy <= stacks / 2 ? iy : iy - 1;
    const double phi = double(k) / double(stacks) * kPi;
    const double z = r * std::cos(phi) + (iy <= stacks / 2 ? h : -h);
    for (int ix = 0; ix <= slices; ix++) {
      const double theta = double(ix) / double(slices) * 2.0 * kPi;
      verts.push_back({{static_cast<float>(r * std::sin(phi) * std::cos(theta)),
                        static_cast<float>(r * std::sin(phi) * std::sin(theta)),
                        static_cast<float>(z)}});
    }
  }
  for (int iy = 0; iy <= stacks; iy++) {
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

Mesh MakeCylinderMesh(double radius, double half_length, int slices = 20) {
  Mesh mesh;
  const float r = static_cast<float>(std::max(radius, 1.0e-6));
  const float h = static_cast<float>(std::max(half_length, 1.0e-6));
  const std::array<float, 3> top{{0.0f, 0.0f, h}};
  const std::array<float, 3> bottom{{0.0f, 0.0f, -h}};
  for (int i = 0; i < slices; i++) {
    const double a0 = 2.0 * 3.14159265358979323846 * double(i) / double(slices);
    const double a1 =
        2.0 * 3.14159265358979323846 * double(i + 1) / double(slices);
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

float ReadFloatLE(const uint8_t* bytes, size_t offset) {
  float v = 0.0f;
  std::memcpy(&v, bytes + offset, sizeof(float));
  return v;
}

// MuJoCo removes repeated STL vertices and computes one normal per vertex:
// face normals weighted by area, and (unless smoothnormal) a face whose normal
// deviates from the vertex average by more than acos(0.8) is left out so
// sharp edges stay sharp. Reproduce that so the USD mesh is indexed (an STL
// is a triangle soup, ~6x the unique positions) and shades like MuJoCo.
void ComputeMujocoVertexNormals(Mesh* mesh, bool smoothnormal) {
  const size_t nv = mesh->positions.size() / 3;
  const size_t nf = mesh->indices.size() / 3;
  std::vector<double> acc(nv * 3, 0.0), face(nf * 3, 0.0);
  auto P = [&](int32_t i, int c) {
    return double(mesh->positions[size_t(i) * 3 + c]);
  };
  for (size_t f = 0; f < nf; ++f) {
    const int32_t a = mesh->indices[f * 3], b = mesh->indices[f * 3 + 1],
                  c = mesh->indices[f * 3 + 2];
    const double e1[3] = {P(b, 0) - P(a, 0), P(b, 1) - P(a, 1),
                          P(b, 2) - P(a, 2)};
    const double e2[3] = {P(c, 0) - P(a, 0), P(c, 1) - P(a, 1),
                          P(c, 2) - P(a, 2)};
    const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                         e1[2] * e2[0] - e1[0] * e2[2],
                         e1[0] * e2[1] - e1[1] * e2[0]};  // length = 2 * area
    for (int k = 0; k < 3; ++k) face[f * 3 + k] = n[k];
    for (int32_t v : {a, b, c}) {
      for (int k = 0; k < 3; ++k) acc[size_t(v) * 3 + k] += n[k];
    }
  }
  auto normalized = [](const double* v, double out[3]) {
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

// ASCII STL: welded by exact float bit pattern like the binary path.
bool LoadASCIISTL(std::string_view text, const fs::path& filename, bool smoothnormal,
                  Mesh* mesh, std::string* err) {
  struct Key {
    uint32_t x, y, z;
    bool operator<(const Key& o) const {
      return x != o.x ? x < o.x : (y != o.y ? y < o.y : z < o.z);
    }
  };
  std::map<Key, int32_t> welded;
  mesh->positions.clear();
  mesh->indices.clear();
  size_t pos = 0, corners = 0;
  auto skip_space = [&]() {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
  };
  auto word = [&]() {
    skip_space();
    const size_t begin = pos;
    while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    return text.substr(begin, pos - begin);
  };
  while (pos < text.size()) {
    const std::string_view token = word();
    if (token.empty()) break;
    if (token != "vertex") continue;
    float v[3];
    for (float& c : v) {
      skip_space();
      const char* begin = text.data() + pos;
      const auto parsed = std::from_chars(begin, text.data() + text.size(), c);
      if (parsed.ec != std::errc() || !std::isfinite(c)) {
        if (err) *err = "Invalid ASCII STL vertex: " + filename.string();
        return false;
      }
      pos += size_t(parsed.ptr - begin);
    }
    if (!active_reader->Charge(96, err)) return false;
    Key key;
    std::memcpy(&key.x, &v[0], 4);
    std::memcpy(&key.y, &v[1], 4);
    std::memcpy(&key.z, &v[2], 4);
    auto found = welded.find(key);
    if (found == welded.end()) {
      if (mesh->positions.size() / 3 >= size_t(INT32_MAX)) {
        if (err) *err = "ASCII STL has too many vertices: " + filename.string();
        return false;
      }
      found = welded.emplace(key, static_cast<int32_t>(mesh->positions.size() / 3)).first;
      mesh->positions.insert(mesh->positions.end(), v, v + 3);
    }
    mesh->indices.push_back(found->second);
    ++corners;
  }
  if (corners == 0 || corners % 3) {
    if (err) *err = "ASCII STL has no complete triangles: " + filename.string();
    return false;
  }
  ComputeMujocoVertexNormals(mesh, smoothnormal);
  return true;
}

bool LoadBinarySTL(const fs::path& filename, bool smoothnormal, Mesh* mesh,
                   std::string* err) {
  MappedAsset asset;
  std::string resolved;
  if (!active_reader->Map(filename.string(), active_reader->root_source, &asset, &resolved,
                          err))
    return false;
  const uint8_t* bytes = asset.data();
  if (asset.size() < 84) {
    if (err) *err = "STL is too small: " + filename.string();
    return false;
  }
  uint32_t tri_count = 0;
  std::memcpy(&tri_count, bytes + 80, sizeof(uint32_t));
  const uint64_t expected = 84ull + uint64_t(tri_count) * 50ull;
  if (expected != asset.size()) {
    // ASCII STL ("solid ... facet ... vertex x y z ... endsolid").
    std::string_view text = asset.text();
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first != std::string_view::npos && text.compare(first, 5, "solid") == 0)
      return LoadASCIISTL(text, filename, smoothnormal, mesh, err);
    if (err)
      *err = "STL is neither binary with an exact triangle count nor ASCII: " +
             filename.string();
    return false;
  }

  if (!active_reader->Charge(size_t(tri_count) * 3 * 96, err)) return false;
  // Weld bit-identical positions (MuJoCo's repeated-vertex removal) through
  // an open-addressed table; indices follow first occurrence.
  struct Slot {
    uint32_t x, y, z;
    int32_t index;  // -1: empty
  };
  size_t capacity = 16;
  while (capacity < size_t(tri_count) * 6) capacity <<= 1;
  std::vector<Slot> table(capacity, Slot{0, 0, 0, -1});
  const size_t mask = capacity - 1;
  mesh->positions.clear();
  mesh->indices.clear();
  mesh->indices.reserve(size_t(tri_count) * 3);
  for (uint32_t i = 0; i < tri_count; i++) {
    const size_t base = 84ull + size_t(i) * 50ull;
    for (size_t v = 0; v < 3; v++) {
      const size_t vo = base + 12 + v * 12;
      uint32_t x, y, z;
      std::memcpy(&x, bytes + vo + 0, 4);
      std::memcpy(&y, bytes + vo + 4, 4);
      std::memcpy(&z, bytes + vo + 8, 4);
      uint64_t h = (uint64_t(x) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(y) * 0xC2B2AE3D27D4EB4Full) ^
                   (uint64_t(z) * 0x165667B19E3779F9ull);
      h ^= h >> 29;
      size_t at = size_t(h) & mask;
      while (table[at].index >= 0 && !(table[at].x == x && table[at].y == y && table[at].z == z))
        at = (at + 1) & mask;
      if (table[at].index < 0) {
        table[at] = Slot{x, y, z, static_cast<int32_t>(mesh->positions.size() / 3)};
        mesh->positions.push_back(ReadFloatLE(bytes, vo + 0));
        mesh->positions.push_back(ReadFloatLE(bytes, vo + 4));
        mesh->positions.push_back(ReadFloatLE(bytes, vo + 8));
      }
      mesh->indices.push_back(table[at].index);
    }
  }
  ComputeMujocoVertexNormals(mesh, smoothnormal);
  return true;
}

// One polygon as MuJoCo's OBJ decoder sees it: tinyobjloader 2945a96 (the
// version MuJoCo 3.16 pins), float as its real_t. Quads split along the
// shorter diagonal; larger polygons are ear-clipped in the plane picked from
// the first non-degenerate corner. A fan differs for concave polygons, and legacy
// inertia sums |volume| per triangle, so the masses would too. Like
// tinyobjloader, a polygon the clipper cannot finish drops its remainder.
// Appends triangles as corner numbers 0..n-1 of the polygon. `work` bounds
// the clipper's O(n^3) worst case: when it runs out the polygon is a fan and
// false is returned through *exhausted.
bool ClipPolygon(const std::vector<float>& pos, const int32_t* poly, size_t n,
                 std::vector<int32_t>* tri, size_t* work = nullptr, bool* exhausted = nullptr) {
  for (size_t k = 0; k < n; ++k)
    if (poly[k] < 0 || size_t(poly[k]) * 3 + 2 >= pos.size()) return false;
  auto at = [&](int32_t vi, size_t axis) { return pos[size_t(vi) * 3 + axis]; };
  if (n == 4) {
    float d02 = 0, d13 = 0;
    for (size_t a = 0; a < 3; ++a) {
      const float e02 = at(poly[2], a) - at(poly[0], a), e13 = at(poly[3], a) - at(poly[1], a);
      d02 += e02 * e02;
      d13 += e13 * e13;
    }
    if (d02 < d13) tri->insert(tri->end(), {0, 1, 2, 0, 2, 3});
    else tri->insert(tri->end(), {0, 1, 3, 1, 2, 3});
    return true;
  }
  size_t axes[2] = {1, 2};
  for (size_t k = 0; k < n; ++k) {
    const int32_t i0 = poly[k % n], i1 = poly[(k + 1) % n], i2 = poly[(k + 2) % n];
    const float e0x = at(i1, 0) - at(i0, 0), e0y = at(i1, 1) - at(i0, 1), e0z = at(i1, 2) - at(i0, 2);
    const float e1x = at(i2, 0) - at(i1, 0), e1y = at(i2, 1) - at(i1, 1), e1z = at(i2, 2) - at(i1, 2);
    const float cx = std::fabs(e0y * e1z - e0z * e1y), cy = std::fabs(e0z * e1x - e0x * e1z),
                cz = std::fabs(e0x * e1y - e0y * e1x);
    const float epsilon = std::numeric_limits<float>::epsilon();
    if (cx > epsilon || cy > epsilon || cz > epsilon) {
      if (!(cx > cy && cx > cz)) {
        axes[0] = 0;
        if (cz > cx && cz > cy) axes[1] = 1;
      }
      break;
    }
  }
  std::vector<int32_t> rest(n);  // remaining corners
  for (size_t k = 0; k < n; ++k) rest[k] = int32_t(k);
  size_t guess = 0, iterations = n, previous = n;
  const size_t first = tri->size();
  while (rest.size() > 3 && iterations > 0) {
    const size_t m = rest.size();
    if (work) {
      if (*work < m) {
        tri->resize(first);
        for (size_t k = 1; k + 1 < n; ++k) tri->insert(tri->end(), {0, int32_t(k), int32_t(k + 1)});
        if (exhausted) *exhausted = true;
        return true;
      }
      *work -= m;
    }
    if (guess >= m) guess -= m;
    if (previous != m) {
      previous = m;
      iterations = m;
    } else {
      --iterations;
    }
    int32_t ind[3];
    float vx[3], vy[3];
    for (size_t k = 0; k < 3; ++k) {
      ind[k] = rest[(guess + k) % m];
      vx[k] = at(poly[ind[k]], axes[0]);
      vy[k] = at(poly[ind[k]], axes[1]);
    }
    const float cross = (vx[1] - vx[0]) * (vy[2] - vy[1]) - (vy[1] - vy[0]) * (vx[2] - vx[1]);
    // (tinyobjloader's per-candidate "area" term, not the polygon's.)
    const float area = (vx[0] * vy[1] - vy[0] * vx[1]) * 0.5f;
    if (cross * area < 0.0f) {
      ++guess;
      continue;
    }
    bool overlap = false;
    for (size_t other = 3; other < m && !overlap; ++other) {
      const int32_t o = poly[rest[(guess + other) % m]];
      const float tx = at(o, axes[0]), ty = at(o, axes[1]);
      bool inside = false;
      for (size_t i = 0, j = 2; i < 3; j = i++)
        if (((vy[i] > ty) != (vy[j] > ty)) &&
            (tx < (vx[j] - vx[i]) * (ty - vy[i]) / (vy[j] - vy[i]) + vx[i]))
          inside = !inside;
      overlap = inside;
    }
    if (overlap) {
      ++guess;
      continue;
    }
    tri->insert(tri->end(), ind, ind + 3);
    rest.erase(rest.begin() + std::ptrdiff_t((guess + 1) % m));
  }
  if (rest.size() == 3) tri->insert(tri->end(), rest.begin(), rest.end());
  return true;
}

// Polygon faces -> triangles as MuJoCo's OBJ decoder makes them
// (ClipPolygon), keeping per-corner normals/UVs and per-face materials.
bool TriangulateLikeMuJoCo(Mesh* mesh) {
  if (mesh->counts.empty()) return true;
  const bool corner_normals = mesh->normal_indices.size() == mesh->indices.size();
  const bool corner_uvs = mesh->uv_indices.size() == mesh->indices.size();
  const bool face_materials = mesh->materials.size() == mesh->counts.size();
  std::vector<int32_t> indices, normal_indices, uv_indices, materials, corners;
  size_t work = size_t(1) << 28;
  bool exhausted = false;
  size_t offset = 0;
  for (size_t face = 0; face < mesh->counts.size(); ++face) {
    const int32_t count = mesh->counts[face];
    if (count < 3 || offset + size_t(count) > mesh->indices.size()) return false;
    corners.clear();
    if (count == 3) corners = {0, 1, 2};
    else if (!ClipPolygon(mesh->positions, mesh->indices.data() + offset, size_t(count), &corners,
                          &work, &exhausted))
      return false;
    for (int32_t c : corners) {
      indices.push_back(mesh->indices[offset + size_t(c)]);
      if (corner_normals) normal_indices.push_back(mesh->normal_indices[offset + size_t(c)]);
      if (corner_uvs) uv_indices.push_back(mesh->uv_indices[offset + size_t(c)]);
    }
    if (face_materials) materials.insert(materials.end(), corners.size() / 3, mesh->materials[face]);
    offset += size_t(count);
  }
  if (offset != mesh->indices.size()) return false;
  if (exhausted)
    active_reader->Warn("OBJ polygons exceed the triangulation work limit; the rest are fans",
                        "mjcf.obj_polygon_fan");
  mesh->counts.assign(indices.size() / 3, 3);
  mesh->indices = std::move(indices);
  if (corner_normals) mesh->normal_indices = std::move(normal_indices);
  if (corner_uvs) mesh->uv_indices = std::move(uv_indices);
  if (face_materials) mesh->materials = std::move(materials);
  return true;
}

bool LoadOBJ(const fs::path& filename, Mesh* mesh, std::string* err) {
  MappedAsset asset;
  std::string resolved;
  if (!active_reader->Map(filename.string(), active_reader->root_source, &asset, &resolved,
                          err))
    return false;
  const std::string_view text = asset.text();
  OBJ obj;
  const bool dae = ToLower(filename.extension().string()) == ".dae";
  // Non-legacy profiles triangulate OBJ polygons as MuJoCo does, so the
  // collider and the inferred mass properties match the compiled model.
  const bool mujoco_faces = !dae && active_reader->options.profile != "legacy";
  if (!(dae ? ReadDAE(text, filename.string(), *active_reader, &obj, err)
            : ReadOBJ(text, filename.string(), *active_reader, !mujoco_faces, &obj, err)))
    return false;
  if (mujoco_faces) {
    // Untriangulated reads also return line/point primitives.
    const size_t before = obj.meshes.size();
    obj.meshes.erase(std::remove_if(obj.meshes.begin(), obj.meshes.end(),
                                    [](const Mesh& m) { return m.type != "Mesh"; }),
                     obj.meshes.end());
    if (obj.meshes.size() != before)
      active_reader->Warn(
          "OBJ line, point or free-form primitives are not converted to "
          "polygon meshes");
    if (obj.meshes.empty()) {
      if (err) *err = "OBJ contains no polygon faces";
      return false;
    }
    for (auto& part : obj.meshes)
      if (!TriangulateLikeMuJoCo(&part)) {
        if (err) *err = "Invalid OBJ polygon in " + filename.string();
        return false;
      }
  }
  const int32_t material_base = static_cast<int32_t>(active_materials->size());
  for (auto& mat : obj.materials) {
    mat["name"] = "obj_" + std::to_string(active_materials->size()) + "_" +
                  mat["name"].get<std::string>();
    active_materials->push_back(mat);
  }
  // MuJoCo's OBJ decoder converts only the first object of a mesh asset
  // (plugin/obj_decoder, GetShapes()[0]); non-legacy profiles match it so
  // geometry, colliders and inferred inertia agree with the simulator.
  if (active_reader->options.profile != "legacy" && obj.meshes.size() > 1) {
    active_reader->Warn("MJCF mesh " + filename.filename().string() + " has " +
                            std::to_string(obj.meshes.size()) +
                            " OBJ objects; MuJoCo uses only the first",
                        "mjcf.obj_first_object", filename.string());
    obj.meshes.resize(1);
  }
  for (auto& part : obj.meshes) {
    const int32_t base = static_cast<int32_t>(mesh->positions.size() / 3);
    mesh->positions.insert(mesh->positions.end(), part.positions.begin(),
                           part.positions.end());
    mesh->normals.insert(mesh->normals.end(), part.normals.begin(),
                         part.normals.end());
    mesh->uvs.insert(mesh->uvs.end(), part.uvs.begin(), part.uvs.end());
    for (int32_t i : part.indices) mesh->indices.push_back(base + i);
    mesh->counts.insert(mesh->counts.end(), part.counts.begin(),
                        part.counts.end());
    for (int32_t i : part.materials)
      mesh->materials.push_back(i < 0 ? -1 : material_base + i);
  }
  if (mesh->uvs.size() != mesh->positions.size() / 3 * 2) mesh->uvs.clear();
  return true;
}

// Per-import cache of loaded mesh assets: a mesh geom is otherwise parsed
// for its visual, its collider and mass inference, and an OBJ's MTL
// materials would be registered once per parse.
thread_local std::map<std::pair<std::string, bool>, Mesh>* active_mesh_cache = nullptr;

bool LoadMeshFileUncached(const fs::path& filename, bool smoothnormal, Mesh* mesh,
                          std::string* err);

// Mesh assets parsed ahead of use by worker threads with private readers.
// Only a load whose side effects were reads, budget charges, warnings and
// OBJ materials is kept; they are replayed into the import's reader at first
// use (materials renumbered after the import's own), together with its
// warnings, so traces, diagnostics, budgets and outputs match a serial
// import. Anything else (textures, failures) is parsed again serially there.
struct PreloadedMesh {
  Mesh mesh;
  OBJ obj;               // URDF OBJ/DAE parse (all parts, own materials)
  std::string resolved;  // URDF: the mesh's resolved identifier
  Json materials = Json::array();  // OBJ/MTL materials, numbered from 0
  std::vector<Reader::Access> accesses;
  std::vector<Reader::Warning> warnings;
  size_t bytes = 0, peak = 0;
};
thread_local std::map<std::pair<std::string, bool>, PreloadedMesh>* active_mesh_preload = nullptr;

using PreloadKey = std::pair<std::string, bool>;
using PreloadJob = std::function<bool(PreloadedMesh*, std::string*)>;

void PreloadJobs(const std::vector<std::pair<PreloadKey, PreloadJob>>& jobs) {
#if (defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)) || defined(__wasi__)
  (void)jobs;
#else
  const auto& keys = jobs;
  Reader& parent = *active_reader;
  if (!active_mesh_preload || parent.options.open || parent.options.read ||
      !parent.failure.empty() || parent.options.threads == 1 || keys.size() < 2)
    return;
  const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
  const size_t workers = std::min<size_t>(
      keys.size(), parent.options.threads > 1 ? size_t(parent.options.threads)
                                              : std::min(hardware, 8u));
  if (workers < 2) return;
  Options child_options = parent.options;
  child_options.report = nullptr;
  child_options.progress = nullptr;
  child_options.threads = 1;
  const size_t remaining = parent.options.max_memory -
                           std::min(parent.bytes, parent.options.max_memory);
  std::vector<PreloadedMesh> results(keys.size());
  std::vector<char> kept(keys.size(), 0);
  std::atomic<size_t> next{0}, committed{0};
  auto work = [&]() {
    for (size_t i = next++; i < keys.size(); i = next++) {
      if (committed.load() >= remaining) return;
      Reader child(child_options);
      child.packages = parent.packages;
      child.root_source = parent.root_source;
      child.allow_unanchored = parent.allow_unanchored;
      child.record_accesses = true;
      Json materials = Json::array();
      active_reader = &child;
      active_materials = &materials;
      std::string err;
      PreloadedMesh& out = results[i];
      if (!jobs[i].second(&out, &err) ||
          !child.failure.empty() || !child.textures.empty() || !child.texture_requests.empty() || !child.meshes.empty() ||
          child.preserved.size() || committed.fetch_add(child.bytes) + child.bytes > remaining) {
        out = PreloadedMesh();
        continue;
      }
      out.materials = std::move(materials);
      out.accesses = std::move(child.accesses);
      out.warnings = std::move(child.recorded_warnings);
      out.bytes = child.bytes;
      out.peak = child.peak_bytes;
      kept[i] = 1;
    }
  };
  std::vector<std::thread> pool;
  for (size_t t = 1; t < workers; ++t) pool.emplace_back(work);
  {
    // The calling thread works too; restore its import state afterwards.
    Reader* reader = active_reader;
    Json* materials = active_materials;
    work();
    active_reader = reader;
    active_materials = materials;
  }
  for (auto& thread : pool) thread.join();
  for (size_t i = 0; i < keys.size(); ++i)
    if (kept[i]) active_mesh_preload->emplace(keys[i].first, std::move(results[i]));
#endif
}

void PreloadMeshFiles(const std::vector<PreloadKey>& keys) {
  std::vector<std::pair<PreloadKey, PreloadJob>> jobs;
  for (const auto& key : keys)
    jobs.emplace_back(key, [key](PreloadedMesh* out, std::string* err) {
      return LoadMeshFileUncached(fs::path(key.first), key.second, &out->mesh, err);
    });
  PreloadJobs(jobs);
}

// Replay a preloaded load's reads, warnings, charges and materials into the
// import; false leaves it to a serial load (for example when the budget has
// since been used up).
// `keep` leaves the entry for later uses that a serial import would also
// read and parse again (URDF meshes are not cached).
bool TakePreloaded(const PreloadKey& key, PreloadedMesh* out, std::string* err,
                   bool keep = false) {
  if (!active_mesh_preload) return false;
  auto found = active_mesh_preload->find(key);
  if (found == active_mesh_preload->end()) return false;
  PreloadedMesh preloaded;
  if (keep) {
    preloaded = found->second;
  } else {
    preloaded = std::move(found->second);
    active_mesh_preload->erase(found);
  }
  Reader& reader = *active_reader;
  // A serial parse fails once its transient peak exceeds the budget left.
  if (!reader.failure.empty() ||
      std::max(preloaded.bytes, preloaded.peak) >
          reader.options.max_memory - std::min(reader.bytes, reader.options.max_memory))
    return false;
  for (size_t k = 0; k < preloaded.materials.size(); ++k) {
    const Json& name = preloaded.materials[k]["name"];
    if (!name.is_string() || name.get<std::string>().rfind("obj_" + std::to_string(k) + "_", 0) != 0)
      return false;
  }
  const size_t base = reader.bytes;
  size_t read = 0;
  for (const auto& access : preloaded.accesses) {
    if (access.read && access.success && !access.cached && !access.texture) {
      if (!reader.Account(access.name, access.anchor, access.identifier, access.bytes, err))
        return false;
      read += access.bytes;
    } else if (reader.options.report) {
      // A failed read (for example a missing MTL) charges nothing.
      reader.options.report->Dependency(access.name, access.anchor, access.identifier,
                                        access.bytes, access.read, access.cached,
                                        access.success, access.texture);
    }
  }
  for (const auto& warning : preloaded.warnings)
    reader.Warn(warning.message, warning.code.c_str(), warning.source, warning.line);
  if (reader.options.report) reader.options.report->ObserveMemory(base + preloaded.peak);
  reader.peak_bytes = std::max(reader.peak_bytes, base + preloaded.peak);
  if (preloaded.bytes > read && !reader.Charge(preloaded.bytes - read, err)) return false;
  // LoadOBJ names materials obj_<import index>_<name>; renumber from the
  // import's current count as a serial load would have.
  const int32_t offset = static_cast<int32_t>(active_materials->size());
  for (size_t k = 0; k < preloaded.materials.size(); ++k) {
    Json mat = std::move(preloaded.materials[k]);
    const std::string name = mat["name"].get<std::string>();
    const std::string prefix = "obj_" + std::to_string(k) + "_";
    mat["name"] = "obj_" + std::to_string(offset + int32_t(k)) + "_" + name.substr(prefix.size());
    active_materials->push_back(std::move(mat));
  }
  if (!preloaded.materials.empty())
    for (int32_t& index : preloaded.mesh.materials)
      if (index >= 0) index += offset;
  *out = std::move(preloaded);
  return true;
}

bool TakePreloadedMesh(const PreloadKey& key, Mesh* mesh, std::string* err) {
  PreloadedMesh preloaded;
  if (!TakePreloaded(key, &preloaded, err)) return false;
  *mesh = std::move(preloaded.mesh);
  return true;
}

bool LoadMeshFile(const fs::path& filename, bool smoothnormal, Mesh* mesh,
                  std::string* err) {
  if (!active_mesh_cache) return LoadMeshFileUncached(filename, smoothnormal, mesh, err);
  const auto key = std::make_pair(filename.string(), smoothnormal);
  auto found = active_mesh_cache->find(key);
  if (found == active_mesh_cache->end()) {
    Mesh loaded;
    if (!TakePreloadedMesh(key, &loaded, err)) {
      if (!active_reader->failure.empty()) return false;
      if (!LoadMeshFileUncached(filename, smoothnormal, &loaded, err)) return false;
    }
    found = active_mesh_cache->emplace(key, std::move(loaded)).first;
  }
  // The importer charged the first load; a copy is an ordinary allocation.
  *mesh = found->second;
  return true;
}

bool LoadMeshFileUncached(const fs::path& filename, bool smoothnormal, Mesh* mesh,
                          std::string* err) {
  const std::string ext = ToLower(filename.extension().string());
  if (ext == ".stl") return LoadBinarySTL(filename, smoothnormal, mesh, err);
  if (ext == ".obj" || ext == ".dae") return LoadOBJ(filename, mesh, err);
  if (err) *err = "Unsupported native mesh extension: " + ext;
  return false;
}

Json MeshPayloadToJson(const MeshPayload& payload) {
  if (payload.shape.is_object())
    return {{"name", payload.name},
            {"matrix", Array(payload.matrix)},
            {"shape", payload.shape}};
  Json j = MeshRefJSON(payload.mesh, *active_reader);
  j["name"] = payload.name;
  j["matrix"] = Array(payload.matrix);
  return j;
}

std::string Eff(const pugi::xml_node& node, const AttrMap& cls,
                const std::string& name, const std::string& fallback = "") {
  if (node && node.attribute(name.c_str())) {
    return node.attribute(name.c_str()).as_string();
  }
  const auto it = cls.find(name);
  return it != cls.end() ? it->second : fallback;
}

bool HasEff(const pugi::xml_node& node, const AttrMap& cls,
            const std::string& name) {
  return (node && node.attribute(name.c_str())) || cls.count(name) > 0;
}

double EffDouble(const pugi::xml_node& node, const AttrMap& cls,
                 const std::string& name, double fallback) {
  if (!HasEff(node, cls, name)) return fallback;
  const std::string s = Eff(node, cls, name);
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (!end || end == s.c_str() || *end || !std::isfinite(v)) {
    active_reader->failure = "Invalid XML number for " + name;
    return fallback;
  }
  return v;
}

void WalkDefault(const pugi::xml_node& node, const AttrMap& parent_geom,
                 const AttrMap& parent_joint, Defaults* d) {
  AttrMap g = parent_geom;
  AttrMap j = parent_joint;
  if (const auto gn = Child(node, "geom")) {
    for (const auto& a : gn.attributes()) g[a.name()] = a.value();
  }
  if (const auto jn = Child(node, "joint")) {
    for (const auto& a : jn.attributes()) j[a.name()] = a.value();
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
  for (const auto& child : Children(node, "default")) {
    WalkDefault(child, g, j, d);
  }
}

Defaults ParseDefaults(const pugi::xml_node& root) {
  Defaults d;
  for (const auto& def : Children(root, "default")) {
    WalkDefault(def, AttrMap{}, AttrMap{}, &d);
  }
  return d;
}

const AttrMap& ResolveGeomClass(const pugi::xml_node& geom_node,
                                const Defaults& defaults,
                                const std::string& childclass) {
  std::string cls = Attr(geom_node, "class");
  if (cls.empty()) cls = childclass;
  if (!cls.empty()) {
    const auto it = defaults.geom.find(cls);
    if (it != defaults.geom.end()) return it->second;
  }
  return defaults.root_geom;
}

const AttrMap& ResolveJointClass(const pugi::xml_node& joint_node,
                                 const Defaults& defaults,
                                 const std::string& childclass) {
  std::string cls = joint_node ? Attr(joint_node, "class") : std::string();
  if (cls.empty()) cls = childclass;
  if (!cls.empty()) {
    const auto it = defaults.joint.find(cls);
    if (it != defaults.joint.end()) return it->second;
  }
  return defaults.root_joint;
}

void AddGeomPhysicsAttrs(const pugi::xml_node& geom_node, const AttrMap& cls,
                         Json* geom_json) {
  if (!geom_json) return;
  if (HasEff(geom_node, cls, "group")) {
    (*geom_json)["group"] =
        ToInt32(EffDouble(geom_node, cls, "group", 3.0), "group");
  }
  if (HasEff(geom_node, cls, "condim")) {
    (*geom_json)["condim"] =
        ToInt32(EffDouble(geom_node, cls, "condim", 3.0), "condim");
  }
  if (HasEff(geom_node, cls, "margin")) {
    (*geom_json)["mjc"]["margin"] = EffDouble(geom_node, cls, "margin", 0.0);
  }
  if (HasEff(geom_node, cls, "solmix")) {
    (*geom_json)["mjc"]["solmix"] = EffDouble(geom_node, cls, "solmix", 1.0);
  }
  for (const char* key : {"contype", "conaffinity", "priority"})
    if (HasEff(geom_node, cls, key))
      (*geom_json)["mjc"][key] =
          ToInt32(EffDouble(geom_node, cls, key, 0), key);
  if (HasEff(geom_node, cls, "gap"))
    (*geom_json)["mjc"]["gap"] = EffDouble(geom_node, cls, "gap", 0);
  for (const char* key : {"friction", "solref", "solimp", "size"})
    if (HasEff(geom_node, cls, key))
      (*geom_json)["mjc"][std::string(key) == "size" ? "geomSize" : key] =
          Array(ParseDoubles(Eff(geom_node, cls, key)));
  // Geom mass/density drive MuJoCo's inferred body inertia; non-legacy
  // profiles author them as collider PhysicsMassAPI values.
  for (const char* key : {"mass", "density"})
    if (HasEff(geom_node, cls, key))
      (*geom_json)["mjc"][key] = EffDouble(geom_node, cls, key, 0.0);
}

// Basename -> path index of mesh files under `root_dir`. Used as a last-resort
// fallback when a <mesh file="..."> path does not resolve against meshdir --
// e.g. MuJoCo scenes whose assets are referenced relative to an included
// file's directory (the include flattening here drops that provenance).
// Names that occur in more than one location are dropped to avoid ambiguity.
std::map<std::string, fs::path> BuildMeshIndex(const fs::path&) { return {}; }

fs::path ResolveMeshPath(const fs::path& base_dir, const fs::path& mesh_dir,
                         const std::string& file,
                         const std::map<std::string, fs::path>& index) {
  if (fs::path(file).is_absolute()) return fs::path(file);
  if (active_reader->options.read || active_reader->options.open) {
    const auto colon = file.find(':');
    const auto slash = file.find_first_of("/\\");
    if (colon != std::string::npos && (slash == std::string::npos || colon < slash))
      return fs::path(file);
    return mesh_dir / file;
  }
  std::error_code ec;
  const std::array<fs::path, 3> candidates{
      {mesh_dir / file, base_dir / "assets" / file, base_dir / file}};
  for (const auto& c : candidates) {
    if (fs::exists(c, ec)) return c;
  }
  const auto it = index.find(ToLower(fs::path(file).filename().string()));
  if (it != index.end()) return it->second;
  return mesh_dir / file;  // primary candidate; errors later with a clear msg
}

std::map<std::string, MeshAsset> CollectMujocoAssets(const pugi::xml_node& root,
                                                     const fs::path& base_dir) {
  std::map<std::string, MeshAsset> assets;
  const auto compiler = Child(root, "compiler");
  // MuJoCo <compiler meshdir> takes precedence; assetdir is the shared
  // default for meshdir/texturedir when meshdir is absent.
  std::string dir_attr = compiler ? Attr(compiler, "meshdir") : std::string();
  if (dir_attr.empty() && compiler) dir_attr = Attr(compiler, "assetdir");
  const fs::path mesh_dir = dir_attr.empty() ? base_dir : base_dir / dir_attr;
  const std::map<std::string, fs::path> index = BuildMeshIndex(base_dir);

  for (const auto& asset_node : Children(root, "asset")) {
    for (const auto& mesh_node : Children(asset_node, "mesh")) {
      const std::string file = Attr(mesh_node, "file");
      if (file.empty()) continue;
      std::string name = Attr(mesh_node, "name");
      if (name.empty()) name = fs::path(file).stem().string();
      MeshAsset asset;
      asset.path = ResolveMeshPath(base_dir, mesh_dir, file, index);
      asset.scale = ParseDouble3(Attr(mesh_node, "scale"), {{1.0, 1.0, 1.0}});
      if (HasAttr(mesh_node, "refpos")) {
        asset.refpos = ParseDouble3(Attr(mesh_node, "refpos"), {{0, 0, 0}});
        asset.has_refpos = true;
      }
      asset.smoothnormal = Attr(mesh_node, "smoothnormal") == "true";
      if (HasAttr(mesh_node, "maxhullvert"))
        asset.maxhullvert = ToInt32(ParseDoubleAttr(mesh_node, "maxhullvert", -1.0), "maxhullvert");
      if (HasAttr(mesh_node, "inertia")) asset.inertia = ToLower(Attr(mesh_node, "inertia"));
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
void NormalizeHField(std::vector<float>* data) {
  if (data->empty()) return;
  float emin = (*data)[0], emax = (*data)[0];
  for (float v : *data) {
    emin = std::min(emin, v);
    emax = std::max(emax, v);
  }
  const float range = emax - emin;
  for (float& v : *data) {
    v -= emin;
    if (range > 1e-9f) v /= range;
  }
}

// Collect <asset><hfield> declarations. File-based fields decode a (grayscale)
// PNG with rows reversed (MuJoCo stores row 0 at the -y edge); inline fields
// read nrow/ncol/elevation. All are normalized to [0,1].
std::map<std::string, HFieldAsset> CollectMujocoHFields(
    const pugi::xml_node& root, const fs::path& base_dir) {
  std::map<std::string, HFieldAsset> hfields;
  const auto compiler = Child(root, "compiler");
  std::string dir_attr = compiler ? Attr(compiler, "meshdir") : std::string();
  if (dir_attr.empty() && compiler) dir_attr = Attr(compiler, "assetdir");
  const fs::path mesh_dir = dir_attr.empty() ? base_dir : base_dir / dir_attr;
  const std::map<std::string, fs::path> index = BuildMeshIndex(base_dir);

  for (const auto& asset_node : Children(root, "asset")) {
    for (const auto& hf_node : Children(asset_node, "hfield")) {
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
        const fs::path path = ResolveMeshPath(base_dir, mesh_dir, file, index);
        int w = 0, h = 0, comp = 0;
        std::vector<uint8_t> png;
        std::string resolved, why;
        if (!active_reader->Read(path.string(), active_reader->root_source,
                                 &png, &resolved, &why)) {
          active_reader->failure = why;
          return {};
        }
        if (png.size() > size_t(INT32_MAX) ||
            !stbi_info_from_memory(png.data(), static_cast<int>(png.size()), &w,
                                   &h, &comp) ||
            w <= 0 || h <= 0 ||
            size_t(w) > active_reader->options.max_memory / size_t(h) / 64 ||
            !active_reader->Charge(size_t(w) * size_t(h) * 64, &why)) {
          active_reader->failure =
              "Heightfield dimensions exceed memory budget";
          return {};
        }
        unsigned char* img = stbi_load_from_memory(
            png.data(), static_cast<int>(png.size()), &w, &h, &comp, 1);
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
        hf.nrow = ToInt32(ParseDoubleAttr(hf_node, "nrow", 0.0), "nrow");
        hf.ncol = ToInt32(ParseDoubleAttr(hf_node, "ncol", 0.0), "ncol");
        const std::vector<double> elev =
            ParseDoubles(Attr(hf_node, "elevation"));
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
Mesh TessellateHField(const HFieldAsset& hf) {
  Mesh mesh;
  const int nr = hf.nrow, nc = hf.ncol;
  if (nr < 2 || nc < 2) return mesh;
  // Vertex ids are int32 (r * nc + c); reject grids whose ids would overflow.
  if (uint64_t(nr) * uint64_t(nc) > uint64_t(INT32_MAX) ||
      uint64_t(nr) * uint64_t(nc) != hf.data.size()) {
    if (active_reader)
      active_reader->Warn("Heightfield grid exceeds int32 vertex ids or data size",
                          "mjcf.hfield_size");
    return mesh;
  }
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
      if (len > 1e-12) {
        nx /= len;
        ny /= len;
        nz /= len;
      }
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

// Symmetric 3x3 eigen-decomposition (cyclic Jacobi), eigenvalues sorted in
// decreasing order like mjuu_eig3; eigenvector columns form a right-handed
// rotation.
void EigenDescending(const double in[3][3], double eval[3], double evec[3][3]) {
  double a[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      a[i][j] = in[i][j];
      evec[i][j] = i == j ? 1.0 : 0.0;
    }
  // Converge relative to the tensor scale: round-off off-diagonals of an
  // (almost) isotropic tensor must not pick an arbitrary principal frame.
  const double scale = std::abs(a[0][0]) + std::abs(a[1][1]) + std::abs(a[2][2]);
  for (int sweep = 0; sweep < 64; ++sweep) {
    const double off = std::abs(a[0][1]) + std::abs(a[0][2]) + std::abs(a[1][2]);
    if (off <= 1e-14 * scale || off < 1e-300) break;
    for (int p = 0; p < 2; ++p)
      for (int q = p + 1; q < 3; ++q) {
        if (std::abs(a[p][q]) < 1e-300) continue;
        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0), sn = t * c;
        for (int k = 0; k < 3; ++k) {
          const double akp = a[k][p], akq = a[k][q];
          a[k][p] = c * akp - sn * akq;
          a[k][q] = sn * akp + c * akq;
        }
        for (int k = 0; k < 3; ++k) {
          const double apk = a[p][k], aqk = a[q][k];
          a[p][k] = c * apk - sn * aqk;
          a[q][k] = sn * apk + c * aqk;
        }
        for (int k = 0; k < 3; ++k) {
          const double vkp = evec[k][p], vkq = evec[k][q];
          evec[k][p] = c * vkp - sn * vkq;
          evec[k][q] = sn * vkp + c * vkq;
        }
      }
  }
  int order[3] = {0, 1, 2};
  std::sort(order, order + 3, [&](int x, int y) { return a[x][x] > a[y][y]; });
  double v[3][3];
  for (int j = 0; j < 3; ++j) {
    eval[j] = a[order[j]][order[j]];
    for (int i = 0; i < 3; ++i) v[i][j] = evec[i][order[j]];
  }
  // Right-handed: third column = first x second.
  v[0][2] = v[1][0] * v[2][1] - v[2][0] * v[1][1];
  v[1][2] = v[2][0] * v[0][1] - v[0][0] * v[2][1];
  v[2][2] = v[0][0] * v[1][1] - v[1][0] * v[0][1];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) evec[i][j] = v[i][j];
}

bool MeshMassProperties(const std::vector<std::array<double, 3>>& v,
                        const std::vector<int32_t>& tri, const std::string& mode,
                        double* volume, std::array<double, 3>* com, double inert[3][3]);

// Triangle list of a loaded mesh (polygons triangulated as ClipPolygon).
bool MeshTriangles(const Mesh& mesh, std::vector<int32_t>* tri) {
  tri->clear();
  if (mesh.counts.empty()) {
    if (mesh.indices.size() % 3) return false;
    *tri = mesh.indices;
    return true;
  }
  size_t offset = 0;
  for (int32_t count : mesh.counts) {
    if (count < 3 || offset + size_t(count) > mesh.indices.size()) return false;
    std::vector<int32_t> corners;
    if (count == 3) {
      corners = {0, 1, 2};
    } else if (!ClipPolygon(mesh.positions, mesh.indices.data() + offset, size_t(count), &corners)) {
      return false;
    }
    for (int32_t c : corners) tri->push_back(mesh.indices[offset + size_t(c)]);
    offset += size_t(count);
  }
  return offset == mesh.indices.size();
}

// A primitive geom with a mesh= attribute is fitted to the mesh by MuJoCo's
// compiler (mjCMesh::FitGeom): sized from the mesh's equivalent inertia box
// (or its AABB with <compiler fitaabb>) and placed in the mesh's COM /
// principal-axes frame. Returns the geom-local frame and fitted sizes.
struct MeshFit {
  std::array<double, 3> size{{0, 0, 0}};
  std::vector<double> frame;  // relative to the geom's own pos/orientation
};
thread_local std::map<std::string, MeshFit>* active_mesh_fits = nullptr;
thread_local bool active_fitaabb = false;

bool FitMeshGeom(const pugi::xml_node& geom_node, const AttrMap& cls,
                 const std::map<std::string, MeshAsset>& assets,
                 const std::string& type, MeshFit* fit, std::string* err) {
  const std::string mesh_name = Eff(geom_node, cls, "mesh");
  const auto it = assets.find(mesh_name);
  if (it == assets.end()) {
    if (err) *err = "Missing MJCF mesh asset: " + mesh_name;
    return false;
  }
  const std::string key = mesh_name + "|" + type;
  if (active_mesh_fits) {
    auto cached = active_mesh_fits->find(key);
    if (cached != active_mesh_fits->end()) {
      *fit = cached->second;
      goto scale;
    }
  }
  {
    Mesh mesh;
    if (!LoadMeshFile(it->second.path, it->second.smoothnormal, &mesh, err)) return false;
    const std::vector<double> m =
        MultiplyMatrix(MeshRefMatrix(it->second), ScaleMatrix(it->second.scale));
    std::vector<std::array<double, 3>> v(mesh.positions.size() / 3);
    for (size_t i = 0; i < v.size(); ++i) {
      const double x = mesh.positions[3 * i], y = mesh.positions[3 * i + 1],
                   z = mesh.positions[3 * i + 2];
      v[i] = {{m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
               m[2] * x + m[6] * y + m[10] * z + m[14]}};
    }
    std::vector<int32_t> tri;
    double volume = 0, unit[3][3];
    std::array<double, 3> com{{0, 0, 0}};
    if (!MeshTriangles(mesh, &tri) ||
        !MeshMassProperties(v, tri, it->second.inertia, &volume, &com, unit)) {
      if (err) *err = "Cannot fit MJCF geom to degenerate mesh: " + mesh_name;
      return false;
    }
    double eval[3], evec[3][3];
    EigenDescending(unit, eval, evec);
    std::array<double, 3> box;
    for (int i = 0; i < 3; ++i) {
      const double e = eval[(i + 1) % 3] + eval[(i + 2) % 3] - eval[i];
      box[i] = 0.5 * std::sqrt(std::max(0.0, 6.0 * e / volume));
    }
    std::array<double, 3> center{{0, 0, 0}};
    if (active_fitaabb) {
      // AABB of the mesh in its principal frame (mjCMesh aamm_).
      std::array<double, 3> lo{{1e300, 1e300, 1e300}}, hi{{-1e300, -1e300, -1e300}};
      for (const auto& p : v)
        for (int j = 0; j < 3; ++j) {
          double d = 0;
          for (int i = 0; i < 3; ++i) d += evec[i][j] * (p[i] - com[i]);
          lo[j] = std::min(lo[j], d);
          hi[j] = std::max(hi[j], d);
        }
      for (int j = 0; j < 3; ++j) {
        center[j] = 0.5 * (lo[j] + hi[j]);
        box[j] = hi[j] - center[j];
      }
    }
    std::array<double, 3>& size = fit->size;
    if (type == "sphere") {
      size[0] = active_fitaabb ? std::max({box[0], box[1], box[2]})
                               : (box[0] + box[1] + box[2]) / 3;
    } else if (type == "capsule" || type == "cylinder") {
      size[0] = active_fitaabb ? std::max(box[0], box[1]) : (box[0] + box[1]) / 2;
      size[1] = active_fitaabb ? box[2] - (type == "capsule" ? size[0] : 0.0)
                               : (type == "capsule" ? std::max(0.0, box[2] - size[0] / 2) : box[2]);
    } else if (type == "box" || type == "ellipsoid") {
      size = box;
    } else {
      if (err) *err = "MJCF mesh fitting does not support geom type " + type;
      return false;
    }
    fit->frame = IdentityMatrix();
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) fit->frame[j * 4 + i] = evec[i][j];
      double c = com[i];
      for (int j = 0; j < 3; ++j) c += evec[i][j] * center[j];
      fit->frame[12 + i] = c;
    }
    if (active_mesh_fits) active_mesh_fits->emplace(key, *fit);
  }
scale:
  const double fitscale = EffDouble(geom_node, cls, "fitscale", 1.0);
  for (double& x : fit->size) x *= fitscale;
  return true;
}

// MJCF fromto for box/ellipsoid: the segment sets the frame (midpoint, Z along
// the segment) and the half-size along Z (size[axis]).
void ApplyFromtoFrame(const pugi::xml_node& geom_node, const AttrMap& cls,
                      const std::vector<double>& body_world, size_t axis,
                      std::array<double, 3>* size, std::vector<double>* matrix) {
  const auto fromto = ParseDoubles(Eff(geom_node, cls, "fromto"));
  if (fromto.size() < 6) return;
  const std::array<double, 3> dir{{fromto[3] - fromto[0], fromto[4] - fromto[1],
                                   fromto[5] - fromto[2]}};
  (*size)[axis] = 0.5 * std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
  *matrix = MultiplyMatrix(
      body_world, MatrixFromPosQuat({{0.5 * (fromto[0] + fromto[3]), 0.5 * (fromto[1] + fromto[4]),
                                      0.5 * (fromto[2] + fromto[5])}},
                                    QuatFromTwoVecs({{0.0, 0.0, 1.0}}, dir)));
}

bool BuildGeomPayload(const pugi::xml_node& geom_node, const AttrMap& cls,
                      const std::string& cls_name,
                      const std::map<std::string, MeshAsset>& assets,
                      const std::map<std::string, HFieldAsset>& hfields,
                      const Options& opts, const Context& ctx,
                      const std::vector<double>& body_world,
                      MeshPayload* payload, bool* is_visual, std::string* err,
                      bool force_collision = false, bool native_shape = false) {
  const std::string type = HasEff(geom_node, cls, "type")
                               ? Eff(geom_node, cls, "type")
                           : HasEff(geom_node, cls, "mesh") ? "mesh"
                                                            : "sphere";
  payload->name = Attr(geom_node, "name");
  if (payload->name.empty())
    payload->name = Eff(geom_node, cls, "mesh", "geom");
  // Bake the accumulated body-chain world transform into the geom matrix: the
  // body_world is the requested geometry anchor (identity for body-local
  // meshes, a world frame for static world geoms). The geom pose may
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
  // Mesh-fit primitive (mesh= on a non-mesh type): fitted size and frame.
  std::vector<double> fitted_size;
  if (type != "mesh" && type != "hfield" && type != "plane" && HasEff(geom_node, cls, "mesh")) {
    MeshFit fit;
    if (!FitMeshGeom(geom_node, cls, assets, type, &fit, err)) return false;
    fitted_size.assign(fit.size.begin(), fit.size.end());
    payload->matrix = MultiplyMatrix(payload->matrix, fit.frame);
  }
  auto geom_sizes = [&]() {
    return fitted_size.empty() ? ParseDoubles(Eff(geom_node, cls, "size")) : fitted_size;
  };
  auto geom_size3 = [&](std::array<double, 3> fallback) {
    const auto v = geom_sizes();
    for (size_t i = 0; i < 3 && i < v.size(); ++i) fallback[i] = v[i];
    return fallback;
  };

  // Classification mirrors the JS converter: a resolved geom group (own or
  // inherited from <default>) wins -- groups 0-2 are visible, 3-5 are
  // collision. With no group anywhere, fall back to the class name and then
  // MuJoCo's contype/conaffinity==0 (visual-only) convention.
  if (HasEff(geom_node, cls, "group")) {
    (*is_visual) =
        ToInt32(EffDouble(geom_node, cls, "group", 3.0), "group") < 3;
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

  if (force_collision) *is_visual = false;
  // Native gprim (collider, or a primitive visual in non-legacy profiles)
  // instead of a tessellated mesh.
  const bool native = native_shape || (!(*is_visual) && !opts.tessellate_collision_shapes);

  if (type == "mesh") {
    const std::string mesh_name = Eff(geom_node, cls, "mesh");
    const auto it = assets.find(mesh_name);
    if (it == assets.end()) {
      if (err) *err = "Missing MJCF mesh asset: " + mesh_name;
      return false;
    }
    if (!LoadMeshFile(it->second.path, it->second.smoothnormal, &payload->mesh,
                      err))
      return false;

    const std::array<double, 3> scale =
        ParseDouble3(Eff(geom_node, cls, "scale"), it->second.scale);
    // G * refframe(refpos/refquat) * scale, matching the JS mesh handling.
    payload->matrix =
        MultiplyMatrix(payload->matrix, MeshRefMatrix(it->second));
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
    auto half = geom_size3({{0.05, 0.05, 0.05}});
    ApplyFromtoFrame(geom_node, cls, body_world, 2, &half, &payload->matrix);
    if (native) {
      payload->shape = {{"type", "box"}};
      payload->matrix = MultiplyMatrix(payload->matrix, ScaleMatrix(half));
    } else {
      payload->mesh = MakeBoxMesh(half);
    }
  } else if (type == "sphere") {
    const auto size = geom_sizes();
    const double radius = size.empty() ? 0.05 : size[0];
    if (native) {
      payload->shape = {{"type", "sphere"}, {"radius", radius}};
    } else {
      payload->mesh = MakeSphereMesh(radius);
    }
  } else if (type == "ellipsoid") {
    auto radii = geom_size3({{0.05, 0.05, 0.05}});
    ApplyFromtoFrame(geom_node, cls, body_world, 2, &radii, &payload->matrix);
    const bool uniform = radii[0] == radii[1] && radii[1] == radii[2];
    if (!uniform && opts.profile != "legacy") {
      // UsdGeom has no ellipsoid and consumers ignore a nonuniformly scaled
      // sphere's scale (Newton keeps the unit radius; OpenUSD rejects such
      // colliders): bake the radii into a mesh, a convex hull as a collider.
      payload->mesh = MakeSphereMesh(1.0, 24, 12);
      for (size_t i = 0; i < payload->mesh.positions.size(); ++i)
        payload->mesh.positions[i] = static_cast<float>(payload->mesh.positions[i] * radii[i % 3]);
    } else if (native) {
      if (!uniform && !(*is_visual) &&
          !UnsupportedXML(geom_node,
              "Ellipsoid collider is represented by a nonuniformly scaled sphere; enable tessellate_collision_shapes for a mesh collider",
              "mjcf.ellipsoid_collision_approximated", "mjcf.ellipsoid_collision",
              active_reader->root_source, true)) {
        if (err) *err = active_reader->failure;
        return false;
      }
      payload->shape = {{"type", "sphere"}, {"radius", 1.0}};
      payload->matrix = MultiplyMatrix(payload->matrix, ScaleMatrix(radii));
    } else {
      payload->mesh = MakeSphereMesh(1.0);
      payload->matrix = MultiplyMatrix(payload->matrix, ScaleMatrix(radii));
    }
  } else if (type == "cylinder" || type == "capsule") {
    const auto size = geom_sizes();
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
    if (native) {
      payload->shape = {{"type", type},
                        {"radius", radius},
                        {"height", half_length * 2.0},
                        {"axis", "Z"}};
    } else {
      payload->mesh = type == "capsule" ? MakeCapsuleMesh(radius, half_length)
                                        : MakeCylinderMesh(radius, half_length);
    }
  } else if (type == "plane") {
    const auto size = ParseDoubles(Eff(geom_node, cls, "size"));
    const double sx = size.size() >= 1 && size[0] > 0.0 ? size[0] : 1.0;
    const double sy = size.size() >= 2 && size[1] > 0.0 ? size[1] : 1.0;
    const double sz = size.size() >= 3 && size[2] > 0.0 ? size[2] : 0.001;
    if (native) {
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

std::string AxisToken(const std::array<double, 3>& axis) {
  const double ax = std::abs(axis[0]);
  const double ay = std::abs(axis[1]);
  const double az = std::abs(axis[2]);
  if (ay >= ax && ay >= az) return "Y";
  if (az >= ax && az >= ay) return "Z";
  return "X";
}

Json InertialToJson(const pugi::xml_node& body_node, const Context& ictx) {
  Json inertial = Json::object();
  const auto inertial_node = Child(body_node, "inertial");
  if (!inertial_node) return inertial;
  inertial["mass"] = ParseDoubleAttr(inertial_node, "mass", 0.0);
  const auto com = ParseDouble3(Attr(inertial_node, "pos"), {{0.0, 0.0, 0.0}});
  inertial["centerOfMass"] = Json::array({com[0], com[1], com[2]});
  // MJCF inertia: <inertial fullinertia="Ixx Iyy Izz Ixy Ixz Iyz"> (full
  // symmetric tensor) or the more common <inertial diaginertia="Ixx Iyy Izz">.
  const std::vector<double> full =
      ParseDoubles(Attr(inertial_node, "fullinertia"));
  if (full.size() >= 6) {
    // Carry all 6 components so the converter can diagonalize them into
    // diagonalInertia (eigenvalues) + principalAxes (eigenvector quaternion).
    // Keep a diagonal fallback for consumers that ignore fullInertia.
    inertial["fullInertia"] =
        Json::array({full[0], full[1], full[2], full[3], full[4], full[5]});
    inertial["diagonalInertia"] = Json::array({full[0], full[1], full[2]});
  } else if (full.size() >= 3) {
    inertial["diagonalInertia"] = Json::array({full[0], full[1], full[2]});
  } else {
    const std::vector<double> diag =
        ParseDoubles(Attr(inertial_node, "diaginertia"));
    if (diag.size() >= 3) {
      inertial["diagonalInertia"] = Json::array({diag[0], diag[1], diag[2]});
    }
  }
  const auto iq = OrientationQuat(
      Attr(inertial_node, "quat"), Attr(inertial_node, "axisangle"),
      Attr(inertial_node, "euler"), Attr(inertial_node, "xyaxes"),
      Attr(inertial_node, "zaxis"), ictx);
  inertial["principalAxes"] = Json::array({iq[0], iq[1], iq[2], iq[3]});
  // An explicit zero tensor with a positive mass (MuJoCo accepts it) stays
  // zero instead of being re-derived from colliders.
  bool zero = HasAttr(inertial_node, "fullinertia") || HasAttr(inertial_node, "diaginertia");
  for (double v : full.size() >= 3 ? full : ParseDoubles(Attr(inertial_node, "diaginertia")))
    zero = zero && v == 0.0;
  if (zero && inertial["mass"].get<double>() > 0.0) inertial["zeroInertia"] = true;
  return inertial;
}

// Build one joint JSON entry from an explicit <joint> node (or a null node for
// a fixed connection), wiring parent_name -> child_name with the given origin
// frame. `origin_matrix` (16 elems) and `origin_pos` are the joint frame; in a
// multi-DOF chain only the first joint carries the body offset (the rest are at
// identity). A null joint_node yields a PhysicsFixedJoint.
void DecomposePosQuat(const std::vector<double>&, std::array<double, 3>*,
                      Quat*);
bool MjcBoolAttr(const std::string& v);  // defined below

bool IsSupportedFlexcompType(const std::string& type) {
  for (const char* t : {"grid", "circle", "square", "disc", "direct", "box", "cylinder",
                        "ellipsoid", "mesh", "gmsh"})
    if (type == t) return true;
  return false;
}

// Directory flexcomp `file` attributes resolve against (MuJoCo meshdir, or
// assetdir, relative to the model file).
fs::path FlexMeshDir(const pugi::xml_node& root, const fs::path& base_dir) {
  const auto compiler = Child(root, "compiler");
  std::string dir = compiler ? Attr(compiler, "meshdir") : std::string();
  if (dir.empty() && compiler) dir = Attr(compiler, "assetdir");
  return dir.empty() ? base_dir : base_dir / dir;
}

// mjCFlexcomp::MakeBox with MuJoCo's default open=true: points on the box
// surface projected to a box, cylinder or ellipsoid (dim 3 adds the center
// as point 0 and builds tets to it; dim 2 has no top face).
void MakeFlexBox(const std::string& type, int dim, const std::array<int, 3>& count,
                 const std::array<double, 3>& spacing, std::vector<double>* point,
                 std::vector<int>* element) {
  const bool open = true;
  auto project = [&](int ix, int iy, int iz) {
    double p[3] = {2.0 * ix / (count[0] - 1) - 1, 2.0 * iy / (count[1] - 1) - 1,
                   2.0 * iz / (count[2] - 1) - 1};
    const double size[3] = {0.5 * spacing[0] * (count[0] - 1), 0.5 * spacing[1] * (count[1] - 1),
                            0.5 * spacing[2] * (count[2] - 1)};
    auto normalize = [&](int n) {
      double l = 0;
      for (int k = 0; k < n; ++k) l += p[k] * p[k];
      if (l < 1e-6) return;  // mjuu_normvec: tiny vectors are left as is
      l = std::sqrt(l);
      if (std::abs(l - 1) > 1e-6)
        for (int k = 0; k < n; ++k) p[k] /= l;
    };
    if (type == "cylinder") {
      const double l0 = std::max(std::abs(p[0]), std::abs(p[1]));
      normalize(2);
      p[0] *= size[0] * l0;
      p[1] *= size[1] * l0;
      p[2] *= size[2];
    } else {
      if (type == "ellipsoid") normalize(3);
      for (int k = 0; k < 3; ++k) p[k] *= size[k];
    }
    point->insert(point->end(), {p[0], p[1], p[2]});
  };
  auto lin = [&](int ix, int iy, int iz) { return ix * count[1] * count[2] + iy * count[2] + iz; };
  auto box_id = [&](int ix, int iy, int iz) {
    if (iz == 0) return ix * count[1] + iy + 1;
    if (iz == count[2] - 1) return count[0] * count[1] + ix * count[1] + iy + 1;
    if (iy == 0) return 2 * count[0] * count[1] + ix * (count[2] - 2) + iz - 1 + 1;
    if (iy == count[1] - 1)
      return 2 * count[0] * count[1] + count[0] * (count[2] - 2) + ix * (count[2] - 2) + iz - 1 + 1;
    if (ix == 0)
      return 2 * count[0] * count[1] + 2 * count[0] * (count[2] - 2) + (iy - 1) * (count[2] - 2) +
             iz - 1 + 1;
    return 2 * count[0] * count[1] + 2 * count[0] * (count[2] - 2) +
           (count[1] - 2) * (count[2] - 2) + (iy - 1) * (count[2] - 2) + iz - 1 + 1;
  };
  if (dim == 3) point->insert(point->end(), {0, 0, 0});
  std::vector<int> idx(static_cast<size_t>(count[0] * count[1] * count[2]), 0);
  int n = 0;
  const bool open2 = open && dim == 2;
  for (int iz = 0; iz < count[2]; iz += count[2] - 1)
    for (int ix = 0; ix < count[0]; ++ix)
      for (int iy = 0; iy < count[1]; ++iy) {
        if (open2 && iz != 0) continue;
        project(ix, iy, iz);
        idx[size_t(lin(ix, iy, iz))] = n++;
      }
  for (int iy = 0; iy < count[1]; iy += count[1] - 1)
    for (int ix = 0; ix < count[0]; ++ix)
      for (int iz = 0; iz < count[2]; ++iz)
        if (iz > 0 && (open2 || iz < count[2] - 1)) {
          project(ix, iy, iz);
          idx[size_t(lin(ix, iy, iz))] = n++;
        }
  for (int ix = 0; ix < count[0]; ix += count[0] - 1)
    for (int iy = 0; iy < count[1]; ++iy)
      for (int iz = 0; iz < count[2]; ++iz)
        if (iz > 0 && (open2 || iz < count[2] - 1) && iy > 0 && iy < count[1] - 1) {
          project(ix, iy, iz);
          idx[size_t(lin(ix, iy, iz))] = n++;
        }
  auto id = [&](int ix, int iy, int iz) { return idx[size_t(lin(ix, iy, iz))]; };
  auto push = [&](std::initializer_list<int> v) { element->insert(element->end(), v); };
  for (int iz = 0; iz < count[2]; iz += count[2] - 1)
    for (int ix = 0; ix < count[0]; ++ix)
      for (int iy = 0; iy < count[1]; ++iy) {
        if (open2 && iz != 0) continue;
        if (ix >= count[0] - 1 || iy >= count[1] - 1) continue;
        if (dim == 3) {
          push({0, box_id(ix, iy, iz), box_id(ix + 1, iy, iz), box_id(ix + 1, iy + 1, iz)});
          push({0, box_id(ix, iy, iz), box_id(ix, iy + 1, iz), box_id(ix + 1, iy + 1, iz)});
        } else {
          const int s1 = iz == 0 ? 1 : 0, s2 = iz == 0 ? 0 : 1;
          push({id(ix, iy, iz), id(ix + 1, iy + s1, iz), id(ix + 1, iy + s2, iz)});
          push({id(ix, iy, iz), id(ix + s2, iy + 1, iz), id(ix + s1, iy + 1, iz)});
        }
      }
  for (int iy = 0; iy < count[1]; iy += count[1] - 1)
    for (int ix = 0; ix < count[0]; ++ix)
      for (int iz = 0; iz < count[2]; ++iz) {
        if (ix >= count[0] - 1 || iz >= count[2] - 1) continue;
        if (dim == 3) {
          push({0, box_id(ix, iy, iz), box_id(ix + 1, iy, iz), box_id(ix + 1, iy, iz + 1)});
          push({0, box_id(ix, iy, iz), box_id(ix, iy, iz + 1), box_id(ix + 1, iy, iz + 1)});
        } else {
          const int ix0 = iy == 0 ? ix : ix + 1, dx = iy == 0 ? 1 : -1;
          push({id(ix0, iy, iz), id(ix0 + dx, iy, iz), id(ix0 + dx, iy, iz + 1)});
          push({id(ix0, iy, iz), id(ix0 + dx, iy, iz + 1), id(ix0, iy, iz + 1)});
        }
      }
  for (int ix = 0; ix < count[0]; ix += count[0] - 1)
    for (int iy = 0; iy < count[1]; ++iy)
      for (int iz = 0; iz < count[2]; ++iz) {
        if (iy >= count[1] - 1 || iz >= count[2] - 1) continue;
        if (dim == 3) {
          push({0, box_id(ix, iy, iz), box_id(ix, iy + 1, iz), box_id(ix, iy + 1, iz + 1)});
          push({0, box_id(ix, iy, iz), box_id(ix, iy, iz + 1), box_id(ix, iy + 1, iz + 1)});
        } else {
          const int iy0 = ix != 0 ? iy : iy + 1, dy = ix != 0 ? 1 : -1;
          push({id(ix, iy0, iz), id(ix, iy0 + dy, iz), id(ix, iy0 + dy, iz + 1)});
          push({id(ix, iy0, iz), id(ix, iy0 + dy, iz + 1), id(ix, iy0, iz + 1)});
        }
      }
}

// mjCFlexcomp::MakeMesh: the surface mesh's vertices (exact repeats merged
// in first-occurrence order, OBJ vertices in file order) and faces; dim 1
// takes edge pairs from degenerate triangles and dim 3 builds tets from the
// origin to every face that sees it with positive volume.
bool LoadFlexMesh(const fs::path& path, const std::string& text, int dim,
                  const std::array<double, 3>& origin, bool has_origin,
                  std::vector<double>* point, std::vector<int>* element, std::string* err) {
  Mesh mesh;
  const std::string ext = ToLower(path.extension().string());
  if (ext == ".obj") {
    OBJ obj;
    if (!ReadOBJ(text, path.string(), *active_reader, true, &obj, err)) return false;
    if (!obj.meshes.empty()) mesh = std::move(obj.meshes[0]);
  } else if (ext == ".stl") {
    if (!LoadBinarySTL(path, false, &mesh, err)) return false;
  } else {
    if (err) *err = "mesh flexcomp supports OBJ and STL files";
    return false;
  }
  std::vector<int32_t> tri;
  if (!MeshTriangles(mesh, &tri) || tri.empty()) {
    if (err) *err = "mesh has no faces";
    return false;
  }
  const size_t corners = mesh.positions.size() / 3;
  std::vector<int> remap(corners, -1);
  if (mesh.source_vertices.size() == corners) {
    // OBJ: rebuild the file's vertex array, then merge exact repeats.
    int32_t max_source = -1;
    for (int32_t v : mesh.source_vertices) max_source = std::max(max_source, v);
    std::vector<float> raw(size_t(max_source + 1) * 3, 0.0f);
    std::vector<bool> seen(size_t(max_source + 1), false);
    for (size_t i = 0; i < corners; ++i) {
      const size_t v = size_t(mesh.source_vertices[i]);
      std::copy(&mesh.positions[3 * i], &mesh.positions[3 * i] + 3, &raw[3 * v]);
      seen[v] = true;
    }
    std::map<std::array<float, 3>, int> keys;
    std::vector<int> vertex_id(seen.size(), -1);
    for (size_t v = 0; v < seen.size(); ++v) {
      if (!seen[v]) continue;  // unreferenced: dropped later anyway
      const std::array<float, 3> key{{raw[3 * v], raw[3 * v + 1], raw[3 * v + 2]}};
      auto it = keys.emplace(key, static_cast<int>(point->size() / 3));
      if (it.second) point->insert(point->end(), key.begin(), key.end());
      vertex_id[v] = it.first->second;
    }
    for (size_t i = 0; i < corners; ++i) remap[i] = vertex_id[size_t(mesh.source_vertices[i])];
  } else {
    std::map<std::array<float, 3>, int> keys;
    for (size_t i = 0; i < corners; ++i) {
      const std::array<float, 3> key{{mesh.positions[3 * i], mesh.positions[3 * i + 1],
                                      mesh.positions[3 * i + 2]}};
      auto it = keys.emplace(key, static_cast<int>(point->size() / 3));
      if (it.second) point->insert(point->end(), key.begin(), key.end());
      remap[i] = it.first->second;
    }
  }
  if (dim == 2) {
    for (int32_t c : tri) element->push_back(remap[size_t(c)]);
  } else if (dim == 1) {
    for (size_t i = 0; i + 2 < tri.size(); i += 3)
      element->insert(element->end(), {remap[size_t(tri[i])], remap[size_t(tri[i + 1])]});
  } else {
    if (!has_origin) {
      if (err) *err = "origin must be specified for mesh flexcomps if dim=3";
      return false;
    }
    point->insert(point->begin(), origin.begin(), origin.end());
    for (size_t i = 0; i + 2 < tri.size(); i += 3) {
      const int t[3] = {remap[size_t(tri[i])] + 1, remap[size_t(tri[i + 1])] + 1,
                        remap[size_t(tri[i + 2])] + 1};
      double e[3][3];
      for (int a = 0; a < 3; ++a)
        for (int k = 0; k < 3; ++k) e[a][k] = (*point)[3 * size_t(t[a]) + size_t(k)] - origin[size_t(k)];
      const double nx = e[0][1] * e[1][2] - e[0][2] * e[1][1];
      const double ny = e[0][2] * e[1][0] - e[0][0] * e[1][2];
      const double nz = e[0][0] * e[1][1] - e[0][1] * e[1][0];
      if (nx * e[2][0] + ny * e[2][1] + nz * e[2][2] < 1e-15) continue;  // mjMINVAL
      element->insert(element->end(), {0, t[0], t[1], t[2]});
    }
  }
  return true;
}

// ASCII GMSH 4.1 (single node and element block) and 2.2, as
// mjCFlexcomp::LoadGMSH reads them; binary files are rejected.
bool ParseGMSH(const std::string& text, int* dim, std::vector<double>* point,
               std::vector<int>* element, std::string* err) {
  if (text.compare(0, 11, "$MeshFormat") != 0) {
    *err = "GMSH file must begin with $MeshFormat";
    return false;
  }
  std::istringstream header(text.substr(11, 64));
  double version = 0;
  int binary = 0;
  if (!(header >> version >> binary)) {
    *err = "could not read the GMSH header";
    return false;
  }
  const long v = std::lround(100 * version);
  if (v != 220 && v != 410) {
    *err = "only GMSH file format versions 4.1 and 2.2 are supported";
    return false;
  }
  if (binary) {
    *err = "binary GMSH files are not supported";
    return false;
  }
  const size_t nb = text.find("$Nodes"), ne = text.find("$EndNodes");
  const size_t eb = text.find("$Elements"), ee = text.find("$EndElements");
  if (nb == std::string::npos || ne == std::string::npos || eb == std::string::npos ||
      ee == std::string::npos || ne < nb || ee < eb) {
    *err = "GMSH file is missing its $Nodes or $Elements section";
    return false;
  }
  std::istringstream nodes(text.substr(nb + 6, ne - nb - 6));
  std::istringstream elems(text.substr(eb + 9, ee - eb - 9));
  const size_t kMax = 50000000;
  if (v == 410) {
    size_t blocks = 0, num = 0, min_tag = 0, max_tag = 0, in_block = 0;
    int edim = 0, etag = 0, param = 0;
    if (!(nodes >> blocks >> num >> min_tag >> max_tag >> edim >> etag >> param >> in_block) ||
        blocks != 1 || num != in_block || max_tag != in_block || num > kMax || edim < 1 || edim > 3) {
      *err = "GMSH nodes must be one sequential block of 1D, 2D or 3D entities";
      return false;
    }
    *dim = edim;
    for (size_t i = 0; i < num; ++i) {
      size_t tag = 0;
      if (!(nodes >> tag) || tag != i + min_tag) {
        *err = "GMSH node tags must be sequential";
        return false;
      }
    }
    for (size_t i = 0; i < 3 * num; ++i) {
      double x = 0;
      if (!(nodes >> x)) {
        *err = "error reading GMSH nodes";
        return false;
      }
      point->push_back(x);
    }
    size_t eblocks = 0, enumb = 0, emin = 0, emax = 0, ein = 0;
    int type = 0;
    if (!(elems >> eblocks >> enumb >> emin >> emax >> edim >> etag >> type >> ein) ||
        eblocks != 1 || enumb != ein || edim != *dim || enumb > kMax ||
        type != (edim == 1 ? 1 : edim == 2 ? 2 : 4)) {
      *err = "GMSH elements must be one block matching the node dimension";
      return false;
    }
    for (size_t i = 0; i < enumb; ++i) {
      size_t tag = 0, node = 0;
      elems >> tag;
      for (int k = 0; k <= edim; ++k) {
        if (!(elems >> node) || node < min_tag || node - min_tag >= num) {
          *err = "error reading GMSH elements";
          return false;
        }
        element->push_back(static_cast<int>(node - min_tag));
      }
    }
    return true;
  }
  size_t num = 0;
  if (!(nodes >> num) || num > kMax) {
    *err = "error reading the GMSH node count";
    return false;
  }
  for (size_t i = 0; i < num; ++i) {
    size_t tag = 0;
    double x[3];
    if (!(nodes >> tag >> x[0] >> x[1] >> x[2])) {
      *err = "error reading GMSH nodes";
      return false;
    }
    point->insert(point->end(), {x[0], x[1], x[2]});
  }
  size_t count = 0;
  if (!(elems >> count) || count > kMax) {
    *err = "error reading the GMSH element count";
    return false;
  }
  int nodes_per = 0;
  for (size_t i = 0; i < count; ++i) {
    int tag = 0, type = 0, ntags = 0;
    if (!(elems >> tag >> type >> ntags) || ntags < 0 || ntags > 64) {
      *err = "error reading GMSH elements";
      return false;
    }
    const int per = type == 2 ? 3 : type == 4 ? 4 : 0;
    if (!per || (nodes_per && per != nodes_per)) {
      *err = "GMSH 2.2 elements must all be triangles or tetrahedra";
      return false;
    }
    nodes_per = per;
    for (int k = 0; k < ntags; ++k) elems >> tag;
    for (int k = 0; k < per; ++k) {
      long node = 0;
      if (!(elems >> node) || node < 1 || size_t(node) > num) {
        *err = "invalid GMSH node tag";
        return false;
      }
      element->push_back(static_cast<int>(node - 1));
    }
  }
  *dim = nodes_per == 3 ? 2 : 3;
  return true;
}

// Shared <flex>/<flexcomp> appearance, elasticity, edge and contact values.
void ReadFlexProperties(const pugi::xml_node& f, Json* out) {
  Json& entry = *out;
  const auto rgba = ParseDoubles(Attr(f, "rgba"));
  if (rgba.size() >= 4) entry["rgba"] = Array(rgba);
  if (const auto el = Child(f, "elasticity")) {
    for (const char* key : {"young", "poisson", "damping", "thickness"})
      if (HasAttr(el, key)) entry[key] = ParseDoubleAttr(el, key, 0.0);
    if (HasAttr(el, "elastic2d")) entry["elastic2d"] = ToLower(Attr(el, "elastic2d"));
  }
  if (const auto edge = Child(f, "edge")) {
    if (HasAttr(edge, "stiffness")) entry["edgeStiffness"] = ParseDoubleAttr(edge, "stiffness", 0.0);
    if (HasAttr(edge, "damping")) entry["edgeDamping"] = ParseDoubleAttr(edge, "damping", 0.0);
    if (HasAttr(edge, "equality")) entry["edgeEquality"] = ToLower(Attr(edge, "equality"));
  }
  int contype = 1, conaffinity = 1;
  if (const auto contact = Child(f, "contact")) {
    contype = ToInt32(ParseDoubleAttr(contact, "contype", 1.0), "contype");
    conaffinity = ToInt32(ParseDoubleAttr(contact, "conaffinity", 1.0), "conaffinity");
    const auto friction = ParseDoubles(Attr(contact, "friction"));
    if (!friction.empty()) entry["friction"] = friction[0];
    if (HasAttr(contact, "selfcollide")) entry["selfcollide"] = ToLower(Attr(contact, "selfcollide"));
  }
  entry["collides"] = contype != 0 || conaffinity != 0;
}

// MJCF <flexcomp> (grid, circle, square, disc, direct) expanded the way
// MuJoCo's mjCFlexcomp::Make does, but kept as one soft body instead of a
// point-mass body per vertex: world-space points, elements, the pinned
// vertices (attached to the owning body) and the elasticity/contact values.
bool CollectBodyFlexcompsJson(const pugi::xml_node& body_node,
                              const std::vector<double>& body_world,
                              const Context& ctx, bool is_world,
                              const fs::path& mesh_dir, Json* out,
                              std::string* err) {
  for (const auto& f : Children(body_node, "flexcomp")) {
    const std::string name = Attr(f, "name");
    const std::string type = ToLower(Attr(f, "type", "grid"));
    auto fail = [&](const std::string& message, const char* code) {
      if (UnsupportedXML(f, "flexcomp `" + name + "`: " + message, code, "mjcf.flexcomp",
                         active_reader->root_source, false))
        return true;  // skipped
      if (err) *err = active_reader->failure;
      return false;
    };
    // Other types were already reported as unsupported while reading the XML.
    if (!IsSupportedFlexcompType(type)) continue;
    int dim = ToInt32(ParseDoubleAttr(f, "dim", 2.0), "dim");
    if (type == "circle") dim = 1;
    if (type == "square" || type == "disc") dim = 2;
    const bool box_type = type == "box" || type == "cylinder" || type == "ellipsoid";
    if (dim < 1 || dim > 3) {
      if (err) *err = "flexcomp `" + name + "`: dim must be between 1 and 3";
      return false;
    }
    const std::string dof = ToLower(Attr(f, "dof", "full"));
    if (dof != "full" &&
        !UnsupportedXML(f, "flexcomp `" + name + "` dof=`" + dof +
                               "` is approximated by free vertices (dof=full)",
                        "mjcf.flexcomp_dof_approximated", "mjcf.flexcomp",
                        active_reader->root_source, true)) {
      if (err) *err = active_reader->failure;
      return false;
    }
    if (Child(f, "plugin") &&
        !UnsupportedXML(f, "flexcomp `" + name + "` plugin is ignored",
                        "mjcf.flexcomp_plugin", "mjcf.flexcomp", active_reader->root_source, true)) {
      if (err) *err = active_reader->failure;
      return false;
    }
    std::array<int, 3> count{{10, 10, 10}};
    {
      const auto c = ParseDoubles(Attr(f, "count"));
      for (size_t i = 0; i < 3 && i < c.size(); ++i) count[i] = ToInt32(c[i], "count");
    }
    const auto spacing = ParseDouble3(Attr(f, "spacing"), {{0.02, 0.02, 0.02}});
    const auto scale = ParseDouble3(Attr(f, "scale"), {{1, 1, 1}});
    for (int c : count)
      if (c < 1 || c > 100000) {
        if (err) *err = "flexcomp `" + name + "`: invalid count";
        return false;
      }
    if (double(count[0]) * count[1] * count[2] > 1e7) {
      if (err) *err = "flexcomp `" + name + "`: more than 10 million grid points";
      return false;
    }
    std::vector<double> point;
    std::vector<int> element;
    const bool direct = type == "direct" || type == "mesh" || type == "gmsh";
    if (type == "direct") {
      point = ParseDoubles(Attr(f, "point"));
      for (double v : ParseDoubles(Attr(f, "element"))) element.push_back(ToInt32(v, "element"));
    } else if (type == "mesh" || type == "gmsh") {
      const std::string file = Attr(f, "file");
      if (file.empty()) {
        if (err) *err = "flexcomp `" + name + "`: file is required";
        return false;
      }
      const fs::path path = ResolveMeshPath(mesh_dir, mesh_dir, file, {});
      std::string text;
      if (!ReadFile(path, &text, err)) return false;
      if (type == "gmsh") {
        if (!ParseGMSH(text, &dim, &point, &element, err)) {
          if (err) *err = "flexcomp `" + name + "`: " + *err;
          return false;
        }
      } else if (!LoadFlexMesh(path, text, dim, ParseDouble3(Attr(f, "origin"), {{0, 0, 0}}),
                               HasAttr(f, "origin"), &point, &element, err)) {
        if (err) *err = "flexcomp `" + name + "`: " + *err;
        return false;
      }
    } else if (box_type) {
      if (count[0] < 2 || count[1] < 2 || count[2] < 2) {
        if (err) *err = "flexcomp `" + name + "`: box, cylinder and ellipsoid need count >= 2";
        return false;
      }
      MakeFlexBox(type, dim, count, spacing, &point, &element);
    } else if (dim == 1) {
      for (int ix = 0; ix < count[0]; ++ix) {
        if (type == "circle") {
          if (ix >= count[0] - 1) continue;
          const double theta = 2 * kPi / (count[0] - 1);
          const double radius = spacing[0] / std::sin(theta / 2) / 2;
          point.insert(point.end(), {radius * std::cos(theta * ix), radius * std::sin(theta * ix), 0});
          element.insert(element.end(), {ix, ix == count[0] - 2 ? 0 : ix + 1});
        } else {
          point.insert(point.end(), {spacing[0] * (ix - 0.5 * (count[0] - 1)), 0, 0});
          if (ix < count[0] - 1) element.insert(element.end(), {ix, ix + 1});
        }
      }
    } else if (dim == 2) {
      for (int ix = 0; ix < count[0]; ++ix)
        for (int iy = 0; iy < count[1]; ++iy) {
          double pos[2] = {spacing[0] * (ix - 0.5 * (count[0] - 1)),
                           spacing[1] * (iy - 0.5 * (count[1] - 1))};
          int tri[2][3] = {{0, 1, 2}, {0, 2, 3}};
          if (type == "disc" && ((pos[0] < -1e-6 && pos[1] > -1e-6) || (pos[0] > -1e-6 && pos[1] < -1e-6))) {
            tri[0][2] = 3;
            tri[1][0] = 1;
          }
          point.insert(point.end(), {pos[0], pos[1], 0});
          if (ix < count[0] - 1 && iy < count[1] - 1) {
            const int v[4] = {count[2] * count[1] * ix + count[2] * iy,
                              count[2] * count[1] * (ix + 1) + count[2] * iy,
                              count[2] * count[1] * (ix + 1) + count[2] * (iy + 1),
                              count[2] * count[1] * ix + count[2] * (iy + 1)};
            for (auto& t : tri) element.insert(element.end(), {v[t[0]], v[t[1]], v[t[2]]});
          }
        }
      if (type == "disc") {
        const double size[2] = {0.5 * spacing[0] * (count[0] - 1), 0.5 * spacing[1] * (count[1] - 1)};
        for (size_t i = 0; i + 2 < point.size(); i += 3) {
          double* p = &point[i];
          const double l0 = std::max(std::abs(p[0]), std::abs(p[1]));
          const double n = std::sqrt(p[0] * p[0] + p[1] * p[1]);
          if (n >= 1e-6 && std::abs(n - 1) > 1e-6) { p[0] /= n; p[1] /= n; }
          p[0] *= size[0] * l0;
          p[1] *= size[1] * l0;
        }
      }
    } else {
      static const int kCube2Tets[6][4] = {{0, 3, 1, 7}, {0, 1, 4, 7}, {1, 3, 2, 7},
                                           {1, 2, 6, 7}, {1, 5, 4, 7}, {1, 6, 5, 7}};
      for (int ix = 0; ix < count[0]; ++ix)
        for (int iy = 0; iy < count[1]; ++iy)
          for (int iz = 0; iz < count[2]; ++iz) {
            point.insert(point.end(), {spacing[0] * (ix - 0.5 * (count[0] - 1)),
                                       spacing[1] * (iy - 0.5 * (count[1] - 1)),
                                       spacing[2] * (iz - 0.5 * (count[2] - 1))});
            if (ix < count[0] - 1 && iy < count[1] - 1 && iz < count[2] - 1) {
              const int cz = count[2], cyz = count[2] * count[1];
              const int v[8] = {cyz * ix + cz * iy + iz,           cyz * (ix + 1) + cz * iy + iz,
                                cyz * (ix + 1) + cz * (iy + 1) + iz, cyz * ix + cz * (iy + 1) + iz,
                                cyz * ix + cz * iy + iz + 1,       cyz * (ix + 1) + cz * iy + iz + 1,
                                cyz * (ix + 1) + cz * (iy + 1) + iz + 1,
                                cyz * ix + cz * (iy + 1) + iz + 1};
              for (const auto& t : kCube2Tets)
                element.insert(element.end(), {v[t[0]], v[t[1]], v[t[2]], v[t[3]]});
            }
          }
    }
    if (direct)
      for (size_t i = 0; i < point.size(); ++i) point[i] *= scale[i % 3];
    const int npnt = static_cast<int>(point.size() / 3);
    if (point.empty() || point.size() % 3 || element.empty() ||
        element.size() % static_cast<size_t>(dim + 1)) {
      if (err) *err = "flexcomp `" + name + "`: point and element sizes are invalid";
      return false;
    }
    for (int e : element)
      if (e < 0 || e >= npnt) {
        if (err) *err = "flexcomp `" + name + "`: element references a missing point";
        return false;
      }
    // Pins (ids before compaction, as MuJoCo applies them).
    std::vector<bool> pinned(static_cast<size_t>(npnt), MjcBoolAttr(Attr(f, "rigid", "false")));
    for (const auto& pin : Children(f, "pin")) {
      auto ints = [&](const char* key) {
        std::vector<int> v;
        for (double d : ParseDoubles(Attr(pin, key))) v.push_back(ToInt32(d, key));
        return v;
      };
      auto grid_id = [&](const std::vector<int>& g, size_t i) {
        return dim == 2 ? g[i] * count[1] + g[i + 1]
                        : g[i] * count[1] * count[2] + g[i + 1] * count[2] + g[i + 2];
      };
      auto set = [&](int id) {
        if (id < 0 || id >= npnt) return false;
        pinned[size_t(id)] = true;
        return true;
      };
      bool ok = true;
      for (int id : ints("id")) ok &= set(id);
      const auto range = ints("range");
      for (size_t i = 0; i + 1 < range.size(); i += 2)
        for (int k = range[i]; ok && k <= range[i + 1]; ++k) ok &= set(k);
      const size_t d = static_cast<size_t>(dim);
      const auto grid = ints("grid");
      const auto gridrange = ints("gridrange");
      if ((!grid.empty() || !gridrange.empty()) && (type != "grid" || dim == 1)) ok = false;
      for (size_t i = 0; ok && i + d <= grid.size(); i += d) ok &= set(grid_id(grid, i));
      for (size_t i = 0; ok && i + 2 * d <= gridrange.size(); i += 2 * d) {
        std::vector<int> g(3, 0);
        if (dim == 2) {
          for (g[0] = gridrange[i]; g[0] <= gridrange[i + 2]; ++g[0])
            for (g[1] = gridrange[i + 1]; g[1] <= gridrange[i + 3]; ++g[1]) ok &= set(grid_id(g, 0));
        } else {
          for (g[0] = gridrange[i]; g[0] <= gridrange[i + 3]; ++g[0])
            for (g[1] = gridrange[i + 1]; g[1] <= gridrange[i + 4]; ++g[1])
              for (g[2] = gridrange[i + 2]; g[2] <= gridrange[i + 5]; ++g[2]) ok &= set(grid_id(g, 0));
        }
      }
      if (!ok) {
        if (err) *err = "flexcomp `" + name + "`: pin is out of range";
        return false;
      }
    }
    // Direct, mesh and gmsh types drop unreferenced points.
    if (direct) {
      std::vector<int> remap(static_cast<size_t>(npnt), -1);
      for (int e : element) remap[size_t(e)] = 0;
      std::vector<double> kept;
      std::vector<bool> kept_pins;
      for (int i = 0; i < npnt; ++i)
        if (remap[size_t(i)] == 0) {
          remap[size_t(i)] = static_cast<int>(kept.size() / 3);
          kept.insert(kept.end(), point.begin() + 3 * i, point.begin() + 3 * i + 3);
          kept_pins.push_back(pinned[size_t(i)]);
        }
      for (int& e : element) e = remap[size_t(e)];
      point.swap(kept);
      pinned.swap(kept_pins);
    }
    // Pose in the owning body, then world.
    const std::array<double, 3> fpos = ParseDouble3(Attr(f, "pos"), {{0, 0, 0}});
    const Quat fq = OrientationQuat(Attr(f, "quat"), Attr(f, "axisangle"), Attr(f, "euler"),
                                    Attr(f, "xyaxes"), Attr(f, "zaxis"), ctx);
    const std::vector<double> local = MatrixFromPosQuat(fpos, fq);
    const std::vector<double> world = MultiplyMatrix(body_world, local);
    auto apply = [](const std::vector<double>& m, const double* p) {
      return std::array<double, 3>{{m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
                                    m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
                                    m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]}};
    };
    Json points = Json::array(), pins = Json::array(), pin_coords = Json::array();
    for (size_t i = 0; i < point.size() / 3; ++i) {
      const auto w = apply(world, &point[3 * i]);
      points.push_back(Json::array({w[0], w[1], w[2]}));
      if (pinned[i]) {
        const auto b = apply(local, &point[3 * i]);
        pins.push_back(static_cast<int>(i));
        pin_coords.push_back(Json::array({b[0], b[1], b[2]}));
      }
    }
    if (pins.size() == points.size()) {
      if (!fail("rigid (fully pinned) flexes are not supported", "mjcf.flexcomp_rigid")) return false;
      continue;
    }
    Json entry = {{"name", name.empty() ? "flexcomp_" + std::to_string(out->size()) : name},
                  {"type", type},
                  {"dim", dim},
                  {"points", std::move(points)},
                  {"elements", Array(std::vector<double>(element.begin(), element.end()))},
                  {"mass", ParseDoubleAttr(f, "mass", 1.0)},
                  {"radius", ParseDoubleAttr(f, "radius", 0.005)},
                  {"periodic", type == "circle"}};
    if (!pins.empty()) {
      entry["pins"] = std::move(pins);
      entry["pinCoords"] = std::move(pin_coords);
      auto owner = active_body_names.find(body_node.internal_object());
      if (!is_world && owner != active_body_names.end()) entry["pinBody"] = owner->second;
    }
    ReadFlexProperties(f, &entry);
    out->push_back(std::move(entry));
  }
  return true;
}

// Depth-first over the body tree with an explicit stack: cables nest one body
// per segment, deeper than this walker's frame allows recursively.
bool CollectMujocoFlexcompsJson(const pugi::xml_node& worldbody, const Context& ctx,
                                const fs::path& mesh_dir, Json* out, std::string* err) {
  struct Item {
    pugi::xml_node node;
    std::vector<double> world;
  };
  std::vector<Item> stack{{worldbody, IdentityMatrix()}};
  bool world = true;
  while (!stack.empty()) {
    Item item = std::move(stack.back());
    stack.pop_back();
    if (!CollectBodyFlexcompsJson(item.node, item.world, ctx, world, mesh_dir, out, err))
      return false;
    world = false;
    std::vector<pugi::xml_node> children = Children(item.node, "body");
    for (auto it = children.rbegin(); it != children.rend(); ++it)
      stack.push_back({*it, MultiplyMatrix(item.world, PoseMatrix(*it, ctx))});
  }
  return true;
}


// MJCF vertex bodies absorbed into raw <flex> soft bodies (skipped by
// VisitMujocoBody).
thread_local std::set<const void*>* active_flex_bodies = nullptr;

// Raw <deformable><flex> (newton profile): each vertex follows a body. A
// vertex body that is only a point mass (three independent slide joints, or
// a free joint, an inertial and nothing else, referenced by no other
// element) is absorbed into the soft body and dropped; `world` and static
// world children pin their vertex to the world; any other body keeps its rigid body and pins
// the vertex to it. Writes `consumed` for VisitMujocoBody.
bool CollectMujocoRawFlexesJson(const pugi::xml_node& root,
                                const std::vector<pugi::xml_node>& worldbodies,
                                const Context& ctx, Json* out, std::set<const void*>* consumed,
                                std::string* err) {
  std::vector<pugi::xml_node> flexes;
  for (const auto& deformable : Children(root, "deformable"))
    for (const auto& f : Children(deformable, "flex"))
      if (HasAttr(f, "body") && HasAttr(f, "element")) flexes.push_back(f);
  if (flexes.empty()) return true;
  // Named bodies with their world frames and parents.
  struct BodyInfo {
    pugi::xml_node node;
    std::vector<double> world;
    bool world_child;
  };
  std::map<std::string, BodyInfo> bodies;
  bodies.emplace("world", BodyInfo{pugi::xml_node(), IdentityMatrix(), true});
  {
    struct Item {
      pugi::xml_node node;
      std::vector<double> world;
      bool world_child;
    };
    std::vector<Item> stack;
    for (const auto& wb : worldbodies)
      for (const auto& b : Children(wb, "body"))
        stack.push_back({b, PoseMatrix(b, ctx), true});
    while (!stack.empty()) {
      Item item = std::move(stack.back());
      stack.pop_back();
      if (HasAttr(item.node, "name"))
        bodies.emplace(Attr(item.node, "name"), BodyInfo{item.node, item.world, item.world_child});
      for (const auto& c : Children(item.node, "body"))
        stack.push_back({c, MultiplyMatrix(item.world, PoseMatrix(c, ctx)), false});
    }
  }
  // Names other elements refer to: such bodies and joints must stay.
  std::set<std::string> referenced;
  {
    static const char* const kKeys[] = {"body", "body1", "body2", "objname", "refname", "target",
                                        "joint", "joint1", "joint2", "jointinparent"};
    std::vector<pugi::xml_node> stack{root};
    while (!stack.empty()) {
      const pugi::xml_node node = stack.back();
      stack.pop_back();
      const bool flex = std::string(node.name()) == "flex";
      for (const char* key : kKeys)
        if (!(flex && std::strcmp(key, "body") == 0) && node.attribute(key))
          referenced.insert(node.attribute(key).as_string());
      for (auto c = node.first_child(); c; c = c.next_sibling())
        if (c.type() == pugi::node_element) stack.push_back(c);
    }
  }
  bool keyframes = false;
  for (const auto& kf : Children(root, "keyframe"))
    for (const auto& key : Children(kf, "key")) keyframes = keyframes || HasAttr(key, "qpos");
  // 1 free point, 0 static (pinned to the world), -1 keep the rigid body.
  auto classify = [&](const BodyInfo& b, double* mass) {
    const auto& n = b.node;
    if (!n) return 0;  // "world": a fixed vertex
    if (referenced.count(Attr(n, "name")) || Child(n, "body") || Child(n, "geom") ||
        Child(n, "site") || Child(n, "camera") || Child(n, "light") || Child(n, "frame") ||
        Child(n, "flexcomp") || Child(n, "composite") || Child(n, "replicate") ||
        MjcBoolAttr(Attr(n, "mocap", "false")))
      return -1;
    const auto inertial = Child(n, "inertial");
    *mass = inertial ? ParseDoubleAttr(inertial, "mass", 0.0) : 0.0;
    std::vector<std::array<double, 3>> axes;
    bool free = static_cast<bool>(Child(n, "freejoint"));
    for (const auto& j : Children(n, "joint")) {
      if (HasAttr(j, "name") && referenced.count(Attr(j, "name"))) return -1;
      const std::string type = ToLower(Attr(j, "type", "hinge"));
      if (type == "free") free = true;
      else if (type == "slide") axes.push_back(ParseDouble3(Attr(j, "axis"), {{0, 0, 1}}));
      else return -1;
    }
    // A free joint only exists on world children; three slide joints free a
    // point below any body too (MuJoCo's flexcomp output nests them there).
    if (free && axes.empty()) return *mass > 0 ? 1 : -1;
    if (free) return -1;
    if (axes.empty()) return b.world_child ? 0 : -1;
    if (axes.size() != 3 || !(*mass > 0)) return -1;
    const auto& a = axes[0];
    const auto& b2 = axes[1];
    const auto& c = axes[2];
    const double det = a[0] * (b2[1] * c[2] - b2[2] * c[1]) - a[1] * (b2[0] * c[2] - b2[2] * c[0]) +
                       a[2] * (b2[0] * c[1] - b2[1] * c[0]);
    return std::fabs(det) > 1e-9 ? 1 : -1;
  };
  for (const auto& f : flexes) {
    const std::string name = Attr(f, "name");
    auto skip = [&](const std::string& message, const char* code) {
      if (UnsupportedXML(f, "flex `" + name + "`: " + message, code, "mjcf.deformable",
                         active_reader->root_source, false))
        return true;
      if (err) *err = active_reader->failure;
      return false;
    };
    if (keyframes) {
      // Dropping the vertex bodies' joints would shift keyframe qpos.
      if (!skip("keyframes address the vertex bodies' joints", "mjcf.flex_keyframes")) return false;
      continue;
    }
    if (HasAttr(f, "node") || HasAttr(f, "dof")) {
      if (!skip("interpolated (node/dof) flexes are not supported", "mjcf.flex_interpolated"))
        return false;
      continue;
    }
    const int dim = ToInt32(ParseDoubleAttr(f, "dim", 2.0), "dim");
    std::vector<std::string> names;
    {
      std::istringstream in(Attr(f, "body"));
      for (std::string b; in >> b;) names.push_back(b);
    }
    const std::vector<double> vertex = ParseDoubles(Attr(f, "vertex"));
    std::vector<int> element;
    for (double e : ParseDoubles(Attr(f, "element"))) element.push_back(ToInt32(e, "element"));
    const size_t npnt = vertex.empty() ? names.size() : vertex.size() / 3;
    if (dim < 1 || dim > 3 || names.empty() || vertex.size() % 3 ||
        (names.size() != 1 && names.size() != npnt) || element.empty() ||
        element.size() % size_t(dim + 1)) {
      if (!skip("body, vertex and element sizes are invalid", "mjcf.flex_invalid")) return false;
      continue;
    }
    bool valid = true;
    for (int e : element) valid = valid && e >= 0 && size_t(e) < npnt;
    for (const auto& b : names) valid = valid && bodies.count(b);
    if (!valid) {
      if (!skip("element or body reference is missing", "mjcf.flex_invalid")) return false;
      continue;
    }
    Json points = Json::array(), pins = Json::array(), pin_coords = Json::array(),
         pin_bodies = Json::array(), masses = Json::array();
    std::vector<const void*> absorbed;
    double damping = 0.0;
    size_t free_points = 0;
    for (size_t i = 0; i < npnt; ++i) {
      const std::string& bname = names.size() == 1 ? names[0] : names[i];
      const BodyInfo& b = bodies.at(bname);
      const double local[3] = {vertex.empty() ? 0.0 : vertex[3 * i],
                               vertex.empty() ? 0.0 : vertex[3 * i + 1],
                               vertex.empty() ? 0.0 : vertex[3 * i + 2]};
      const auto& m = b.world;
      points.push_back(Json::array({m[0] * local[0] + m[4] * local[1] + m[8] * local[2] + m[12],
                                    m[1] * local[0] + m[5] * local[1] + m[9] * local[2] + m[13],
                                    m[2] * local[0] + m[6] * local[1] + m[10] * local[2] + m[14]}));
      double mass = 0.0;
      // A single body carries every vertex rigidly: pin them all to it.
      const int kind = names.size() == 1 ? -1 : classify(b, &mass);
      masses.push_back(kind == 1 ? mass : 0.0);
      if (kind == 1) {
        ++free_points;
        absorbed.push_back(b.node.internal_object());
        for (const auto& j : Children(b.node, "joint"))
          damping = std::max(damping, ParseDoubleAttr(j, "damping", 0.0));
        continue;
      }
      if (kind == 0 && b.node) absorbed.push_back(b.node.internal_object());
      pins.push_back(static_cast<int>(i));
      pin_coords.push_back(Json::array({local[0], local[1], local[2]}));
      pin_bodies.push_back(kind == 0 || !b.node ? std::string() : bname);
    }
    if (!free_points) {
      if (!skip("no vertex body is a free point mass", "mjcf.flex_rigid")) return false;
      continue;
    }
    // Pinned vertices have no mass of their own in MuJoCo; give them the
    // mean free mass so engines deriving a density see a uniform body (as
    // the flexcomp path's equal shares).
    double free_mass = 0.0;
    for (size_t i = 0; i < masses.size(); ++i) free_mass += masses[i].get<double>();
    const double mean = free_mass / double(free_points);
    std::set<int> pinned_ids;
    for (size_t i = 0; i < pins.size(); ++i) pinned_ids.insert(pins[i].get<int>());
    for (int id : pinned_ids) masses[size_t(id)] = mean;
    for (const void* b : absorbed) consumed->insert(b);
    bool periodic = false;
    if (dim == 1 && element.size() == 2 * npnt) periodic = element.back() == 0;
    Json entry = {{"name", name.empty() ? "flex_" + std::to_string(out->size()) : name},
                  {"type", "flex"},
                  {"dim", dim},
                  {"points", std::move(points)},
                  {"elements", Array(std::vector<double>(element.begin(), element.end()))},
                  {"masses", std::move(masses)},
                  {"radius", ParseDoubleAttr(f, "radius", 0.005)},
                  {"periodic", periodic}};
    if (!pins.empty()) {
      entry["pins"] = std::move(pins);
      entry["pinCoords"] = std::move(pin_coords);
      entry["pinBodies"] = std::move(pin_bodies);
    }
    ReadFlexProperties(f, &entry);
    if (damping > 0)
      active_reader->Warn("flex `" + name + "`: vertex joint damping is dropped with the "
                              "absorbed point bodies",
                          "mjcf.flex_vertex_damping");
    out->push_back(std::move(entry));
  }
  return true;
}

void AddJointFromNode(const pugi::xml_node& joint_node, const AttrMap& jcls,
                      const Context& ctx, const std::string& parent_name,
                      const std::string& child_name,
                      const std::vector<double>& origin_matrix,
                      const std::array<double, 3>& origin_pos, Json* joints) {
  Json joint = Json::object();
  joint["name"] =
      joint_node ? Attr(joint_node, "name", parent_name + "_to_" + child_name)
                 : parent_name + "_to_" + child_name + "_fixed";
  const std::string mj_type =
      joint_node ? Eff(joint_node, jcls, "type", "hinge") : "fixed";
  // MuJoCo ball (3-DOF rotation) -> USD PhysicsSphericalJoint. Matches the JS
  // parsers (web/js/cli/urdf-to-usd.js, web/js/urdf.js); the USD converter
  // already handles "spherical". (free/floating base is still emitted as
  // "fixed" here — handled separately as the articulation root.)
  joint["type"] = (mj_type == "hinge")   ? "revolute"
                  : (mj_type == "slide") ? "prismatic"
                  : (mj_type == "ball")  ? "spherical"
                                         : "fixed";
  joint["parent"] = parent_name;
  joint["child"] = child_name;
  const auto axis = ParseDouble3(
      joint_node ? Eff(joint_node, jcls, "axis") : "", {{0.0, 0.0, 1.0}});
  if (axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] < 1e-24)
    active_reader->failure = "MJCF joint has zero axis";
  joint["axis"] = Json::array({axis[0], axis[1], axis[2]});
  joint["axisToken"] = AxisToken(axis);
  joint["origin"] = Json::array({origin_pos[0], origin_pos[1], origin_pos[2]});
  joint["originMatrix"] = Array(origin_matrix);
  const auto jp =
      ParseDouble3(joint_node ? Eff(joint_node, jcls, "pos") : "", {{0, 0, 0}});
  std::array<double, 3> body_pos;
  Quat body_q;
  DecomposePosQuat(origin_matrix, &body_pos, &body_q);
  const Quat align = QuatFromTwoVecs({{1, 0, 0}}, axis);
  const Quat q0 = QuatMul(body_q, align);
  joint["axisToken"] = "X";
  joint["localPos0"] =
      Json::array({origin_matrix[0] * jp[0] + origin_matrix[4] * jp[1] +
                       origin_matrix[8] * jp[2] + origin_matrix[12],
                   origin_matrix[1] * jp[0] + origin_matrix[5] * jp[1] +
                       origin_matrix[9] * jp[2] + origin_matrix[13],
                   origin_matrix[2] * jp[0] + origin_matrix[6] * jp[1] +
                       origin_matrix[10] * jp[2] + origin_matrix[14]});
  joint["localPos1"] = Json::array({jp[0], jp[1], jp[2]});
  joint["localRot0"] = Json::array({q0[0], q0[1], q0[2], q0[3]});
  joint["localRot1"] = Json::array({align[0], align[1], align[2], align[3]});

  if (joint_node) {
    const std::vector<double> range =
        ParseDoubles(Eff(joint_node, jcls, "range"));
    // MJCF `limited` is true/false/auto; auto means limited exactly when a
    // range is authored and <compiler autolimits> is enabled (the default).
    const std::string limited_token = ToLower(Eff(joint_node, jcls, "limited", "auto"));
    const bool limited =
        limited_token == "true" ||
        (limited_token == "auto" && ctx.autolimits && range.size() >= 2);
    const bool legacy = active_reader->options.profile == "legacy";
    if (range.size() >= 2 && limited) {
      const double scale = (mj_type == "hinge" || mj_type == "ball") ? ctx.angle_to_rad : 1.0;
      if (mj_type == "ball") {
        // MuJoCo ball joints limit only the swing angle (range[1]); UsdPhysics
        // expresses that as equal spherical cone half-angles.
        if (legacy) {
          if (!UnsupportedXML(joint_node,
                  "MJCF spherical joint cone limits are not converted",
                  "mjcf.ball_limits_unsupported", "mjcf.ball_limits",
                  active_reader->root_source)) return;
        } else {
          joint["coneLimit"] = range[1] * scale;
        }
      } else {
        // The USD converter expects revolute limits in radians (it re-converts
        // to degrees). MJCF hinge ranges are in the compiler's angle unit
        // (degrees by default); slide ranges are meters and pass through.
        joint["limit"] = {{"lower", range[0] * scale},
                          {"upper", range[1] * scale}};
      }
    }
    // MjcJointAPI source values, authored by non-legacy profiles. Angles stay
    // in the compiler unit recorded by mjc:compiler:angle (MuJoCo's decoder
    // interprets ref/springref through it).
    Json mjc = Json::object();
    for (const char* key : {"ref", "springref", "margin", "stiffness"})
      if (HasEff(joint_node, jcls, key)) mjc[key] = EffDouble(joint_node, jcls, key, 0.0);
    for (const char* key : {"solreflimit", "solimplimit", "solreffriction",
                            "solimpfriction", "springdamper"}) {
      const auto values = ParseDoubles(Eff(joint_node, jcls, key));
      if (!values.empty()) mjc[key] = Array(values);
    }
    const auto frc = ParseDoubles(Eff(joint_node, jcls, "actuatorfrcrange"));
    if (frc.size() >= 2) mjc["actuatorfrcrange"] = Json::array({frc[0], frc[1]});
    if (HasEff(joint_node, jcls, "actuatorfrclimited"))
      mjc["actuatorfrclimited"] = ToLower(Eff(joint_node, jcls, "actuatorfrclimited"));
    if (HasEff(joint_node, jcls, "actuatorgravcomp"))
      mjc["actuatorgravcomp"] = MjcBoolAttr(Eff(joint_node, jcls, "actuatorgravcomp"));
    if (HasEff(joint_node, jcls, "group"))
      mjc["group"] = ToInt32(EffDouble(joint_node, jcls, "group", 0.0), "group");
    if (!mjc.empty()) joint["mjc"] = std::move(mjc);
    Json dynamics = Json::object();
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

// MuJoCo's compiler expansion of actuator shortcuts into the generic
// gain/bias/dynamics model (mjs_setToMotor/Position/... in user_api). USD
// MjcActuator prims carry only the expanded form; Newton maps position/velocity
// shaped rows back to joint targets and MuJoCo's USD decoder reads them as is.
// Returns false for actuator kinds the MjcPhysics schema cannot express.
bool ExpandMjcfActuator(const pugi::xml_node& node, const std::string& type,
                        Json* a) {
  auto num = [&](const char* key, double fallback) {
    return HasAttr(node, key) ? ParseDoubleAttr(node, key, fallback) : fallback;
  };
  auto vec = [&](const char* key, std::vector<double> fallback) {
    const auto v = ParseDoubles(Attr(node, key));
    return v.empty() ? fallback : v;
  };
  std::vector<double> gain{1}, bias, dyn{1};
  std::string gaintype = "fixed", biastype = "none", dyntype = "none";
  if (type == "general") {
    gain = vec("gainprm", {1});
    bias = vec("biasprm", {});
    dyn = vec("dynprm", {1});
    gaintype = ToLower(Attr(node, "gaintype", "fixed"));
    biastype = ToLower(Attr(node, "biastype", "none"));
    dyntype = ToLower(Attr(node, "dyntype", "none"));
  } else if (type == "motor") {
  } else if (type == "position" || type == "intvelocity") {
    const double kp = num("kp", 1.0), kv = num("kv", 0.0);
    const double dampratio = num("dampratio", 0.0);
    gain = {kp};
    biastype = "affine";
    // A positive biasprm[2] is MuJoCo's unresolved dampratio placeholder.
    bias = {0.0, -kp, kv > 0.0 ? -kv : dampratio};
    if (type == "intvelocity") {
      dyntype = "integrator";
      (*a)["actLimited"] = "true";
      const auto range = ParseDoubles(Attr(node, "actrange"));
      if (range.size() >= 2) (*a)["actRange"] = Json::array({range[0], range[1]});
    } else {
      const double timeconst = num("timeconst", 0.0);
      if (timeconst > 0.0) {
        dyntype = "filterexact";
        dyn = {timeconst};
      }
    }
    if (HasAttr(node, "inheritrange")) (*a)["inheritRange"] = num("inheritrange", 0.0);
  } else if (type == "velocity") {
    const double kv = num("kv", 1.0);
    gain = {kv};
    biastype = "affine";
    bias = {0.0, 0.0, -kv};
  } else if (type == "damper") {
    const double kv = num("kv", 1.0);
    gaintype = "affine";
    gain = {0.0, 0.0, -kv};
    (*a)["ctrlLimited"] = "true";
  } else if (type == "cylinder") {
    double area = num("area", 1.0);
    if (HasAttr(node, "diameter")) {
      const double d = num("diameter", 1.0);
      area = 0.25 * 3.14159265358979323846 * d * d;
    }
    dyntype = "filter";
    dyn = {num("timeconst", 1.0)};
    gain = {area};
    biastype = "affine";
    bias = vec("bias", {0.0, 0.0, 0.0});
  } else if (type == "muscle") {
    const auto timeconst = vec("timeconst", {0.01, 0.04});
    const auto range = vec("range", {0.75, 1.05});
    dyntype = gaintype = biastype = "muscle";
    dyn = {timeconst.size() > 0 ? timeconst[0] : 0.01,
           timeconst.size() > 1 ? timeconst[1] : 0.04, num("tausmooth", 0.0)};
    gain = {range.size() > 0 ? range[0] : 0.75, range.size() > 1 ? range[1] : 1.05,
            num("force", -1.0), num("scale", 200.0), num("lmin", 0.5),
            num("lmax", 1.6), num("vmax", 1.5), num("fpmax", 1.3),
            num("fvmax", 1.2)};
    bias = gain;
  } else if (type == "adhesion") {
    gain = {num("gain", 1.0)};
    (*a)["ctrlLimited"] = "true";
  } else {
    return false;  // plugin and future shortcuts
  }
  static const std::set<std::string> kGain = {"fixed", "affine", "muscle", "user"};
  static const std::set<std::string> kBias = {"none", "affine", "muscle", "user"};
  static const std::set<std::string> kDyn = {"none", "integrator", "filter",
                                             "filterexact", "muscle", "user"};
  // pid/dcmotor/so3 and similar have no MjcPhysics token; neither MuJoCo's
  // USD decoder nor Newton supports them (mujoco-usd-converter 0.8 skips).
  if (!kGain.count(gaintype) || !kBias.count(biastype) || !kDyn.count(dyntype))
    return false;
  auto pad = [](std::vector<double> v, size_t n) {
    v.resize(std::max(v.size(), n), 0.0);
    return v;
  };
  (*a)["gainType"] = gaintype;
  (*a)["biasType"] = biastype;
  (*a)["dynType"] = dyntype;
  (*a)["gainPrm"] = Array(pad(gain, 10));
  (*a)["biasPrm"] = Array(pad(bias, 10));
  (*a)["dynPrm"] = Array(pad(dyn, 10));
  (*a)["gear"] = Array(pad(vec("gear", {1}), 6));
  for (const char* key : {"ctrlrange", "forcerange", "actrange", "lengthrange"}) {
    const auto range = ParseDoubles(Attr(node, key));
    static const std::map<std::string, std::string> kNames = {
        {"ctrlrange", "ctrlRange"}, {"forcerange", "forceRange"},
        {"actrange", "actRange"}, {"lengthrange", "lengthRange"}};
    if (range.size() >= 2) (*a)[kNames.at(key)] = Json::array({range[0], range[1]});
  }
  for (const auto& entry : {std::make_pair("ctrllimited", "ctrlLimited"),
                            std::make_pair("forcelimited", "forceLimited"),
                            std::make_pair("actlimited", "actLimited")})
    if (HasAttr(node, entry.first)) (*a)[entry.second] = ToLower(Attr(node, entry.first));
  if (HasAttr(node, "group")) (*a)["group"] = ToInt32(num("group", 0.0), "group");
  if (HasAttr(node, "actdim")) (*a)["actDim"] = ToInt32(num("actdim", -1.0), "actdim");
  if (HasAttr(node, "actearly")) (*a)["actEarly"] = MjcBoolAttr(Attr(node, "actearly"));
  if (HasAttr(node, "cranklength")) (*a)["crankLength"] = num("cranklength", 0.0);
  if (HasAttr(node, "refsite")) (*a)["refSite"] = Attr(node, "refsite");
  if (HasAttr(node, "slidersite")) (*a)["sliderSite"] = Attr(node, "slidersite");
  if (HasAttr(node, "jointinparent"))
    (*a)["jointInParent"] = MjcBoolAttr(Attr(node, "jointinparent"));
  return true;
}

void AddMujocoActuatorsJson(const pugi::xml_node& root, Json* actuators,
                            Json* mjc_actuators, Stats* stats) {
  if (!actuators) return;
  const auto actuator_root = Child(root, "actuator");
  if (!actuator_root) return;
  const bool legacy = active_reader->options.profile == "legacy";

  for (const auto& act_node : actuator_root.children()) {
    const std::string type = act_node.name();
    const std::string joint = Attr(act_node, "joint");
    const std::string tendon = Attr(act_node, "tendon");
    const std::string site = Attr(act_node, "site");
    const std::string body = Attr(act_node, "body");
    if (!legacy && mjc_actuators && type != "plugin") {
      Json a = Json::object();
      const std::string fallback =
          !joint.empty() ? joint : (!tendon.empty() ? tendon : (!site.empty() ? site : body));
      a["name"] = Attr(act_node, "name", type + "_" + fallback);
      a["actuatorType"] = type;
      if (!joint.empty()) a["targetJoint"] = joint;
      if (!tendon.empty()) a["targetTendon"] = tendon;
      if (!site.empty()) a["targetSite"] = site;
      if (!body.empty()) a["targetBody"] = body;
      if (!ExpandMjcfActuator(act_node, type, &a)) {
        if (!UnsupportedXML(act_node,
                "MJCF actuator `" + a["name"].get<std::string>() +
                    "` uses a gain/bias/dynamics type without an MjcPhysics token",
                "mjcf.actuator_type_unsupported", "mjcf.actuator_type",
                active_reader->root_source))
          return;
        continue;
      }
      mjc_actuators->push_back(std::move(a));
      if (stats) stats->actuators++;
      continue;
    }

    // Muscle/general/adhesion/cylinder/intvelocity/damper, or any
    // tendon-/site-/ body-targeted actuator -> MjcActuator (preserves the
    // MuJoCo gain/bias/ lengthrange params; e.g. ms_human_700 muscles, flybody
    // adhesion). Plain joint-targeted position/velocity/motor actuators keep
    // the Newton PD path.
    const bool is_mjc =
        (type == "muscle" || type == "general" || type == "adhesion" ||
         type == "cylinder" || type == "intvelocity" || type == "damper" ||
         type == "plugin" || !tendon.empty() || !site.empty() || !body.empty());
    if (is_mjc && mjc_actuators) {
      const std::string fallback_target =
          !joint.empty()
              ? joint
              : (!tendon.empty() ? tendon : (!site.empty() ? site : body));
      Json a = Json::object();
      a["name"] = Attr(act_node, "name", type + "_" + fallback_target);
      a["actuatorType"] = type;
      if (!joint.empty()) a["targetJoint"] = joint;
      if (!tendon.empty()) a["targetTendon"] = tendon;
      if (!site.empty()) a["targetSite"] = site;
      if (!body.empty()) a["targetBody"] = body;
      const auto gainprm = ParseDoubles(Attr(act_node, "gainprm"));
      if (!gainprm.empty())
        a["gainPrm"] = Array(gainprm);
      else if (HasAttr(act_node, "gain"))  // <adhesion gain=..> scalar
        a["gainPrm"] = Json::array({ParseDoubleAttr(act_node, "gain", 0.0)});
      const auto biasprm = ParseDoubles(Attr(act_node, "biasprm"));
      if (!biasprm.empty()) a["biasPrm"] = Array(biasprm);
      const auto lengthrange = ParseDoubles(Attr(act_node, "lengthrange"));
      if (lengthrange.size() >= 2)
        a["lengthRange"] = Json::array({lengthrange[0], lengthrange[1]});
      const auto ctrlrange = ParseDoubles(Attr(act_node, "ctrlrange"));
      if (ctrlrange.size() >= 2)
        a["ctrlRange"] = Json::array({ctrlrange[0], ctrlrange[1]});
      const auto forcerange = ParseDoubles(Attr(act_node, "forcerange"));
      if (forcerange.size() >= 2)
        a["forceRange"] = Json::array({forcerange[0], forcerange[1]});
      const auto gear = ParseDoubles(Attr(act_node, "gear"));
      if (!gear.empty()) a["gear"] = Array(gear);
      if (HasAttr(act_node, "dyntype"))
        a["dynType"] = Attr(act_node, "dyntype");
      if (HasAttr(act_node, "gaintype"))
        a["gainType"] = Attr(act_node, "gaintype");
      if (HasAttr(act_node, "biastype"))
        a["biasType"] = Attr(act_node, "biastype");
      // Engine-plugin actuator (<plugin plugin=".." instance="..">): preserve
      // the plugin id + referenced <extension> instance name.
      if (HasAttr(act_node, "plugin")) a["plugin"] = Attr(act_node, "plugin");
      if (HasAttr(act_node, "instance"))
        a["instance"] = Attr(act_node, "instance");
      mjc_actuators->push_back(std::move(a));
      if (stats) stats->actuators++;
      continue;
    }

    if (joint.empty()) {
      continue;
    }
    Json act = Json::object();
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
      act["maxEffort"] =
          std::max(std::abs(force_range[0]), std::abs(force_range[1]));
    }
    if (HasAttr(act_node, "delay")) {
      act["delaySteps"] =
          ToInt32(ParseDoubleAttr(act_node, "delay", 1.0), "delay");
    }
    Json mjc = Json::object();
    mjc["type"] = type;
    for (const auto& attr : act_node.attributes())
      mjc["source:" + std::string(attr.name())] = std::string(attr.value());
    for (const char* key :
         {"gear", "ctrlrange", "forcerange", "gainprm", "biasprm", "dynprm"}) {
      const auto values = ParseDoubles(Attr(act_node, key));
      if (!values.empty()) mjc[key] = Array(values);
    }
    act["mjc"] = std::move(mjc);
    if (type ==
        "motor") {  // A control range is a bound, not a constant command.
      active_reader->Warn(
          "MJCF motor control ranges are retained; no fixed control command is "
          "inferred");
    }
    actuators->push_back(std::move(act));
    if (stats) stats->actuators++;
  }
}

// MJCF <tendon><fixed> -> JSON tendon entries consumed by AddMjcTendonFromJson.
// A fixed tendon constrains a linear combination of joint coordinates; we carry
// the {joint, coef} list plus the spring/damper/range scalars. Spatial tendons
// carry their ordered site/wrap-geom/pulley path.
void AddMujocoTendonsJson(const pugi::xml_node& root, const Context& ctx,
                          Json* tendons, Stats* stats) {
  if (!tendons) return;
  const auto tendon_root = Child(root, "tendon");
  if (!tendon_root) return;
  for (const auto& t_node : tendon_root.children()) {
    const std::string kind = t_node.name();  // "fixed" or "spatial"
    if (kind != "fixed" && kind != "spatial") continue;
    Json tendon = Json::object();
    tendon["name"] = Attr(
        t_node, "name", "tendon_" + std::to_string(stats ? stats->tendons : 0));
    tendon["type"] = kind;
    if (kind == "fixed") {
      // Fixed tendon: linear combination of joint coordinates.
      Json jlist = Json::array();
      for (const auto& j_node : Children(t_node, "joint")) {
        const std::string jn = Attr(j_node, "joint");
        if (jn.empty()) continue;
        Json je = Json::object();
        je["joint"] = jn;
        je["coef"] = ParseDoubleAttr(j_node, "coef", 1.0);
        jlist.push_back(std::move(je));
      }
      if (jlist.empty()) continue;
      tendon["joints"] = std::move(jlist);
    } else {
      // Spatial tendon (muscle path): ordered <site>, <geom sidesite=...> and
      // <pulley divisor=...> elements; the writer encodes MjcTendon's
      // path/segments/divisors/sideSites arrays from this order.
      Json waypoints = Json::array();
      for (const auto& w : t_node.children()) {
        const std::string wn = w.name();
        if (wn == "site" && HasAttr(w, "site")) {
          waypoints.push_back({{"site", Attr(w, "site")}});
        } else if (wn == "geom" && HasAttr(w, "geom")) {
          Json wp = {{"geom", Attr(w, "geom")}};
          if (HasAttr(w, "sidesite")) wp["sidesite"] = Attr(w, "sidesite");
          waypoints.push_back(std::move(wp));
        } else if (wn == "pulley") {
          waypoints.push_back({{"pulley", ParseDoubleAttr(w, "divisor", 1.0)}});
        }
      }
      if (waypoints.size() < 2) continue;
      tendon["path"] = std::move(waypoints);
      if (HasAttr(t_node, "width"))
        tendon["width"] = ParseDoubleAttr(t_node, "width", 0.003);
      const auto rgba = ParseDoubles(Attr(t_node, "rgba"));
      if (rgba.size() >= 4)
        tendon["rgba"] = Json::array({rgba[0], rgba[1], rgba[2], rgba[3]});
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
    if (range.size() >= 2) tendon["range"] = Json::array({range[0], range[1]});
    {
      const std::string token = ToLower(Attr(t_node, "limited", "auto"));
      const bool limited = token == "true" ||
                           (token == "auto" && ctx.autolimits && range.size() >= 2);
      if (range.size() >= 2 || token != "auto")
        tendon["limited"] = limited ? "true" : "false";
    }
    auto springlen = ParseDoubles(Attr(t_node, "springlength"));
    // One value means a fixed rest length; Newton reads a 2-vector.
    if (springlen.size() == 1) springlen.push_back(springlen[0]);
    if (!springlen.empty()) tendon["springlength"] = Array(springlen);
    if (HasAttr(t_node, "group"))
      tendon["group"] = ToInt32(ParseDoubleAttr(t_node, "group", 0.0), "group");
    if (HasAttr(t_node, "armature"))
      tendon["armature"] = ParseDoubleAttr(t_node, "armature", 0.0);
    for (const char* key : {"solreflimit", "solimplimit", "solreffriction", "solimpfriction"}) {
      const auto values = ParseDoubles(Attr(t_node, key));
      if (!values.empty()) tendon[key] = Array(values);
    }
    const auto frcrange = ParseDoubles(Attr(t_node, "actuatorfrcrange"));
    if (frcrange.size() >= 2)
      tendon["actuatorfrcrange"] = Json::array({frcrange[0], frcrange[1]});
    if (HasAttr(t_node, "actuatorfrclimited")) {
      const std::string token = ToLower(Attr(t_node, "actuatorfrclimited"));
      const bool limited = token == "true" ||
                           (token == "auto" && ctx.autolimits && frcrange.size() >= 2);
      tendon["actuatorfrclimited"] = limited ? "true" : "false";
    } else if (frcrange.size() >= 2 && ctx.autolimits) {
      tendon["actuatorfrclimited"] = "true";
    }
    tendons->push_back(std::move(tendon));
    if (stats) stats->tendons++;
  }
}

// MJCF <site> (in bodies) -> JSON site entries with a baked world transform,
// consumed by the converter to emit MjcSite marker prims. Sites are the routing
// points for spatial (muscle) tendons. Recurses the body tree.
void CollectMujocoSitesJson(const pugi::xml_node& body_node,
                            const std::vector<double>& parent_world,
                            const Context& ctx, Json* sites, Stats* stats) {
  const std::vector<double> body_world =
      MultiplyMatrix(parent_world, PoseMatrix(body_node, ctx));
  for (const auto& s_node : Children(body_node, "site")) {
    const std::string name =
        Attr(s_node, "name", "site_" + std::to_string(sites->size()));
    const std::vector<double> site_world =
        MultiplyMatrix(body_world, PoseMatrix(s_node, ctx));
    Json site = {{"name", name}, {"matrix", Array(site_world)},
                 {"bodyMatrix", Array(body_world)}};
    auto owner = active_body_names.find(body_node.internal_object());
    site["body"] = owner != active_body_names.end() ? owner->second : active_world_name;
    if (HasAttr(s_node, "group"))
      site["group"] = ToInt32(ParseDoubleAttr(s_node, "group", 0.0), "group");
    const auto size = ParseDoubles(Attr(s_node, "size"));
    if (!size.empty()) site["size"] = size[0];
    if (size.size() > 1) site["sizes"] = Array(size);
    const std::string type = ToLower(Attr(s_node, "type"));
    if (!type.empty()) site["type"] = type;
    sites->push_back(std::move(site));
    if (stats) stats->sites++;
  }
  for (const auto& child : Children(body_node, "body")) {
    CollectMujocoSitesJson(child, body_world, ctx, sites, stats);
  }
}

bool MjcBoolAttr(const std::string& v);  // defined below

// MJCF <sensor> children -> JSON sensor entries consumed by the converter's
// AddMjcSensorFromJson -> MjcSensor prim. The sensor kind is the element name;
// the measured object is captured via objtype/objname (frame sensors carry them
// explicitly, others attach via site/joint/actuator/tendon/body/geom).
void AddMujocoSensorsJson(const pugi::xml_node& root, Json* sensors,
                          Stats* stats) {
  if (!sensors) return;
  const auto sensor_root = Child(root, "sensor");
  if (!sensor_root) return;
  for (const auto& s_node : sensor_root.children()) {
    const std::string type = s_node.name();
    if (type.empty()) continue;
    Json s = {{"type", type}};
    s["name"] =
        Attr(s_node, "name", type + "_" + std::to_string(sensors->size()));
    if (HasAttr(s_node, "objtype")) {
      s["objtype"] = Attr(s_node, "objtype");
      s["objname"] = Attr(s_node, "objname");
    } else {
      // Infer objtype from the attachment attribute the sensor type uses.
      for (const char* k :
           {"site", "joint", "actuator", "tendon", "body", "geom"}) {
        if (HasAttr(s_node, k)) {
          s["objtype"] = k;
          s["objname"] = Attr(s_node, k);
          break;
        }
      }
    }
    if (HasAttr(s_node, "reftype")) s["reftype"] = Attr(s_node, "reftype");
    if (HasAttr(s_node, "refname")) s["refname"] = Attr(s_node, "refname");
    if (HasAttr(s_node, "group"))
      s["group"] = ToInt32(ParseDoubleAttr(s_node, "group", 0.0), "group");
    if (HasAttr(s_node, "cutoff"))
      s["cutoff"] = ParseDoubleAttr(s_node, "cutoff", 0.0);
    if (HasAttr(s_node, "noise"))
      s["noise"] = ParseDoubleAttr(s_node, "noise", 0.0);
    const auto user = ParseDoubles(Attr(s_node, "user"));
    if (!user.empty()) s["user"] = Array(user);
    sensors->push_back(std::move(s));
    if (stats) stats->sensors++;
  }
}

// MJCF <custom><numeric|text> -> JSON consumed by the converter to preserve
// model metadata / MJX compile knobs (e.g. max_contact_points) as mjc:custom:*.
void AddMujocoCustomJson(const pugi::xml_node& root, Json* custom) {
  if (!custom) return;
  const auto custom_root = Child(root, "custom");
  if (!custom_root) return;
  Json numerics = Json::array();
  for (const auto& n : Children(custom_root, "numeric")) {
    const std::string name = Attr(n, "name");
    if (name.empty()) continue;
    numerics.push_back(
        {{"name", name}, {"data", Array(ParseDoubles(Attr(n, "data")))}});
  }
  Json texts = Json::array();
  for (const auto& t : Children(custom_root, "text")) {
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
void AddMujocoPluginsJson(const pugi::xml_node& root, Json* plugins) {
  if (!plugins) return;
  const auto extension = Child(root, "extension");
  if (!extension) return;
  for (const auto& plugin_node : Children(extension, "plugin")) {
    const std::string plugin_id = Attr(plugin_node, "plugin");
    for (const auto& inst : Children(plugin_node, "instance")) {
      const std::string inst_name = Attr(inst, "name");
      if (inst_name.empty()) continue;
      Json p = Json::object();
      p["instance"] = inst_name;
      if (!plugin_id.empty()) p["plugin"] = plugin_id;
      Json config = Json::object();
      for (const auto& cfg : Children(inst, "config")) {
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
void CollectMujocoLightsCamerasJson(const pugi::xml_node& frame_node,
                                    const std::vector<double>& frame_world,
                                    const Context& ctx, Json* lights,
                                    Json* cameras) {
  for (const auto& l : Children(frame_node, "light")) {
    Json light = Json::object();
    light["name"] = Attr(l, "name", "light");
    std::string type = Attr(l, "type");
    if (type.empty())
      type =
          MjcBoolAttr(Attr(l, "directional", "false")) ? "directional" : "spot";
    light["type"] = type;
    light["matrix"] = Array(MultiplyMatrix(frame_world, PoseMatrix(l, ctx)));
    const auto dir = ParseDouble3(Attr(l, "dir"), {{0.0, 0.0, -1.0}});
    light["dir"] = Json::array({dir[0], dir[1], dir[2]});
    const auto diffuse = ParseDoubles(Attr(l, "diffuse"));
    if (diffuse.size() >= 3)
      light["color"] = Json::array({diffuse[0], diffuse[1], diffuse[2]});
    if (HasAttr(l, "castshadow"))
      light["castshadow"] = MjcBoolAttr(Attr(l, "castshadow"));
    if (HasAttr(l, "cutoff"))
      light["cutoff"] = ParseDoubleAttr(l, "cutoff", 45.0);
    for (const char* key : {"intensity", "exponent"})
      if (HasAttr(l, key)) light[key] = ParseDoubleAttr(l, key, 0.0);
    lights->push_back(std::move(light));
  }
  for (const auto& c : Children(frame_node, "camera")) {
    Json cam = Json::object();
    cam["name"] = Attr(c, "name", "camera");
    cam["matrix"] = Array(MultiplyMatrix(frame_world, PoseMatrix(c, ctx)));
    // MuJoCo's default vertical field of view is 45 degrees.
    cam["fovy"] = ParseDoubleAttr(c, "fovy", 45.0);
    const std::string proj = Attr(c, "projection");
    if (proj == "orthographic" || MjcBoolAttr(Attr(c, "orthographic", "false")))
      cam["orthographic"] = true;
    cameras->push_back(std::move(cam));
  }
  for (const auto& child : Children(frame_node, "body")) {
    const std::vector<double> body_world =
        MultiplyMatrix(frame_world, PoseMatrix(child, ctx));
    CollectMujocoLightsCamerasJson(child, body_world, ctx, lights, cameras);
  }
}

// MJCF <equality> -> JSON entries consumed by AddMjcEqualityFromJson.
void AddMujocoEqualityJson(const pugi::xml_node& root, Json* equalities,
                           Stats* stats) {
  if (!equalities) return;
  const auto eq_root = Child(root, "equality");
  if (!eq_root) return;
  for (const auto& e_node : eq_root.children()) {
    const std::string kind = e_node.name();  // connect | weld | joint
    if (kind != "connect" && kind != "weld" && kind != "joint") {
      if (!UnsupportedXML(e_node, "Unsupported MJCF equality: " + kind,
              "mjcf.equality_unsupported", "mjcf.equality", active_reader->root_source)) return;
      continue;
    }
    Json eq = Json::object();
    eq["name"] =
        Attr(e_node, "name",
             kind + "_" + std::to_string(stats ? stats->equalities : 0));
    eq["type"] = kind;
    if (kind == "joint") {
      eq["joint1"] = Attr(e_node, "joint1");
      if (HasAttr(e_node, "joint2")) eq["joint2"] = Attr(e_node, "joint2");
      const auto poly = ParseDoubles(Attr(e_node, "polycoef"));
      if (!poly.empty()) eq["polycoef"] = Array(poly);
    } else {
      eq["body1"] = Attr(e_node, "body1");
      if (HasAttr(e_node, "body2")) eq["body2"] = Attr(e_node, "body2");
      const auto anchor = ParseDoubles(Attr(e_node, "anchor"));
      if (!anchor.empty()) eq["anchor"] = Array(anchor);
      if (kind == "weld" && HasAttr(e_node, "torquescale"))
        eq["torquescale"] = ParseDoubleAttr(e_node, "torquescale", 1.0);
    }
    if (kind == "weld") {
      const auto relpose = ParseDoubles(Attr(e_node, "relpose"));
      if (relpose.size() == 7) eq["relpose"] = Array(relpose);
    }
    // Site form (MuJoCo 3.x): each side's frame is its site. The converter
    // resolves sites to their owning bodies; legacy output cannot express it.
    if (HasAttr(e_node, "site1") || HasAttr(e_node, "site2")) {
      if (active_reader->options.profile == "legacy") {
        if (!UnsupportedXML(e_node, "MJCF site-based equality constraints are not converted",
                            "mjcf.equality_site_unsupported", "mjcf.equality_site",
                            active_reader->root_source))
          return;
        continue;
      }
      eq["site1"] = Attr(e_node, "site1");
      eq["site2"] = Attr(e_node, "site2");
    }
    if (HasAttr(e_node, "active")) eq["active"] = MjcBoolAttr(Attr(e_node, "active"));
    const auto solref = ParseDoubles(Attr(e_node, "solref"));
    if (!solref.empty()) eq["solref"] = Array(solref);
    const auto solimp = ParseDoubles(Attr(e_node, "solimp"));
    if (!solimp.empty()) eq["solimp"] = Array(solimp);
    equalities->push_back(std::move(eq));
    if (stats) stats->equalities++;
  }
}

// MJCF <contact><exclude body1 body2> -> filteredPairs entries (maps to
// PhysicsFilteredPairsAPI in the converter, which already handles this array).
void AddMujocoContactExcludesJson(const pugi::xml_node& root,
                                  Json* filtered_pairs, Stats* stats) {
  if (!filtered_pairs) return;
  const auto contact_root = Child(root, "contact");
  if (!contact_root) return;
  for (const auto& c_node : Children(contact_root, "exclude")) {
    const std::string b1 = Attr(c_node, "body1");
    const std::string b2 = Attr(c_node, "body2");
    if (b1.empty() || b2.empty()) continue;
    // MJCF's "world" body is exported as the static link holding the
    // <worldbody> geoms.
    auto body = [](const std::string& name) {
      return name == "world" ? active_world_name + "_geoms" : name;
    };
    filtered_pairs->push_back({{"body1", body(b1)}, {"body2", body(b2)}});
    if (stats) stats->contact_excludes++;
  }
}

// MJCF <contact><pair> -> JSON entries consumed by AddContactPairFromJson. An
// explicit collision pair between two geoms with its own friction/solver
// params.
void AddMujocoContactPairsJson(const pugi::xml_node& root, Json* pairs,
                               Stats* stats) {
  if (!pairs) return;
  const auto contact_root = Child(root, "contact");
  if (!contact_root) return;
  for (const auto& p : Children(contact_root, "pair")) {
    const std::string g1 = Attr(p, "geom1");
    const std::string g2 = Attr(p, "geom2");
    if (g1.empty() || g2.empty()) continue;
    Json pr = {{"geom1", g1}, {"geom2", g2}};
    pr["name"] = Attr(p, "name", "pair_" + std::to_string(pairs->size()));
    if (HasAttr(p, "condim"))
      pr["condim"] = ToInt32(ParseDoubleAttr(p, "condim", 3.0), "condim");
    if (HasAttr(p, "margin")) pr["margin"] = ParseDoubleAttr(p, "margin", 0.0);
    if (HasAttr(p, "gap")) pr["gap"] = ParseDoubleAttr(p, "gap", 0.0);
    const auto friction = ParseDoubles(Attr(p, "friction"));
    if (!friction.empty()) pr["friction"] = Array(friction);
    const auto solref = ParseDoubles(Attr(p, "solref"));
    if (!solref.empty()) pr["solref"] = Array(solref);
    const auto solimp = ParseDoubles(Attr(p, "solimp"));
    if (!solimp.empty()) pr["solimp"] = Array(solimp);
    pairs->push_back(std::move(pr));
    if (stats) stats->contact_pairs++;
  }
}

// Normalize a MuJoCo boolean/flag attribute: <flag x="enable|disable"> and
// <compiler x="true|false"> both occur; map enable/true/1 -> true.
bool MjcBoolAttr(const std::string& v) {
  return v == "true" || v == "enable" || v == "1";
}

// Read the full <option>/<option><flag>/<compiler> attribute set into a
// `mjcScene` JSON block (MJCF attr names) consumed by the converter's
// ApplyMjcSceneOptions -> MjcSceneAPI. (The MjcSceneAPI schema + USDC
// round-trip already support all of these; only timestep was emitted before.)
Json BuildMjcSceneJson(const pugi::xml_node& root) {
  Json ms = Json::object();
  if (const auto option = Child(root, "option")) {
    Json opt = Json::object();
    auto num = [&](const char* k) {
      if (HasAttr(option, k)) opt[k] = ParseDoubleAttr(option, k, 0.0);
    };
    auto inum = [&](const char* k) {
      if (HasAttr(option, k))
        opt[k] = ToInt32(ParseDoubleAttr(option, k, 0.0), k);
    };
    auto tok = [&](const char* k) {
      if (HasAttr(option, k)) {
        const auto authored = Attr(option, k);
        const auto canonical = ToLower(authored);
        opt[k] = canonical;
        if (authored != canonical && active_reader->options.report)
          active_reader->options.report->Diagnostic("info", "mjcf.token_canonicalized",
              std::string(k) + ": " + authored + " -> " + canonical, active_reader->root_source);
      }
    };
    auto vec = [&](const char* k) {
      auto v = ParseDoubles(Attr(option, k));
      if (!v.empty()) opt[k] = Array(v);
    };
    num("timestep");
    num("impratio");
    num("density");
    num("viscosity");
    num("o_margin");
    num("tolerance");
    num("ls_tolerance");
    num("noslip_tolerance");
    num("ccd_tolerance");
    inum("iterations");
    inum("ls_iterations");
    inum("noslip_iterations");
    inum("ccd_iterations");
    inum("sdf_iterations");
    inum("sdf_initpoints");
    tok("integrator");
    tok("cone");
    tok("jacobian");
    tok("solver");
    vec("wind");
    vec("magnetic");
    vec("o_solref");
    vec("o_solimp");
    vec("o_friction");
    if (HasAttr(option, "actuatorgroupdisable")) {
      // Disabled actuator groups, MuJoCo's 0..30 bit range.
      Json groups = Json::array();
      for (double g : ParseDoubles(Attr(option, "actuatorgroupdisable")))
        if (g >= 0 && g <= 30 && g == std::floor(g)) groups.push_back(static_cast<int>(g));
      opt["actuatorgroupdisable"] = std::move(groups);
    }
    if (!opt.empty()) ms["option"] = std::move(opt);
    if (const auto flag = Child(option, "flag")) {
      Json fl = Json::object();
      static const char* kFlags[] = {
          "constraint",   "equality",  "frictionloss", "limit",
          "contact",      "gravity",   "clampctrl",    "warmstart",
          "filterparent", "actuation", "refsafe",      "sensor",
          "midphase",     "nativeccd", "eulerdamp",    "autoreset",
          "island",       "override",  "energy",       "fwdinv",
          "invdiscrete",  "multiccd",     "spring",    "damper"};
      for (const char* k : kFlags)
        if (HasAttr(flag, k)) fl[k] = MjcBoolAttr(Attr(flag, k));
      if (!fl.empty()) ms["flag"] = std::move(fl);
    }
  }
  if (const auto compiler = Child(root, "compiler")) {
    Json comp = Json::object();
    auto num = [&](const char* k) {
      if (HasAttr(compiler, k)) comp[k] = ParseDoubleAttr(compiler, k, 0.0);
    };
    auto tok = [&](const char* k) {
      if (HasAttr(compiler, k)) comp[k] = Attr(compiler, k);
    };
    auto boolean = [&](const char* k) {
      if (HasAttr(compiler, k)) comp[k] = MjcBoolAttr(Attr(compiler, k));
    };
    boolean("autolimits");
    num("boundmass");
    num("boundinertia");
    num("settotalmass");
    boolean("usethread");
    boolean("balanceinertia");
    tok("angle");
    boolean("fitaabb");
    boolean("fusestatic");
    tok("inertiafromgeom");
    boolean("alignfree");
    boolean("saveinertial");
    const auto igr = ParseDoubles(Attr(compiler, "inertiagrouprange"));
    if (igr.size() >= 2) {
      comp["inertiagrouprange_min"] = ToInt32(igr[0], "inertiagrouprange");
      comp["inertiagrouprange_max"] = ToInt32(igr[1], "inertiagrouprange");
    }
    if (!comp.empty()) ms["compiler"] = std::move(comp);
  }
  return ms;
}

// Generate a builtin texture as PNG into the reader's texture cache under
// "textures/builtin_<name>.png": type="2d" as MuJoCo's Builtin2D, and (with
// `skybox`) a type="skybox" cube resampled to a latitude-longitude image for
// a DomeLight. Cube textures on materials have no UsdPreviewSurface mapping
// and are reported instead; the legacy profile keeps its historical output.
bool GenerateBuiltinTexture(const pugi::xml_node& node, const std::string& name, bool skybox,
                            std::string* key, std::string* why) {
  constexpr int64_t kMaxBuiltinPixels = int64_t(4096) * 4096;
  Reader& reader = *active_reader;
  if (reader.options.profile == "legacy") {
    *why = "legacy profile";
    return false;
  }
  const std::string type = HasAttr(node, "type") ? Attr(node, "type") : "cube";
  if (type != "2d" && !(skybox && type == "skybox")) {
    *why = "type=\"" + type + "\" is not supported";
    return false;
  }
  BuiltinTexture t;
  t.builtin = Attr(node, "builtin");
  if (t.builtin != "checker" && t.builtin != "gradient" && t.builtin != "flat") {
    *why = "unknown builtin \"" + t.builtin + "\"";
    return false;
  }
  t.mark = HasAttr(node, "mark") ? Attr(node, "mark") : "none";
  const auto color = [&](const char* attr, double* out) {
    const auto v = ParseDoubles(Attr(node, attr));
    if (v.size() >= 3) std::copy(v.begin(), v.begin() + 3, out);
  };
  color("rgb1", t.rgb1);
  color("rgb2", t.rgb2);
  color("markrgb", t.markrgb);
  t.random = ParseDoubleAttr(node, "random", 0.01);
  const bool cube = type != "2d";
  const double w = ParseDoubleAttr(node, "width", 0);
  // A cube texture is width x 6*width; MuJoCo ignores its height.
  const double h = cube ? 6 * w : ParseDoubleAttr(node, "height", 0);
  if (!(w >= 1 && h >= 1) || w * h > double(kMaxBuiltinPixels) || w != std::floor(w) ||
      h != std::floor(h)) {
    *why = "width/height must be positive integers with at most 4096x4096 pixels";
    return false;
  }
  t.width = static_cast<int>(w);
  t.height = static_cast<int>(h);
  // Latitude-longitude output: 4w x 2w, at most 4096 x 2048.
  const int lat_w = cube ? std::min(4 * t.width, 4096) : t.width;
  const int lat_h = cube ? std::max(1, lat_w / 2) : t.height;
  std::string err;
  const size_t pixels = size_t(t.width) * t.height * 3 + (cube ? size_t(lat_w) * lat_h * 3 : 0);
  if (!reader.Charge(pixels, &err)) {
    *why = err;
    return false;
  }
  std::vector<uint8_t> rgb, png;
  if (cube) {
    std::vector<uint8_t> faces;
    GenerateBuiltinCube(t, &faces);
    CubeToLatLong(faces, t.width, lat_w, lat_h, &rgb);
  } else {
    GenerateBuiltin2D(t, &rgb);
  }
  EncodePNG(rgb.data(), uint32_t(lat_w), uint32_t(lat_h), 3, &png);
  if (!reader.Charge(png.size(), &err)) {
    *why = err;
    return false;
  }
  std::string file = name.empty() ? (cube ? "skybox" : "builtin") : name;
  for (char& c : file)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
  *key = "textures/builtin_" + file + ".png";
  for (int k = 1; reader.textures.count(*key); ++k)
    *key = "textures/builtin_" + file + "_" + std::to_string(k) + ".png";
  reader.textures[*key] = std::make_shared<const std::vector<uint8_t>>(std::move(png));
  return true;
}

// AddMaterialFromJson -> UsdShade Material (UsdPreviewSurface).
// Color/PBR-scalar only; texture maps are a documented follow-on.
// MJCF <asset><material> -> JSON material entries consumed by the converter's
// AddMaterialFromJson -> UsdShade Material (UsdPreviewSurface). The first
// builtin skybox texture becomes `skybox` (a DomeLight in the converter).
void AddMujocoMaterialsJson(const pugi::xml_node& root,
                            const fs::path& base_dir, Json* materials,
                            Json* skybox, Stats* stats) {
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
  std::set<std::string> generated, referenced;
  for (const auto& m_node : Children(asset, "material"))
    if (HasAttr(m_node, "texture")) referenced.insert(Attr(m_node, "texture"));
  for (const auto& t_node : Children(asset, "texture")) {
    const std::string tname = Attr(t_node, "name");
    const std::string tfile = Attr(t_node, "file");
    if (tfile.empty() && HasAttr(t_node, "builtin") && Attr(t_node, "builtin") != "none" &&
        active_reader) {
      // Material textures and the first skybox (MuJoCo renders only that
      // one) map onto USD; unused textures are skipped without pixels.
      const bool is_skybox = Attr(t_node, "type") == "skybox";
      if (!referenced.count(tname) && !is_skybox) continue;
      if (is_skybox && skybox && !skybox->empty()) continue;
      std::string key, why;
      if (GenerateBuiltinTexture(t_node, tname, is_skybox && skybox, &key, &why)) {
        if (is_skybox && skybox) {
          *skybox = {{"name", tname.empty() ? std::string("skybox") : tname},
                     {"texture", key}, {"texture_abs", key}, {"texture_generated", true}};
        } else {
          tex_files[tname] = key;
          generated.insert(key);
        }
      } else {
        active_reader->Warn("MJCF builtin texture" +
                                (tname.empty() ? std::string() : " `" + tname + "`") +
                                " is not generated (" + why +
                                (is_skybox ? ")" : "); materials using it keep their rgba"),
                            "mjcf.texture_builtin_unsupported");
      }
      continue;
    }
    if (!tfile.empty() && Attr(t_node, "type") == "skybox" && skybox && skybox->empty() &&
        active_reader && active_reader->options.profile != "legacy") {
      active_reader->Warn("MJCF skybox texture" +
                              (tname.empty() ? std::string() : " `" + tname + "`") +
                              " is not converted (file-based skyboxes need a cube layout)",
                          "mjcf.texture_builtin_unsupported");
      continue;
    }
    if (tname.empty() || tfile.empty()) continue;
    tex_files[tname] =
        ResolveMeshPath(base_dir, tex_dir, tfile, index).string();
  }
  for (const auto& m_node : Children(asset, "material")) {
    const std::string name = Attr(m_node, "name");
    if (name.empty()) continue;
    Json mat = {{"name", name}};
    const bool legacy = !active_reader || active_reader->options.profile == "legacy";
    auto rgba = ParseDoubles(Attr(m_node, "rgba"));
    if (!legacy) {
      // As mujoco-usd-converter: MuJoCo's material color (default white) is
      // sRGB, UsdPreviewSurface's diffuseColor linear; roughness is
      // 1 - shininess (default 0.5) unless set explicitly.
      if (rgba.size() < 4) rgba = {1.0, 1.0, 1.0, 1.0};
      for (size_t i = 0; i < 3; ++i) {
        const double c = rgba[i];
        rgba[i] = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
      }
      if (!HasAttr(m_node, "roughness"))
        mat["roughness"] = 1.0 - ParseDoubleAttr(m_node, "shininess", 0.5);
    }
    if (rgba.size() >= 4)
      mat["rgba"] = Json::array({rgba[0], rgba[1], rgba[2], rgba[3]});
    if (HasAttr(m_node, "metallic"))
      mat["metallic"] = ParseDoubleAttr(m_node, "metallic", 0.0);
    if (HasAttr(m_node, "roughness"))
      mat["roughness"] = ParseDoubleAttr(m_node, "roughness", 0.5);
    if (HasAttr(m_node, "specular"))
      mat["specular"] = ParseDoubleAttr(m_node, "specular", 0.0);
    if (HasAttr(m_node, "emission"))
      mat["emission"] = ParseDoubleAttr(m_node, "emission", 0.0);
    if (HasAttr(m_node, "reflectance"))
      mat["reflectance"] = ParseDoubleAttr(m_node, "reflectance", 0.0);
    const auto repeat = ParseDoubles(Attr(m_node, "texrepeat"));
    if (repeat.size() >= 2 && (repeat[0] != 1.0 || repeat[1] != 1.0))
      mat["texrepeat"] = Json::array({repeat[0], repeat[1]});
    const std::string tref = Attr(m_node, "texture");
    if (!tref.empty()) {
      const auto it = tex_files.find(tref);
      if (it != tex_files.end() && generated.count(it->second)) {
        // Retained in the reader's texture cache; written beside the output
        // layer on save and packaged into USDZ.
        mat["texture"] = it->second;
        mat["texture_abs"] = it->second;
        mat["texture_generated"] = true;
      } else if (it != tex_files.end()) {
        if (active_reader && (active_reader->options.read || active_reader->options.open)) {
          // Resolver identifiers are not filesystem paths. Keep the authored
          // texture identifier anchored to the source instead of absolutizing
          // a URI against the process working directory.
          mat["texture"] = it->second;
          mat["texture_abs"] = it->second;
          materials->push_back(std::move(mat));
          if (stats) stats->materials++;
          continue;
        }
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
void AddMujocoKeyframesJson(const pugi::xml_node& root, Json* keyframes,
                            Stats* stats) {
  if (!keyframes) return;
  const auto kf_root = Child(root, "keyframe");
  if (!kf_root) return;
  for (const auto& key : Children(kf_root, "key")) {
    Json k = Json::object();
    k["name"] = Attr(key, "name", "key_" + std::to_string(keyframes->size()));
    auto arr = [&](const char* attr) {
      auto v = ParseDoubles(Attr(key, attr));
      if (!v.empty()) k[attr] = Array(v);
    };
    arr("qpos");
    arr("qvel");
    arr("act");
    arr("ctrl");
    arr("mpos");
    arr("mquat");
    if (HasAttr(key, "time")) k["time"] = ParseDoubleAttr(key, "time", 0.0);
    keyframes->push_back(std::move(k));
    if (stats) stats->keyframes++;
  }
}

// Bake one <geom> into a link's "visuals"/"collisions" arrays. Shared by the
// per-body traversal and the worldbody-level (static) geom collection.
// --- MuJoCo inertia inference (compiler inertiafromgeom) -------------------
// Ports mjCGeom::GetVolume/SetInertia and mjCMesh::ComputeVolume/Inertia so a
// body without <inertial> gets the mass properties MuJoCo's compiler derives
// from its geoms (inertiagrouprange, density default 1000, explicit mass).

// Rotation R (column vectors) and translation of a MatrixFromPosQuat matrix:
// p' = R p + t with R[i][j] = m[j * 4 + i].
void FrameRotation(const std::vector<double>& m, double r[3][3]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i][j] = m[j * 4 + i];
}

void RotateInertia(const double r[3][3], const double local[3], double out[3][3]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double sum = 0.0;
      for (int k = 0; k < 3; ++k) sum += r[i][k] * local[k] * r[j][k];
      out[i][j] = sum;
    }
}

// Analytic volume/area and principal inertia per unit mass for primitives.
bool PrimitiveMassProperties(const std::string& type, const std::array<double, 3>& size,
                             bool shell, double* volume, double unit[3]) {
  constexpr double pi = 3.14159265358979323846;
  const double r = size[0];
  if (type == "sphere") {
    *volume = shell ? 4 * pi * r * r : 4 * pi * r * r * r / 3;
    unit[0] = unit[1] = unit[2] = (shell ? 2.0 / 3.0 : 2.0 / 5.0) * r * r;
  } else if (type == "capsule") {
    const double h = 2 * size[1];
    if (!shell) {
      *volume = pi * (r * r * h + 4 * r * r * r / 3);
      const double sphere = 4 * r / (4 * r + 3 * h), cylinder = 1 - sphere;
      unit[0] = unit[1] = cylinder * (3 * r * r + h * h) / 12 + 2 * sphere * r * r / 5 +
                          sphere * h * (3 * r + 2 * h) / 8;
      unit[2] = cylinder * r * r / 2 + 2 * sphere * r * r / 5;
    } else {
      const double as = 4 * pi * r * r, ac = 2 * pi * r * h;
      *volume = as + ac;
      const double sphere = as / (as + ac), cylinder = 1 - sphere;
      const double hs_com = r / 2, hs_pos = h / 2 + hs_com;
      unit[0] = unit[1] = cylinder * (6 * r * r + h * h) / 12 + 2 * sphere * r * r / 3 +
                          sphere * (hs_pos * hs_pos - hs_com * hs_com);
      unit[2] = cylinder * r * r + 2 * sphere * r * r / 3;
    }
  } else if (type == "cylinder") {
    const double h = 2 * size[1];
    if (!shell) {
      *volume = pi * r * r * h;
      unit[0] = unit[1] = (3 * r * r + h * h) / 12;
      unit[2] = r * r / 2;
    } else {
      const double ad = pi * r * r, ac = 2 * pi * r * h;
      *volume = 2 * ad + ac;
      const double disk = ad / *volume, cylinder = 1 - 2 * disk;
      unit[0] = unit[1] = cylinder * (6 * r * r + h * h) / 12 +
                          2 * (disk * r * r / 4 + disk * h * h / 4);
      unit[2] = cylinder * r * r + 2 * disk * r * r / 2;
    }
  } else if (type == "box" && !shell) {
    *volume = 8 * size[0] * size[1] * size[2];
    unit[0] = (size[1] * size[1] + size[2] * size[2]) / 3;
    unit[1] = (size[0] * size[0] + size[2] * size[2]) / 3;
    unit[2] = (size[0] * size[0] + size[1] * size[1]) / 3;
  } else if (type == "ellipsoid" && !shell) {
    *volume = 4 * pi * size[0] * size[1] * size[2] / 3;
    unit[0] = (size[1] * size[1] + size[2] * size[2]) / 5;
    unit[1] = (size[0] * size[0] + size[2] * size[2]) / 5;
    unit[2] = (size[0] * size[0] + size[1] * size[1]) / 5;
  } else {
    return false;  // shell boxes/ellipsoids, planes, hfields, sdf
  }
  return true;
}

// Volume (or area for shell), COM and inertia per unit density of a triangle
// mesh already transformed into the body frame (mjCMesh::Process).
bool MeshMassProperties(const std::vector<std::array<double, 3>>& v,
                        const std::vector<int32_t>& tri, const std::string& mode,
                        double* volume, std::array<double, 3>* com, double inert[3][3]) {
  const bool legacy = mode == "legacy", shell = mode == "shell";
  auto triangle = [&](size_t f, double normal[3], double center[3]) {
    const auto &a = v[tri[3 * f]], &b = v[tri[3 * f + 1]], &c = v[tri[3 * f + 2]];
    const double e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const double e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                   e1[0] * e2[1] - e1[1] * e2[0]};
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int i = 0; i < 3; ++i) {
      normal[i] = len > 0 ? n[i] / len : 0;
      center[i] = (a[i] + b[i] + c[i]) / 3;
    }
    return len / 2;
  };
  const size_t faces = tri.size() / 3;
  double facecen[3] = {0, 0, 0}, area = 0;
  for (size_t f = 0; f < faces; ++f) {
    double n[3], c[3];
    const double a = triangle(f, n, c);
    for (int i = 0; i < 3; ++i) facecen[i] += a * c[i];
    area += a;
  }
  if (!(area > 1e-15)) return false;
  for (double& x : facecen) x /= area;
  double total = 0, cm[3] = {0, 0, 0};
  for (size_t f = 0; f < faces; ++f) {
    double n[3], c[3];
    const double a = triangle(f, n, c);
    double w = a;
    if (!shell) {
      const double d[3] = {c[0] - facecen[0], c[1] - facecen[1], c[2] - facecen[2]};
      w = (d[0] * n[0] + d[1] * n[1] + d[2] * n[2]) * a / 3;
      if (legacy) w = std::abs(w);
    }
    total += w;
    for (int i = 0; i < 3; ++i) cm[i] += w * (c[i] * 0.75 + facecen[i] * 0.25);
  }
  if (!(total > 1e-15)) return false;
  for (int i = 0; i < 3; ++i) (*com)[i] = cm[i] / total;
  // Products of inertia about the COM (mjCMesh::ComputeInertia).
  const int k[6][2] = {{0, 0}, {1, 1}, {2, 2}, {0, 1}, {0, 2}, {1, 2}};
  double P[6] = {0, 0, 0, 0, 0, 0}, measure = 0;
  for (size_t f = 0; f < faces; ++f) {
    std::array<double, 3> q[3];
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 3; ++i) q[j][i] = v[tri[3 * f + j]][i] - (*com)[i];
    double n[3], c[3];
    const double e1[3] = {q[1][0] - q[0][0], q[1][1] - q[0][1], q[1][2] - q[0][2]};
    const double e2[3] = {q[2][0] - q[0][0], q[2][1] - q[0][1], q[2][2] - q[0][2]};
    double nn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0]};
    const double len = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
    const double a = len / 2;
    for (int i = 0; i < 3; ++i) {
      n[i] = len > 0 ? nn[i] / len : 0;
      c[i] = (q[0][i] + q[1][i] + q[2][i]) / 3;
    }
    double w = shell ? a : (c[0] * n[0] + c[1] * n[1] + c[2] * n[2]) * a / 3;
    if (legacy) w = std::abs(w);
    measure += w;
    const double C = shell ? 12 : 20;
    for (int j = 0; j < 6; ++j) {
      const int x = k[j][0], y = k[j][1];
      P[j] += w / C *
              (2 * (q[0][x] * q[0][y] + q[1][x] * q[1][y] + q[2][x] * q[2][y]) +
               q[0][x] * q[1][y] + q[0][y] * q[1][x] + q[0][x] * q[2][y] +
               q[0][y] * q[2][x] + q[1][x] * q[2][y] + q[1][y] * q[2][x]);
    }
  }
  *volume = measure;
  inert[0][0] = P[1] + P[2];
  inert[1][1] = P[0] + P[2];
  inert[2][2] = P[0] + P[1];
  inert[0][1] = inert[1][0] = -P[3];
  inert[0][2] = inert[2][0] = -P[4];
  inert[1][2] = inert[2][1] = -P[5];
  return measure > 1e-15;
}

// Mass properties of one geom in the body frame, or false when it contributes
// nothing (zero density/mass, unsupported shape).
bool GeomMassProperties(const pugi::xml_node& geom_node, const AttrMap& cls,
                        const Context& ctx, const std::map<std::string, MeshAsset>& assets,
                        const MeshPayload* mesh_payload, GeomMass* out) {
  const std::string type = HasEff(geom_node, cls, "type") ? Eff(geom_node, cls, "type")
                           : HasEff(geom_node, cls, "mesh") ? "mesh" : "sphere";
  const bool has_mass = HasEff(geom_node, cls, "mass");
  const double mass_attr = EffDouble(geom_node, cls, "mass", 0.0);
  const double density = EffDouble(geom_node, cls, "density", 1000.0);
  const bool shell = MjcBoolAttr(Eff(geom_node, cls, "shellinertia", "false"));
  if ((has_mass && mass_attr <= 0) || (!has_mass && density <= 0)) return false;
  double volume = 0;
  if (type == "mesh") {
    if (!mesh_payload || mesh_payload->mesh.positions.empty()) return false;
    const auto it = assets.find(Eff(geom_node, cls, "mesh"));
    const std::string mode = it != assets.end() ? it->second.inertia : "legacy";
    const auto& m = mesh_payload->matrix;
    const auto& pos = mesh_payload->mesh.positions;
    std::vector<std::array<double, 3>> v(pos.size() / 3);
    for (size_t i = 0; i < v.size(); ++i) {
      const double x = pos[3 * i], y = pos[3 * i + 1], z = pos[3 * i + 2];
      v[i] = {{m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
               m[2] * x + m[6] * y + m[10] * z + m[14]}};
    }
    std::vector<int32_t> tri;
    if (!MeshTriangles(mesh_payload->mesh, &tri)) return false;
    for (int32_t i : tri)
      if (i < 0 || size_t(i) >= v.size()) return false;
    // Left-handed scaling reverses the triangle orientation (MuJoCo flips
    // faces); keep exact volumes positive.
    const double det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
                       m[8] * (m[1] * m[6] - m[5] * m[2]);
    if (det < 0)
      for (size_t f = 0; f + 2 < tri.size(); f += 3) std::swap(tri[f + 1], tri[f + 2]);
    double unit[3][3];
    if (!MeshMassProperties(v, tri, mode == "shell" || shell ? "shell" : mode, &volume,
                            &out->com, unit))
      return false;
    if (mode == "convex" && active_reader)
      active_reader->Warn("MJCF convex mesh inertia approximated with the exact mesh volume",
                          "mjcf.inertia_convex_approximated");
    out->mass = has_mass ? mass_attr : density * volume;
    const double scale = out->mass / volume;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) out->inertia[i][j] = unit[i][j] * scale;
    return out->mass > 1e-15;
  }
  // Primitive frame: geom pos/orientation, or the fromto segment.
  std::array<double, 3> size{{0.05, 0.05, 0.05}};
  const auto sizes = ParseDoubles(Eff(geom_node, cls, "size"));
  for (size_t i = 0; i < 3 && i < sizes.size(); ++i) size[i] = sizes[i];
  if (type == "capsule" || type == "cylinder") {
    if (sizes.size() < 2) size[1] = size[0];
  } else if (type == "sphere") {
  } else if (type != "box" && type != "ellipsoid") {
    return false;
  }
  std::vector<double> frame = MatrixFromPosQuat(
      ParseDouble3(Eff(geom_node, cls, "pos"), {{0, 0, 0}}),
      OrientationQuat(Eff(geom_node, cls, "quat"), Eff(geom_node, cls, "axisangle"),
                      Eff(geom_node, cls, "euler"), Eff(geom_node, cls, "xyaxes"),
                      Eff(geom_node, cls, "zaxis"), ctx));
  const auto fromto = ParseDoubles(Eff(geom_node, cls, "fromto"));
  if (HasEff(geom_node, cls, "mesh")) {
    MeshFit fit;
    std::string ignored;
    if (!FitMeshGeom(geom_node, cls, assets, type, &fit, &ignored)) return false;
    size = fit.size;
    frame = MultiplyMatrix(frame, fit.frame);
  } else if (fromto.size() >= 6 && type != "sphere") {
    const std::array<double, 3> dir{{fromto[3] - fromto[0], fromto[4] - fromto[1],
                                     fromto[5] - fromto[2]}};
    const double half = 0.5 * std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    size[type == "capsule" || type == "cylinder" ? 1 : 2] = half;
    frame = MatrixFromPosQuat({{0.5 * (fromto[0] + fromto[3]), 0.5 * (fromto[1] + fromto[4]),
                                0.5 * (fromto[2] + fromto[5])}},
                              QuatFromTwoVecs({{0.0, 0.0, 1.0}}, dir));
  }
  double unit[3];
  if (!PrimitiveMassProperties(type, size, shell, &volume, unit) || !(volume > 1e-15))
    return false;
  out->mass = has_mass ? mass_attr : density * volume;
  const double local[3] = {unit[0] * out->mass, unit[1] * out->mass, unit[2] * out->mass};
  double r[3][3];
  FrameRotation(frame, r);
  RotateInertia(r, local, out->inertia);
  out->com = {{frame[12], frame[13], frame[14]}};
  return true;
}

// Combine geom mass properties about the body COM (parallel axis theorem).
Json CombineGeomMasses(const std::vector<GeomMass>& geoms) {
  double mass = 0, com[3] = {0, 0, 0};
  for (const auto& g : geoms) {
    mass += g.mass;
    for (int i = 0; i < 3; ++i) com[i] += g.mass * g.com[i];
  }
  if (!(mass > 1e-15)) return Json::object();
  for (double& c : com) c /= mass;
  double total[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  for (const auto& g : geoms) {
    const double d[3] = {g.com[0] - com[0], g.com[1] - com[1], g.com[2] - com[2]};
    const double d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        total[i][j] += g.inertia[i][j] + g.mass * ((i == j ? d2 : 0.0) - d[i] * d[j]);
  }
  return Json{{"mass", mass},
              {"centerOfMass", Json::array({com[0], com[1], com[2]})},
              {"fullInertia", Json::array({total[0][0], total[1][1], total[2][2], total[0][1],
                                           total[0][2], total[1][2]})},
              {"provenance", "inertiafromgeom"}};
}

bool AppendGeomToLink(const pugi::xml_node& geom_node, const std::string& cc,
                      const std::map<std::string, MeshAsset>& assets,
                      const std::map<std::string, HFieldAsset>& hfields,
                      const Options& opts, const Context& ctx,
                      const std::vector<double>& body_world, Json* link,
                      Stats* stats, std::string* err,
                      bool dual_collider = false) {
  const AttrMap& cls = ResolveGeomClass(geom_node, ctx.defaults, cc);
  std::string cls_name = Attr(geom_node, "class");
  if (cls_name.empty()) cls_name = cc;
  if (opts.profile != "legacy") {
    // MuJoCo semantics: a geom collides iff contype|conaffinity is nonzero;
    // its group only selects visibility (0-2 visible by default). A
    // non-colliding geom outside the visible groups is kept as guide
    // geometry (it still contributes inertia in MuJoCo).
    const int contype = ToInt32(EffDouble(geom_node, cls, "contype", 1.0), "contype");
    const int conaffinity =
        ToInt32(EffDouble(geom_node, cls, "conaffinity", 1.0), "conaffinity");
    const int group = ToInt32(EffDouble(geom_node, cls, "group", 0.0), "group");
    const bool collides = contype != 0 || conaffinity != 0;
    const bool visible = group >= 0 && group < 3;
    const bool weighs = active_geom_masses && group >= ctx.inertia_group_min &&
                        group <= ctx.inertia_group_max;
    if (weighs) {
      // Mesh mass properties need the loaded triangles in the body frame;
      // primitives are analytic.
      MeshPayload mesh_payload;
      bool ignored = true;
      const bool mesh = Eff(geom_node, cls, "type", HasEff(geom_node, cls, "mesh") ? "mesh" : "sphere") == "mesh";
      if (mesh && !BuildGeomPayload(geom_node, cls, cls_name, assets, hfields, opts, ctx,
                                    body_world, &mesh_payload, &ignored, err))
        return false;
      GeomMass mass;
      if (GeomMassProperties(geom_node, cls, ctx, assets, mesh ? &mesh_payload : nullptr, &mass))
        active_geom_masses->push_back(mass);
    }
    const std::string geom_name = Attr(geom_node, "name");
    const bool wrap = !geom_name.empty() && ctx.wrap_geoms.count(geom_name);
    const std::string geom_type =
        Eff(geom_node, cls, "type", HasEff(geom_node, cls, "mesh") ? "mesh" : "sphere");
    // MuJoCo and Newton wrap tendons only around native spheres/cylinders. A
    // colliding wrap geom is referenced through its native collider.
    const bool native_wrap = wrap && !collides && (geom_type == "sphere" || geom_type == "cylinder");
    if (wrap && geom_type != "sphere" && geom_type != "cylinder" &&
        !UnsupportedXML(geom_node, "Tendon wrap geom `" + geom_name + "` must be a sphere or cylinder",
                        "mjcf.tendon_wrap_type", "mjcf.tendon_wrap", active_reader->root_source, true)) {
      if (err) *err = active_reader->failure;
      return false;
    }
    // A visible collider is one prim carrying both roles (as
    // mujoco-usd-converter writes it, and MuJoCo's decoder reads one geom),
    // unless its collider and visual geometry differ (tessellated collision
    // shapes keep a native visual).
    const bool primitive_geom = geom_type != "mesh" && geom_type != "hfield";
    const bool merged = visible && collides && !(primitive_geom && opts.tessellate_collision_shapes);
    if (!merged && (visible || !collides)) {
      MeshPayload payload;
      bool ignored = true;
      // Primitive visuals stay native gprims (as mujoco-usd-converter writes
      // them); meshes and heightfields are triangles.
      const bool primitive = geom_type != "mesh" && geom_type != "hfield";
      if (!BuildGeomPayload(geom_node, cls, cls_name, assets, hfields, opts, ctx,
                            body_world, &payload, &ignored, err, native_wrap, primitive))
        return false;
      Json visual_json = MeshPayloadToJson(payload);
      // MuJoCo's geom color (default 0.5 0.5 0.5 1); a material, when bound,
      // takes precedence in renderers.
      const auto rgba = ParseDoubles(Eff(geom_node, cls, "rgba", "0.5 0.5 0.5 1"));
      if (rgba.size() >= 4) visual_json["rgba"] = Array(rgba);
      if (native_wrap) visual_json["mjcGeom"] = geom_name;
      AddGeomPhysicsAttrs(geom_node, cls, &visual_json);
      const std::string mat = Eff(geom_node, cls, "material");
      if (!mat.empty()) visual_json["material"] = mat;
      if (!visible) visual_json["purpose"] = "guide";
      (*link)["visuals"].push_back(std::move(visual_json));
      if (stats) stats->visuals++;
    }
    if (collides) {
      MeshPayload collider;
      bool ignored = false;
      if (!BuildGeomPayload(geom_node, cls, cls_name, assets, hfields, opts, ctx,
                            body_world, &collider, &ignored, err, true))
        return false;
      Json col = MeshPayloadToJson(collider);
      AddGeomPhysicsAttrs(geom_node, cls, &col);
      // MuJoCo collides every mesh through its convex hull; only a
      // tessellated heightfield keeps exact triangles.
      col["approximation"] = dual_collider && geom_type == "hfield" ? "none" : "convexHull";
      if (wrap) col["mjcGeom"] = geom_name;
      if (merged) {
        col["visual"] = true;
        const auto rgba = ParseDoubles(Eff(geom_node, cls, "rgba", "0.5 0.5 0.5 1"));
        if (rgba.size() >= 4) col["rgba"] = Array(rgba);
        const std::string mat = Eff(geom_node, cls, "material");
        if (!mat.empty()) col["material"] = mat;
        if (stats) stats->visuals++;
      }
      if (geom_type == "mesh") {
        const auto mesh_it = assets.find(Eff(geom_node, cls, "mesh"));
        if (mesh_it != assets.end() && mesh_it->second.maxhullvert > 0) {
          col["newton"]["maxHullVertices"] = mesh_it->second.maxhullvert;
          col["mjc"]["maxhullvert"] = mesh_it->second.maxhullvert;
        }
        // <mesh inertia>: shell is newton:massModel, legacy/exact mjc:inertia,
        // convex the implied default (mujoco-usd-converter 0.8.0).
        if (opts.profile != "legacy")
          col["mjc"]["inertia"] = mesh_it != assets.end() ? mesh_it->second.inertia : "legacy";
      } else if (opts.profile != "legacy" &&
                 MjcBoolAttr(Eff(geom_node, cls, "shellinertia", "false"))) {
        col["newton"]["massModel"] = "shell";
      }
      (*link)["collisions"].push_back(std::move(col));
      if (stats) stats->collisions++;
    }
    return true;
  }
  MeshPayload payload;
  bool visual = true;
  if (!BuildGeomPayload(geom_node, cls, cls_name, assets, hfields, opts, ctx,
                        body_world, &payload, &visual, err)) {
    return false;
  }
  if (visual) {
    Json visual_json = MeshPayloadToJson(payload);
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
    {
      const int contype =
          ToInt32(EffDouble(geom_node, cls, "contype", 1.0), "contype");
      const int conaffinity =
          ToInt32(EffDouble(geom_node, cls, "conaffinity", 1.0), "conaffinity");
      if (contype != 0 || conaffinity != 0) {
        MeshPayload collider = payload;
        const std::string geom_type = Eff(geom_node, cls, "type");
        if (geom_type != "mesh" && geom_type != "hfield" &&
            !HasEff(geom_node, cls, "mesh")) {
          bool ignored = false;
          if (!BuildGeomPayload(geom_node, cls, cls_name, assets, hfields, opts,
                                ctx, body_world, &collider, &ignored, err,
                                true))
            return false;
        }
        Json col = MeshPayloadToJson(collider);
        AddGeomPhysicsAttrs(geom_node, cls, &col);
        col["approximation"] = dual_collider ? "none" : "convexHull";
        (*link)["collisions"].push_back(std::move(col));
        if (stats) stats->collisions++;
      }
    }
  } else {
    Json col = MeshPayloadToJson(payload);
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
// are emitted as both a render mesh and a (triangle) collider via
// dual_collider.
bool AddWorldbodyGeomsLink(const std::vector<pugi::xml_node>& worldbodies,
                           const std::map<std::string, MeshAsset>& assets,
                           const std::map<std::string, HFieldAsset>& hfields,
                           const Options& opts, const Context& ctx, Json* links,
                           Stats* stats, std::string* err) {
  Json link = {{"name", active_world_name + "_geoms"},
               {"static", true},
               {"inertial", {{"mass", 0.0}}},
               {"visuals", Json::array()},
               {"collisions", Json::array()}};
  for (const auto& worldbody : worldbodies) {
    for (const auto& geom_node : Children(worldbody, "geom")) {
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
        active_reader->Warn(gerr.empty() ? "worldbody geom skipped" : gerr);
      }
    }
  }
  if (link["visuals"].empty() && link["collisions"].empty()) return true;
  links->push_back(std::move(link));
  if (stats) stats->links++;
  return true;
}

bool VisitMujocoBody(const pugi::xml_node& body_node,
                     const std::string& parent_name,
                     const std::string& childclass,
                     const std::vector<double>& parent_world,
                     const std::map<std::string, MeshAsset>& assets,
                     const std::map<std::string, HFieldAsset>& hfields,
                     const Options& opts, const Context& ctx, Json* links,
                     Json* joints, Stats* stats, std::string* err) {
  // A body's own `childclass` becomes the default class for its descendant
  // geoms/joints/bodies, unless overridden deeper.
  const std::string cc = HasAttr(body_node, "childclass")
                             ? Attr(body_node, "childclass")
                             : childclass;
  // Accumulate the body-chain world transform: this body's frame relative to
  // its parent, composed onto the parent's world frame.
  if (active_flex_bodies && active_flex_bodies->count(body_node.internal_object())) return true;
  const std::vector<double> body_world =
      MultiplyMatrix(parent_world, PoseMatrix(body_node, ctx));
  const std::string body_name =
      Attr(body_node, "name", "body_" + std::to_string(stats->links));
  active_body_names[body_node.internal_object()] = body_name;
  // A <freejoint> (or <joint type="free">) makes this body a 6-DOF floating
  // base: it is NOT joined to its parent. Mark it so the converter emits a free
  // articulation root (vs a fixed base, which has no such joint).
  bool has_free = static_cast<bool>(Child(body_node, "freejoint"));
  if (!has_free) {
    for (const auto& jn : Children(body_node, "joint")) {
      if (ToLower(Attr(jn, "type")) == "free") {
        has_free = true;
        break;
      }
    }
  }
  Json link = {{"name", body_name},
               {"inertial", InertialToJson(body_node, ctx)},
               {"matrix", Array(body_world)},
               {"visuals", Json::array()},
               {"collisions", Json::array()}};
  if (has_free) link["floating"] = true;
  if (parent_name == active_world_name || has_free)
    link["articulationRoot"] = true;
  // MuJoCo mocap body (driven externally; not simulated). Flag it so the USD
  // link records mjc:mocap.
  if (MjcBoolAttr(Attr(body_node, "mocap", "false"))) link["mocap"] = true;
  if (HasAttr(body_node, "gravcomp"))
    link["gravcomp"] = ParseDoubleAttr(body_node, "gravcomp", 0.0);

  // MuJoCo infers the inertia of a body without <inertial> (or of every body
  // with inertiafromgeom="true") from its geoms. Non-legacy profiles author
  // that compiled result so engines need not recompute it.
  std::vector<GeomMass> geom_masses;
  const bool explicit_inertial = static_cast<bool>(Child(body_node, "inertial"));
  const bool infer = opts.profile != "legacy" && ctx.inertiafromgeom != "false" &&
                     (!explicit_inertial || ctx.inertiafromgeom == "true");
  struct MassScope {
    std::vector<GeomMass>* previous = active_geom_masses;
    explicit MassScope(std::vector<GeomMass>* m) { active_geom_masses = m; }
    ~MassScope() { active_geom_masses = previous; }
  } mass_scope(infer ? &geom_masses : nullptr);
  for (const auto& geom_node : Children(body_node, "geom")) {
    if (!AppendGeomToLink(geom_node, cc, assets, hfields, opts, ctx,
                          IdentityMatrix(), &link, stats, err)) {
      if (opts.allow_missing) {
        active_reader->Warn(err ? *err : "mesh skipped");
        if (err) err->clear();
        continue;
      }
      return false;
    }
  }

  if (infer) {
    Json inferred = CombineGeomMasses(geom_masses);
    if (!inferred.empty()) {
      link["inertial"] = std::move(inferred);
      if (active_reader->options.report) {
        auto& report = *active_reader->options.report;
        auto& count = report.counts["inferred_inertia_bodies"];
        count = (count.is_number() ? count.get<uint64_t>() : uint64_t(0)) + 1;
      }
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

    // Newton merges a stack into one D6 and accepts only hinges and slides
    // there; a stack with a ball keeps the intermediate-link chain.
    bool mergeable = true;
    for (const auto& jn : joint_nodes) {
      const std::string jtype = Eff(jn, ResolveJointClass(jn, ctx.defaults, cc), "type", "hinge");
      mergeable &= jtype == "hinge" || jtype == "slide";
    }
    if (joint_nodes.size() > 1 && active_reader->options.profile != "legacy" &&
        (mergeable || active_reader->options.profile != "newton")) {
      // Stacked joints: every DOF connects parent and body directly, each in
      // the body frame at its own pos, composed in authored order. This is
      // MuJoCo's own description (mujoco-usd-converter, MuJoCo's USD decoder)
      // and Newton merges joints sharing a body pair into one D6 joint. The
      // root records it with newton:jointsAddMobility.
      for (const auto& jn : joint_nodes) {
        const AttrMap& jcls = ResolveJointClass(jn, ctx.defaults, cc);
        AddJointFromNode(jn, jcls, ctx, parent_name, body_name, body_matrix,
                         body_pos, joints);
        stats->joints++;
      }
      if (stacked_joint_bodies) stacked_joint_bodies->insert(body_name);
    } else if (joint_nodes.size() <= 1) {
      // 0 joints -> fixed connection; 1 joint -> single DOF (the common case).
      const pugi::xml_node jn =
          joint_nodes.empty() ? pugi::xml_node() : joint_nodes[0];
      const AttrMap& jcls = ResolveJointClass(jn, ctx.defaults, cc);
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
            last ? body_name
                 : (body_name + "__mjcdof_" + std::to_string(k + 1));
        if (!last) {
          // Emit a massless intermediate link (no geometry, no mass).
          Json dof_link = {{"name", child},
                           {"inertial", {{"mass", 0.0}}},
                           {"matrix", Array(body_world)},
                           {"visuals", Json::array()},
                           {"collisions", Json::array()}};
          links->push_back(std::move(dof_link));
          stats->links++;
        }
        const AttrMap& jcls =
            ResolveJointClass(joint_nodes[k], ctx.defaults, cc);
        AddJointFromNode(joint_nodes[k], jcls, ctx, prev, child,
                         (k == 0) ? body_matrix : identity,
                         (k == 0) ? body_pos : zero_pos, joints);
        stats->joints++;
        prev = child;
      }
    }
  }

  for (const auto& child_body : Children(body_node, "body")) {
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
const std::set<std::string>& AttachRefAttrs() {
  static const std::set<std::string> s = {
      "name",   "class",   "childclass", "material", "mesh",
      "hfield", "texture", "joint",      "joint1",   "joint2",
      "site",   "site1",   "site2",      "refsite",  "sidesite",
      "tendon", "tendon1", "tendon2",    "body",     "body1",
      "body2",  "geom",    "geom1",      "geom2",    "objname"};
  return s;
}

void PrefixAttachSubtree(pugi::xml_node node, const std::string& prefix,
                         double angle_scale) {
  for (pugi::xml_attribute a : node.attributes()) {
    if (AttachRefAttrs().count(a.name())) {
      const std::string v = a.value();
      if (!v.empty()) a.set_value((prefix + v).c_str());
    }
  }
  if (angle_scale != 1.0) {
    const double k = angle_scale;
    if (pugi::xml_attribute e = node.attribute("euler")) {
      const auto v = ParseDoubles(e.value());
      if (v.size() == 3) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%.10g %.10g %.10g", v[0] * k, v[1] * k,
                      v[2] * k);
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
    // Angular joint ranges and reference coordinates use compiler angle units;
    // slide ranges and actuator controls use length/radian units independently.
    const std::string tag = node.name();
    const std::string type = Attr(node, "type", "hinge");
    if (tag == "joint" && (type == "hinge" || type == "ball")) {
      for (const char* key : {"range", "ref", "springref"}) {
        auto attr = node.attribute(key);
        if (!attr) continue;
        std::ostringstream converted;
        converted.precision(17);
        const auto values = ParseDoubles(attr.value());
        for (size_t i = 0; i < values.size(); ++i) {
          if (i) converted << ' ';
          converted << values[i] * k;
        }
        attr.set_value(converted.str().c_str());
      }
    }
  }
  for (pugi::xml_node c : node.children()) {
    PrefixAttachSubtree(c, prefix, angle_scale);
  }
}

void CollectAttachNodes(pugi::xml_node n, std::vector<pugi::xml_node>* out) {
  for (pugi::xml_node c : n.children()) {
    if (std::string(c.name()) == "attach") out->push_back(c);
    CollectAttachNodes(c, out);
  }
}

pugi::xml_node FindBodyByName(pugi::xml_node n, const std::string& name) {
  for (pugi::xml_node b : n.children("body")) {
    if (std::string(b.attribute("name").as_string()) == name) return b;
    if (pugi::xml_node r = FindBodyByName(b, name)) return r;
  }
  return pugi::xml_node();
}

void InlineMJCFDefaults(pugi::xml_node root);

bool ExpandAttachments(pugi::xml_node root, const fs::path& base_dir,
                       std::string* err) {
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
    parent_radian =
        ToLower(comp.attribute("angle").as_string("degree")) == "radian";
  }

  for (pugi::xml_node attach : attaches) {
    const std::string model_name = attach.attribute("model").as_string();
    const std::string body_name = attach.attribute("body").as_string();
    const std::string prefix = attach.attribute("prefix").as_string();
    const auto it = models.find(model_name);
    if (it == models.end()) {
      active_reader->Warn("<attach> references unknown model: " + model_name);
      continue;
    }
    std::string child_xml;
    if (!ReadFile(it->second, &child_xml, err)) return false;
    std::set<fs::path> seen;
    std::string child_expanded;
    if (!ExpandIncludes(child_xml, it->second.parent_path(), &seen,
                        &child_expanded, err, it->second.string())) {
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
      if (child_meshdir.empty())
        child_meshdir = cc.attribute("assetdir").as_string();
      child_radian =
          ToLower(cc.attribute("angle").as_string("degree")) == "radian";
    }
    const double angle_scale = child_radian == parent_radian ? 1.0
                               : child_radian ? 57.295779513082323
                                              : 0.017453292519943295;

    // Make the child's mesh/hfield file paths absolute so they still resolve
    // after the assets are merged into the (differently-rooted) parent model.
    for (pugi::xml_node asset : child_root.children("asset")) {
      for (const char* tag : {"mesh", "hfield", "skin"}) {
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

    InlineMJCFDefaults(child_root);
    PrefixAttachSubtree(child_root, prefix, angle_scale);

    pugi::xml_node found;
    for (pugi::xml_node wb : child_root.children("worldbody")) {
      found = FindBodyByName(wb, prefix + body_name);
      if (found) break;
    }
    if (!found) {
      active_reader->Warn("<attach> body " + body_name +
                          " not found in model " + model_name);
      continue;
    }

    pugi::xml_node parent = attach.parent();
    parent.insert_copy_before(found, attach);
    parent.remove_child(attach);

    // Merge the child's model-level sections (prefixed) into the parent.
    for (const char* sec : {"default", "asset", "tendon", "actuator", "sensor",
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
// (it is NOT a body). MuJoCo 3 uses it to group/offset geoms/bodies. We
// dissolve each frame by composing its transform into every child's pose and
// moving the children up to the frame's parent, so the rest of the pipeline
// never sees a <frame>. Used by apptronik_apollo (frames wrap finger-mesh
// geoms).

void SetNodeAttr(pugi::xml_node n, const char* name, const char* val) {
  pugi::xml_attribute a = n.attribute(name);
  if (a)
    a.set_value(val);
  else
    n.append_attribute(name).set_value(val);
}

// Column-major 4x4 -> translation + quaternion {w,x,y,z}.
void DecomposePosQuat(const std::vector<double>& m, std::array<double, 3>* pos,
                      Quat* q) {
  *pos = {{m[12], m[13], m[14]}};
  const double trace = m[0] + m[5] + m[10];
  double w, x, y, z;
  if (trace > 0.0) {
    const double S = 2.0 * std::sqrt(trace + 1.0);
    w = 0.25 * S;
    x = (m[6] - m[9]) / S;
    y = (m[8] - m[2]) / S;
    z = (m[1] - m[4]) / S;
  } else if (m[0] > m[5] && m[0] > m[10]) {
    const double S = 2.0 * std::sqrt(1.0 + m[0] - m[5] - m[10]);
    w = (m[6] - m[9]) / S;
    x = 0.25 * S;
    y = (m[4] + m[1]) / S;
    z = (m[8] + m[2]) / S;
  } else if (m[5] > m[10]) {
    const double S = 2.0 * std::sqrt(1.0 + m[5] - m[0] - m[10]);
    w = (m[8] - m[2]) / S;
    x = (m[4] + m[1]) / S;
    y = 0.25 * S;
    z = (m[9] + m[6]) / S;
  } else {
    const double S = 2.0 * std::sqrt(1.0 + m[10] - m[0] - m[5]);
    w = (m[1] - m[4]) / S;
    x = (m[8] + m[2]) / S;
    y = (m[9] + m[6]) / S;
    z = 0.25 * S;
  }
  *q = QuatNormalize({{w, x, y, z}});
}

// MJCF <composite type="cable"> expanded into the body chain MuJoCo's
// mjCComposite::MakeCable builds: count-1 bodies along the curve (or the
// explicit vertices), each a capsule/cylinder/box segment on a ball joint (a
// free or no joint for the first per `initial`) in a parallel-transported
// frame, boundary sites S_first/S_last and contact excludes between
// neighbours. Names follow MuJoCo (prefix + B_first, B_<i>, B_last, J_*, G<i>).
// A cable nests one body per segment; bound the chain so body traversal
// stays stack-safe (the XML depth limit is re-checked after expansion).
constexpr int kMaxCableVertices = 512;

bool ExpandCableComposites(pugi::xml_node root, const Context& ctx, std::string* err) {
  auto norm = [](std::array<double, 3>& v) {
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n >= 1e-14 && std::abs(n - 1) > 1e-14) for (double& x : v) x /= n;
    return n;
  };
  auto cross = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return std::array<double, 3>{{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                                  a[0] * b[1] - a[1] * b[0]}};
  };
  auto rotate = [](const Quat& q, const std::array<double, 3>& v) {
    const std::vector<double> m = MatrixFromPosQuat({{0, 0, 0}}, q);
    return std::array<double, 3>{{m[0] * v[0] + m[4] * v[1] + m[8] * v[2],
                                  m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
                                  m[2] * v[0] + m[6] * v[1] + m[10] * v[2]}};
  };
  auto fmt = [](std::initializer_list<double> values) {
    std::string out;
    char buf[40];
    for (double v : values) {
      std::snprintf(buf, sizeof(buf), "%.17g", v);
      if (!out.empty()) out += ' ';
      out += buf;
    }
    return out;
  };
  std::vector<pugi::xml_node> composites;
  std::function<void(pugi::xml_node)> find = [&](pugi::xml_node n) {
    for (pugi::xml_node c : n.children()) {
      if (std::string(c.name()) == "composite") composites.push_back(c);
      else find(c);
    }
  };
  for (pugi::xml_node worldbody : root.children("worldbody")) find(worldbody);
  for (pugi::xml_node comp : composites) {
    const std::string type = ToLower(Attr(comp, "type"));
    if (type != "cable") {
      if (err) *err = "MJCF composite type `" + type + "` is not supported (MuJoCo only builds cables)";
      return false;
    }
    const std::string prefix = Attr(comp, "prefix");
    const pugi::xml_node geom = Child(comp, "geom");
    const std::string geom_type = ToLower(Attr(geom, "type", "sphere"));
    if (geom_type != "capsule" && geom_type != "cylinder" && geom_type != "box") {
      if (err) *err = "MJCF cable composite geom must be a capsule, cylinder or box";
      return false;
    }
    std::vector<double> vert = ParseDoubles(Attr(comp, "vertex"));
    if (vert.empty()) {
      const auto counts = ParseDoubles(Attr(comp, "count"));
      const int count = counts.empty() ? 1 : ToInt32(counts[0], "count");
      // size may list fewer than three values (e.g. size="1").
      std::array<double, 3> size{{0, 0, 0}};
      const auto sizes = ParseDoubles(Attr(comp, "size"));
      for (size_t k = 0; k < 3 && k < sizes.size(); ++k) size[k] = sizes[k];
      std::istringstream curves(Attr(comp, "curve"));
      std::array<std::string, 3> curve{{"0", "0", "0"}};
      for (size_t k = 0; k < 3 && (curves >> curve[k]); ++k) {
        if (curve[k] != "s" && curve[k] != "cos(s)" && curve[k] != "sin(s)" && curve[k] != "0") {
          if (err) *err = "MJCF cable composite curve `" + curve[k] + "` is invalid";
          return false;
        }
      }
      const std::vector<double> q = ParseDoubles(Attr(comp, "quat"));
      const Quat cq = q.size() >= 4 ? QuatNormalize({{q[0], q[1], q[2], q[3]}}) : Quat{{1, 0, 0, 0}};
      if (count < 2 || count > kMaxCableVertices) {
        if (err) *err = "MJCF cable composite needs between 2 and 512 vertices";
        return false;
      }
      for (int ix = 0; ix < count; ++ix) {
        std::array<double, 3> v{};
        for (size_t k = 0; k < 3; ++k) {
          const double t = kPi * ix * size[2] / (count - 1);
          v[k] = curve[k] == "s"      ? ix * size[0] / (count - 1)
                 : curve[k] == "cos(s)" ? size[1] * std::cos(t)
                 : curve[k] == "sin(s)" ? size[1] * std::sin(t)
                                        : 0.0;
        }
        v = rotate(cq, v);
        vert.insert(vert.end(), v.begin(), v.end());
      }
    }
    const int n = static_cast<int>(vert.size() / 3);
    if (n < 2 || n > kMaxCableVertices || vert.size() % 3) {
      if (err) *err = "MJCF cable composite needs at least two vertices";
      return false;
    }
    // mujoco.elasticity.cable: bend torque E*I*(w - w0)/L between segments,
    // twist G*J*(w - w0)/L. A ball-joint spring toward the initial pose with
    // k = E*I/L reproduces bending; twist uses the same isotropic stiffness.
    double bend_modulus = 0.0;
    bool flat = false;
    if (const pugi::xml_node plugin = Child(comp, "plugin")) {
      double twist_modulus = 0.0;
      for (const auto& config : Children(plugin, "config")) {
        const std::string key = Attr(config, "key");
        if (key == "bend") bend_modulus = ParseDoubleAttr(config, "value", 0.0);
        if (key == "twist") twist_modulus = ParseDoubleAttr(config, "value", 0.0);
        if (key == "flat") flat = MjcBoolAttr(Attr(config, "value", "false"));
      }
      const bool exact = bend_modulus > 0 && !flat && twist_modulus == 0.0;
      if (!exact &&
          !UnsupportedXML(comp, bend_modulus > 0
                                    ? "Cable elasticity is approximated by isotropic ball-joint stiffness E*I/L"
                                    : "Cable elasticity plugin has no inline bend config; it is dropped",
                          "mjcf.cable_plugin_approximated", "mjcf.composite",
                          active_reader->root_source, true)) {
        if (err) *err = active_reader->failure;
        return false;
      }
      if (flat) bend_modulus = 0.0;  // a straight rest shape has no joint-spring form
    }
    double second_moment = 0.0;
    {
      const auto gs = ParseDoubles(Attr(geom, "size"));
      if (geom_type == "box") {
        const double h = gs.size() > 1 ? gs[1] : 0.0, w = gs.size() > 2 ? gs[2] : 0.0;
        second_moment = 0.5 * (std::pow(2 * w, 3) * 2 * h + std::pow(2 * h, 3) * 2 * w) / 12.0;
      } else if (!gs.empty()) {
        second_moment = kPi * std::pow(gs[0], 4) / 4.0;
      }
    }
    const auto offset = ParseDouble3(Attr(comp, "offset"), {{0, 0, 0}});
    const std::string initial = Attr(comp, "initial", "ball");
    pugi::xml_node joint_def;
    for (pugi::xml_node j : comp.children("joint"))
      if (Attr(j, "kind", "main") == "main") joint_def = j;
    const pugi::xml_node site_def = Child(comp, "site");
    pugi::xml_node contact = root.child("contact");
    if (!contact) contact = root.append_child("contact");
    auto at = [&](int i) { return std::array<double, 3>{{vert[3 * i], vert[3 * i + 1], vert[3 * i + 2]}}; };
    std::array<double, 3> normal{{0, 1, 0}};
    Quat prev{{1, 0, 0, 0}};
    pugi::xml_node parent = comp.parent();
    pugi::xml_node anchor = comp;
    const int last = n - 2;
    auto body_name = [&](int i) {
      return prefix + (i == 0 ? "B_first" : i == last ? "B_last" : "B_" + std::to_string(i));
    };
    for (int ix = 0; ix <= last; ++ix) {
      const bool first = ix == 0, is_last = ix == last;
      std::array<double, 3> edge, tprev{}, tnext{};
      for (size_t k = 0; k < 3; ++k) edge[k] = at(ix + 1)[k] - at(ix)[k];
      double length_prev = 0;
      if (!first) {
        for (size_t k = 0; k < 3; ++k) tprev[k] = at(ix)[k] - at(ix - 1)[k];
        length_prev = norm(tprev);
      }
      if (!is_last) {
        for (size_t k = 0; k < 3; ++k) tnext[k] = at(ix + 2)[k] - at(ix + 1)[k];
        norm(tnext);
      }
      std::array<double, 3> tangent = edge;
      const double length = norm(tangent);
      std::array<double, 3> binormal;
      if (first) {
        binormal = cross(tangent, tnext);
        norm(binormal);
        normal = cross(binormal, tangent);
        norm(normal);
      } else {
        binormal = cross(tprev, tangent);
        const double bn = norm(binormal);
        const double angle = std::atan2(bn, tprev[0] * tangent[0] + tprev[1] * tangent[1] + tprev[2] * tangent[2]);
        normal = rotate(QuatFromAxisAngle(binormal, angle), normal);
        norm(normal);
        binormal = cross(tangent, normal);
        norm(binormal);
      }
      const Quat quat = QuatFromBasis(tangent, normal, binormal);
      pugi::xml_node body =
          anchor ? parent.insert_child_before("body", anchor) : parent.append_child("body");
      body.append_attribute("name").set_value(body_name(ix).c_str());
      if (first) {
        body.append_attribute("pos").set_value(
            fmt({offset[0] + vert[0], offset[1] + vert[1], offset[2] + vert[2]}).c_str());
        body.append_attribute("quat").set_value(fmt({quat[0], quat[1], quat[2], quat[3]}).c_str());
        if (comp.attribute("childclass")) body.append_attribute("childclass").set_value(Attr(comp, "childclass").c_str());
      } else {
        const Quat d = QuatMul({{prev[0], -prev[1], -prev[2], -prev[3]}}, quat);
        body.append_attribute("pos").set_value(fmt({length_prev, 0, 0}).c_str());
        body.append_attribute("quat").set_value(fmt({d[0], d[1], d[2], d[3]}).c_str());
      }
      if (!first || initial != "none") {
        pugi::xml_node j = body.append_child("joint");
        const bool free = first && initial == "free";
        if (joint_def && !free)
          for (pugi::xml_attribute a : joint_def.attributes())
            if (std::string(a.name()) != "kind") j.append_attribute(a.name()).set_value(a.value());
        j.append_attribute("name").set_value(
            (prefix + (first ? "J_first" : is_last ? "J_last" : "J_" + std::to_string(ix))).c_str());
        if (j.attribute("type")) j.remove_attribute("type");
        j.append_attribute("type").set_value(free ? "free" : "ball");
        if (!first && bend_modulus > 0 && length_prev > 0 && !j.attribute("stiffness")) {
          char buf[40];
          std::snprintf(buf, sizeof(buf), "%.17g", bend_modulus * second_moment / length_prev);
          j.append_attribute("stiffness").set_value(buf);
        }
      }
      pugi::xml_node g = body.append_copy(geom);
      g.remove_attribute("name");
      g.append_attribute("name").set_value((prefix + "G" + std::to_string(ix)).c_str());
      if (geom_type == "box") {
        const auto size = ParseDoubles(Attr(geom, "size"));
        g.remove_attribute("size");
        g.remove_attribute("pos");
        g.append_attribute("pos").set_value(fmt({length / 2, 0, 0}).c_str());
        g.append_attribute("size").set_value(
            fmt({length / 2, size.size() > 1 ? size[1] : 0.0, size.size() > 2 ? size[2] : 0.0}).c_str());
      } else {
        g.remove_attribute("fromto");
        g.append_attribute("fromto").set_value(fmt({0, 0, 0, length, 0, 0}).c_str());
      }
      if (first || is_last) {
        pugi::xml_node site = site_def ? body.append_copy(site_def) : body.append_child("site");
        site.remove_attribute("name");
        site.remove_attribute("pos");
        site.append_attribute("name").set_value((prefix + (is_last ? "S_last" : "S_first")).c_str());
        site.append_attribute("pos").set_value(fmt({is_last ? length : 0.0, 0, 0}).c_str());
      }
      if (!is_last) {
        pugi::xml_node ex = contact.append_child("exclude");
        ex.append_attribute("body1").set_value(body_name(ix).c_str());
        ex.append_attribute("body2").set_value(body_name(ix + 1).c_str());
      }
      prev = quat;
      parent = body;
      anchor = pugi::xml_node();
    }
    comp.parent().remove_child(comp);
  }
  (void)ctx;
  return true;
}

// MJCF <replicate count offset euler sep> as MuJoCo's XML reader expands it:
// copy i sits in a frame at the accumulated offset, rotated by i*euler, and
// every named element gets the suffix sep + i (zero-padded to the digits of
// count; inner replicates append first). Elements elsewhere that reference a
// replicated name (tendons, actuators, sensors, equalities, contacts) are
// replicated and renamed with each copy.
bool ExpandReplicates(pugi::xml_node root, const Context& ctx, std::string* err) {
  static const char* const kRefs[] = {
      "site",   "joint",   "body",    "geom",     "tendon",   "body1",
      "body2",  "joint1",  "joint2",  "site1",    "site2",    "geom1",
      "geom2",  "objname", "refname", "target",   "sidesite", "actuator",
      "jointinparent",     "slidersite",          "cranksite"};
  size_t budget = 1000000;  // total replicated elements
  std::function<bool(pugi::xml_node)> expand = [&](pugi::xml_node parent) -> bool {
    for (pugi::xml_node c = parent.first_child(); c;) {
      pugi::xml_node next = c.next_sibling();
      if (!expand(c)) return false;
      if (std::string(c.name()) != "replicate") {
        c = next;
        continue;
      }
      const int count = ToInt32(ParseDoubleAttr(c, "count", 0.0), "count");
      if (count <= 0) {
        if (err) *err = "MJCF <replicate> count must be positive";
        return false;
      }
      const auto offset = ParseDouble3(Attr(c, "offset"), {{0, 0, 0}});
      const auto euler = ParseDouble3(Attr(c, "euler"), {{0, 0, 0}});
      const std::string sep = Attr(c, "sep");
      std::set<std::string> names;
      size_t nodes = 0;
      std::function<void(pugi::xml_node)> collect = [&](pugi::xml_node n) {
        for (pugi::xml_node d : n.children()) {
          ++nodes;
          if (d.attribute("name")) names.insert(d.attribute("name").as_string());
          collect(d);
        }
      };
      collect(c);
      const size_t digits = std::to_string(count).size();
      auto suffix = [&](int i) {
        std::string s = std::to_string(i);
        return sep + std::string(digits > s.size() ? digits - s.size() : 0, '0') + s;
      };
      std::function<void(pugi::xml_node, const std::string&, bool)> rename =
          [&](pugi::xml_node n, const std::string& sfx, bool own_names) {
            if (own_names)
              if (auto a = n.attribute("name")) a.set_value((std::string(a.value()) + sfx).c_str());
            for (const char* key : kRefs)
              if (auto a = n.attribute(key))
                if (names.count(a.value())) a.set_value((std::string(a.value()) + sfx).c_str());
            for (pugi::xml_node d : n.children()) rename(d, sfx, own_names);
          };
      auto references = [&](pugi::xml_node n) {
        bool found = false;
        std::function<void(pugi::xml_node)> scan = [&](pugi::xml_node m) {
          for (const char* key : kRefs)
            if (auto a = m.attribute(key)) found |= names.count(a.value()) > 0;
          for (pugi::xml_node d : m.children()) scan(d);
        };
        scan(n);
        return found;
      };
      if (nodes * static_cast<size_t>(count) > budget) {
        if (err) *err = "MJCF <replicate> expansion exceeds the element budget";
        return false;
      }
      budget -= nodes * static_cast<size_t>(count);
      std::array<double, 3> pos{{0, 0, 0}};
      for (int i = 0; i < count; ++i) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%.17g %.17g %.17g", i * euler[0], i * euler[1],
                      i * euler[2]);
        const Quat q = OrientationQuat("", "", buf, "", "", ctx);
        pugi::xml_node frame = parent.insert_child_before("frame", c);
        if (c.attribute("childclass"))
          frame.append_attribute("childclass").set_value(c.attribute("childclass").value());
        frame.append_attribute("euler").set_value(buf);
        std::snprintf(buf, sizeof(buf), "%.17g %.17g %.17g", pos[0], pos[1], pos[2]);
        frame.append_attribute("pos").set_value(buf);
        for (pugi::xml_node d : c.children()) rename(frame.append_copy(d), suffix(i), true);
        const std::vector<double> m = MatrixFromPosQuat({{0, 0, 0}}, q);
        for (int k = 0; k < 3; ++k)
          pos[size_t(k)] += m[size_t(k)] * offset[0] + m[size_t(4 + k)] * offset[1] +
                            m[size_t(8 + k)] * offset[2];
      }
      parent.remove_child(c);
      for (const char* section : {"tendon", "actuator", "sensor", "equality", "contact"}) {
        for (pugi::xml_node sec : root.children(section)) {
          for (pugi::xml_node e = sec.first_child(); e;) {
            pugi::xml_node after = e.next_sibling();
            if (e.type() == pugi::node_element && references(e)) {
              for (int i = 0; i < count; ++i) rename(sec.insert_copy_before(e, e), suffix(i), true);
              sec.remove_child(e);
            }
            e = after;
          }
        }
      }
      c = next;
    }
    return true;
  };
  for (pugi::xml_node worldbody : root.children("worldbody"))
    if (!expand(worldbody)) return false;
  return true;
}

void FlattenFramesIn(pugi::xml_node parent, const Context& ctx) {
  // Bottom-up: flatten frames nested inside children before dissolving the
  // frames directly under `parent`.
  for (pugi::xml_node c : parent.children()) FlattenFramesIn(c, ctx);

  pugi::xml_node child = parent.first_child();
  while (child) {
    pugi::xml_node next = child.next_sibling();
    if (std::string(child.name()) == "frame") {
      const std::array<double, 3> fpos =
          ParseDouble3(Attr(child, "pos"), {{0, 0, 0}});
      const Quat fq = OrientationQuat(
          Attr(child, "quat"), Attr(child, "axisangle"), Attr(child, "euler"),
          Attr(child, "xyaxes"), Attr(child, "zaxis"), ctx);
      const std::vector<double> fm = MatrixFromPosQuat(fpos, fq);
      const std::string fcc = Attr(child, "childclass");
      for (pugi::xml_node gc : child.children()) {
        const std::array<double, 3> cpos =
            ParseDouble3(Attr(gc, "pos"), {{0, 0, 0}});
        const Quat cq = OrientationQuat(Attr(gc, "quat"), Attr(gc, "axisangle"),
                                        Attr(gc, "euler"), Attr(gc, "xyaxes"),
                                        Attr(gc, "zaxis"), ctx);
        const std::vector<double> cm =
            MultiplyMatrix(fm, MatrixFromPosQuat(cpos, cq));
        std::array<double, 3> npos;
        Quat nq;
        DecomposePosQuat(cm, &npos, &nq);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%.17g %.17g %.17g", npos[0], npos[1],
                      npos[2]);
        SetNodeAttr(gc, "pos", buf);
        // Joints have an axis and lights a direction instead of an
        // orientation: rotate those vectors into the parent frame.
        const std::string gname = gc.name();
        const char* vector_key = gname == "joint" ? "axis" : gname == "light" ? "dir" : nullptr;
        if (vector_key) {
          const auto v = ParseDouble3(
              Attr(gc, vector_key, gname == "joint" ? "0 0 1" : "0 0 -1"), {{0, 0, 1}});
          const std::array<double, 3> r{{fm[0] * v[0] + fm[4] * v[1] + fm[8] * v[2],
                                         fm[1] * v[0] + fm[5] * v[1] + fm[9] * v[2],
                                         fm[2] * v[0] + fm[6] * v[1] + fm[10] * v[2]}};
          std::snprintf(buf, sizeof(buf), "%.17g %.17g %.17g", r[0], r[1], r[2]);
          SetNodeAttr(gc, vector_key, buf);
        } else {
          std::snprintf(buf, sizeof(buf), "%.17g %.17g %.17g %.17g", nq[0], nq[1],
                        nq[2], nq[3]);
          SetNodeAttr(gc, "quat", buf);
          gc.remove_attribute("euler");
          gc.remove_attribute("axisangle");
          gc.remove_attribute("xyaxes");
          gc.remove_attribute("zaxis");
        }
        // A capsule/cylinder fromto is in the frame's coordinates too.
        if (pugi::xml_attribute ft = gc.attribute("fromto")) {
          const auto v = ParseDoubles(ft.value());
          if (v.size() == 6) {
            auto tp = [&](double a, double b, double c) {
              return std::array<double, 3>{
                  {fm[0] * a + fm[4] * b + fm[8] * c + fm[12],
                   fm[1] * a + fm[5] * b + fm[9] * c + fm[13],
                   fm[2] * a + fm[6] * b + fm[10] * c + fm[14]}};
            };
            const auto p0 = tp(v[0], v[1], v[2]);
            const auto p1 = tp(v[3], v[4], v[5]);
            std::snprintf(buf, sizeof(buf),
                          "%.17g %.17g %.17g %.17g %.17g %.17g", p0[0], p0[1],
                          p0[2], p1[0], p1[1], p1[2]);
            SetNodeAttr(gc, "fromto", buf);
          }
        }
        // Propagate the frame's childclass to children that don't set one.
        if (!fcc.empty() && !gc.attribute("childclass") &&
            !gc.attribute("class")) {
          SetNodeAttr(gc, "childclass", fcc.c_str());
        }
        parent.insert_copy_before(gc, child);
      }
      parent.remove_child(child);
    }
    child = next;
  }
}

bool IsSolverArray(std::string_view name) {
  for (const char* key : {"solimp", "solimplimit", "solimpfriction", "o_solimp",
                          "solref", "solreflimit", "solreffriction", "o_solref"})
    if (name == key) return true;
  return false;
}
std::string CompleteSolverArray(const std::string& name, const std::string& value,
                                const std::string& inherited, const std::string& kind) {
  std::vector<double> defaults;
  if (name == "solimp" || name == "solimplimit" || name == "solimpfriction" || name == "o_solimp")
    defaults = {0.9, 0.95, 0.001, 0.5, 2.0};
  else if (name == "solref" || name == "solreflimit" || name == "solreffriction" || name == "o_solref")
    defaults = name == "solreffriction" && kind == "pair" ? std::vector<double>{0, 0} : std::vector<double>{0.02, 1};
  else return value;
  const auto authored = ParseDoubles(value);
  if (authored.empty()) return value;
  if (authored.size() > defaults.size()) {
    active_reader->failure = "Too many MJCF solver values for " + name;
    return value;
  }
  if (authored.size() == defaults.size()) return value;
  const auto parent = ParseDoubles(inherited);
  for (size_t i = 0; i < std::min(parent.size(), defaults.size()); ++i) defaults[i] = parent[i];
  for (size_t i = 0; i < authored.size(); ++i) defaults[i] = authored[i];
  std::string completed;
  for (double number : defaults) {
    char text[40];
    std::snprintf(text, sizeof(text), "%.17g", number);
    if (!completed.empty()) completed += ' ';
    completed += text;
  }
  if (active_reader->options.report) {
    auto& report = *active_reader->options.report;
    auto& count = report.counts["completed_solver_arrays"];
    const auto previous = count.is_number() ? count.get<uint64_t>() : uint64_t(0);
    count = previous == UINT64_MAX ? previous : previous + 1;
    report.Diagnostic("info", "mjcf.solver_defaults", name + " completed using inherited/MJCF defaults", active_reader->root_source);
  }
  return completed;
}

// Resolve default classes for every supported element, including actuator,
// tendon, site and asset defaults (not only geoms and joints).
void InlineMJCFDefaults(pugi::xml_node root) {
  using Elements = std::map<std::string, AttrMap>;
  std::map<std::string, Elements> classes;
  std::function<void(pugi::xml_node, Elements)> collect =
      [&](pugi::xml_node node, Elements inherited) {
        for (auto child : node.children()) {
          if (std::string(child.name()) == "default") continue;
          for (auto attr : child.attributes()) {
            auto& previous = inherited[child.name()][attr.name()];
            previous = IsSolverArray(attr.name()) ? CompleteSolverArray(attr.name(), attr.value(), previous, child.name()) : attr.value();
          }
        }
        const std::string name = Attr(node, "class");
        classes[name] = inherited;
        // The top-level <default> is MuJoCo's implicit class "main", whether
        // or not it is named; unclassed elements use it.
        if (std::string(node.parent().name()) != "default") {
          classes[""] = inherited;
          classes["main"] = inherited;
        }
        for (auto child : node.children("default")) collect(child, inherited);
      };
  for (auto node : root.children("default")) collect(node, {});
  std::function<void(pugi::xml_node, std::string)> apply =
      [&](pugi::xml_node node, std::string inherited_class) {
        const std::string kind = node.name();
        if (kind == "default") return;
        std::string selected = Attr(node, "class", inherited_class);
        auto found = classes.find(selected);
        if (found == classes.end()) {
          if (node.attribute("class") && !classes.empty() && active_reader) {
            const std::string message = "MJCF <" + kind + "> references unknown default class `" +
                                        selected + "`";
            if (active_reader->options.unsupported_policy == UnsupportedPolicy::Error)
              active_reader->failure = message;
            else
              active_reader->Warn(message + "; using the root default", "mjcf.unknown_class");
          }
          found = classes.find("");
        }
        static const char* const kOrientation[] = {"quat", "euler", "axisangle",
                                                   "xyaxes", "zaxis"};
        bool own_orientation = false;
        for (const char* key : kOrientation)
          own_orientation |= static_cast<bool>(node.attribute(key));
        auto merge = [&](const std::string& element) {
          if (found == classes.end()) return;
          auto attrs = found->second.find(element);
          if (attrs == found->second.end()) return;
          for (const auto& attr : attrs->second) {
            // An element's own orientation (in any of the alternative
            // spellings) overrides every orientation inherited from a class.
            if (own_orientation &&
                std::find(std::begin(kOrientation), std::end(kOrientation),
                          attr.first) != std::end(kOrientation))
              continue;
            if (auto own = node.attribute(attr.first.c_str())) {
              if (IsSolverArray(attr.first)) {
                const auto completed = CompleteSolverArray(attr.first, own.value(), attr.second, kind);
                if (completed != own.value()) own.set_value(completed.c_str());
              }
            } else
              node.append_attribute(attr.first.c_str())
                  .set_value(attr.second.c_str());
          }
        };
        // General actuator parameters are the baseline for actuator shortcuts.
        if (std::string(node.parent().name()) == "actuator" &&
            kind != "general")
          merge("general");
        if (std::string(node.parent().name()) == "tendon") merge("tendon");
        if (std::string(node.parent().name()) == "equality") merge("equality");
        merge(kind);
        for (auto attr : node.attributes()) {
          if (!IsSolverArray(attr.name())) continue;
          const auto completed = CompleteSolverArray(attr.name(), attr.value(), "", kind);
          if (completed != attr.value()) attr.set_value(completed.c_str());
        }
        if (node.attribute("childclass"))
          inherited_class = Attr(node, "childclass");
        for (auto child : node.children()) apply(child, inherited_class);
      };
  apply(root, "");
}

bool BuildMujocoPayload(const std::string& xml, const fs::path& input_filename,
                        const Options& opts, Json* payload, Stats* stats,
                        std::string* err) {
  std::set<fs::path> seen;
  std::string expanded;
  if (!ExpandIncludes(xml, input_filename.parent_path(), &seen, &expanded,
                      err, input_filename.string())) {
    return false;
  }

  pugi::xml_document doc;
  const pugi::xml_parse_result parse_result = doc.load_string(expanded.c_str());
  if (!parse_result) {
    if (err)
      *err =
          std::string("MJCF XML parse failed: ") + parse_result.description();
    return false;
  }
  active_world_name = "__world";
  while (expanded.find(active_world_name) != std::string::npos)
    active_world_name += "_";
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

  InlineMJCFDefaults(root);
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
    ctx.autolimits = MjcBoolAttr(Attr(compiler, "autolimits", "true"));
    ctx.inertiafromgeom = ToLower(Attr(compiler, "inertiafromgeom", "auto"));
    const auto igr = ParseDoubles(Attr(compiler, "inertiagrouprange"));
    if (igr.size() >= 2) {
      ctx.inertia_group_min = ToInt32(igr[0], "inertiagrouprange");
      ctx.inertia_group_max = ToInt32(igr[1], "inertiagrouprange");
    }
  }
  ctx.defaults = ParseDefaults(root);
  if (!ExpandCableComposites(root, ctx, err)) return false;
  if (!ExpandReplicates(root, ctx, err)) return false;
  // Expanded cables may nest deeper than the source XML.
  if (!ValidateXMLDepth(root.root(), opts.max_depth + kMaxCableVertices,
                        active_reader->root_source, err))
    return false;
  if (const auto tendon_root = Child(root, "tendon")) {
    for (const auto& spatial : Children(tendon_root, "spatial"))
      for (const auto& wrap : Children(spatial, "geom"))
        if (HasAttr(wrap, "geom")) ctx.wrap_geoms.insert(Attr(wrap, "geom"));
  }

  // Dissolve <frame> grouping transforms now that the angle/eulerseq context is
  // known, so body/geom/site traversal never has to look inside a <frame>.
  // MuJoCo 3.16 frame scale/massscale scale the frame's contents; the
  // flattening keeps only the pose (massscale 0 derives the mass factor
  // from scale, so it matters only with a non-unit scale).
  for (const auto& worldbody : Children(root, "worldbody")) {
    std::vector<pugi::xml_node> stack{worldbody};
    while (!stack.empty()) {
      const pugi::xml_node node = stack.back();
      stack.pop_back();
      for (auto c = node.first_child(); c; c = c.next_sibling())
        if (c.type() == pugi::node_element) stack.push_back(c);
      if (std::string(node.name()) != "frame") continue;
      const auto scale = ParseDouble3(Attr(node, "scale"), {{1.0, 1.0, 1.0}});
      const double massscale = ParseDoubleAttr(node, "massscale", 0.0);
      if (scale[0] == 1.0 && scale[1] == 1.0 && scale[2] == 1.0 &&
          (massscale == 0.0 || massscale == 1.0))
        continue;
      if (!UnsupportedXML(node, "MJCF frame scale/massscale is not applied to the frame's contents",
                          "mjcf.frame_scale_unsupported", "mjcf.frame_scale",
                          active_reader->root_source, true)) {
        if (err) *err = active_reader->failure;
        return false;
      }
    }
  }
  for (const auto& worldbody : Children(root, "worldbody")) {
    FlattenFramesIn(worldbody, ctx);
  }

  active_body_names.clear();
  std::map<std::string, MeshFit> mesh_fits;
  struct FitScope {
    std::map<std::string, MeshFit>* previous = active_mesh_fits;
    bool previous_aabb = active_fitaabb;
    FitScope(std::map<std::string, MeshFit>* fits, bool aabb) {
      active_mesh_fits = fits;
      active_fitaabb = aabb;
    }
    ~FitScope() {
      active_mesh_fits = previous;
      active_fitaabb = previous_aabb;
    }
  } fit_scope(&mesh_fits, Child(root, "compiler") &&
                              MjcBoolAttr(Attr(Child(root, "compiler"), "fitaabb", "false")));
  std::map<std::pair<std::string, bool>, Mesh> mesh_cache;
  struct MeshCacheScope {
    std::map<std::pair<std::string, bool>, Mesh>* previous = active_mesh_cache;
    explicit MeshCacheScope(std::map<std::pair<std::string, bool>, Mesh>* c) { active_mesh_cache = c; }
    ~MeshCacheScope() { active_mesh_cache = previous; }
  } mesh_cache_scope(&mesh_cache);
  std::map<std::pair<std::string, bool>, PreloadedMesh> mesh_preload;
  struct MeshPreloadScope {
    std::map<std::pair<std::string, bool>, PreloadedMesh>* previous = active_mesh_preload;
    explicit MeshPreloadScope(std::map<std::pair<std::string, bool>, PreloadedMesh>* p) {
      active_mesh_preload = p;
    }
    ~MeshPreloadScope() { active_mesh_preload = previous; }
  } mesh_preload_scope(&mesh_preload);
  {
    std::set<std::pair<std::string, bool>> unique;
    for (const auto& entry : assets) {
      const std::string ext = ToLower(entry.second.path.extension().string());
      if (ext == ".stl" || ext == ".obj" || ext == ".dae")
        unique.emplace(entry.second.path.string(), entry.second.smoothnormal);
    }
    PreloadMeshFiles({unique.begin(), unique.end()});
  }
  std::set<std::string> stacked;
  struct StackedScope {
    std::set<std::string>* previous = stacked_joint_bodies;
    explicit StackedScope(std::set<std::string>* s) { stacked_joint_bodies = s; }
    ~StackedScope() { stacked_joint_bodies = previous; }
  } stacked_scope(&stacked);
  Json links = Json::array();
  Json joints = Json::array();
  links.push_back(Json{{"name", active_world_name}, {"static", true}});
  Json actuators = Json::array();
  Json tendons = Json::array();
  Json equalities = Json::array();
  Json filtered_pairs = Json::array();
  Json sites = Json::array();
  Json deformables = Json::array();
  Json mjc_actuators = Json::array();
  Json keyframes = Json::array();
  Json lights = Json::array();
  Json cameras = Json::array();
  Json materials = Json::array();
  Json sensors = Json::array();
  Json contact_pairs = Json::array();
  Json raw_flexes = Json::array();
  std::set<const void*> flex_bodies;
  if (opts.profile == "newton" &&
      !CollectMujocoRawFlexesJson(root, worldbodies, ctx, &raw_flexes, &flex_bodies, err))
    return false;
  struct FlexBodyScope {
    std::set<const void*>* previous = active_flex_bodies;
    explicit FlexBodyScope(std::set<const void*>* b) { active_flex_bodies = b; }
    ~FlexBodyScope() { active_flex_bodies = previous; }
  } flex_body_scope(&flex_bodies);
  for (const auto& worldbody : worldbodies) {
    for (const auto& body : Children(worldbody, "body")) {
      if (!VisitMujocoBody(body, active_world_name, "", IdentityMatrix(),
                           assets, hfields, opts, ctx, &links, &joints, stats,
                           err)) {
        return false;
      }
    }
    CollectMujocoSitesJson(worldbody, IdentityMatrix(), ctx, &sites, stats);
    if (opts.profile == "newton" &&
        !CollectMujocoFlexcompsJson(worldbody, ctx, FlexMeshDir(root, input_filename.parent_path()),
                                    &deformables, err))
      return false;
    CollectMujocoLightsCamerasJson(worldbody, IdentityMatrix(), ctx, &lights,
                                   &cameras);
  }
  for (auto& flex : raw_flexes) deformables.push_back(std::move(flex));
  // World-fixed geoms living directly under <worldbody> (floor/ground/hfield).
  if (!AddWorldbodyGeomsLink(worldbodies, assets, hfields, opts, ctx, &links,
                             stats, err)) {
    return false;
  }
  // Mark each articulation root that owns stacked joints so the converter can
  // author newton:jointsAddMobility.
  if (!stacked.empty()) {
    std::map<std::string, std::string> parent_of;
    for (const Json& joint : joints)
      parent_of[joint["child"].get<std::string>()] = joint["parent"].get<std::string>();
    std::set<std::string> roots;
    for (const std::string& body : stacked) {
      std::string current = body;
      for (size_t hops = 0; hops <= links.size(); ++hops) {
        auto up = parent_of.find(current);
        if (up == parent_of.end() || up->second == active_world_name) break;
        current = up->second;
      }
      roots.insert(current);
    }
    for (Json& link : links)
      if (roots.count(link["name"].get<std::string>())) link["jointsAddMobility"] = true;
  }
  AddMujocoActuatorsJson(root, &actuators, &mjc_actuators, stats);
  AddMujocoTendonsJson(root, ctx, &tendons, stats);
  AddMujocoEqualityJson(root, &equalities, stats);
  AddMujocoContactExcludesJson(root, &filtered_pairs, stats);
  AddMujocoKeyframesJson(root, &keyframes, stats);
  Json skybox = Json::object();
  AddMujocoMaterialsJson(root, input_filename.parent_path(), &materials, &skybox, stats);
  for (auto& mat : *active_materials) materials.push_back(mat);
  AddMujocoSensorsJson(root, &sensors, stats);
  AddMujocoContactPairsJson(root, &contact_pairs, stats);
  Json custom = Json::object();
  AddMujocoCustomJson(root, &custom);
  Json plugins = Json::array();
  AddMujocoPluginsJson(root, &plugins);

  (*payload) = {{"name", Attr(root, "model", input_filename.stem().string())},
                {"upAxis", opts.up_axis}, {"profile", opts.profile},
                {"layout", opts.layout}, {"sourceFormat", "mjcf"},
                {"gravity", opts.up_axis == "Z" ? Json::array({0, 0, -1})
                                                : Json::array({0, -1, 0})}};
  // Assign the large arrays by move (a braced list would copy them).
  (*payload)["links"] = std::move(links);
  (*payload)["joints"] = std::move(joints);
  (*payload)["actuators"] = std::move(actuators);
  if (!tendons.empty()) (*payload)["tendons"] = std::move(tendons);
  if (!equalities.empty()) (*payload)["equalities"] = std::move(equalities);
  if (!filtered_pairs.empty())
    (*payload)["filteredPairs"] = std::move(filtered_pairs);
  if (!sites.empty()) (*payload)["sites"] = std::move(sites);
  if (!deformables.empty()) (*payload)["deformables"] = std::move(deformables);
  if (!mjc_actuators.empty())
    (*payload)["mjcActuators"] = std::move(mjc_actuators);
  Json mjc_scene = BuildMjcSceneJson(root);
  if (!mjc_scene.empty()) (*payload)["mjcScene"] = std::move(mjc_scene);
  if (!keyframes.empty()) (*payload)["keyframes"] = std::move(keyframes);
  if (!lights.empty()) (*payload)["lights"] = std::move(lights);
  if (!skybox.empty()) (*payload)["skybox"] = std::move(skybox);
  // Clipping planes are <visual><map znear/zfar> scaled by the model extent;
  // only an authored <statistic extent> makes them known before compiling.
  if (const auto statistic = Child(root, "statistic")) {
    if (HasAttr(statistic, "extent")) {
      const double extent = ParseDoubleAttr(statistic, "extent", 1.0);
      const auto map = Child(Child(root, "visual"), "map");
      const double znear = map ? ParseDoubleAttr(map, "znear", 0.01) : 0.01;
      const double zfar = map ? ParseDoubleAttr(map, "zfar", 50.0) : 50.0;
      for (auto& cam : cameras) cam["clippingRange"] = Json::array({znear * extent, zfar * extent});
    }
  }
  if (!cameras.empty()) (*payload)["cameras"] = std::move(cameras);
  if (!materials.empty()) (*payload)["materials"] = std::move(materials);
  if (!sensors.empty()) (*payload)["sensors"] = std::move(sensors);
  if (!contact_pairs.empty())
    (*payload)["contactPairs"] = std::move(contact_pairs);
  if (!custom.empty()) (*payload)["custom"] = std::move(custom);
  if (!plugins.empty()) (*payload)["plugins"] = std::move(plugins);
  const auto option = Child(root, "option");
  if (option && HasAttr(option, "gravity")) {
    auto g = ParseDouble3(Attr(option, "gravity"), {{0, 0, -9.80665}});
    double magnitude = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    (*payload)["gravityMagnitude"] = magnitude;
    if (magnitude > 0)
      for (double& v : g) v /= magnitude;
    if (opts.up_axis == "Y") g = {{g[0], g[2], -g[1]}};
    (*payload)["gravity"] = Json::array({g[0], g[1], g[2]});
  }
  if (option && HasAttr(option, "timestep")) {
    (*payload)["timestep"] = ParseDoubleAttr(option, "timestep", 0.0);
  }
  return true;
}

std::vector<double> URDFPose(const pugi::xml_node& origin) {
  auto xyz = ParseDouble3(Attr(origin, "xyz"), {{0, 0, 0}});
  Context ctx;
  ctx.angle_to_rad = 1.0;
  // URDF uses fixed-axis roll/pitch/yaw: Rz(yaw) * Ry(pitch) * Rx(roll).
  ctx.eulerseq = "XYZ";
  auto q = OrientationQuat("", "", Attr(origin, "rpy"), "", "", ctx);
  return MatrixFromPosQuat(xyz, q);
}

// URDF OBJ/DAE meshes keep every part and their own materials.
bool LoadURDFObj(const std::string& file, const std::string& source, OBJ* obj,
                 std::string* resolved, std::string* err) {
  std::vector<uint8_t> data;
  if (!active_reader->Read(file, source, &data, resolved, err)) return false;
  const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
  return ToLower(fs::path(file).extension().string()) == ".dae"
             ? ReadDAE(text, *resolved, *active_reader, obj, err)
             : ReadOBJ(text, *resolved, *active_reader, true, obj, err);
}

PreloadKey URDFObjKey(const std::string& file, const std::string& source) {
  return {"urdf-obj\n" + source + "\n" + file, true};
}

// Resolve a URDF mesh without reading it; the mesh loader reads it once. A
// read-only resolver hook cannot resolve separately, and a failed
// resolution reads to report the failure as before.
bool ResolveURDFMesh(const std::string& file, const std::string& source, std::string* resolved,
                     std::string* err) {
  Reader& reader = *active_reader;
  if (!reader.options.read || reader.options.open) {
    AssetSource asset;
    std::string why;
    if (reader.Open(file, source, &asset, &why)) {
      *resolved = asset.identifier;
      return true;
    }
  }
  std::vector<uint8_t> data;
  return reader.Read(file, source, &data, resolved, err);
}

bool URDFGeometry(const pugi::xml_node& node, const std::string& source,
                  Json* geometries, std::string* err) {
  const auto geometry = node.child("geometry");
  const auto pose = URDFPose(node.child("origin"));
  std::string name = Attr(node, "name", "geometry");
  Json j{{"name", name}, {"matrix", Array(pose)}};
  const bool collision = std::string(node.name()) == "collision" &&
                         !active_reader->options.tessellate_collision_shapes;
  // Non-legacy profiles keep primitive visuals native too (as
  // urdf-usd-converter authors Cube/Sphere/Cylinder visuals).
  const bool native = collision || (std::string(node.name()) == "visual" &&
                                    active_reader->options.profile != "legacy");
  if (auto mesh = geometry.child("mesh")) {
    std::string file = Attr(mesh, "filename");
    std::string resolved;
    auto scale = ParseDouble3(Attr(mesh, "scale"), {{1, 1, 1}});
    const auto matrix = MultiplyMatrix(pose, ScaleMatrix(scale));
    const std::string ext = ToLower(fs::path(file).extension().string());
    if (ext == ".obj" || ext == ".dae") {
      // Embedded OBJ/DAE materials take priority over URDF <material>.
      OBJ obj;
      PreloadedMesh preloaded;
      if (TakePreloaded(URDFObjKey(file, source), &preloaded, err, true)) {
        obj = std::move(preloaded.obj);
        resolved = std::move(preloaded.resolved);
      } else if (!active_reader->failure.empty() ||
                 !LoadURDFObj(file, source, &obj, &resolved, err)) {
        return false;
      }
      int32_t offset = static_cast<int32_t>(active_materials->size());
      for (auto& m : obj.materials) {
        m["name"] = "obj_" + std::to_string(active_materials->size()) + "_" +
                    m["name"].get<std::string>();
        active_materials->push_back(m);
      }
      for (auto& part : obj.meshes) {
        for (auto& mi : part.materials)
          if (mi >= 0) mi += offset;
        Json part_json = MeshRefJSON(part, *active_reader);
        part_json["name"] = name + "_" + part.name;
        part_json["matrix"] = Array(matrix);
        geometries->push_back(std::move(part_json));
      }
      return true;
    }
    if (!ResolveURDFMesh(file, source, &resolved, err)) return false;
    Mesh m;
    if (!LoadMeshFile(fs::path(resolved), false, &m, err)) return false;
    j = MeshRefJSON(m, *active_reader);
    j["name"] = name;
    j["matrix"] = Array(matrix);
  } else {
    Json shape = Json::object();
    Mesh generated;
    if (auto box = geometry.child("box")) {
      auto size = ParseDouble3(Attr(box, "size"), {{1, 1, 1}});
      if (size[0] <= 0 || size[1] <= 0 || size[2] <= 0) {
        if (err) *err = "URDF box size must be positive";
        return false;
      }
      if (native) {
        shape["type"] = "box";
        j["matrix"] = Array(MultiplyMatrix(
            pose, ScaleMatrix({{size[0] / 2, size[1] / 2, size[2] / 2}})));
      } else
        generated = MakeBoxMesh({{size[0] / 2, size[1] / 2, size[2] / 2}});
    } else if (auto sphere = geometry.child("sphere")) {
      double radius = ParseDoubleAttr(sphere, "radius", 0);
      if (radius <= 0) {
        if (err) *err = "URDF radius must be positive";
        return false;
      }
      if (native)
        shape = Json{{"type", "sphere"}, {"radius", radius}};
      else
        generated = MakeSphereMesh(radius);
    } else if (auto cylinder = geometry.child("cylinder")) {
      double radius = ParseDoubleAttr(cylinder, "radius", 0),
             length = ParseDoubleAttr(cylinder, "length", 0);
      if (radius <= 0 || length <= 0) {
        if (err) *err = "URDF cylinder dimensions must be positive";
        return false;
      }
      if (native)
        shape = Json{{"type", "cylinder"},
                     {"radius", radius},
                     {"height", length},
                     {"axis", "Z"}};
      else
        generated = MakeCylinderMesh(radius, length / 2);
    } else if (auto capsule = geometry.child("capsule");
               capsule && active_reader->options.profile != "legacy") {
      // Non-standard but common (Gazebo, Newton's add_urdf): `length` or
      // `height` is the cylindrical part, along Z.
      const double radius = ParseDoubleAttr(capsule, "radius", 0),
                   length = ParseDoubleAttr(capsule, HasAttr(capsule, "length") ? "length" : "height", 0);
      if (radius <= 0 || length < 0) {
        if (err) *err = "URDF capsule dimensions must be positive";
        return false;
      }
      if (native)
        shape = Json{{"type", "capsule"}, {"radius", radius}, {"height", length}, {"axis", "Z"}};
      else
        generated = MakeCapsuleMesh(radius, length / 2);
    } else {
      // Missing or unknown geometry: skip it under the warn/preserve policies
      // (urdf-usd-converter warns and continues).
      std::string what = "URDF ";
      what += collision ? "collision" : "visual";
      what += " `" + name + "` has no supported geometry";
      if (!active_reader->Unsupported(what, "urdf.geometry_unsupported", "urdf.geometry",
                                      source, 0, Json{{"element", name}}.dump(), "json")) {
        if (err) *err = active_reader->failure.empty() ? what : active_reader->failure;
        return false;
      }
      return true;
    }
    if (native)
      j["shape"] = std::move(shape);
    else {
      Json built = MeshRefJSON(generated, *active_reader);
      j["geometry"] = std::move(built["geometry"]);
      if (built.contains("meshRef")) j["meshRef"] = std::move(built["meshRef"]);
    }
  }
  geometries->push_back(std::move(j));
  return true;
}

// Inverse of a rigid column-major transform: [R t] -> [R^T  -R^T t].
std::vector<double> InverseRigidMatrix(const std::vector<double>& m) {
  std::vector<double> r(16, 0.0);
  for (int c = 0; c < 3; ++c)
    for (int k = 0; k < 3; ++k) r[size_t(c * 4 + k)] = m[size_t(k * 4 + c)];
  for (int k = 0; k < 3; ++k)
    r[size_t(12 + k)] = -(r[size_t(k)] * m[12] + r[size_t(4 + k)] * m[13] + r[size_t(8 + k)] * m[14]);
  r[15] = 1.0;
  return r;
}

// urdf-usd-converter's ghost links: a link with no (or zero) inertia, no
// visuals and no collisions on a fixed joint (or the root) is a pure frame.
// It is authored without RigidBodyAPI, its fixed joint is dropped, and joints
// from it re-anchor on the nearest rigid ancestor (or the world). A link on a
// fixed joint that a mimic joint references is kept.
// KDL-style dummy inertia (urdf-usd-converter, Gazebo lumping): a parent
// with geometry but no inertia and an inertial-only, childless child on a
// fixed joint. The first such child's inertia moves into the parent frame
// (com = R c + t, I = R I R^T) and the child link and joint are dropped.
size_t MergeDummyInertiaLinks(Json* links, Json* joints,
                              const std::map<std::string, std::string>& parent,
                              const std::map<std::string, std::vector<double>>& origins) {
  std::map<std::string, Json*> by_name;
  for (auto& link : *links) by_name[link["name"].get<std::string>()] = &link;
  auto zero_inertia = [](const Json& link) {
    if (!link.contains("inertial")) return true;
    const Json& in = link["inertial"];
    if (in["mass"].get<double>() > 0.0) return false;
    for (const auto& v : in["fullInertia"])
      if (v.get<double>() != 0.0) return false;
    return true;
  };
  auto has_geometry = [](const Json& link) {
    return !link["visuals"].empty() || !link["collisions"].empty();
  };
  std::set<std::string> has_children, mimic_children, merged_into;
  std::map<std::string, std::string> joint_child;
  for (const auto& j : *joints) {
    has_children.insert(j["parent"].get<std::string>());
    joint_child[j["name"].get<std::string>()] = j["child"].get<std::string>();
  }
  for (const auto& j : *joints)
    if (j.contains("mimic")) {
      auto child = joint_child.find(j["mimic"]["joint"].get<std::string>());
      if (child != joint_child.end()) mimic_children.insert(child->second);
    }
  std::set<std::string> dropped;
  for (const auto& j : *joints) {
    const std::string child = j["child"].get<std::string>();
    const std::string from = j["parent"].get<std::string>();
    if (j["type"].get<std::string>() != "fixed" || !by_name.count(child) || !by_name.count(from) ||
        merged_into.count(from) || has_children.count(child) || mimic_children.count(child))
      continue;
    Json& p = *by_name[from];
    const Json& c = *by_name[child];
    if (!zero_inertia(p) || !has_geometry(p) || has_geometry(c) || zero_inertia(c)) continue;
    const std::vector<double>& m = origins.at(child);  // child frame in parent
    const Json& in = c["inertial"];
    double com[3] = {in["centerOfMass"][0].get<double>(), in["centerOfMass"][1].get<double>(),
                     in["centerOfMass"][2].get<double>()};
    double t[3][3];
    const double f[6] = {in["fullInertia"][0].get<double>(), in["fullInertia"][1].get<double>(),
                         in["fullInertia"][2].get<double>(), in["fullInertia"][3].get<double>(),
                         in["fullInertia"][4].get<double>(), in["fullInertia"][5].get<double>()};
    const double full[3][3] = {{f[0], f[3], f[4]}, {f[3], f[1], f[5]}, {f[4], f[5], f[2]}};
    for (int a = 0; a < 3; ++a)
      for (int b = 0; b < 3; ++b) {
        t[a][b] = 0;
        for (int k = 0; k < 3; ++k)
          for (int l = 0; l < 3; ++l) t[a][b] += m[size_t(k * 4 + a)] * full[k][l] * m[size_t(l * 4 + b)];
      }
    const Json moved_com = Json::array({m[0] * com[0] + m[4] * com[1] + m[8] * com[2] + m[12],
                                        m[1] * com[0] + m[5] * com[1] + m[9] * com[2] + m[13],
                                        m[2] * com[0] + m[6] * com[1] + m[10] * com[2] + m[14]});
    p["inertial"] = Json{{"mass", in["mass"].get<double>()},
                         {"centerOfMass", moved_com},
                         {"fullInertia", Json::array({t[0][0], t[1][1], t[2][2], t[0][1], t[0][2], t[1][2]})}};
    merged_into.insert(from);
    dropped.insert(child);
  }
  if (dropped.empty()) return 0;
  Json kept_links = Json::array(), kept_joints = Json::array();
  for (auto& link : *links)
    if (!dropped.count(link["name"].get<std::string>())) kept_links.push_back(std::move(link));
  for (auto& j : *joints)
    if (!dropped.count(j["child"].get<std::string>())) kept_joints.push_back(std::move(j));
  *links = std::move(kept_links);
  *joints = std::move(kept_joints);
  (void)parent;
  return dropped.size();
}

size_t CollapseGhostLinks(Json* links, Json* joints, const std::map<std::string, std::string>& parent,
                          const std::map<std::string, std::vector<double>>& worlds) {
  std::map<std::string, std::string> parent_joint_type, joint_child;
  for (const auto& j : *joints) {
    parent_joint_type[j["child"].get<std::string>()] = j["type"].get<std::string>();
    joint_child[j["name"].get<std::string>()] = j["child"].get<std::string>();
  }
  std::set<std::string> mimic_children;
  for (const auto& j : *joints)
    if (j.contains("mimic")) {
      const std::string leader = j["mimic"]["joint"].get<std::string>();
      auto child = joint_child.find(leader);
      if (child != joint_child.end() && parent_joint_type[child->second] == "fixed")
        mimic_children.insert(child->second);
    }
  std::set<std::string> ghosts;
  for (auto& link : *links) {
    if (link.contains("static") && link["static"].is_boolean() && link["static"].get<bool>()) continue;
    const std::string name = link["name"].get<std::string>();
    bool massless = true;
    if (link.contains("inertial")) {
      const Json& in = link["inertial"];
      const double mass = in["mass"].get<double>();
      massless = mass <= 0.0;
      if (massless && in.contains("fullInertia"))
        for (const auto& v : in["fullInertia"]) massless &= v.get<double>() == 0.0;
    }
    const bool shapes = !link["visuals"].empty() || !link["collisions"].empty();
    const auto type = parent_joint_type.find(name);
    const bool root = !parent.count(name);
    if (!massless || shapes || mimic_children.count(name) ||
        !(root || (type != parent_joint_type.end() && type->second == "fixed")))
      continue;
    ghosts.insert(name);
    link["static"] = true;
    link["ghost"] = true;
    link["articulationRoot"] = false;
  }
  if (ghosts.empty()) return 0;
  auto rigid_ancestor = [&](std::string name) {
    while (ghosts.count(name)) {
      auto up = parent.find(name);
      if (up == parent.end()) return active_world_name;
      name = up->second;
    }
    return name;
  };
  Json kept = Json::array();
  std::set<std::string> new_roots;
  for (auto& j : *joints) {
    const std::string child = j["child"].get<std::string>();
    std::string from = j["parent"].get<std::string>();
    if (ghosts.count(child)) continue;  // the ghost's own fixed joint
    if (ghosts.count(from)) {
      const std::string anchor = rigid_ancestor(from);
      auto read = [&](const char* key, std::initializer_list<double> fallback) {
        std::vector<double> v(fallback);
        if (j.contains(key) && j[key].is_array() && j[key].size() == v.size())
          for (size_t k = 0; k < v.size(); ++k) v[k] = j[key][k].get<double>();
        return v;
      };
      const auto pos = read("localPos0", {0, 0, 0});
      const auto rot = read("localRot0", {1, 0, 0, 0});
      const std::vector<double> frame = MultiplyMatrix(
          worlds.at(from), MatrixFromPosQuat({{pos[0], pos[1], pos[2]}},
                                             {{rot[0], rot[1], rot[2], rot[3]}}));
      const std::vector<double> anchor_world =
          anchor == active_world_name ? IdentityMatrix() : worlds.at(anchor);
      std::array<double, 3> p;
      Quat q;
      DecomposePosQuat(MultiplyMatrix(InverseRigidMatrix(anchor_world), frame), &p, &q);
      j["parent"] = anchor;
      j["localPos0"] = Json::array({p[0], p[1], p[2]});
      j["localRot0"] = Json::array({q[0], q[1], q[2], q[3]});
      if (anchor == active_world_name) new_roots.insert(child);
    }
    kept.push_back(std::move(j));
  }
  *joints = std::move(kept);
  for (auto& link : *links)
    if (new_roots.count(link["name"].get<std::string>())) link["articulationRoot"] = true;
  return ghosts.size();
}

// URDF elements and attributes without a USD mapping, kept as urdf:*
// metadata the way urdf-usd-converter keeps "undefined" elements:
// {tag, attributes, text, elements}. Non-legacy profiles only.
Json URDFCustomElement(const pugi::xml_node& node, size_t depth) {
  Json e{{"tag", std::string(node.name())}};
  Json attributes = Json::object();
  for (auto a : node.attributes()) attributes[a.name()] = std::string(a.value());
  if (!attributes.empty()) e["attributes"] = std::move(attributes);
  std::string text;
  Json elements = Json::array();
  for (auto c = node.first_child(); c; c = c.next_sibling()) {
    if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) text += c.value();
    else if (c.type() == pugi::node_element && depth < 64) elements.push_back(URDFCustomElement(c, depth + 1));
  }
  const size_t b = text.find_first_not_of(" \t\r\n");
  if (b != std::string::npos) e["text"] = text.substr(b, text.find_last_not_of(" \t\r\n") - b + 1);
  if (!elements.empty()) e["elements"] = std::move(elements);
  return e;
}

// Unknown attributes and child elements of `node` (null when none).
Json URDFCustomJson(const pugi::xml_node& node, std::initializer_list<const char*> known_children,
                    std::initializer_list<const char*> known_attributes) {
  auto known = [](std::initializer_list<const char*> names, const char* name) {
    for (const char* n : names)
      if (std::strcmp(n, name) == 0) return true;
    return false;
  };
  Json out = Json::object();
  Json attributes = Json::object();
  for (auto a : node.attributes())
    if (!known(known_attributes, a.name())) attributes[a.name()] = std::string(a.value());
  if (!attributes.empty()) out["attributes"] = std::move(attributes);
  Json elements = Json::array();
  for (auto c : node.children())
    if (c.type() == pugi::node_element && !known(known_children, c.name()))
      elements.push_back(URDFCustomElement(c, 1));
  if (!elements.empty()) out["elements"] = std::move(elements);
  return out;
}

// Roots for `package://NAME/...` URDF references that Options::packages does
// not map, found the way Newton's and urdf-usd-converter's URDF importers do:
// the nearest ancestor of the URDF's directory named NAME, otherwise the
// nearest ancestor ADIR for which ADIR/<relative path> exists (relative paths
// with `..` are not searched). Explicit mappings always win.
void DiscoverURDFPackages(const pugi::xml_node& root, const std::string& source) {
  if (source.empty() || active_reader->options.open || active_reader->options.read) return;
  std::error_code ec;
  fs::path start = fs::path(source);
  if (!start.is_absolute()) start = fs::current_path(ec) / start;
  start = fs::weakly_canonical(start, ec).parent_path();
  if (start.empty()) return;
  std::vector<pugi::xml_node> stack{root};
  while (!stack.empty()) {
    const pugi::xml_node node = stack.back();
    stack.pop_back();
    for (auto c = node.first_child(); c; c = c.next_sibling())
      if (c.type() == pugi::node_element) stack.push_back(c);
    const std::string tag = node.name();
    if (tag != "mesh" && tag != "texture") continue;
    const std::string file = Attr(node, "filename");
    if (file.rfind("package://", 0) != 0) continue;
    const size_t end = file.find('/', 10);
    if (end == std::string::npos || end == 10 || end + 1 >= file.size()) continue;
    const std::string package = file.substr(10, end - 10);
    if (active_reader->packages.count(package)) continue;
    const fs::path relative = fs::path(file.substr(end + 1)).lexically_normal();
    bool escapes = relative.is_absolute();
    for (const auto& part : relative) escapes = escapes || part == "..";
    fs::path found;
    for (fs::path dir = start; !dir.empty(); dir = dir.parent_path()) {
      if (dir.filename() == package) {
        found = dir;
        break;
      }
      if (dir == dir.root_path()) break;
    }
    for (fs::path dir = start; found.empty() && !escapes && !dir.empty(); dir = dir.parent_path()) {
      if (fs::is_regular_file(dir / relative, ec)) found = dir;
      if (dir == dir.root_path()) break;
    }
    if (found.empty()) continue;
    active_reader->packages[package] = found.string();
    active_reader->Warn("URDF package `" + package + "` resolved to " + found.generic_string() +
                            " (pass packages to map it explicitly)",
                        "urdf.package_discovered");
  }
}

bool BuildURDF(const pugi::xml_node& root, const std::string& source,
               const Options& opts, Json* payload, std::string* err) {
  std::map<std::pair<std::string, bool>, Mesh> mesh_cache;
  struct MeshCacheScope {
    std::map<std::pair<std::string, bool>, Mesh>* previous = active_mesh_cache;
    explicit MeshCacheScope(std::map<std::pair<std::string, bool>, Mesh>* c) { active_mesh_cache = c; }
    ~MeshCacheScope() { active_mesh_cache = previous; }
  } mesh_cache_scope(&mesh_cache);
  std::map<PreloadKey, PreloadedMesh> mesh_preload;
  struct MeshPreloadScope {
    std::map<PreloadKey, PreloadedMesh>* previous = active_mesh_preload;
    explicit MeshPreloadScope(std::map<PreloadKey, PreloadedMesh>* p) { active_mesh_preload = p; }
    ~MeshPreloadScope() { active_mesh_preload = previous; }
  } mesh_preload_scope(&mesh_preload);
  DiscoverURDFPackages(root, source);
  if (!opts.open && !opts.read) {
    // Parse every distinct mesh ahead on worker threads (see PreloadJobs).
    std::vector<std::pair<PreloadKey, PreloadJob>> jobs;
    std::set<PreloadKey> seen;
    for (auto link : root.children("link"))
      for (const char* kind : {"visual", "collision"})
        for (auto geom : link.children(kind)) {
          const auto mesh = geom.child("geometry").child("mesh");
          if (!mesh) continue;
          const std::string file = Attr(mesh, "filename");
          const std::string ext = ToLower(fs::path(file).extension().string());
          if (ext == ".obj" || ext == ".dae") {
            const PreloadKey key = URDFObjKey(file, source);
            if (seen.insert(key).second)
              jobs.emplace_back(key, [file, source](PreloadedMesh* out, std::string* why) {
                return LoadURDFObj(file, source, &out->obj, &out->resolved, why);
              });
          } else if (ext == ".stl") {
            AssetSource asset;
            std::string why;
            if (!active_reader->Open(file, source, &asset, &why)) continue;
            const PreloadKey key{asset.identifier, false};
            if (seen.insert(key).second)
              jobs.emplace_back(key, [key](PreloadedMesh* out, std::string* e) {
                return LoadMeshFileUncached(fs::path(key.first), false, &out->mesh, e);
              });
          }
        }
    PreloadJobs(jobs);
  }
  Json links = Json::array(), joints = Json::array(), materials = Json::array(),
       actuators = Json::array();
  std::map<std::string, pugi::xml_node> nodes;
  std::map<std::string, std::string> parent;
  std::map<std::string, std::vector<double>> origins, worlds;
  std::set<std::string> joint_names;
  for (auto node : root.children("link")) {
    std::string name = Attr(node, "name");
    if (name.empty() || nodes.count(name)) {
      if (err) *err = "URDF link names must be nonempty and unique";
      return false;
    }
    nodes[name] = node;
  }
  active_world_name = "__world";
  while (nodes.count(active_world_name)) active_world_name += "_";
  if (nodes.empty()) {
    if (err) *err = "URDF contains no links";
    return false;
  }
  for (auto node : root.children("joint")) {
    std::string name = Attr(node, "name"),
                pname = Attr(node.child("parent"), "link"),
                child = Attr(node.child("child"), "link");
    if (name.empty() || !joint_names.insert(name).second ||
        !nodes.count(pname) || !nodes.count(child) || parent.count(child)) {
      if (err)
        *err = "URDF has duplicate joints, unknown links or multiple parents";
      return false;
    }
    parent[child] = pname;
    origins[child] = URDFPose(node.child("origin"));
    Json j{{"name", name},
           {"type", Attr(node, "type", "fixed")},
           {"parent", pname},
           {"child", child}};
    auto type = j["type"].get<std::string>();
    if (type == "floating") {
      j["type"] = "floating";
      joints.push_back(std::move(j));
      continue;
    }
    // planar: 2 translations in the plane normal to `axis` + rotation about
    // it. Non-legacy profiles author a D6 joint with the other DOFs locked.
    const bool planar = type == "planar" && opts.profile != "legacy";
    if (type != "fixed" && type != "revolute" && type != "continuous" &&
        type != "prismatic" && !planar) {
      if (err) *err = "Unsupported URDF joint type: " + type;
      return false;
    }
    auto axis = ParseDouble3(Attr(node.child("axis"), "xyz"), {{1, 0, 0}});
    if (axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] < 1e-24) {
      // A fixed joint never uses its axis (urdfdom accepts "0 0 0").
      if (type != "fixed") {
        if (err) *err = "URDF joint has zero axis";
        return false;
      }
      axis = {{1, 0, 0}};
    }
    std::array<double, 3> pos;
    Quat rot;
    DecomposePosQuat(origins[child], &pos, &rot);
    Quat align = QuatFromTwoVecs({{1, 0, 0}}, axis), q0 = QuatMul(rot, align);
    j["axisToken"] = "X";
    j["axis"] = Json::array({axis[0], axis[1], axis[2]});
    j["localPos0"] = Json::array({pos[0], pos[1], pos[2]});
    j["localPos1"] = Json::array({0, 0, 0});
    j["localRot0"] = Json::array({q0[0], q0[1], q0[2], q0[3]});
    j["localRot1"] = Json::array({align[0], align[1], align[2], align[3]});
    if (auto l = node.child("limit")) {
      Json limits = Json::object();
      for (const char* key : {"lower", "upper", "effort", "velocity"})
        if (l.attribute(key)) limits[key] = ParseDoubleAttr(l, key, 0);
      j["limit"] = std::move(limits);
    }
    if (auto d = node.child("dynamics")) {
      Json dynamics = Json::object();
      for (const char* key : {"damping", "friction", "stiffness", "armature"})
        if (d.attribute(key)) dynamics[key] = ParseDoubleAttr(d, key, 0);
      j["dynamics"] = std::move(dynamics);
    }
    // URDF metadata without a UsdPhysics/Newton home; authored as urdf:*
    // attributes by non-legacy profiles.
    for (const char* section : {"safety_controller", "calibration"})
      if (auto c = node.child(section)) {
        Json values = Json::object();
        for (auto a : c.attributes())
          values[a.name()] = ParseDoubleAttr(c, a.name(), 0);
        if (!values.empty()) j[section] = std::move(values);
      }
    if (auto m = node.child("mimic"))
      j["mimic"] = Json{{"joint", Attr(m, "joint")},
                        {"offset", ParseDoubleAttr(m, "offset", 0)},
                        {"multiplier", ParseDoubleAttr(m, "multiplier", 1)}};
    if (opts.profile != "legacy") {
      Json custom = URDFCustomJson(node, {"origin", "parent", "child", "axis", "limit", "dynamics",
                                          "mimic", "safety_controller", "calibration"},
                                   {"name", "type"});
      if (!custom.empty()) j["urdfCustom"] = std::move(custom);
    }
    joints.push_back(std::move(j));
  }
  // Resolve link world transforms iteratively: URDF chains (snakes, ropes,
  // cables) can be far deeper than the XML depth limit, and recursion depth
  // must not grow with chain length.
  const size_t max_chain = std::max<size_t>(opts.max_depth, nodes.size() + 1);
  std::function<bool(const std::string&, size_t)> world =
      [&](const std::string& name, size_t) {
        std::vector<std::string> chain;
        std::set<std::string> visiting;
        std::string current = name;
        while (!worlds.count(current)) {
          if (!visiting.insert(current).second || chain.size() > max_chain) {
            if (err) *err = "URDF cycle or hierarchy depth exceeded";
            return false;
          }
          chain.push_back(current);
          auto up = parent.find(current);
          if (up == parent.end()) break;
          current = up->second;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
          auto up = parent.find(*it);
          worlds[*it] = up == parent.end()
                            ? IdentityMatrix()
                            : MultiplyMatrix(worlds[up->second], origins[*it]);
        }
        return true;
      };
  auto material = [&](pugi::xml_node node,
                      const std::string& fallback) -> std::string {
    std::string name = Attr(node, "name", fallback);
    if (!node.child("color") && !node.child("texture")) return name;
    Json m{{"name", name}};
    if (auto c = node.child("color"))
      m["rgba"] = Array(ParseDoubles(Attr(c, "rgba")));
    if (auto t = node.child("texture")) {
      AssetBytes data;
      std::string resolved, why;
      if (active_reader->ReadTexture(Attr(t, "filename"), source, &data, &resolved,
                              &why)) {
        m["texture"] = resolved;
        m["texture_abs"] = resolved;
      } else
        active_reader->Warn("Texture: " + why, "import.texture_unresolved");
    }
    materials.push_back(std::move(m));
    return name;
  };
  for (auto m : root.children("material")) material(m, "material");
  // A synthetic static body anchors fixed roots, while floating joints omit
  // their parent constraint and remain free articulations.
  links.push_back(Json{{"name", active_world_name}, {"static", true}});
  for (const auto& entry : nodes) {
    const auto& name = entry.first;
    auto node = entry.second;
    if (!world(name, 0)) return false;
    Json link{{"name", name},
              {"matrix", Array(worlds[name])},
              {"visuals", Json::array()},
              {"collisions", Json::array()}};
    if (!parent.count(name)) {
      // A URDF without joints is a lone rigid body, not a one-link
      // articulation (urdf-usd-converter 0.3.1).
      link["articulationRoot"] = opts.profile == "legacy" || root.child("joint");
      joints.push_back(Json{{"name", active_world_name + "_to_" + name},
                            {"type", "fixed"},
                            {"parent", active_world_name},
                            {"child", name}});
    }
    if (auto i = node.child("inertial")) {
      auto origin = URDFPose(i.child("origin"));
      auto tensor = i.child("inertia");
      double t[3][3] = {
          {ParseDoubleAttr(tensor, "ixx", 0), ParseDoubleAttr(tensor, "ixy", 0),
           ParseDoubleAttr(tensor, "ixz", 0)},
          {ParseDoubleAttr(tensor, "ixy", 0), ParseDoubleAttr(tensor, "iyy", 0),
           ParseDoubleAttr(tensor, "iyz", 0)},
          {ParseDoubleAttr(tensor, "ixz", 0), ParseDoubleAttr(tensor, "iyz", 0),
           ParseDoubleAttr(tensor, "izz", 0)}};
      double rotated[3][3]{};
      for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++)
          for (int c = 0; c < 3; c++)
            for (int d = 0; d < 3; d++)
              rotated[a][b] += origin[c * 4 + a] * t[c][d] * origin[d * 4 + b];
      link["inertial"] = Json{
          {"mass", ParseDoubleAttr(i.child("mass"), "value", 0)},
          {"centerOfMass", Json::array({origin[12], origin[13], origin[14]})},
          {"fullInertia",
           Json::array({rotated[0][0], rotated[1][1], rotated[2][2],
                        rotated[0][1], rotated[0][2], rotated[1][2]})}};
    }
    for (const char* kind : {"visual", "collision"})
      for (auto geom : node.children(kind)) {
        Json items = Json::array();
        std::string why;
        if (!URDFGeometry(geom, source, &items, &why)) {
          if (!opts.allow_missing) {
            if (err) *err = why;
            return false;
          }
          active_reader->Warn(why);
          continue;
        }
        std::string mat;
        if (auto m = geom.child("material"))
          mat = material(m, name + "_material");
        Json custom = opts.profile != "legacy"
                          ? URDFCustomJson(geom, {"origin", "geometry", "material"}, {"name"})
                          : Json::object();
        for (auto& item : items) {
          if (!mat.empty()) item["material"] = mat;
          if (!custom.empty()) item["urdfCustom"] = custom;
          link[kind == std::string("visual") ? "visuals" : "collisions"]
              .push_back(std::move(item));
        }
      }
    if (opts.profile != "legacy") {
      Json custom = URDFCustomJson(node, {"inertial", "visual", "collision"}, {"name"});
      if (!custom.empty()) link["urdfCustom"] = std::move(custom);
    }
    links.push_back(std::move(link));
  }
  for (auto& j : joints)
    if (j["type"].get<std::string>() == "floating")
      for (auto& l : links)
        if (l["name"].get<std::string>() == j["child"].get<std::string>()) {
          l["floating"] = true;
          l["articulationRoot"] = true;
        }
  Json filtered = Json::array();
  for (auto& j : joints)
    if (j["type"].get<std::string>() != "floating")
      filtered.push_back(std::move(j));
  joints = std::move(filtered);
  if (opts.profile != "legacy") {
    const size_t dummies = MergeDummyInertiaLinks(&links, &joints, parent, origins);
    if (dummies && opts.report) {
      auto& count = opts.report->counts["merged_dummy_inertia_links"];
      count = (count.is_number() ? count.get<uint64_t>() : uint64_t(0)) + dummies;
    }
    const size_t ghosts = CollapseGhostLinks(&links, &joints, parent, worlds);
    if (ghosts && opts.report) {
      auto& count = opts.report->counts["ghost_links"];
      count = (count.is_number() ? count.get<uint64_t>() : uint64_t(0)) + ghosts;
    }
  }
  for (auto transmission : root.children("transmission")) {
    auto joint = transmission.child("joint"),
         act = transmission.child("actuator");
    std::string target = Attr(joint, "name");
    if (!joint_names.count(target)) {
      active_reader->Warn("URDF transmission has unknown joint " + target);
      continue;
    }
    if (opts.profile != "legacy") {
      // Neither URDF importer of Newton nor urdf-usd-converter infers a
      // controller from a transmission (and Newton's add_usd rejects
      // actuators on joints that are not single-DOF): keep it as urdf:*
      // metadata on the joint.
      Json* owner = nullptr;
      for (auto& j : joints)
        if (j.contains("name") && j["name"].is_string() && j["name"].get<std::string>() == target)
          owner = &j;
      if (!owner) continue;
      if (owner->contains("transmission")) {
        active_reader->Warn("URDF joint " + target + " has several transmissions; keeping the first",
                            "urdf.transmission_duplicate");
        continue;
      }
      Json t = Json::object();
      auto text = [](const pugi::xml_node& n, const char* tag) {
        std::string v = n.child(tag).text().as_string();
        const size_t b = v.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string();
        return v.substr(b, v.find_last_not_of(" \t\r\n") - b + 1);
      };
      if (HasAttr(transmission, "name")) t["name"] = Attr(transmission, "name");
      std::string type = text(transmission, "type");
      if (type.empty()) type = Attr(transmission, "type");
      if (!type.empty()) t["type"] = type;
      if (act && HasAttr(act, "name")) t["actuator"] = Attr(act, "name");
      std::string hardware = text(joint, "hardwareInterface");
      if (hardware.empty()) hardware = text(act, "hardwareInterface");
      if (!hardware.empty()) t["hardwareInterface"] = hardware;
      if (act && act.child("mechanicalReduction"))
        t["mechanicalReduction"] = act.child("mechanicalReduction").text().as_double(1.0);
      (*owner)["transmission"] = std::move(t);
      continue;
    }
    Json a{{"name", Attr(act, "name", target + "_actuator")},
           {"joint", target},
           {"control", "pd"}};
    active_reader->Warn(
        "URDF transmission hardware/reduction is retained as metadata; no "
        "controller dynamics are inferred");
    a["mechanicalReduction"] =
        act.child("mechanicalReduction").text().as_double(1.0);
    actuators.push_back(std::move(a));
  }
  for (auto& m : *active_materials) materials.push_back(m);
  *payload = Json{{"name", Attr(root, "name", "robot")},
                  {"upAxis", opts.up_axis}, {"profile", opts.profile},
                  {"layout", opts.layout}, {"sourceFormat", "urdf"},
                  {"gravity", opts.up_axis == "Z" ? Json::array({0, 0, -1})
                                                  : Json::array({0, -1, 0})}};
  // Assign the large arrays by move (a braced list would copy them).
  (*payload)["links"] = std::move(links);
  (*payload)["joints"] = std::move(joints);
  (*payload)["materials"] = std::move(materials);
  (*payload)["actuators"] = std::move(actuators);
  if (opts.profile != "legacy") {
    // Robot-level extensions (<gazebo>, <ros2_control>, ...).
    // (Root attributes are name/version and xmlns declarations.)
    Json custom = URDFCustomJson(root, {"link", "joint", "material", "transmission"}, {});
    if (custom.contains("elements"))
      (*payload)["urdfCustom"] = Json{{"elements", std::move(custom["elements"])}};
  }
  return true;
}

}  // namespace

bool BuildPayload(const std::string& text, const std::string& source,
                  const Options& options, Result* result, std::string* err) {
  Options effective = options;
  ImportSession session(&effective, source, options.format, err);
  effective.report->input_bytes = text.size();
  if (!result) {
    if (err) *err = "Null import result";
    return false;
  }
  *result = Result{};
  result->report = effective.report;
  if (text.size() > options.max_input || options.max_memory == 0) {
    if (err) *err = "Import input or memory limit exceeded";
    return false;
  }
  if (options.max_depth == 0 || options.max_depth > 1024) {
    if (err) *err = "Import depth limit must be in [1, 1024]";
    return false;
  }
  if (options.up_axis != "Z" && options.up_axis != "Y") {
    if (err) *err = "up_axis must be Z or Y";
    return false;
  }
  if (options.profile != "newton" && options.profile != "openusd" &&
      options.profile != "legacy") {
    if (err) *err = "profile must be newton, openusd or legacy";
    return false;
  }
  if (ToLower(options.format) == "obj" ||
      (ToLower(options.format) == "auto" &&
       ToLower(fs::path(source).extension().string()) == ".obj"))
  {
    if (!BuildOBJPayload(text, source, effective, result, err)) return false;
    session.Succeed();
    return true;
  }
  Reader reader(effective);
  if (!reader.Charge(text.size(), err)) return false;
  // Filesystem dependencies may refer back to a parent document's directory.
  reader.root_source = source;
  reader.allow_unanchored = !source.empty();
  struct Scope {
    Reader* previous;
    Json* previous_materials;
    std::string previous_world = active_world_name;
    Scope(Reader* r, Json* m)
        : previous(active_reader), previous_materials(active_materials) {
      active_reader = r;
      active_materials = m;
    }
    ~Scope() {
      active_reader = previous;
      active_materials = previous_materials;
      active_world_name = previous_world;
    }
  };
  Json obj_materials = Json::array();
  Scope scope(&reader, &obj_materials);
  std::string format = ToLower(options.format);
  if (format == "auto" &&
      ToLower(fs::path(source).extension().string()) == ".obj")
    format = "obj";
  bool ok = false;
  {
    if (!reader.Charge(text.size() * 8, err)) return false;
    pugi::xml_document doc;
    auto parsed = doc.load_buffer(text.data(), text.size());
    if (!parsed) {
      if (err) *err = std::string("XML parse failed: ") + parsed.description();
      return false;
    }
    // Reject DTDs and entities rather than accepting a misleading partial
    // parse.
    if (text.find("<!DOCTYPE") != std::string::npos ||
        text.find("<!ENTITY") != std::string::npos) {
      if (err) *err = "XML DTD and entity declarations are unsupported";
      return false;
    }
    if (!ValidateXMLDepth(doc, options.max_depth, source, err)) return false;
    auto root = doc.document_element();
    std::string tag = root.name();
    if (format == "auto")
      format = tag == "mujoco" ? "mjcf" : tag == "robot" ? "urdf" : "unknown";
    effective.report->format = format;
    if (format == "mjcf" && tag == "mujoco") {
      Stats stats;
      ok = BuildMujocoPayload(text, fs::path(source), effective, &result->payload,
                              &stats, err);
    } else if (format == "urdf" && tag == "robot")
      ok = BuildURDF(root, source, effective, &result->payload, err);
    else {
      if (err) *err = "Expected OBJ, <robot> URDF or <mujoco> MJCF input";
      return false;
    }
  }
  if (!ok) return false;
  if (!reader.failure.empty()) {
    if (err) *err = reader.failure;
    return false;
  }
  // Geometry index tables refer to the shared OBJ material prefix.
  if (format != "obj" && !obj_materials.empty()) {
    Json ordered = Json::array();
    for (auto& m : obj_materials)
      if (!m.is_null()) ordered.push_back(m);
    for (auto& m : result->payload["materials"])
      if (m["name"].get<std::string>().rfind("obj_", 0) != 0)
        ordered.push_back(std::move(m));
    result->payload["materials"] = std::move(ordered);
  }
  Json material_names = Json::array();
  for (const Json& m : result->payload["materials"])
    material_names.push_back(m["name"]);
  for (Json& link : result->payload["links"])
    for (const char* kind : {"visuals", "collisions"})
      for (Json& mesh : link[kind]) {
        if (mesh.contains("geometry"))
          mesh["geometry"]["materialNames"] = material_names;
        if (format == "obj") mesh["renderOnly"] = true;
      }
  if (result->payload.contains("timestep")) {
    double dt = result->payload["timestep"].get<double>();
    if (!std::isfinite(dt) || dt <= 0 || 1.0 / dt > double(INT32_MAX)) {
      if (err) *err = "Invalid MJCF timestep";
      return false;
    }
  }
  if (!GatherTextures(reader, result, err)) return false;
  if (!reader.preserved.empty()) result->payload["unsupported"] = std::move(reader.preserved);
  result->warnings = reader.warnings;
  result->memory_estimate = reader.bytes;
  result->meshes = std::move(reader.meshes);
  for (const char* key : {"links", "joints", "materials", "actuators", "tendons",
                         "equalities", "sites", "sensors", "contactPairs", "filteredPairs", "keyframes",
                         "deformables"})
    if (result->payload[key].is_array()) {
      effective.report->counts[key] = uint64_t(result->payload[key].size());
      if (!result->payload[key].empty()) effective.report->Feature(key, "imported", source);
    }
  session.Succeed();
  return true;
}

bool BuildPayloadFile(const std::string& filename, const Options& options,
                      Result* result, std::string* err) {
  Options effective = options;
  ImportSession session(&effective, filename, options.format, err);
  if (ToLower(options.format) == "obj" || (ToLower(options.format) == "auto" &&
      ToLower(fs::path(filename).extension().string()) == ".obj"))
  {
    if (!BuildOBJPayloadFile(filename, effective, result, err)) return false;
    session.Succeed();
    return true;
  }
  Reader reader(effective);
  std::vector<uint8_t> bytes;
  std::string resolved;
  if (!reader.Read(filename, "", &bytes, &resolved, err)) return false;
  std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (!BuildPayload(text, resolved, effective, result, err)) return false;
  session.Succeed();
  return true;
}

}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
