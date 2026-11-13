// SPDX-License-Identifier: Apache 2.0
// Copyright 2026-Present Light Transport Entertainment Inc.

#include "core/mtlx-resolve.hh"

#include <cstring>
#include <limits>

#include "external/fast_float/include/fast_float/fast_float.h"

namespace lightusd {
namespace mtlx {

namespace {

inline bool IsAsciiAlpha(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
inline bool IsAsciiDigit(char c) { return c >= '0' && c <= '9'; }
inline bool IsNameChar(char c) {
  return IsAsciiAlpha(c) || IsAsciiDigit(c) || c == '_';
}
inline bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool MtlxChannelConversionMatches(const MtlxElem &input,
                                  const std::string &source_type,
                                  const std::string &input_type) {
  const std::string channels = input.AttrOr("channels");
  if (channels.empty() || source_type != "float") return false;
  const size_t expected_count = input_type == "color3" || input_type == "vector3"
                                    ? 3
                                    : input_type == "color4" || input_type == "vector4"
                                          ? 4
                                          : input_type == "vector2" ? 2 : 0;
  if (expected_count == 0 || channels.size() != expected_count) return false;
  const char first = channels[0];
  if (first != 'r' && first != 'g' && first != 'b' && first != 'a' &&
      first != 'x' && first != 'y' && first != 'z' && first != 'w')
    return false;
  for (char channel : channels) {
    if (channel != first) return false;
  }
  return true;
}

std::string Trim(const std::string &s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && IsSpace(s[b])) b++;
  while (e > b && IsSpace(s[e - 1])) e--;
  return s.substr(b, e - b);
}

// Split on a separator, keeping empty fields ("" -> [""]).
std::vector<std::string> SplitOn(char sep, const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

std::vector<std::string> Fields(const std::string &s) {
  std::vector<std::string> out = SplitOn(',', s);
  for (std::string &f : out) f = Trim(f);
  return out;
}

std::vector<std::string> ArrayFields(const std::string &s) {
  if (Trim(s).empty()) return {};
  return Fields(s);
}

bool AllDigits(const std::string &s, size_t b, size_t e) {
  if (b >= e) return false;
  for (size_t i = b; i < e; i++) {
    if (!IsAsciiDigit(s[i])) return false;
  }
  return true;
}

// Optional sign, one or more digits; whole token.
bool ParseIntToken(const std::string &s, int64_t *out) {
  size_t b = 0;
  bool neg = false;
  if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
    neg = s[0] == '-';
    b = 1;
  }
  if (!AllDigits(s, b, s.size())) return false;
  // Accumulate as a negative number so INT64_MIN is representable.
  int64_t acc = 0;
  const int64_t kMin = std::numeric_limits<int64_t>::min();
  for (size_t i = b; i < s.size(); i++) {
    const int64_t d = s[i] - '0';
    if (acc < (kMin + d) / 10) return false;
    acc = acc * 10 - d;
  }
  if (!neg) {
    if (acc == kMin) return false;
    acc = -acc;
  }
  *out = acc;
  return true;
}

// [+-]? digits? ('.' digits?)? ([eE] [+-]? digits)? with at least one mantissa
// digit; whole token.
bool ParseFloatToken(const std::string &s, double *out) {
  size_t b = 0;
  bool neg = false;
  if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
    neg = s[0] == '-';
    b = 1;
  }
  size_t mant_end = b;
  while (mant_end < s.size() && s[mant_end] != 'e' && s[mant_end] != 'E') {
    mant_end++;
  }
  size_t dot = b;
  while (dot < mant_end && s[dot] != '.') dot++;
  const std::string ip = s.substr(b, dot - b);
  const std::string fp = dot < mant_end ? s.substr(dot + 1, mant_end - dot - 1)
                                        : std::string();
  const std::string digits = ip + fp;
  if (!AllDigits(digits, 0, digits.size())) return false;
  std::string norm;
  if (neg) norm.push_back('-');
  norm += ip.empty() ? "0" : ip;
  norm.push_back('.');
  norm += fp.empty() ? "0" : fp;
  if (mant_end < s.size()) {
    size_t eb = mant_end + 1;
    bool eneg = false;
    if (eb < s.size() && (s[eb] == '-' || s[eb] == '+')) {
      eneg = s[eb] == '-';
      eb++;
    }
    if (!AllDigits(s, eb, s.size())) return false;
    norm.push_back('e');
    if (eneg) norm.push_back('-');
    norm.append(s, eb, std::string::npos);
  }
  double v = 0.0;
  const char *first = norm.data();
  const char *last = norm.data() + norm.size();
  // Out-of-range literals saturate (inf / 0); the grammar was checked above.
  (void)fast_float::from_chars(first, last, v, fast_float::chars_format::general);
  *out = v;
  return true;
}

bool ParseFloats(const std::vector<std::string> &fields,
                 std::vector<double> *out) {
  out->clear();
  out->reserve(fields.size());
  for (const std::string &f : fields) {
    double v = 0.0;
    if (!ParseFloatToken(f, &v)) return false;
    out->push_back(v);
  }
  return true;
}

// MaterialX.Specification.md "MTLX File Format Definition": "\," and "\;"
// are literal, "\\" is one backslash, an unescaped comma separates.
std::vector<std::string> SplitStringArray(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '\\' && i + 1 < s.size() &&
        (s[i + 1] == ',' || s[i + 1] == ';' || s[i + 1] == '\\')) {
      cur.push_back(s[i + 1]);
      i += 2;
    } else if (c == ',') {
      out.push_back(cur);
      cur.clear();
      i++;
    } else {
      cur.push_back(c);
      i++;
    }
  }
  out.push_back(cur);
  return out;
}

const char *const kBuiltinTypes[] = {
    "integer",      "boolean",      "float",        "color3",
    "color4",       "vector2",      "vector3",      "vector4",
    "matrix33",     "matrix44",     "string",       "filename",
    "integerarray", "floatarray",   "color3array",  "color4array",
    "vector2array", "vector3array", "vector4array", "stringarray"};

}  // namespace

// ---- Names -----------------------------------------------------------------

bool IsValidMtlxName(const std::string &name) {
  if (name.empty()) return false;
  if (!(IsAsciiAlpha(name[0]) || name[0] == '_')) return false;
  for (size_t i = 1; i < name.size(); i++) {
    if (!IsNameChar(name[i])) return false;
  }
  return true;
}

bool IsReferenceValidMtlxName(const std::string &name) {
  for (char c : name) {
    if (!(IsNameChar(c) || c == ':')) return false;
  }
  return true;
}

// ---- Values ----------------------------------------------------------------

size_t MtlxTypeArity(const std::string &type) {
  if (type == "color3" || type == "vector3") return 3;
  if (type == "color4" || type == "vector4") return 4;
  if (type == "vector2") return 2;
  if (type == "matrix33") return 9;
  if (type == "matrix44") return 16;
  return 0;
}

size_t MtlxArrayStride(const std::string &type) {
  if (type == "floatarray") return 1;
  if (type == "vector2array") return 2;
  if (type == "color3array" || type == "vector3array") return 3;
  if (type == "color4array" || type == "vector4array") return 4;
  return 0;
}

bool IsBuiltinMtlxType(const std::string &type) {
  for (const char *t : kBuiltinTypes) {
    if (type == t) return true;
  }
  return false;
}

bool ParseMtlxValue(const std::string &type, const std::string &text,
                    MtlxTypedValue *out) {
  if (!out) return false;
  *out = MtlxTypedValue();
  if (type == "integer") {
    if (!ParseIntToken(Trim(text), &out->i)) return false;
    out->kind = MtlxValueKind::Int;
    return true;
  }
  if (type == "boolean") {
    if (text == "true") {
      out->b = true;
    } else if (text == "false") {
      out->b = false;
    } else {
      return false;
    }
    out->kind = MtlxValueKind::Bool;
    return true;
  }
  if (type == "float") {
    if (!ParseFloatToken(Trim(text), &out->num)) return false;
    out->kind = MtlxValueKind::Num;
    return true;
  }
  if (type == "string" || type == "filename") {
    out->str = text;
    out->kind = MtlxValueKind::Str;
    return true;
  }
  if (type == "integerarray") {
    for (const std::string &f : ArrayFields(text)) {
      int64_t v = 0;
      if (!ParseIntToken(f, &v)) {
        out->ints.clear();
        return false;
      }
      out->ints.push_back(v);
    }
    out->kind = MtlxValueKind::Ints;
    return true;
  }
  if (type == "stringarray") {
    if (!Trim(text).empty()) {
      out->strs = SplitStringArray(text);
      for (std::string &s : out->strs) s = Trim(s);
    }
    out->kind = MtlxValueKind::Strs;
    return true;
  }
  const size_t arity = MtlxTypeArity(type);
  if (arity > 0) {
    if (!ParseFloats(Fields(text), &out->nums) || out->nums.size() != arity) {
      out->nums.clear();
      return false;
    }
    out->kind = MtlxValueKind::Agg;
    return true;
  }
  const size_t stride = MtlxArrayStride(type);
  if (stride > 0) {
    if (!ParseFloats(ArrayFields(text), &out->nums) ||
        out->nums.size() % stride != 0) {
      out->nums.clear();
      return false;
    }
    out->kind = MtlxValueKind::Nums;
    return true;
  }
  return false;
}

bool DefaultMtlxValue(const std::string &type, MtlxTypedValue *out) {
  if (!out) return false;
  *out = MtlxTypedValue();
  if (type == "integer") {
    out->kind = MtlxValueKind::Int;
  } else if (type == "boolean") {
    out->kind = MtlxValueKind::Bool;
  } else if (type == "float") {
    out->kind = MtlxValueKind::Num;
  } else if (type == "matrix33" || type == "matrix44") {
    const size_t n = type == "matrix33" ? 3 : 4;
    out->nums.assign(n * n, 0.0);
    for (size_t i = 0; i < n; i++) out->nums[i * n + i] = 1.0;
    out->kind = MtlxValueKind::Agg;
  } else if (MtlxTypeArity(type) > 0) {
    out->nums.assign(MtlxTypeArity(type), 0.0);
    out->kind = MtlxValueKind::Agg;
  } else if (type == "string" || type == "filename") {
    out->kind = MtlxValueKind::Str;
  } else if (type == "integerarray") {
    out->kind = MtlxValueKind::Ints;
  } else if (type == "stringarray") {
    out->kind = MtlxValueKind::Strs;
  } else if (MtlxArrayStride(type) > 0) {
    out->kind = MtlxValueKind::Nums;
  } else {
    return false;
  }
  return true;
}

std::string LexMtlxValue(const std::string &type, const std::string &text) {
  if (type == "string" || type == "filename") return text;
  std::string out;
  const std::vector<std::string> fields = Fields(text);
  for (size_t i = 0; i < fields.size(); i++) {
    if (i) out.push_back(',');
    out += fields[i];
  }
  return out;
}

// ---- Element tree ----------------------------------------------------------

const std::string *MtlxElem::Attr(const std::string &key) const {
  for (const auto &kv : attrs) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

std::string MtlxElem::AttrOr(const std::string &key,
                             const std::string &fallback) const {
  const std::string *v = Attr(key);
  return v ? *v : fallback;
}

void MtlxElem::SetAttr(const std::string &key, const std::string &value) {
  std::vector<std::pair<std::string, std::string>> kept;
  kept.reserve(attrs.size() + 1);
  for (auto &kv : attrs) {
    if (kv.first != key) kept.push_back(std::move(kv));
  }
  kept.emplace_back(key, value);
  attrs.swap(kept);
}

const MtlxElem *MtlxElem::Child(const std::string &name) const {
  for (const MtlxElem &c : children) {
    if (c.Name() == name) return &c;
  }
  return nullptr;
}

namespace {

struct XmlCursor {
  const std::string &src;
  size_t pos = 0;

  explicit XmlCursor(const std::string &s) : src(s) {}
  bool AtEnd() const { return pos >= src.size(); }
  char Peek(size_t k = 0) const {
    return pos + k < src.size() ? src[pos + k] : '\0';
  }
  bool StartsWith(const char *p) const {
    const size_t n = std::strlen(p);
    return src.compare(pos, n, p) == 0 && pos + n <= src.size();
  }
  void SkipWs() {
    while (!AtEnd() && IsSpace(src[pos])) pos++;
  }
  void SkipUntil(const char *p) {
    const size_t at = src.find(p, pos);
    pos = at == std::string::npos ? src.size() : at + std::strlen(p);
  }
  // Whitespace, comments, processing instructions, DOCTYPE, text content.
  void SkipMisc() {
    for (;;) {
      SkipWs();
      if (StartsWith("<!--")) {
        SkipUntil("-->");
      } else if (StartsWith("<?")) {
        SkipUntil("?>");
      } else if (StartsWith("<!")) {
        SkipUntil(">");
      } else if (!AtEnd() && src[pos] != '<') {
        pos++;
      } else {
        return;
      }
    }
  }
  std::string ReadName() {
    const size_t b = pos;
    while (!AtEnd()) {
      const char c = src[pos];
      if (!(IsNameChar(c) || c == ':' || c == '-' || c == '.')) break;
      pos++;
    }
    return src.substr(b, pos - b);
  }
};

std::string DecodeEntities(const std::string &s) {
  static const struct {
    const char *name;
    char c;
  } kEntities[] = {{"quot;", '"'}, {"amp;", '&'}, {"apos;", '\''},
                   {"lt;", '<'},   {"gt;", '>'}};
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '&') {
      bool hit = false;
      for (const auto &e : kEntities) {
        const size_t n = std::strlen(e.name);
        if (s.compare(i + 1, n, e.name) == 0) {
          out.push_back(e.c);
          i += 1 + n;
          hit = true;
          break;
        }
      }
      if (hit) continue;
    }
    out.push_back(s[i]);
    i++;
  }
  return out;
}

// Reads "<tag attr=... " up to but not including '>' or "/>".
bool ReadStartTag(XmlCursor &c, MtlxElem *e, std::string *err) {
  if (c.Peek() != '<') {
    *err = "expected '<' at offset " + std::to_string(c.pos);
    return false;
  }
  c.pos++;
  e->tag = c.ReadName();
  if (e->tag.empty()) {
    *err = "bad element name at offset " + std::to_string(c.pos);
    return false;
  }
  for (;;) {
    c.SkipWs();
    if (c.AtEnd()) {
      *err = "unexpected end in tag";
      return false;
    }
    if (c.Peek() == '>' || c.Peek() == '/') return true;
    const std::string key = c.ReadName();
    if (key.empty()) {
      *err = "bad attribute at offset " + std::to_string(c.pos);
      return false;
    }
    c.SkipWs();
    if (c.Peek() != '=') {
      *err = "expected '=' after attribute " + key;
      return false;
    }
    c.pos++;
    c.SkipWs();
    const char q = c.Peek();
    if (q != '"' && q != '\'') {
      *err = "expected quoted value for attribute " + key;
      return false;
    }
    c.pos++;
    const size_t close = c.src.find(q, c.pos);
    if (close == std::string::npos) {
      *err = "unterminated attribute value";
      return false;
    }
    e->attrs.emplace_back(key,
                          DecodeEntities(c.src.substr(c.pos, close - c.pos)));
    c.pos = close + 1;
  }
}

const size_t kMaxXmlDepth = 1024;

}  // namespace

bool ParseMtlxXml(const std::string &xml, MtlxElem *root, std::string *err) {
  std::string local_err;
  if (!err) err = &local_err;
  if (!root) return false;
  XmlCursor c(xml);
  c.SkipMisc();
  if (c.AtEnd()) {
    *err = "no root element";
    return false;
  }
  // Explicit stack of open elements; a closed element is moved into its
  // parent's child list.
  std::vector<MtlxElem> open;
  bool done = false;
  MtlxElem result;
  while (!done) {
    MtlxElem e;
    if (!ReadStartTag(c, &e, err)) return false;
    bool leaf = false;
    if (c.StartsWith("/>")) {
      c.pos += 2;
      leaf = true;
    } else if (c.Peek() == '>') {
      c.pos++;
    } else {
      *err = "malformed start tag <" + e.tag + ">";
      return false;
    }
    if (leaf) {
      if (open.empty()) {
        result = std::move(e);
        done = true;
      } else {
        open.back().children.push_back(std::move(e));
      }
    } else {
      if (open.size() >= kMaxXmlDepth) {
        *err = "maximum element depth exceeded";
        return false;
      }
      open.push_back(std::move(e));
    }
    // Consume any run of end tags.
    while (!done && !open.empty()) {
      c.SkipMisc();
      if (c.AtEnd()) {
        *err = "unterminated element <" + open.back().tag + ">";
        return false;
      }
      if (!c.StartsWith("</")) break;
      c.pos += 2;
      const std::string close = c.ReadName();
      if (close != open.back().tag) {
        *err = "mismatched end tag for <" + open.back().tag + ">";
        return false;
      }
      c.SkipWs();
      if (c.Peek() != '>') {
        *err = "malformed end tag for <" + open.back().tag + ">";
        return false;
      }
      c.pos++;
      MtlxElem closed = std::move(open.back());
      open.pop_back();
      if (open.empty()) {
        result = std::move(closed);
        done = true;
      } else {
        open.back().children.push_back(std::move(closed));
      }
    }
  }
  c.SkipMisc();
  if (!c.AtEnd()) {
    *err = "content after root element";
    return false;
  }
  *root = std::move(result);
  return true;
}

// ---- Scoped attributes -----------------------------------------------------

bool ScopedMtlxAttr(const std::string &key, const MtlxScope &chain,
                    std::string *out) {
  for (const MtlxElem *e : chain) {
    if (!e) continue;
    if (const std::string *v = e->Attr(key)) {
      if (out) *out = *v;
      return true;
    }
  }
  return false;
}

bool ActiveMtlxColorSpace(const MtlxScope &chain, std::string *out) {
  std::string cs;
  if (!ScopedMtlxAttr("colorspace", chain, &cs)) return false;
  if (cs == "data" || cs == "none") return false;
  if (out) *out = cs;
  return true;
}

bool ResolvedMtlxFilename(const MtlxScope &chain, std::string *out) {
  if (chain.empty() || !chain[0]) return false;
  const MtlxElem &inp = *chain[0];
  if (inp.AttrOr("type") != "filename") return false;
  const std::string *v = inp.Attr("value");
  if (!v) return false;
  std::string prefix;
  (void)ScopedMtlxAttr("fileprefix", chain, &prefix);
  const std::string full = prefix + *v;
  // Interface tokens of every enclosing element, nearest first.
  std::vector<std::pair<std::string, std::string>> subs;
  const auto add_tokens = [&subs](const MtlxElem &e) {
    for (const MtlxElem &t : e.children) {
      if (t.tag != "token") continue;
      if (const std::string *tv = t.Attr("value")) {
        subs.emplace_back("[" + t.Name() + "]", *tv);
      }
    }
  };
  const MtlxElem *root = chain.size() > 1 ? chain.back() : nullptr;
  for (size_t i = 1; i < chain.size(); i++) {
    if (!chain[i]) continue;
    add_tokens(*chain[i]);
    // A functional nodegraph also sees the tokens of the nodedef it
    // implements (looked up among the children of the document root).
    if (chain[i]->tag == "nodegraph" && root) {
      if (const std::string *nd = chain[i]->Attr("nodedef")) {
        for (const MtlxElem &c : root->children) {
          if (c.tag == "nodedef" && c.Name() == *nd) {
            add_tokens(c);
            break;
          }
        }
      }
    }
  }
  // One left-to-right pass; substituted text is not rescanned.
  std::string res;
  size_t i = 0;
  while (i < full.size()) {
    bool hit = false;
    for (const auto &kv : subs) {
      if (full.compare(i, kv.first.size(), kv.first) == 0) {
        res += kv.second;
        i += kv.first.size();
        hit = true;
        break;
      }
    }
    if (!hit) res.push_back(full[i++]);
  }
  if (out) *out = res;
  return true;
}

namespace {

// Decimal digit string -> canonical form without leading zeros ("" if the
// text is not a non-empty digit string).
bool CanonNat(const std::string &s, std::string *out) {
  if (!AllDigits(s, 0, s.size())) return false;
  size_t b = 0;
  while (b + 1 < s.size() && s[b] == '0') b++;
  *out = s.substr(b);
  return true;
}

bool ParseNatU64(const std::string &s, uint64_t *out) {
  if (!AllDigits(s, 0, s.size())) return false;
  uint64_t acc = 0;
  for (char ch : s) {
    const uint64_t d = static_cast<uint64_t>(ch - '0');
    if (acc > (std::numeric_limits<uint64_t>::max() - d) / 10) return false;
    acc = acc * 10 + d;
  }
  *out = acc;
  return true;
}

}  // namespace

bool ParseMtlxVersion(const std::string &text, uint64_t *major,
                      uint64_t *minor) {
  const std::vector<std::string> parts = SplitOn('.', text);
  if (parts.size() != 2) return false;
  uint64_t a = 0, b = 0;
  if (!ParseNatU64(parts[0], &a) || !ParseNatU64(parts[1], &b)) return false;
  if (major) *major = a;
  if (minor) *minor = b;
  return true;
}

// ---- XInclude --------------------------------------------------------------

namespace {

const char *const kDistributedKeys[] = {"fileprefix", "geomprefix",
                                        "colorspace", "namespace"};
const size_t kMaxIncludeDepth = 64;

bool IsInclude(const MtlxElem &e) { return e.tag == "xi:include"; }

void AddUnique(std::vector<MtlxElem> *acc, MtlxElem &&c) {
  const std::string name = c.Name();
  for (const MtlxElem &e : *acc) {
    if (e.Name() == name) return;
  }
  acc->push_back(std::move(c));
}

// Children of an expanded library root, distributed and qualified.
void ImportLibrary(std::vector<MtlxElem> *acc, MtlxElem &&lib) {
  for (MtlxElem &c : lib.children) {
    for (const char *k : kDistributedKeys) {
      if (!c.Attr(k)) {
        if (const std::string *v = lib.Attr(k)) c.SetAttr(k, *v);
      }
    }
    const std::string *ns = c.Attr("namespace");
    if (ns && !ns->empty()) {
      const std::string prefix = *ns + ":";
      for (auto &kv : c.attrs) {
        if (kv.first == "name") kv.second = prefix + kv.second;
      }
    }
    AddUnique(acc, std::move(c));
  }
}

struct IncludeFrame {
  MtlxElem root;                  // document being expanded
  std::string href;               // how it was reached ("" for the top)
  std::vector<std::string> incs;  // hrefs of its xi:include children
  size_t next = 0;
  std::vector<MtlxElem> imported;
};

void InitFrame(IncludeFrame *f) {
  for (const MtlxElem &c : f->root.children) {
    if (IsInclude(c)) f->incs.push_back(c.AttrOr("href"));
  }
}

}  // namespace

bool ExpandMtlxIncludes(MtlxElem *root,
                        const MtlxAnchoredIncludeLoader &loader,
                        std::string *err) {
  std::string local_err;
  if (!err) err = &local_err;
  if (!root) return false;
  std::vector<IncludeFrame> stack;
  stack.emplace_back();
  stack.back().root = std::move(*root);
  InitFrame(&stack.back());
  for (;;) {
    IncludeFrame &f = stack.back();
    if (f.next < f.incs.size()) {
      const std::string href = f.incs[f.next++];
      for (size_t i = 1; i < stack.size(); i++) {
        if (stack[i].href == href) {
          *err = "XInclude cycle detected";
          return false;
        }
      }
      std::string text;
      if (!loader || !loader(href, f.href, &text)) {
        *err = "missing include: " + href;
        return false;
      }
      if (stack.size() >= kMaxIncludeDepth) {
        *err = "maximum XInclude depth exceeded";
        return false;
      }
      IncludeFrame child;
      child.href = href;
      if (!ParseMtlxXml(text, &child.root, err)) {
        *err = href + ": " + *err;
        return false;
      }
      InitFrame(&child);
      stack.push_back(std::move(child));
      continue;
    }
    // All includes of this document are imported: add its own children.
    std::vector<MtlxElem> kids = std::move(f.imported);
    for (MtlxElem &c : f.root.children) {
      if (!IsInclude(c)) AddUnique(&kids, std::move(c));
    }
    f.root.children = std::move(kids);
    MtlxElem finished = std::move(f.root);
    stack.pop_back();
    if (stack.empty()) {
      *root = std::move(finished);
      return true;
    }
    ImportLibrary(&stack.back().imported, std::move(finished));
  }
}

bool ExpandMtlxIncludes(MtlxElem *root, const MtlxIncludeLoader &loader,
                        std::string *err) {
  return ExpandMtlxIncludes(
      root,
      MtlxAnchoredIncludeLoader([&loader](const std::string &href,
                                          const std::string &,
                                          std::string *xml) {
        return loader && loader(href, xml);
      }),
      err);
}

// ---- Node definitions ------------------------------------------------------

namespace {

// Qualify `name` with a namespace unless it already carries it.
std::string QualifyWith(const std::string *ns, const std::string &name) {
  if (!ns || ns->empty()) return name;
  const std::string prefix = *ns + ":";
  if (name.compare(0, prefix.size(), prefix) == 0) return name;
  return prefix + name;
}

// Namespace in scope at `e` (its own attribute first).
bool NamespaceAt(const MtlxElem &e, const MtlxScope &outer, std::string *ns) {
  if (const std::string *v = e.Attr("namespace")) {
    *ns = *v;
    return true;
  }
  return ScopedMtlxAttr("namespace", outer, ns);
}

}  // namespace

void MtlxLibrary::AddNodeDef(const MtlxElem &e, const MtlxScope &outer) {
  MtlxNodeDefInfo nd;
  nd.name = e.Name();
  nd.node = e.AttrOr("node");
  std::string ns;
  nd.key = NamespaceAt(e, outer, &ns) ? QualifyWith(&ns, nd.node) : nd.node;
  if (const std::string *v = e.Attr("version")) {
    nd.has_version = true;
    nd.version = *v;
  }
  nd.is_default = e.AttrOr("isdefaultversion") == "true";
  nd.target = e.AttrOr("target");
  if (const std::string *v = e.Attr("inherit")) {
    nd.has_inherit = true;
    nd.inherit = *v;
  }
  for (const MtlxElem &c : e.children) {
    if (c.tag != "input" && c.tag != "output") continue;
    MtlxPort p;
    p.name = c.Name();
    p.type = c.AttrOr("type");
    if (const std::string *v = c.Attr("value")) {
      p.has_value = true;
      p.value = *v;
    }
    (c.tag == "input" ? nd.inputs : nd.outputs).push_back(std::move(p));
  }
  defs_.push_back(std::move(nd));
}

void MtlxLibrary::AddFrom(const MtlxElem &root) {
  const MtlxScope outer = {&root};
  for (const MtlxElem &e : root.children) {
    if (e.tag == "nodedef") AddNodeDef(e, outer);
  }
}

void MtlxLibrary::AddLibrary(const MtlxLibrary &library) {
  defs_.insert(defs_.end(), library.defs_.begin(), library.defs_.end());
}

const MtlxNodeDefInfo *MtlxLibrary::Find(const std::string &name) const {
  for (const MtlxNodeDefInfo &nd : defs_) {
    if (nd.name == name) return &nd;
  }
  return nullptr;
}

std::vector<MtlxPort> MtlxLibrary::ActivePorts(const MtlxNodeDefInfo &def,
                                               bool inputs) const {
  // Walk the inheritance chain (bounded by the library size, so a cyclic
  // `inherit` terminates), then overlay from the most-base definition up.
  std::vector<const MtlxNodeDefInfo *> chain;
  const MtlxNodeDefInfo *cur = &def;
  chain.push_back(cur);
  for (size_t fuel = defs_.size(); fuel > 0 && cur->has_inherit; fuel--) {
    const MtlxNodeDefInfo *base = Find(cur->inherit);
    if (!base) break;
    chain.push_back(base);
    cur = base;
  }
  std::vector<MtlxPort> acc =
      inputs ? chain.back()->inputs : chain.back()->outputs;
  for (size_t i = chain.size() - 1; i-- > 0;) {
    const std::vector<MtlxPort> &own =
        inputs ? chain[i]->inputs : chain[i]->outputs;
    std::vector<MtlxPort> merged = own;
    for (MtlxPort &b : acc) {
      bool overridden = false;
      for (const MtlxPort &o : own) {
        if (o.name == b.name) {
          overridden = true;
          break;
        }
      }
      if (!overridden) merged.push_back(std::move(b));
    }
    acc.swap(merged);
  }
  return acc;
}

std::vector<MtlxPort> MtlxLibrary::ActiveInputs(
    const MtlxNodeDefInfo &def) const {
  return ActivePorts(def, true);
}

std::vector<MtlxPort> MtlxLibrary::ActiveOutputs(
    const MtlxNodeDefInfo &def) const {
  return ActivePorts(def, false);
}

std::string MtlxLibrary::OutType(const MtlxNodeDefInfo &def) const {
  const std::vector<MtlxPort> outs = ActiveOutputs(def);
  // A definition without outputs is invalid; the reference reports color3.
  if (outs.empty()) return "color3";
  return outs.size() == 1 ? outs[0].type : std::string("multioutput");
}

namespace {

// "major[.minor]" -> canonical (major, minor) digit strings.
bool ParseNodeVersion(const std::string &s, std::string *major,
                      std::string *minor) {
  const std::vector<std::string> parts = SplitOn('.', s);
  if (parts.size() == 1) {
    *minor = "0";
    return CanonNat(parts[0], major);
  }
  if (parts.size() == 2) {
    return CanonNat(parts[0], major) && CanonNat(parts[1], minor);
  }
  return false;
}

bool VersionCompatible(const MtlxNodeDefInfo &nd, const std::string *req) {
  if (!req) return nd.has_version ? nd.is_default : true;
  if (!nd.has_version) return false;
  std::string a0, a1, b0, b1;
  if (ParseNodeVersion(*req, &a0, &a1) &&
      ParseNodeVersion(nd.version, &b0, &b1)) {
    return a0 == b0 && a1 == b1;
  }
  return *req == nd.version;
}

}  // namespace

std::vector<const MtlxNodeDefInfo *> MtlxLibrary::Candidates(
    const MtlxElem &node, const MtlxScope &outer, bool exact_only) const {
  std::vector<const MtlxNodeDefInfo *> out;
  const std::string type = node.AttrOr("type");
  const std::string *version = node.Attr("version");
  // Look the category up qualified by the instance namespace, then as written.
  std::string ns;
  std::vector<std::string> keys;
  if (NamespaceAt(node, outer, &ns)) keys.push_back(QualifyWith(&ns, node.tag));
  if (keys.empty() || keys[0] != node.tag) keys.push_back(node.tag);
  for (const std::string &key : keys) {
    for (const MtlxNodeDefInfo &nd : defs_) {
      if (nd.key != key) continue;
      if (!VersionCompatible(nd, version)) continue;
      if (OutType(nd) != type) continue;
      if (exact_only) {
        const std::vector<MtlxPort> decl = ActiveInputs(nd);
        bool exact = true;
        for (const MtlxElem &in : node.children) {
          if (in.tag != "input") continue;
          const std::string in_name = in.Name();
          const std::string in_type = in.AttrOr("type");
          bool found = false;
          for (const MtlxPort &p : decl) {
            if (p.name == in_name && p.type == in_type) {
              found = true;
              break;
            }
          }
          if (!found) {
            exact = false;
            break;
          }
        }
        if (!exact) continue;
      }
      out.push_back(&nd);
    }
  }
  return out;
}

const MtlxNodeDefInfo *MtlxLibrary::FindNamed(const MtlxElem &node,
                                              const MtlxScope &outer,
                                              const std::string &name) const {
  std::string ns;
  if (NamespaceAt(node, outer, &ns)) {
    if (const MtlxNodeDefInfo *nd = Find(QualifyWith(&ns, name))) return nd;
  }
  return Find(name);
}

const MtlxNodeDefInfo *MtlxLibrary::ResolveStrict(
    const MtlxElem &node, const MtlxScope &outer) const {
  if (const std::string *n = node.Attr("nodedef")) {
    return FindNamed(node, outer, *n);
  }
  const std::vector<const MtlxNodeDefInfo *> exact =
      Candidates(node, outer, true);
  return exact.size() == 1 ? exact[0] : nullptr;
}

const MtlxNodeDefInfo *MtlxLibrary::ResolveReference(
    const MtlxElem &node, const MtlxScope &outer) const {
  if (const std::string *n = node.Attr("nodedef")) {
    return FindNamed(node, outer, *n);
  }
  const std::vector<const MtlxNodeDefInfo *> exact =
      Candidates(node, outer, true);
  return exact.empty() ? nullptr : exact[0];
}

const MtlxNodeDefInfo *MtlxLibrary::ResolveRough(
    const MtlxElem &node, const MtlxScope &outer) const {
  if (const MtlxNodeDefInfo *nd = ResolveReference(node, outer)) return nd;
  if (node.Attr("nodedef")) return nullptr;
  const std::vector<const MtlxNodeDefInfo *> rough =
      Candidates(node, outer, false);
  return rough.empty() ? nullptr : rough[0];
}

bool IsMtlxNodeElem(const MtlxElem &e) {
  static const char *const kNonNode[] = {
      "nodedef",     "nodegraph",   "typedef",        "unittypedef",
      "unitdef",     "geompropdef", "implementation", "look",
      "lookgroup",   "collection",  "geominfo",       "propertyset",
      "variantset",  "targetdef",   "attributedef",   "backdrop",
      "xi:include",  "comment",     "newline",        "input",
      "output",      "token"};
  for (const char *t : kNonNode) {
    if (e.tag == t) return false;
  }
  return true;
}

namespace {

const MtlxElem *FindMtlxChild(const MtlxElem &parent, const char *tag,
                              const std::string &name) {
  for (const MtlxElem &child : parent.children) {
    if (child.tag == tag && child.Name() == name) return &child;
  }
  return nullptr;
}

const MtlxElem *FindMtlxScopedChild(const MtlxElem &parent, const char *tag,
                                   const std::string &name, const MtlxElem &at,
                                   const MtlxScope &outer) {
  std::string ns;
  if (NamespaceAt(at, outer, &ns)) {
    if (const MtlxElem *found =
            FindMtlxChild(parent, tag, QualifyWith(&ns, name)))
      return found;
  }
  return FindMtlxChild(parent, tag, name);
}

const MtlxElem *FindMtlxNode(const MtlxElem &graph, const std::string &name) {
  for (const MtlxElem &child : graph.children) {
    if (IsMtlxNodeElem(child) && child.Name() == name) return &child;
  }
  return nullptr;
}

bool MtlxNodeOutputType(const MtlxLibrary &library, const MtlxElem &node,
                        const MtlxScope &outer, const std::string &output,
                        std::string *type, std::string *err) {
  MtlxScope scope;
  scope.reserve(outer.size() + 1);
  scope.push_back(&node);
  scope.insert(scope.end(), outer.begin(), outer.end());
  if (const MtlxNodeDefInfo *def = library.ResolveReference(node, scope)) {
    const std::vector<MtlxPort> outputs = library.ActiveOutputs(*def);
    if (output.empty() && outputs.size() == 1) {
      *type = outputs[0].type;
      return true;
    }
    const std::string requested = output.empty() ? "out" : output;
    for (const MtlxPort &port : outputs) {
      if (port.name == requested) {
        *type = port.type;
        return true;
      }
    }
    if (outputs.size() == 1 && outputs[0].name == "out" && requested == "out") {
      *type = outputs[0].type;
      return true;
    }
    *err = "node '" + node.Name() + "' has no output '" + requested + "'";
    return false;
  }
  if (output.empty() || output == "out") {
    if (const std::string *declared = node.Attr("type")) {
      *type = *declared;
      return true;
    }
  }
  *err = "cannot resolve output type for node '" + node.Name() + "'";
  return false;
}

bool ParseMtlxAuthoredValue(const std::string &type, const std::string &text,
                            MtlxTypedValue *value) {
  if (ParseMtlxValue(type, text, value)) return true;
  if (text.find(',') != std::string::npos || MtlxTypeArity(type) == 0)
    return false;
  std::vector<std::string> fields;
  std::string field;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      if (!field.empty()) {
        fields.push_back(field);
        field.clear();
      }
    } else {
      field.push_back(c);
    }
  }
  if (!field.empty()) fields.push_back(field);
  if (fields.empty()) return false;
  std::string comma_separated;
  for (const std::string &field : fields) {
    if (!comma_separated.empty()) comma_separated.push_back(',');
    comma_separated += field;
  }
  return ParseMtlxValue(type, comma_separated, value);
}

// Output values remain subject to value validation even when a connection
// takes precedence. MaterialX permits interface names only on inputs/tokens.
bool ValidateMtlxGraphOutputAttributes(const MtlxElem &output,
                                       const std::string &type,
                                       std::string *err) {
  if (output.Attr("interfacename")) {
    *err = "graph output '" + output.Name() + "' cannot use interfacename";
    return false;
  }
  if (const std::string *literal = output.Attr("value")) {
    MtlxTypedValue parsed;
    if (type.empty() || (IsBuiltinMtlxType(type) &&
                         !ParseMtlxAuthoredValue(type, *literal, &parsed))) {
      *err = "invalid literal graph output '" + output.Name() + "'";
      return false;
    }
  }
  return true;
}

}  // namespace

bool ResolveMtlxGraph(const MtlxElem &document, const MtlxElem &graph,
                      const MtlxLibrary &library, const MtlxScope &outer,
                      MtlxResolvedGraph *resolved, std::string *err) {
  std::string ignored_error;
  if (!err) err = &ignored_error;
  err->clear();
  if (!resolved) {
    if (err) *err = "missing resolved-graph output";
    return false;
  }
  resolved->node_order.clear();
  if (graph.tag != "nodegraph") {
    if (err) *err = "expected a nodegraph element";
    return false;
  }

  MtlxScope graph_scope;
  graph_scope.reserve(outer.size() + 1);
  graph_scope.push_back(&graph);
  graph_scope.insert(graph_scope.end(), outer.begin(), outer.end());
  const MtlxNodeDefInfo *graph_def = library.ResolveReference(graph, outer);
  if (!graph_def && !graph.Attr("nodedef")) {
    // Nodegraph implementations commonly point to a nodedef from a sibling
    // <implementation> element instead of repeating it on the graph.
    for (const MtlxElem &child : document.children) {
      if (child.tag != "implementation" ||
          child.AttrOr("nodegraph") != graph.Name())
        continue;
      const std::string *nodedef = child.Attr("nodedef");
      if (!nodedef) continue;
      MtlxElem reference = graph;
      reference.SetAttr("nodedef", *nodedef);
      graph_def = library.ResolveReference(reference, outer);
      if (graph_def) break;
    }
  }
  if (graph.Attr("nodedef") && !graph_def) {
    *err = "cannot resolve nodedef '" + graph.AttrOr("nodedef") +
           "' for nodegraph '" + graph.Name() + "'";
    return false;
  }
  const std::vector<MtlxPort> graph_inputs =
      graph_def ? library.ActiveInputs(*graph_def) : std::vector<MtlxPort>{};
  const std::vector<MtlxPort> graph_outputs =
      graph_def ? library.ActiveOutputs(*graph_def) : std::vector<MtlxPort>{};

  std::vector<const MtlxElem *> nodes;
  for (const MtlxElem &child : graph.children) {
    if (!IsMtlxNodeElem(child)) continue;
    if (child.Name().empty()) {
      if (err) *err = "nodegraph contains an unnamed node";
      return false;
    }
    if (FindMtlxNode(graph, child.Name()) != &child) {
      if (err) *err = "duplicate node name '" + child.Name() + "'";
      return false;
    }
    nodes.push_back(&child);
  }

  std::vector<std::vector<size_t>> successors(nodes.size());
  std::vector<size_t> indegree(nodes.size(), 0);
  for (size_t i = 0; i < nodes.size(); ++i) {
    const MtlxElem &node = *nodes[i];
    MtlxScope node_scope;
    node_scope.reserve(graph_scope.size() + 1);
    node_scope.push_back(&node);
    node_scope.insert(node_scope.end(), graph_scope.begin(), graph_scope.end());
    const MtlxNodeDefInfo *def = library.ResolveReference(node, node_scope);
    if (!def) {
      if (err) *err = "cannot resolve nodedef for node '" + node.Name() +
                      "' in category '" + node.tag + "'";
      return false;
    }
    if (node.tag != def->node && node.tag != def->key) {
      *err = "node '" + node.Name() + "' category '" + node.tag +
             "' does not match nodedef category '" + def->node + "'";
      return false;
    }
    const std::string declared_output_type = library.OutType(*def);
    if (!node.AttrOr("type").empty() && declared_output_type != "multioutput" &&
        node.AttrOr("type") != declared_output_type) {
      *err = "node '" + node.Name() + "' has type " + node.AttrOr("type") +
             ", nodedef requires " + declared_output_type;
      return false;
    }
    const std::vector<MtlxPort> declared_inputs =
        library.ActiveInputs(*def);

    for (const MtlxElem &input : node.children) {
      if (input.tag != "input") continue;
      const std::string input_name = input.Name();
      const MtlxPort *declared = nullptr;
      for (const MtlxPort &port : declared_inputs) {
        if (port.name == input_name) {
          declared = &port;
          break;
        }
      }
      if (!declared) {
        if (err) *err = "input '" + input_name + "' is not declared by the nodedef of node '" +
                        node.Name() + "'";
        return false;
      }
      const std::string input_type = input.AttrOr(
          "type", declared ? declared->type : std::string());
      if (declared && !input.AttrOr("type").empty() &&
          input.AttrOr("type") != declared->type) {
        if (err) *err = "input '" + input_name + "' on node '" + node.Name() +
                        "' has type " + input.AttrOr("type") +
                        ", nodedef requires " + declared->type;
        return false;
      }
      if (input_type.empty()) {
        if (err) *err = "cannot resolve type of input '" + input_name +
                        "' on node '" + node.Name() + "'";
        return false;
      }

      const std::string *node_name = input.Attr("nodename");
      const std::string *graph_name = input.Attr("nodegraph");
      const std::string *interface_name = input.Attr("interfacename");
      const std::string *value = input.Attr("value");
      const int binding_count = (node_name != nullptr) + (graph_name != nullptr) +
                                (interface_name != nullptr) + (value != nullptr) +
                                ((input.Attr("output") != nullptr) &&
                                 !node_name && !graph_name);
      if (binding_count == 0) {
        if (err) *err = "input '" + input_name + "' has no authored binding";
        return false;
      }
      if (binding_count > 1) {
        if (err) *err = "input '" + input_name + "' has multiple connection bindings";
        return false;
      }
      if (node_name) {
        const MtlxElem *source = FindMtlxNode(graph, *node_name);
        if (!source) {
          if (err) *err = "input '" + input_name + "' on node '" + node.Name() +
                          "' references missing node '" + *node_name + "'";
          return false;
        }
        std::string source_type;
        if (!MtlxNodeOutputType(library, *source, graph_scope,
                                input.AttrOr("output"), &source_type, err))
          return false;
        if (source_type != input_type &&
            !MtlxChannelConversionMatches(input, source_type, input_type)) {
          if (err) *err = "connection to input '" + input_name + "' on node '" +
                          node.Name() + "' has type " + source_type +
                          ", expected " + input_type;
          return false;
        }
        size_t source_pos = 0;
        while (source_pos < nodes.size() && nodes[source_pos] != source) ++source_pos;
        if (source_pos == nodes.size()) {
          if (err) *err = "internal node lookup failure";
          return false;
        }
        successors[source_pos].push_back(i);
        ++indegree[i];
      } else if (graph_name) {
        MtlxScope input_scope{&input};
        input_scope.insert(input_scope.end(), node_scope.begin(), node_scope.end());
        const MtlxElem *source_graph = FindMtlxScopedChild(
            document, "nodegraph", *graph_name, input, input_scope);
        if (!source_graph) {
          if (err) *err = "input '" + input_name + "' references missing nodegraph '" +
                          *graph_name + "'";
          return false;
        }
        std::string output_name = input.AttrOr("output");
        if (output_name.empty()) {
          const MtlxElem *only_output = nullptr;
          for (const MtlxElem &child : source_graph->children) {
            if (child.tag != "output") continue;
            if (only_output) {
              only_output = nullptr;
              break;
            }
            only_output = &child;
          }
          output_name = only_output ? only_output->Name() : "out";
        }
        const MtlxElem *output = FindMtlxChild(*source_graph, "output", output_name);
        if (!output) {
          if (err) *err = "nodegraph '" + *graph_name + "' has no output '" +
                          output_name + "'";
          return false;
        }
        const std::string source_name = output->AttrOr("nodename");
        const MtlxElem *source = FindMtlxNode(*source_graph, source_name);
        if (!source) {
          if (err) *err = "output '" + output_name + "' of nodegraph '" +
                          *graph_name + "' references missing node '" + source_name + "'";
          return false;
        }
        std::string source_type;
        MtlxScope source_scope{source_graph};
        source_scope.insert(source_scope.end(), outer.begin(), outer.end());
        if (!MtlxNodeOutputType(library, *source, source_scope,
                                output->AttrOr("output"), &source_type, err))
          return false;
        const MtlxNodeDefInfo *source_graph_def =
            library.ResolveReference(*source_graph, outer);
        const MtlxPort *declared_output = nullptr;
        const std::vector<MtlxPort> source_graph_outputs = source_graph_def
            ? library.ActiveOutputs(*source_graph_def)
            : std::vector<MtlxPort>{};
        for (const MtlxPort &port : source_graph_outputs) {
          if (port.name == output_name) {
            declared_output = &port;
            break;
          }
        }
        const std::string resolved_graph_type =
            !output->AttrOr("type").empty()
                ? output->AttrOr("type")
                : (declared_output ? declared_output->type : std::string());
        if (!output->AttrOr("type").empty() && declared_output &&
            output->AttrOr("type") != declared_output->type) {
          if (err) *err = "output '" + output_name + "' of nodegraph '" + *graph_name +
                          "' disagrees with its nodedef output type";
          return false;
        }
        if (!ValidateMtlxGraphOutputAttributes(*output, resolved_graph_type, err))
          return false;
        if (!resolved_graph_type.empty() && resolved_graph_type != source_type) {
          if (err) *err = "output '" + output_name + "' of nodegraph '" + *graph_name +
                          "' declares type " + resolved_graph_type +
                          ", source has type " + source_type;
          return false;
        }
        if (resolved_graph_type.empty()) {
          if (err) *err = "cannot resolve output type '" + output_name +
                          "' of nodegraph '" + *graph_name + "'";
          return false;
        }
        if (resolved_graph_type != input_type &&
            !MtlxChannelConversionMatches(input, resolved_graph_type, input_type)) {
          if (err) *err = "nodegraph connection to input '" + input_name +
                          "' has type " + resolved_graph_type + ", expected " + input_type;
          return false;
        }
      } else if (interface_name) {
        const MtlxElem *interface = FindMtlxChild(graph, "input", *interface_name);
        const MtlxPort *declared_interface = nullptr;
        for (const MtlxPort &port : graph_inputs) {
          if (port.name == *interface_name) {
            declared_interface = &port;
            break;
          }
        }
        if (!interface && !declared_interface) {
          if (err) *err = "input '" + input_name + "' references missing graph interface '" +
                          *interface_name + "'";
          return false;
        }
        const std::string interface_type =
            interface && !interface->AttrOr("type").empty()
                ? interface->AttrOr("type")
                : (declared_interface ? declared_interface->type : std::string());
        if (!interface_type.empty() && declared_interface &&
            interface_type != declared_interface->type) {
          if (err) *err = "graph interface '" + *interface_name +
                          "' disagrees with its nodedef input type";
          return false;
        }
        if (interface_type != input_type &&
            !MtlxChannelConversionMatches(input, interface_type, input_type)) {
          if (err) *err = "graph interface '" + *interface_name + "' has type " +
                          interface_type + ", expected " + input_type;
          return false;
        }
      } else if (const std::string *output_name = input.Attr("output")) {
        const MtlxElem *document_output =
            FindMtlxChild(document, "output", *output_name);
        if (!document_output) {
          *err = "input '" + input_name + "' references missing document output '" +
                 *output_name + "'";
          return false;
        }
        const std::string output_type = document_output->AttrOr("type");
        if (output_type.empty() ||
            !ValidateMtlxGraphOutputAttributes(*document_output, output_type, err)) {
          if (err->empty()) *err = "cannot resolve document output type for '" + *output_name + "'";
          return false;
        }
        const std::string source_name = document_output->AttrOr("nodename");
        const MtlxElem *source = FindMtlxNode(document, source_name);
        if (!source) {
          *err = "document output '" + *output_name + "' references missing node '" +
                 source_name + "'";
          return false;
        }
        std::string source_type;
        if (!MtlxNodeOutputType(library, *source, MtlxScope{&document},
                                document_output->AttrOr("output"), &source_type, err))
          return false;
        if (output_type != source_type ||
            (output_type != input_type &&
             !MtlxChannelConversionMatches(input, output_type, input_type))) {
          *err = "document output '" + *output_name + "' has incompatible type";
          return false;
        }
      } else if (value) {
        MtlxTypedValue parsed;
        if (IsBuiltinMtlxType(input_type) &&
            !ParseMtlxAuthoredValue(input_type, *value, &parsed)) {
          if (err) *err = "invalid value for input '" + input_name + "' on node '" +
                          node.Name() + "'";
          return false;
        }
      } else if ((!declared || !declared->has_value) &&
                 !IsBuiltinMtlxType(input_type)) {
        if (err) *err = "input '" + input_name + "' has no value or nodedef default";
        return false;
      }
    }
  }

  for (const MtlxElem &output : graph.children) {
    if (output.tag != "output") continue;
    const MtlxPort *declared_output = nullptr;
    for (const MtlxPort &port : graph_outputs) {
      if (port.name == output.Name()) {
        declared_output = &port;
        break;
      }
    }
    const std::string output_type = !output.AttrOr("type").empty()
                                        ? output.AttrOr("type")
                                        : (declared_output ? declared_output->type
                                                           : std::string());
    if (declared_output && !output.AttrOr("type").empty() &&
        output.AttrOr("type") != declared_output->type) {
      *err = "graph output '" + output.Name() + "' disagrees with its nodedef output type";
      return false;
    }
    if (!ValidateMtlxGraphOutputAttributes(output, output_type, err))
      return false;
    const std::string source_name = output.AttrOr("nodename");
    if (!output.Attr("nodename") && !output.Attr("nodegraph") &&
        !output.Attr("output")) {
      if (output_type.empty()) {
        *err = "cannot resolve graph output type for '" + output.Name() + "'";
        return false;
      }
      continue;
    }
    const MtlxElem *source = FindMtlxNode(graph, source_name);
    if (!source) {
      if (err) *err = "graph output '" + output.Name() + "' references missing node '" +
                      source_name + "'";
      return false;
    }
    std::string source_type;
    if (!MtlxNodeOutputType(library, *source, graph_scope,
                            output.AttrOr("output"), &source_type, err))
      return false;
    if (output_type.empty()) {
      *err = "cannot resolve graph output type for '" + output.Name() + "'";
      return false;
    }
    if (output_type != source_type) {
      if (err) *err = "graph output '" + output.Name() + "' has type " +
                      output_type + ", source has type " + source_type;
      return false;
    }
  }

  std::vector<size_t> ready;
  for (size_t i = 0; i < indegree.size(); ++i) {
    if (indegree[i] == 0) ready.push_back(i);
  }
  for (size_t cursor = 0; cursor < ready.size(); ++cursor) {
    const size_t source = ready[cursor];
    resolved->node_order.push_back(nodes[source]->Name());
    for (size_t consumer : successors[source]) {
      if (--indegree[consumer] == 0) ready.push_back(consumer);
    }
  }
  if (resolved->node_order.size() != nodes.size()) {
    resolved->node_order.clear();
    if (err) *err = "nodegraph contains a cycle";
    return false;
  }
  return true;
}

bool ResolveMtlxGraphs(const MtlxElem &document, const MtlxLibrary &library,
                       std::vector<std::string> *graph_order,
                       std::string *err) {
  std::string ignored_error;
  if (!err) err = &ignored_error;
  err->clear();
  if (!graph_order) {
    if (err) *err = "missing nodegraph-order output";
    return false;
  }
  graph_order->clear();
  std::vector<const MtlxElem *> graphs;
  for (const MtlxElem &child : document.children) {
    if (child.tag != "nodegraph") continue;
    if (child.Name().empty()) {
      if (err) *err = "document contains an unnamed nodegraph";
      return false;
    }
    for (const MtlxElem *prior : graphs) {
      if (prior->Name() == child.Name()) {
        if (err) *err = "duplicate nodegraph name '" + child.Name() + "'";
        return false;
      }
    }
    graphs.push_back(&child);
  }

  const MtlxScope outer{&document};
  for (const MtlxElem *graph : graphs) {
    MtlxResolvedGraph resolved;
    if (!ResolveMtlxGraph(document, *graph, library, outer, &resolved, err))
      return false;
  }

  std::vector<std::vector<size_t>> successors(graphs.size());
  std::vector<size_t> indegree(graphs.size(), 0);
  for (size_t consumer = 0; consumer < graphs.size(); ++consumer) {
    for (const MtlxElem &node : graphs[consumer]->children) {
      if (!IsMtlxNodeElem(node)) continue;
      for (const MtlxElem &input : node.children) {
        if (input.tag != "input") continue;
        const std::string *dependency_name = input.Attr("nodegraph");
        if (!dependency_name) continue;
        MtlxScope input_scope{&input, &node, graphs[consumer], &document};
        std::string ns;
        const bool has_namespace = NamespaceAt(input, input_scope, &ns);
        const std::string qualified_name =
            has_namespace ? QualifyWith(&ns, *dependency_name) : *dependency_name;
        size_t dependency = 0;
        while (dependency < graphs.size() &&
               graphs[dependency]->Name() != qualified_name)
          ++dependency;
        if (dependency == graphs.size() && qualified_name != *dependency_name) {
          dependency = 0;
          while (dependency < graphs.size() &&
                 graphs[dependency]->Name() != *dependency_name)
            ++dependency;
        }
        if (dependency == graphs.size()) {
          if (err) *err = "nodegraph connection references missing graph '" +
                          *dependency_name + "'";
          return false;
        }
        successors[dependency].push_back(consumer);
        ++indegree[consumer];
      }
    }
  }

  std::vector<size_t> ready;
  for (size_t i = 0; i < indegree.size(); ++i) {
    if (indegree[i] == 0) ready.push_back(i);
  }
  for (size_t cursor = 0; cursor < ready.size(); ++cursor) {
    const size_t source = ready[cursor];
    graph_order->push_back(graphs[source]->Name());
    for (size_t consumer : successors[source]) {
      if (--indegree[consumer] == 0) ready.push_back(consumer);
    }
  }
  if (graph_order->size() != graphs.size()) {
    graph_order->clear();
    if (err) *err = "document nodegraphs contain a dependency cycle";
    return false;
  }
  return true;
}

// ---- Canonical dump --------------------------------------------------------

namespace {

std::string OrDash(const std::string *v) { return v ? *v : std::string("-"); }

std::string BindingOf(const MtlxElem &port, const MtlxScope &outer) {
  const std::string type = port.AttrOr("type");
  const std::string out = " out=" + OrDash(port.Attr("output"));
  if (const std::string *i = port.Attr("interfacename")) return "iface=" + *i;
  if (const std::string *n = port.Attr("nodename")) return "node=" + *n + out;
  if (const std::string *g = port.Attr("nodegraph")) return "graph=" + *g + out;
  const std::string *v = port.Attr("value");
  if (!v) return "none";
  MtlxScope chain;
  chain.reserve(outer.size() + 1);
  chain.push_back(&port);
  chain.insert(chain.end(), outer.begin(), outer.end());
  std::string text;
  if (type == "filename") {
    if (!ResolvedMtlxFilename(chain, &text)) text = *v;
  } else {
    text = LexMtlxValue(type, *v);
  }
  std::string res = "value=" + text;
  if (type == "color3" || type == "color4" || type == "filename") {
    std::string cs;
    if (ActiveMtlxColorSpace(chain, &cs)) res += " cs=" + cs;
  }
  return res;
}

void PortLine(std::string *o, const char *indent, const char *kind,
              const MtlxElem &port, const MtlxScope &outer) {
  *o += indent;
  *o += kind;
  *o += ' ';
  *o += port.Name();
  *o += ' ';
  *o += port.AttrOr("type");
  *o += ' ';
  *o += BindingOf(port, outer);
  *o += '\n';
}

void DumpNode(std::string *o, const MtlxLibrary &lib, const char *indent,
              const MtlxElem &e, const MtlxScope &outer) {
  const MtlxNodeDefInfo *nd = lib.ResolveReference(e, outer);
  *o += indent;
  *o += "node " + e.Name() + " " + e.tag + " " + e.AttrOr("type") +
        " def=" + (nd ? nd->name : std::string("?")) + "\n";
  MtlxScope chain;
  chain.push_back(&e);
  chain.insert(chain.end(), outer.begin(), outer.end());
  const std::string in_indent = std::string(indent) + "  ";
  for (const MtlxElem &c : e.children) {
    if (c.tag == "input") PortLine(o, in_indent.c_str(), "in", c, chain);
  }
}

void DumpNodeDef(std::string *o, const MtlxLibrary &lib, const MtlxElem &e,
                 const MtlxScope &outer) {
  MtlxLibrary one;
  one.AddNodeDef(e, outer);
  const MtlxNodeDefInfo &nd = one.Defs()[0];
  *o += "nodedef " + nd.name + " node=" + nd.node + " type=" + lib.OutType(nd) +
        " version=" + (nd.has_version ? nd.version : std::string("-")) +
        " default=" + (nd.is_default ? "1" : "0") +
        " inherit=" + (nd.has_inherit ? nd.inherit : std::string("-")) + "\n";
  for (const MtlxPort &p : lib.ActiveInputs(nd)) {
    *o += "  in " + p.name + " " + p.type + " " +
          (p.has_value ? LexMtlxValue(p.type, p.value) : std::string("-")) +
          "\n";
  }
  for (const MtlxPort &p : nd.outputs) {
    *o += "  out " + p.name + " " + p.type + "\n";
  }
}

}  // namespace

std::string DumpMtlxDocument(const MtlxLibrary &lib, const MtlxElem &root) {
  std::string o = "doc version=" + OrDash(root.Attr("version")) +
                  " colorspace=" + OrDash(root.Attr("colorspace")) + "\n";
  const MtlxScope root_scope = {&root};
  for (const MtlxElem &e : root.children) {
    if (e.tag == "nodedef") {
      DumpNodeDef(&o, lib, e, root_scope);
    } else if (e.tag == "nodegraph") {
      o += "nodegraph " + e.Name() + " nodedef=" + OrDash(e.Attr("nodedef")) +
           "\n";
      const MtlxScope scope = {&e, &root};
      for (const MtlxElem &c : e.children) {
        if (c.tag == "input") PortLine(&o, "  ", "in", c, scope);
      }
      for (const MtlxElem &c : e.children) {
        if (IsMtlxNodeElem(c)) DumpNode(&o, lib, "  ", c, scope);
      }
      for (const MtlxElem &c : e.children) {
        if (c.tag == "output") PortLine(&o, "  ", "out", c, scope);
      }
    } else if (IsMtlxNodeElem(e)) {
      DumpNode(&o, lib, "", e, root_scope);
    }
  }
  return o;
}

}  // namespace mtlx
}  // namespace lightusd
