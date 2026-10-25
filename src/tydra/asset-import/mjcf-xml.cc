// SPDX-License-Identifier: Apache-2.0
// XML and string helpers, MJCF <include> expansion.
#include "mjcf-internal.hh"

namespace lightusd {
namespace tydra {
namespace import_asset {
namespace mjcf_detail {
std::string ToLower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

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
                 const std::string& fallback) {
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
    bool approximated) {
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
  if (!active_reader->ChargeArray(xml.size(), 8, err)) return false;
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
  // Depth-first over the tree with an explicit stack (no recursion: depth is
  // attacker-controlled). Each frame holds the next child to visit, taken
  // before an <include> is replaced, so spliced copies are not revisited.
  const auto depth_exceeded = [&]() {
    if (err) *err = "XML depth limit exceeded";
    if (active_reader->options.report)
      active_reader->options.report->Diagnostic("error", "import.depth_limit", "XML depth limit exceeded", source);
    return false;
  };
  struct Frame {
    pugi::xml_node parent, node;
    size_t depth;
  };
  std::vector<Frame> stack{{root, root.first_child(), 0}};
  while (!stack.empty()) {
    if (!stack.back().node) {
      stack.pop_back();
      continue;
    }
    pugi::xml_node parent = stack.back().parent;
    const pugi::xml_node node = stack.back().node;
    const size_t depth = stack.back().depth;
    stack.back().node = node.next_sibling();
    const std::string tag = node.name();
    // The newton profile converts <flexcomp> into a soft body later.
    const std::string flex_type = ToLower(node.attribute("type").as_string("grid"));
    const bool newton = active_reader->options.profile == "newton";
    const bool soft_body = tag == "flexcomp" && newton && IsSupportedFlexcompType(flex_type);
    // Raw <deformable><flex body=... element=...> becomes a soft body too
    // (CollectMujocoRawFlexesJson); <skin> and other children stay reported.
    const bool raw_flex = tag == "flex" && newton && std::string(parent.name()) == "deformable" &&
                          node.attribute("body") && node.attribute("element");
    // Non-legacy profiles import <skin> as UsdSkel (AddMujocoSkinsJson).
    const bool legacy = active_reader->options.profile == "legacy";
    const bool flex_container =
        tag == "deformable" && (newton || (!legacy && node.child("skin")));
    const bool skin = tag == "skin" && !legacy;
    if (!soft_body && !raw_flex && !flex_container && !skin &&
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
      // Each nested include file is one ExpandIncludes call: bound the chain.
      if (seen->size() > kMaxIncludeNesting) {
        if (err) *err = "MJCF includes are nested too deeply: " + resolved;
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
    } else {
      if (depth + 1 > active_reader->options.max_depth) return depth_exceeded();
      stack.push_back({node, node.first_child(), depth + 1});
    }
  }
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
}  // namespace mjcf_detail
}  // namespace import_asset
}  // namespace tydra
}  // namespace lightusd
