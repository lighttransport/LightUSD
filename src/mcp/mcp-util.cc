// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
#include "mcp-util.hh"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <random>

#include "c-stage-bridge.hh"
#include "next/layer/prim-spec.hh"
#include "next/reader/usda-reader.hh"
#include "next/stage/stage.hh"
#include "next/types/value.hh"
#include "next/writer/value-printer.hh"

namespace lightusd {
namespace mcp {

namespace {

constexpr char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int B64Index(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

// ---- USDA literal -> JSON --------------------------------------------------
struct LiteralReader {
  const char* p;
  const char* end;
  void ws() {
    while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
  }
  bool parse(json* out, int depth = 0) {
    if (depth > 64) return false;
    ws();
    if (p >= end) return false;
    const char c = *p;
    if (c == '(' || c == '[') {
      const char close = c == '(' ? ')' : ']';
      ++p;
      json arr = json::array();
      ws();
      if (p < end && *p == close) {
        ++p;
        *out = std::move(arr);
        return true;
      }
      while (true) {
        json item;
        if (!parse(&item, depth + 1)) return false;
        arr.push_back(std::move(item));
        ws();
        if (p < end && *p == ',') {
          ++p;
          ws();
          if (p < end && *p == close) {  // trailing comma
            ++p;
            break;
          }
          continue;
        }
        if (p < end && *p == close) {
          ++p;
          break;
        }
        return false;
      }
      *out = std::move(arr);
      return true;
    }
    if (c == '"' || c == '\'') return parse_string(out);
    if (c == '@') return parse_asset(out);
    if (c == '<') {
      const char* b = ++p;
      while (p < end && *p != '>') ++p;
      if (p >= end) return false;
      *out = std::string(b, p);
      ++p;
      return true;
    }
    // Number or bare word.
    const char* b = p;
    while (p < end && *p != ',' && *p != ')' && *p != ']' &&
           !std::isspace(static_cast<unsigned char>(*p))) {
      ++p;
    }
    const std::string word(b, p);
    if (word.empty()) return false;
    if (word == "true") { *out = true; return true; }
    if (word == "false") { *out = false; return true; }
    if (word == "None") { *out = nullptr; return true; }
    char* num_end = nullptr;
    const double d = std::strtod(word.c_str(), &num_end);
    if (num_end && *num_end == '\0' && std::isfinite(d)) {
      if (word.find_first_of(".eE") == std::string::npos &&
          d >= -9.0e15 && d <= 9.0e15) {
        *out = static_cast<int64_t>(d);
      } else {
        *out = d;
      }
      return true;
    }
    *out = word;  // inf / nan / tokens stay textual
    return true;
  }
  bool parse_string(json* out) {
    const char q = *p;
    const bool triple = end - p >= 3 && p[1] == q && p[2] == q;
    p += triple ? 3 : 1;
    std::string s;
    while (p < end) {
      if (triple ? (end - p >= 3 && p[0] == q && p[1] == q && p[2] == q)
                 : *p == q) {
        p += triple ? 3 : 1;
        *out = std::move(s);
        return true;
      }
      if (*p == '\\' && p + 1 < end) {
        ++p;
        switch (*p) {
          case 'n': s += '\n'; break;
          case 't': s += '\t'; break;
          case 'r': s += '\r'; break;
          default: s += *p; break;
        }
        ++p;
        continue;
      }
      s += *p++;
    }
    return false;
  }
  bool parse_asset(json* out) {
    const bool triple = end - p >= 3 && p[1] == '@' && p[2] == '@';
    p += triple ? 3 : 1;
    const char* b = p;
    while (p < end) {
      if (triple ? (end - p >= 3 && p[0] == '@' && p[1] == '@' && p[2] == '@')
                 : *p == '@') {
        *out = std::string(b, p);
        p += triple ? 3 : 1;
        return true;
      }
      ++p;
    }
    return false;
  }
};

// ---- JSON -> USDA literal --------------------------------------------------
std::string QuoteString(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n') { out += "\\n"; continue; }
    out += c;
  }
  out += '"';
  return out;
}

bool ScalarLiteral(const json& j, const std::string& elem, int depth,
                   std::string* out, std::string* err) {
  if (depth > 8) {
    *err = "value nesting is too deep";
    return false;
  }
  if (j.is_array()) {
    std::string s = "(";
    for (size_t i = 0; i < j.size(); ++i) {
      if (i) s += ", ";
      std::string item;
      if (!ScalarLiteral(j[i], elem, depth + 1, &item, err)) return false;
      s += item;
    }
    *out = s + ")";
    return true;
  }
  if (j.is_boolean()) {
    *out = j.get<bool>() ? "1" : "0";
    return true;
  }
  if (j.is_number()) {
    *out = j.dump();
    return true;
  }
  if (j.is_null()) {
    *out = "None";
    return true;
  }
  if (j.is_string()) {
    const std::string s = j.get<std::string>();
    if (elem == "asset") *out = "@" + s + "@";
    else if (elem == "string" || elem == "token" || elem == "pathExpression")
      *out = QuoteString(s);
    else *out = s;  // numeric words (inf, nan) or a raw literal
    return true;
  }
  *err = "unsupported JSON value for type " + elem;
  return false;
}

}  // namespace

bool DecodeBase64(const std::string& in, std::string* out, std::string* err) {
  if (in.size() > kMaxBase64InputBytes) {
    if (err) *err = "Input base64 payload is too large.";
    return false;
  }
  std::string s;
  s.reserve(in.size() / 4 * 3);
  uint32_t buf = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' ) break;
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    const int v = B64Index(c);
    if (v < 0) {
      if (err) *err = "Invalid base64 data.";
      return false;
    }
    buf = (buf << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      s.push_back(static_cast<char>((buf >> bits) & 0xFF));
    }
    if (s.size() > kMaxBase64DecodedBytes) {
      if (err) *err = "Decoded payload exceeds size limit.";
      return false;
    }
  }
  *out = std::move(s);
  return true;
}

std::string EncodeBase64(const uint8_t* data, size_t size) {
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t n = (uint32_t(data[i]) << 16) |
                       (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0) |
                       (i + 2 < size ? uint32_t(data[i + 2]) : 0);
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += i + 1 < size ? kB64[(n >> 6) & 63] : '=';
    out += i + 2 < size ? kB64[n & 63] : '=';
  }
  return out;
}

std::string GenerateUUID() {
  static std::mt19937_64 rng{std::random_device{}()};
  const uint64_t a = rng(), b = rng();
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%08x-%04x-4%03x-%04x-%012llx",
                static_cast<unsigned>(a >> 32),
                static_cast<unsigned>((a >> 16) & 0xFFFF),
                static_cast<unsigned>(a & 0x0FFF),
                static_cast<unsigned>(((b >> 48) & 0x3FFF) | 0x8000),
                static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
  return buf;
}

const next::Stage* NativeStage(const StageRef& ref) {
  return ref ? lightusd_internal::BorrowNativeStage(ref.stage) : nullptr;
}

bool RequireStage(const Context& ctx, std::string* err) {
  if (!ctx.stage) {
    *err = "No stage loaded";
    return false;
  }
  return true;
}

std::string FindLayerUUID(const Context& ctx, const std::string& name) {
  for (const auto& it : ctx.layers) {
    if (it.second.name == name) return it.first;
  }
  return {};
}

bool FindPrimSite(Context& ctx, const std::string& path, PrimSite* site,
                  std::string* err) {
  if (path.empty() || path[0] != '/') {
    *err = "Invalid path: " + path;
    return false;
  }
  if (const next::Stage* s = NativeStage(ctx.stage)) {
    if (s->GetPrimAtPath(path)) {
      site->stage = ctx.stage.stage;
      site->layer.clear();
      return true;
    }
  }
  for (auto& it : ctx.layers) {
    const next::Stage* s = NativeStage(it.second.stage);
    if (s && s->GetPrimAtPath(path)) {
      site->stage = it.second.stage.stage;
      site->layer = it.second.name;
      return true;
    }
  }
  *err = "Prim not found in the session stage or any loaded layer: " + path;
  return false;
}

json ValueToJSON(const next::Value& value, const std::string& type_name,
                 size_t max_elements) {
  json j;
  j["type"] = type_name.empty() ? std::string("unknown") : type_name;
  next::PrintOptions opts;
  opts.float_precision = 9;
  opts.double_precision = 17;
  if (value.is_array()) {
    const size_t n = value.array_size();
    j["count"] = n;
    if (max_elements && n > max_elements) {
      opts.max_array_elements = max_elements;
      j["truncated"] = true;
    }
  }
  std::string text = next::PrintValue(value, opts);
  // A truncated print ends with ", ...]"; drop the marker before parsing.
  const std::string marker = ", ...";
  const size_t mark = text.rfind(marker);
  std::string parse_text = text;
  if (j.contains("truncated") && mark != std::string::npos) {
    parse_text = text.substr(0, mark) + text.substr(mark + marker.size());
  }
  LiteralReader r{parse_text.data(), parse_text.data() + parse_text.size()};
  json v;
  if (r.parse(&v)) {
    r.ws();
    j["value"] = r.p == r.end ? v : json(text);
  } else {
    j["value"] = text;
  }
  j["usda"] = text;
  return j;
}

bool ValueFromJSON(const json& j, std::string* type_name, next::Value* out,
                   std::string* err) {
  if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
    *err = "value must be an object {\"type\": \"<usd type>\", \"value\": ...}";
    return false;
  }
  const std::string type = j["type"].get<std::string>();
  const bool is_array = type.size() > 2 && type.compare(type.size() - 2, 2, "[]") == 0;
  const std::string elem = is_array ? type.substr(0, type.size() - 2) : type;
  for (char c : type) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '[' || c == ']')) {
      *err = "invalid type name: " + type;
      return false;
    }
  }
  const json v = j.contains("value") ? j["value"] : json();
  std::string literal;
  if (v.is_null()) {
    literal = "None";
  } else if (v.is_string() && elem != "string" && elem != "token" &&
             elem != "asset" && elem != "pathExpression") {
    literal = v.get<std::string>();  // USDA literal escape hatch
  } else if (is_array) {
    if (!v.is_array()) {
      *err = "array type " + type + " needs a JSON array value";
      return false;
    }
    literal = "[";
    for (size_t i = 0; i < v.size(); ++i) {
      if (i) literal += ", ";
      std::string item;
      if (!ScalarLiteral(v[i], elem, 0, &item, err)) return false;
      literal += item;
    }
    literal += "]";
  } else if (!ScalarLiteral(v, elem, 0, &literal, err)) {
    return false;
  }

  const std::string doc = "#usda 1.0\ndef \"MCPValue\"\n{\n    " + type +
                          " v = " + literal + "\n}\n";
  next::LoadResult parsed = next::LoadUSDAFromString(doc);
  const next::Layer* layer = parsed.stage.GetRootLayer();
  const next::PrimSpec* spec =
      parsed.success && layer ? layer->prim_at_path(std::string("/MCPValue"))
                              : nullptr;
  const next::Value* value = spec ? spec->property_value("v") : nullptr;
  if (!value) {
    *err = "cannot parse a " + type + " value from " + literal +
           (parsed.error_summary.empty() ? "" : ": " + parsed.error_summary);
    return false;
  }
  *out = value->is_lazy() ? value->materialized_copy() : *value;
  *type_name = type;
  return true;
}

const char* SpecifierName(int specifier) {
  switch (specifier) {
    case 0: return "def";
    case 1: return "over";
    case 2: return "class";
    default: return "invalid";
  }
}

std::string PropertyTypeName(const next::UsdPrim& prim, const std::string& name) {
  const next::PrimSpec* spec = prim.GetPrimSpec();
  if (spec) {
    if (const std::string* t = spec->property_type_name(name)) return *t;
  }
  if (const next::Value* v = prim.GetPropertyValue(name)) {
    return next::PrintTypeName(v->type_id(), v->is_array());
  }
  return {};
}

json PrimToJSON(const next::UsdPrim& prim, lightusd_stage* stage,
                int max_depth, bool include_attributes) {
  json j;
  j["name"] = prim.GetName();
  j["type"] = prim.GetTypeName();
  j["specifier"] = SpecifierName(static_cast<int>(prim.GetSpecifier()));
  if (include_attributes) {
    const lightusd_prim cprim =
        lightusd_stage_prim_at_path(stage, prim.GetPath().str().c_str());
    json attrs = json::array();
    for (const std::string& name : prim.GetPropertyNames()) {
      json a;
      a["name"] = name;
      a["type"] = PropertyTypeName(prim, name);
      const uint16_t flags = lightusd_prim_property_flags(cprim, name.c_str());
      a["variability"] = (flags & LIGHTUSD_PROP_UNIFORM) ? "Uniform" : "Varying";
      a["custom"] = (flags & LIGHTUSD_PROP_CUSTOM) != 0;
      a["hasValue"] = prim.GetPropertyValue(name) != nullptr;
      a["timeSampled"] = prim.HasTimeSamples(name);
      attrs.push_back(std::move(a));
    }
    for (const std::string& name : prim.GetRelationshipNames()) {
      json r;
      r["name"] = name;
      r["type"] = "rel";
      attrs.push_back(std::move(r));
    }
    if (!attrs.empty()) j["attributes"] = std::move(attrs);
  }
  const size_t child_count = prim.GetChildCount();
  if (max_depth != 0 && child_count) {
    json children = json::array();
    const int next_depth = max_depth > 0 ? max_depth - 1 : max_depth;
    for (const next::UsdPrim& child : prim.GetChildren()) {
      children.push_back(PrimToJSON(child, stage, next_depth, include_attributes));
    }
    j["children"] = std::move(children);
  } else if (child_count) {
    j["childCount"] = child_count;
  }
  return j;
}

json PrimMetaToJSON(const next::UsdPrim& prim) {
  const next::PrimSpecMeta& m = prim.GetMeta();
  json j = json::object();
  j["active"] = m.active;
  if (m.hidden_authored) j["hidden"] = m.hidden;
  if (m.instanceable_authored) j["instanceable"] = m.instanceable;
  if (m.kindAuthored()) j["kind"] = m.kind();
  if (m.doc_authored()) j["documentation"] = m.doc();
  if (m.comment_authored()) j["comment"] = m.comment();
  if (m.displayNameAuthored()) j["displayName"] = m.displayName();
  if (!m.apiSchemas().empty()) j["apiSchemas"] = m.apiSchemas();
  auto arcs = [&](const char* key, const std::vector<std::string>& v) {
    if (!v.empty()) j[key] = v;
  };
  arcs("references", m.references);
  arcs("payloads", m.payloads);
  arcs("inherits", m.inherits);
  arcs("specializes", m.specializes);
  if (!m.variantSets().empty()) {
    json sets = json::array();
    for (const auto& vs : m.variantSets()) sets.push_back(vs.name);
    j["variantSets"] = std::move(sets);
  }
  if (!m.variantSelections().empty()) {
    json sel = json::object();
    for (const auto& kv : m.variantSelections()) sel[kv.first] = kv.second;
    j["variantSelection"] = std::move(sel);
  }
  if (m.customDataAuthored()) j["customData"] = ValueToJSON(m.customData(), "dictionary");
  if (m.assetInfoAuthored()) j["assetInfo"] = ValueToJSON(m.assetInfo(), "dictionary");
  return j;
}

void SetTextContent(json& result, const std::string& text, const char* mime_type) {
  json content;
  content["type"] = "text";
  if (mime_type) content["mimeType"] = mime_type;
  content["text"] = text;
  result["content"] = json::array();
  result["content"].push_back(std::move(content));
}

}  // namespace mcp
}  // namespace lightusd
