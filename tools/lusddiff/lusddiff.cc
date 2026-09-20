// SPDX-License-Identifier: Apache 2.0
// Copyright 2025-Present Light Transport Entertainment, Inc.
//
// lusddiff — USD Layer Diff Tool
//
// Usage:
//   lusddiff file1.usd file2.usd
//   lusddiff --json file1.usd file2.usd
//   lusddiff --help
//
// Exit codes:
//   0 = no differences found
//   1 = differences found
//   2 = error (file not found, parse failure, etc.)
//

#include <algorithm>
#include <iostream>
#include <fstream>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "lightusd.hh"
#include "io-util.hh"
#include "layer.hh"
#include "tiny-format.hh"
#include "str-util.hh"
#include "tydra/diff-and-compare.hh"
#include "next/lightusd-next.hh"
#include "next/diff/layer-diff.hh"
#include "collection-diff.hh"

namespace {

// ============================================================================
// --fast: a structural BYTE-LEVEL diff that never parses values.
//
// Both files are memory-mapped (page cache, ~no anonymous RSS), scanned into a
// tree of prim byte-ranges by INDENTATION (machine-written USDA puts each prim's
// `{`/`}` on its own line at the prim's indent, and children are deeper-indented,
// so the first `<indent>}` after a prim's `<indent>{` is unambiguously its
// close). Byte-identical prim subtrees are skipped with a single memcmp — so on
// two mostly-identical flattens the bulk is never touched. Only the prim paths
// that actually differ are reported (with a line-level diff of the prim's own
// content). Fast + low-memory; text-level (no ULP / no instance canonicalization).
// ============================================================================

struct MMapFile {
  const char *p = nullptr;
  size_t n = 0;
  lightusd::io::MMapFileHandle handle;
};

// Reuses the project's own cross-platform mmap primitive (io::MMapFile /
// io::UnmapFile, io-util.hh/.cc), which already has a working Windows branch
// via CreateFileMapping/MapViewOfFile, instead of the raw POSIX mmap(2) this
// file used to call directly with no _WIN32 fallback at all.
//
// NOTE: unlike the old hand-rolled version, io::MMapFile returns false for a
// 0-byte file (neither the POSIX nor the Windows implementation can map zero
// pages). A 0-byte file is never valid USDA/USDC, so this only changes which
// fallback path handles that edge case (the caller falls through to the
// existing semantic-diff path) -- not observable behavior for real inputs.
bool mmap_open(const std::string &path, MMapFile &m) {
  std::string err;
  if (!lightusd::io::MMapFile(path, &m.handle, /* writable */ false, &err)) {
    return false;
  }
  m.p = reinterpret_cast<const char *>(m.handle.addr);
  m.n = static_cast<size_t>(m.handle.size);
  return true;
}

void mmap_close(MMapFile &m) {
  if (m.handle.addr) {
    std::string err;
    lightusd::io::UnmapFile(m.handle, &err);
  }
  m.p = nullptr;
  m.n = 0;
}

struct FBlock {
  std::string name;          // prim name (the quoted element name)
  size_t blk0 = 0, blk1 = 0; // whole block bytes [header line start, after `}` line)
  size_t open1 = 0, close0 = 0; // body content range [after `{` line, before `}` line)
  std::vector<FBlock> kids;
};

// A def/over/class prim header line (after the leading indent).
bool is_prim_kw(const char *p, size_t s, size_t le) {
  auto kw = [&](const char *k) {
    size_t n = std::strlen(k);
    return (s + n <= le) && std::memcmp(p + s, k, n) == 0;
  };
  return kw("def ") || kw("over ") || kw("class ");
}

// The element name = the last quoted "..." on the header text [s, le).
std::string extract_name(const char *p, size_t s, size_t le) {
  size_t q1 = std::string::npos, q0 = std::string::npos;
  for (size_t i = s; i < le; ++i) {
    if (p[i] == '"') {
      if (q0 == std::string::npos || q1 != std::string::npos) {
        q0 = i;
        q1 = std::string::npos;
      } else {
        q1 = i;
      }
    }
  }
  if (q0 != std::string::npos && q1 != std::string::npos && q1 > q0 + 1)
    return std::string(p + q0 + 1, q1 - q0 - 1);
  return std::string();
}

// True if line [off,le) is exactly `indent` spaces followed by `c` (+ optional
// trailing spaces / CR).
bool is_brace_line(const char *p, size_t off, size_t le, int indent, char c) {
  if (static_cast<size_t>(indent) >= le - off) return false;
  for (int i = 0; i < indent; ++i)
    if (p[off + i] != ' ') return false;
  size_t i = off + indent;
  if (i >= le || p[i] != c) return false;
  for (++i; i < le; ++i)
    if (p[i] != ' ' && p[i] != '\r') return false;
  return true;
}

// Scan child prims at `indent` within [begin,end). `*ok=false` on a structural
// surprise (caller falls back to the semantic diff).
std::vector<FBlock> scan_prims(const char *p, size_t begin, size_t end,
                               int indent, bool *ok) {
  std::vector<FBlock> out;
  size_t off = begin;
  while (off < end) {
    size_t le = off;
    while (le < end && p[le] != '\n') ++le;
    size_t next = (le < end) ? le + 1 : end;
    int ind = 0;
    size_t s = off;
    while (s < le && p[s] == ' ') { ++s; ++ind; }
    if (ind != indent || !is_prim_kw(p, s, le)) { off = next; continue; }

    FBlock b;
    b.name = extract_name(p, s, le);
    b.blk0 = off;
    // Find this prim's body-open `<indent>{` (skipping any `( ... )` metadata).
    size_t l = next, openEnd = std::string::npos;
    while (l < end) {
      size_t l2 = l;
      while (l2 < end && p[l2] != '\n') ++l2;
      if (is_brace_line(p, l, l2, indent, '{')) {
        openEnd = (l2 < end) ? l2 + 1 : end;
        break;
      }
      // Another prim or a dedent before `{` => malformed for our purposes.
      int ind2 = 0; size_t s2 = l;
      while (s2 < l2 && p[s2] == ' ') { ++s2; ++ind2; }
      if (s2 < l2 && ind2 <= indent && is_prim_kw(p, s2, l2)) { if (ok) *ok = false; return out; }
      l = (l2 < end) ? l2 + 1 : end;
    }
    if (openEnd == std::string::npos) { if (ok) *ok = false; return out; }
    b.open1 = openEnd;
    // Find body-close `<indent>}` (children are deeper-indented; first match wins).
    size_t c = openEnd, closeStart = std::string::npos, closeEnd = std::string::npos;
    while (c < end) {
      size_t c2 = c;
      while (c2 < end && p[c2] != '\n') ++c2;
      if (is_brace_line(p, c, c2, indent, '}')) {
        closeStart = c;
        closeEnd = (c2 < end) ? c2 + 1 : end;
        break;
      }
      c = (c2 < end) ? c2 + 1 : end;
    }
    if (closeStart == std::string::npos) { if (ok) *ok = false; return out; }
    b.close0 = closeStart;
    b.blk1 = closeEnd;
    b.kids = scan_prims(p, openEnd, closeStart, indent + 4, ok);
    if (ok && !*ok) return out;
    out.push_back(std::move(b));
    off = closeEnd;
  }
  return out;
}

bool bytes_equal(const char *a, size_t a0, size_t a1, const char *b, size_t b0,
                 size_t b1) {
  return (a1 - a0) == (b1 - b0) && std::memcmp(a + a0, b + b0, a1 - a0) == 0;
}

// Lines of a prim block EXCLUDING its child sub-blocks (the prim's own content:
// header + property lines).
std::vector<std::string> own_lines(const char *p, const FBlock &b) {
  std::vector<std::string> out;
  size_t off = b.blk0;
  size_t ki = 0;
  while (off < b.blk1) {
    if (ki < b.kids.size() && off == b.kids[ki].blk0) {
      off = b.kids[ki].blk1;
      ++ki;
      continue;
    }
    size_t le = off;
    while (le < b.blk1 && p[le] != '\n') ++le;
    out.emplace_back(p + off, le - off);
    off = (le < b.blk1) ? le + 1 : b.blk1;
  }
  return out;
}

// ---- --faster semantic helpers (whitespace-insensitive + ULP + canon) ------

inline bool is_ws(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}
inline bool is_num_start(char c) {
  return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
}

// Parse a number token at [i,e); advance i; false if not a number.
bool parse_dbl(const char *p, size_t &i, size_t e, double &v) {
  size_t st = i;
  if (i < e && (p[i] == '-' || p[i] == '+')) ++i;
  bool dig = false;
  while (i < e && p[i] >= '0' && p[i] <= '9') { ++i; dig = true; }
  if (i < e && p[i] == '.') {
    ++i;
    while (i < e && p[i] >= '0' && p[i] <= '9') { ++i; dig = true; }
  }
  if (dig && i < e && (p[i] == 'e' || p[i] == 'E')) {
    size_t k = i + 1;
    if (k < e && (p[k] == '-' || p[k] == '+')) ++k;
    if (k < e && p[k] >= '0' && p[k] <= '9') {
      i = k;
      while (i < e && p[i] >= '0' && p[i] <= '9') ++i;
    }
  }
  if (!dig) { i = st; return false; }
  char buf[64];
  size_t n = i - st;
  if (n >= sizeof(buf)) { i = st; return false; }
  std::memcpy(buf, p + st, n);
  buf[n] = '\0';
  v = std::strtod(buf, nullptr);
  return true;
}

uint64_t ordered_double_bits(double v) {
  uint64_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  return (bits & (uint64_t(1) << 63)) ? ~bits + 1
                                      : bits | (uint64_t(1) << 63);
}

uint32_t ordered_float_bits(float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  return (bits & (uint32_t(1) << 31)) ? ~bits + 1
                                      : bits | (uint32_t(1) << 31);
}

bool has_float_storage(const std::string &text) {
  const size_t end = text.find('=');
  std::istringstream stream(text.substr(0, end));
  std::string token;
  while (stream >> token) {
    while (!token.empty() &&
           (token.back() == '[' || token.back() == ']' ||
            token.back() == '(')) {
      token.pop_back();
    }
    std::transform(token.begin(), token.end(), token.begin(),
                   [](unsigned char c) {
                     return static_cast<char>(std::tolower(c));
                   });
    if (token == "float" || token == "float2" || token == "float3" ||
        token == "float4" || token == "matrix2f" || token == "matrix3f" ||
        token == "matrix4f" ||
        (!token.empty() && token.back() == 'f' &&
         (token.rfind("color", 0) == 0 || token.rfind("normal", 0) == 0 ||
          token.rfind("point", 0) == 0 || token.rfind("vector", 0) == 0 ||
          token.rfind("texcoord", 0) == 0 ||
          token.rfind("quat", 0) == 0))) {
      return true;
    }
  }
  return false;
}

bool num_eq(double a, double b, bool float_storage,
            const lightusd::tydra::DiffOptions &o) {
  if (a == b) return true;
  double d = std::fabs(a - b);
  if (o.absEps >= 0.0 && d <= o.absEps) return true;
  if (float_storage) {
    const uint32_t ai = ordered_float_bits(static_cast<float>(a));
    const uint32_t bi = ordered_float_bits(static_cast<float>(b));
    const uint32_t distance = ai > bi ? ai - bi : bi - ai;
    return distance <= o.floatUlps;
  }
  const uint64_t ai = ordered_double_bits(a);
  const uint64_t bi = ordered_double_bits(b);
  const uint64_t distance = ai > bi ? ai - bi : bi - ai;
  return distance <= o.floatUlps;
}

std::string canonicalize_attribute_metadata(const std::string &text) {
  const size_t open = text.find("(\n");
  if (open == std::string::npos) return text;
  const size_t close = text.rfind('\n');
  if (close == std::string::npos || close <= open + 3) return text;
  size_t closing_token = close;
  while (closing_token < text.size() && is_ws(text[closing_token])) {
    ++closing_token;
  }
  if (closing_token >= text.size() || text[closing_token] != ')') return text;

  std::vector<std::string> fields;
  size_t cursor = open + 3;
  while (cursor < close) {
    size_t end = text.find('\n', cursor);
    if (end == std::string::npos || end > close) end = close;
    size_t begin = cursor;
    while (begin < end && is_ws(text[begin])) ++begin;
    while (end > begin && is_ws(text[end - 1])) --end;
    if (begin < end) fields.emplace_back(text.substr(begin, end - begin));
    cursor = end + 1;
  }
  std::sort(fields.begin(), fields.end());
  size_t declaration_end = open;
  while (declaration_end > 0 && is_ws(text[declaration_end - 1])) {
    --declaration_end;
  }
  std::string result = text.substr(0, declaration_end);
  for (const std::string &field : fields) {
    result += '\n';
    result += field;
  }
  return result;
}

// Whitespace/indent-insensitive + ULP-tolerant line equality.
bool line_sem_equal(const std::string &a, const std::string &b,
                    const lightusd::tydra::DiffOptions &o) {
  const std::string left = canonicalize_attribute_metadata(a);
  const std::string right = canonicalize_attribute_metadata(b);
  const bool float_storage =
      has_float_storage(left) && has_float_storage(right);
  size_t i = 0, j = 0, na = left.size(), nb = right.size();
  for (;;) {
    while (i < na && is_ws(left[i])) ++i;
    while (j < nb && is_ws(right[j])) ++j;
    if (i >= na || j >= nb) break;
    if (is_num_start(left[i]) && is_num_start(right[j])) {
      size_t i2 = i, j2 = j;
      double va, vb;
      if (parse_dbl(left.data(), i2, na, va) &&
          parse_dbl(right.data(), j2, nb, vb)) {
        if (!num_eq(va, vb, float_storage, o)) return false;
        i = i2;
        j = j2;
        continue;
      }
    }
    if (left[i] != right[j]) return false;
    ++i;
    ++j;
  }
  while (i < na && is_ws(left[i])) ++i;
  while (j < nb && is_ws(right[j])) ++j;
  return i == na && j == nb;
}

// Replace `/Name` (reference targets) and `"Name"` (a prototype prim's own
// header name) tokens via map m (instance-prototype canonicalization).
std::string subst_protos(const std::string &s,
                         const std::unordered_map<std::string, std::string> &m) {
  if (m.empty()) return s;
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '/') {
      size_t k = i + 1;
      while (k < s.size() &&
             (std::isalnum(static_cast<unsigned char>(s[k])) || s[k] == '_'))
        ++k;
      auto it = m.find(s.substr(i + 1, k - i - 1));
      if (it != m.end()) { o += '/'; o += it->second; i = k; continue; }
    } else if (s[i] == '"') {
      size_t k = i + 1;
      while (k < s.size() && s[k] != '"') ++k;
      if (k < s.size()) {
        auto it = m.find(s.substr(i + 1, k - i - 1));
        if (it != m.end()) { o += '"'; o += it->second; o += '"'; i = k + 1; continue; }
      }
    }
    o += s[i++];
  }
  return o;
}

struct DiffCtx {
  enum class Domain {
    All,
    Material,
    Geometry,
    Skinning
  };

  bool semantic = false;
  bool fuzzyAssets = false;  // --fuzzy-assets: compare @path@ by leaf/suffix
  std::string pathFilter;
  Domain domain = Domain::All;
  lightusd::tydra::DiffOptions opts;
  const std::unordered_map<std::string, std::string> *protoA = nullptr;
  const std::unordered_map<std::string, std::string> *protoB = nullptr;
};

bool path_selected(const std::string &filter, const std::string &path) {
  return filter.empty() || lightusd::GlobMatchPath(filter, path);
}

bool contains_any(const std::string &s,
                  const std::initializer_list<const char *> needles) {
  for (const char *needle : needles) {
    if (s.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool line_matches_domain(const std::string &line, DiffCtx::Domain domain) {
  if (domain == DiffCtx::Domain::All) {
    return true;
  }
  switch (domain) {
    case DiffCtx::Domain::All:
      return true;
    case DiffCtx::Domain::Material:
      return contains_any(line, {"\"Material\"", "\"Shader\"", "\"NodeGraph\"",
                                 "material:binding", "inputs:", "outputs:",
                                 "info:id", "@", "file", "texture"});
    case DiffCtx::Domain::Geometry:
      return contains_any(line, {"\"Mesh\"", "\"GeomSubset\"", "\"PointInstancer\"",
                                 "points", "faceVertexCounts",
                                 "faceVertexIndices", "normals", "primvars:",
                                 "indices", "elementType", "familyName"});
    case DiffCtx::Domain::Skinning:
      return contains_any(line, {"\"SkelRoot\"", "\"Skeleton\"",
                                 "\"SkelAnimation\"", "\"BlendShape\"", "skel:",
                                 "jointIndices", "jointWeights", "joints",
                                 "bindTransforms", "restTransforms",
                                 "blendShape", "rotations", "translations",
                                 "scales"});
  }
  return false;
}

bool block_matches_domain(const char *p, const FBlock &b,
                          DiffCtx::Domain domain) {
  if (domain == DiffCtx::Domain::All) {
    return true;
  }
  const std::vector<std::string> lines = own_lines(p, b);
  for (const std::string &line : lines) {
    if (line_matches_domain(line, domain)) {
      return true;
    }
  }
  return false;
}

// ---- --faster order-insensitive own-line units --------------------------

// Property/metadata key of a line: the identifier token immediately before '='
// (keeping a `.connect` / `.timeSamples` aspect suffix), else the last
// identifier. Used to match properties/metadata by NAME, not position.
std::string line_key(const std::string &s) {
  size_t a = 0;
  while (a < s.size() && is_ws(s[a])) ++a;
  size_t eq = s.find('=', a);
  size_t e = (eq == std::string::npos) ? s.size() : eq;
  while (e > a && is_ws(s[e - 1])) --e;
  while (e > a && s[e - 1] == '(') {
    --e;
    while (e > a && is_ws(s[e - 1])) --e;
  }
  size_t st = e;
  auto idch = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' ||
           c == '.';
  };
  while (st > a && idch(s[st - 1])) --st;
  return s.substr(st, e - st);
}

// Net open brackets ([ { minus ] }) on a line (approx; ignores string contents).
int net_brackets(const std::string &L) {
  int d = 0;
  for (char c : L) {
    if (c == '{' || c == '[' || c == '(') ++d;
    else if (c == '}' || c == ']' || c == ')') --d;
  }
  return d;
}

struct OwnUnit {
  std::string key;
  std::string text;
};

// Split a prim's own_lines into (header, keyed units), skipping blank / comment
// (#3) and pure structural delimiter lines, and grouping multi-line property
// values ( `= {`/`[` ... `}`/`]` ) into one unit so they key/compare as a whole.
void to_units(const std::vector<std::string> &lines, std::string &header,
              std::vector<OwnUnit> &units) {
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string &L = lines[i];
    size_t a = 0;
    while (a < L.size() && is_ws(L[a])) ++a;
    if (a >= L.size() || L[a] == '#') continue;  // blank / comment (#3)
    std::string t = L.substr(a);
    size_t te = t.size();
    while (te > 0 && is_ws(t[te - 1])) --te;
    t.resize(te);
    if (t == "{" || t == "}" || t == "(" || t == ")") continue;  // pure delimiter
    if (header.empty() &&
        (t.rfind("def ", 0) == 0 || t.rfind("over ", 0) == 0 ||
         t.rfind("class ", 0) == 0 || t == "def" || t == "over" || t == "class")) {
      if (!t.empty() && t.back() == '(') {
        t.pop_back();
        while (!t.empty() && is_ws(t.back())) t.pop_back();
      }
      header = t;
      continue;
    }
    std::string key = line_key(L);
    std::string txt = L;
    int d = net_brackets(L);
    while (d > 0 && i + 1 < lines.size()) {
      txt += "\n";
      txt += lines[++i];
      d += net_brackets(lines[i]);
    }
    if (key.empty()) key = std::string("\1") + std::to_string(units.size());
    units.push_back({std::move(key), std::move(txt)});
  }
}

// --fuzzy-assets: replace each @asset@ with @<leaf>@ (suffix after the last '/',
// './' prefixes stripped) so cross-tool path prefixes do not show as diffs.
std::string normalize_assets(const std::string &s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '@') {
      size_t k = i + 1;
      while (k < s.size() && s[k] != '@') ++k;
      if (k < s.size()) {
        std::string p = s.substr(i + 1, k - i - 1);
        size_t sl = p.find_last_of('/');
        std::string leaf = (sl == std::string::npos) ? p : p.substr(sl + 1);
        o += '@';
        o += leaf;
        o += '@';
        i = k + 1;
        continue;
      }
    }
    o += s[i++];
  }
  return o;
}

// Role-type alias -> canonical base (point3f/normal3f/vector3f/color3f are all
// float3, etc.). Matches the default diff's GetUnderlyingTypeId equivalence.
std::string canon_type(std::string t) {
  bool arr = t.size() >= 2 && t.compare(t.size() - 2, 2, "[]") == 0;
  if (arr) t.resize(t.size() - 2);
  static const std::unordered_map<std::string, std::string> alias = {
      {"point3f", "float3"},  {"normal3f", "float3"},  {"vector3f", "float3"},
      {"color3f", "float3"},  {"point3d", "double3"},  {"normal3d", "double3"},
      {"vector3d", "double3"}, {"color3d", "double3"}, {"point3h", "half3"},
      {"normal3h", "half3"},  {"vector3h", "half3"},   {"color3h", "half3"},
      {"texcoord2f", "float2"}, {"texcoord2d", "double2"}, {"texcoord2h", "half2"},
      {"texcoord3f", "float3"}, {"texcoord3d", "double3"}, {"texcoord3h", "half3"},
      {"color4f", "float4"},  {"color4d", "double4"},  {"color4h", "half4"},
      {"frame4d", "matrix4d"},
  };
  auto it = alias.find(t);
  if (it != alias.end()) t = it->second;
  if (arr) t += "[]";
  return t;
}

// Normalize a property/metadata DECLARATION: drop the deprecated `custom`
// qualifier and canonicalize the type token (role -> base). The `= value` part
// is left intact (compared by the ULP/whitespace line comparator).
std::string canon_decl(const std::string &line) {
  size_t eq = line.find('=');
  std::string decl = (eq == std::string::npos) ? line : line.substr(0, eq);
  std::string rest = (eq == std::string::npos) ? std::string() : line.substr(eq);
  std::vector<std::string> toks;
  for (size_t i = 0; i < decl.size();) {
    while (i < decl.size() && is_ws(decl[i])) ++i;
    size_t st = i;
    while (i < decl.size() && !is_ws(decl[i])) ++i;
    if (i > st) toks.push_back(decl.substr(st, i - st));
  }
  toks.erase(std::remove(toks.begin(), toks.end(), std::string("custom")),
             toks.end());
  if (toks.size() >= 2) toks[toks.size() - 2] = canon_type(toks[toks.size() - 2]);
  std::string out;
  for (size_t k = 0; k < toks.size(); ++k) { if (k) out += ' '; out += toks[k]; }
  if (!rest.empty()) { if (!out.empty()) out += ' '; out += rest; }
  return out;
}

// Path / token / asset items (`<...>`, `"..."`, `@...@`) in a value, for
// order-insensitive list-op comparison.
std::vector<std::string> extract_items(const std::string &s) {
  std::vector<std::string> out;
  for (size_t i = 0; i < s.size();) {
    char open = s[i], close = (open == '<') ? '>' : open;
    if (open == '<' || open == '"' || open == '@') {
      size_t k = i + 1;
      while (k < s.size() && s[k] != close) ++k;
      size_t end = (k < s.size()) ? k + 1 : k;
      out.push_back(s.substr(i, end - i));
      i = end;
    } else {
      ++i;
    }
  }
  return out;
}

bool setlike_equal(const std::string &a, const std::string &b) {
  auto ia = extract_items(a), ib = extract_items(b);
  std::sort(ia.begin(), ia.end());
  std::sort(ib.begin(), ib.end());
  return ia == ib;
}

bool listop_key(const std::string &k) {
  return k == "references" || k == "payload" || k == "payloads" ||
         k == "inherits" || k == "specializes" || k == "apiSchemas" ||
         k == "variantSets" || k == "prototypes" || k == "proxyPrim";
}

// Canonical (whitespace-normalized, name-independent) content hash of a block
// BODY (excludes the header line that carries the prototype's varying name).
uint64_t block_body_hash(const char *p, const FBlock &b) {
  uint64_t h = 1469598103934665603ULL;
  bool insp = false;
  for (size_t i = b.open1; i < b.close0; ++i) {
    char c = p[i];
    if (is_ws(c) || c == '\n') { insp = true; continue; }
    if (insp) { h ^= ' '; h *= 1099511628211ULL; insp = false; }
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

// Map each top-level prototype prim (a `/Name` reference target) to a
// content-derived canonical name, so differently-numbered but identical
// prototypes match across files. (Textual + non-recursive: covers leaf
// prototypes; nested ones fall back to name matching — use the default diff for
// full cross-tool robustness.)
std::unordered_map<std::string, std::string>
build_proto_map(const char *p, size_t n, const std::vector<FBlock> &roots) {
  // Reference targets `</Name>` with no inner slash.
  std::unordered_map<std::string, bool> targets;
  for (size_t i = 0; i + 1 < n; ++i) {
    if (p[i] == '<' && p[i + 1] == '/') {
      size_t k = i + 2;
      while (k < n &&
             (std::isalnum(static_cast<unsigned char>(p[k])) || p[k] == '_'))
        ++k;
      if (k < n && p[k] == '>') targets[std::string(p + i + 2, k - i - 2)] = true;
    }
  }
  std::unordered_map<std::string, std::string> m;
  for (const auto &r : roots) {
    if (!targets.count(r.name)) continue;
    m[r.name] = "__P" + lightusd::fmt::hex(block_body_hash(p, r), 16);
  }
  return m;
}

// Recursively diff two matched prim blocks (same path). Returns true if any
// difference was reported under `path`.
bool diff_block(const char *pa, const FBlock &a, const char *pb, const FBlock &b,
                const std::string &path, std::ostream &os, const DiffCtx &ctx) {
  if (bytes_equal(pa, a.blk0, a.blk1, pb, b.blk0, b.blk1)) return false;
  const bool report_this_path =
      path_selected(ctx.pathFilter, path) &&
      (block_matches_domain(pa, a, ctx.domain) ||
       block_matches_domain(pb, b, ctx.domain));

  // Children by name.
  std::vector<const FBlock *> amiss, bmiss;
  std::vector<std::pair<const FBlock *, const FBlock *>> both;
  std::vector<bool> bused(b.kids.size(), false);
  for (const auto &ak : a.kids) {
    const FBlock *m = nullptr;
    for (size_t j = 0; j < b.kids.size(); ++j)
      if (!bused[j] && b.kids[j].name == ak.name) { m = &b.kids[j]; bused[j] = true; break; }
    if (m) both.push_back({&ak, m}); else amiss.push_back(&ak);
  }
  for (size_t j = 0; j < b.kids.size(); ++j)
    if (!bused[j]) bmiss.push_back(&b.kids[j]);

  bool any = false;
  // The prim's OWN content (excluding children).
  auto la = own_lines(pa, a);
  auto lb = own_lines(pb, b);

  if (ctx.semantic) {
    // --faster: match metadata/properties by NAME (order-insensitive, #1),
    // skipping blank/comment lines (#3); compare whitespace/ULP/proto/asset-aware.
    auto norm = [&](const std::string &x, bool aSide) {
      std::string r = ctx.fuzzyAssets ? normalize_assets(x) : x;
      const auto *m = aSide ? ctx.protoA : ctx.protoB;
      return m ? subst_protos(r, *m) : r;
    };
    // Semantic equality for one own-unit keyed `key`: list-ops / relationships
    // compare item SETS (order-insensitive); other declarations drop `custom` +
    // canonicalize role types, then compare whitespace/ULP-aware.
    auto semeq_key = [&](const std::string &key, const std::string &x,
                         const std::string &y) {
      std::string nx = norm(x, true), ny = norm(y, false);
      bool setlike = listop_key(key);
      if (!setlike) {
        size_t pos = 0;
        while (pos < x.size() && is_ws(x[pos])) ++pos;
        setlike = (x.compare(pos, 4, "rel ") == 0);
      }
      if (setlike) return setlike_equal(nx, ny);
      nx = canon_decl(nx);
      ny = canon_decl(ny);
      return nx == ny || line_sem_equal(nx, ny, ctx.opts);
    };
    auto semeq = [&](const std::string &x, const std::string &y) {
      return semeq_key(std::string(), x, y);
    };
    auto firstline = [](const std::string &s) {
      size_t nl = s.find('\n');
      return nl == std::string::npos ? s : s.substr(0, nl) + " ...";
    };
    std::string hdrA, hdrB;
    std::vector<OwnUnit> ua, ub;
    to_units(la, hdrA, ua);
    to_units(lb, hdrB, ub);
    std::unordered_map<std::string, size_t> mb;
    for (size_t k = 0; k < ub.size(); ++k) mb.emplace(ub[k].key, k);
    std::vector<bool> bused_u(ub.size(), false);
    std::vector<std::pair<std::string, std::string>> mods;  // (a, b)
    std::vector<std::string> dels, adds;
    for (const auto &u : ua) {
      auto it = mb.find(u.key);
      if (it == mb.end()) {
        if (line_matches_domain(u.text, ctx.domain)) {
          dels.push_back(u.text);
        }
        continue;
      }
      bused_u[it->second] = true;
      if (!semeq_key(u.key, u.text, ub[it->second].text) &&
          (line_matches_domain(u.text, ctx.domain) ||
           line_matches_domain(ub[it->second].text, ctx.domain))) {
        mods.push_back({u.text, ub[it->second].text});
      }
    }
    for (size_t k = 0; k < ub.size(); ++k)
      if (!bused_u[k] && line_matches_domain(ub[k].text, ctx.domain)) {
        adds.push_back(ub[k].text);
      }
    const bool hdrDiff = !semeq(hdrA, hdrB) &&
                         (line_matches_domain(hdrA, ctx.domain) ||
                          line_matches_domain(hdrB, ctx.domain));
    if (report_this_path &&
        (hdrDiff || !mods.empty() || !dels.empty() || !adds.empty())) {
      any = true;
      os << "~ " << path << " (modified)\n";
      if (hdrDiff) {
        auto pr = lightusd::tydra::CenterValuePairForDiff(hdrA, hdrB);
        os << "  - " << pr.first << "\n  + " << pr.second << "\n";
      }
      for (auto &m : mods) {
        auto pr = lightusd::tydra::CenterValuePairForDiff(m.first, m.second);
        os << "  - " << pr.first << "\n  + " << pr.second << "\n";
      }
      for (auto &d : dels) os << "  - " << firstline(d) << "\n";
      for (auto &x : adds) os << "  + " << firstline(x) << "\n";
    }
  } else if (report_this_path && la != lb) {
    // --fast: positional text comparison.
    std::ostringstream body;
    if (la.size() == lb.size()) {
      for (size_t i = 0; i < la.size(); ++i) {
        if (la[i] == lb[i]) continue;
        if (!line_matches_domain(la[i], ctx.domain) &&
            !line_matches_domain(lb[i], ctx.domain)) {
          continue;
        }
        auto pr = lightusd::tydra::CenterValuePairForDiff(la[i], lb[i]);
        body << "  - " << pr.first << "\n  + " << pr.second << "\n";
      }
    } else {
      std::vector<std::string> sb(lb.begin(), lb.end());
      for (const auto &x : la)
        if (std::find(sb.begin(), sb.end(), x) == sb.end() &&
            line_matches_domain(x, ctx.domain)) {
          body << "  - " << x << "\n";
        }
      std::vector<std::string> sa(la.begin(), la.end());
      for (const auto &x : lb)
        if (std::find(sa.begin(), sa.end(), x) == sa.end() &&
            line_matches_domain(x, ctx.domain)) {
          body << "  + " << x << "\n";
        }
    }
    if (!body.str().empty()) {
      any = true;
      os << "~ " << path << " (modified)\n" << body.str();
    }
  }
  for (const auto *d : amiss) {
    const std::string child_path = path + "/" + d->name;
    if (path_selected(ctx.pathFilter, child_path) &&
        block_matches_domain(pa, *d, ctx.domain)) {
      os << "- " << child_path << " (deleted)\n";
      any = true;
    }
  }
  for (const auto *d : bmiss) {
    const std::string child_path = path + "/" + d->name;
    if (path_selected(ctx.pathFilter, child_path) &&
        block_matches_domain(pb, *d, ctx.domain)) {
      os << "+ " << child_path << " (added)\n";
      any = true;
    }
  }
  for (const auto &pr : both)
    any |= diff_block(pa, *pr.first, pb, *pr.second, path + "/" + pr.first->name, os, ctx);
  return any;
}

// Structural byte-level diff. `semantic` toggles --faster behavior (whitespace /
// ULP / instance canonicalization on differing blocks). Returns: 0 no diffs,
// 1 diffs, -1 fall back to the full semantic path.
int struct_diff(const std::string &f1, const std::string &f2, std::ostream &os,
                bool semantic, bool fuzzyAssets,
                const lightusd::tydra::DiffOptions &opts,
                const std::string &pathFilter, DiffCtx::Domain domain) {
  MMapFile m1, m2;
  if (!mmap_open(f1, m1) || !mmap_open(f2, m2)) {
    mmap_close(m1);
    mmap_close(m2);
    return -1;
  }
  int rc = 1;
  if (bytes_equal(m1.p, 0, m1.n, m2.p, 0, m2.n)) {
    rc = 0;
  } else {
    bool ok = true;
    auto t1 = scan_prims(m1.p, 0, m1.n, 0, &ok);
    auto t2 = ok ? scan_prims(m2.p, 0, m2.n, 0, &ok) : std::vector<FBlock>();
    if (!ok) {
      rc = -1;  // structural surprise -> full semantic fallback
    } else {
      DiffCtx ctx;
      ctx.semantic = semantic;
      ctx.fuzzyAssets = fuzzyAssets;
      ctx.pathFilter = pathFilter;
      ctx.domain = domain;
      ctx.opts = opts;
      std::unordered_map<std::string, std::string> pa, pb;
      if (semantic) {
        pa = build_proto_map(m1.p, m1.n, t1);
        pb = build_proto_map(m2.p, m2.n, t2);
        ctx.protoA = &pa;
        ctx.protoB = &pb;
      }
      // Canonical key of a top-level prim (prototype -> content hash).
      auto keyA = [&](const std::string &nm) {
        auto it = pa.find(nm);
        return it != pa.end() ? it->second : nm;
      };
      auto keyB = [&](const std::string &nm) {
        auto it = pb.find(nm);
        return it != pb.end() ? it->second : nm;
      };
      os << "--- " << f1 << "\n+++ " << f2 << "\n";
      bool any = false;
      std::vector<bool> used(t2.size(), false);
      for (const auto &a : t1) {
        const std::string ka = keyA(a.name);
        const FBlock *m = nullptr;
        for (size_t j = 0; j < t2.size(); ++j)
          if (!used[j] && keyB(t2[j].name) == ka) { m = &t2[j]; used[j] = true; break; }
        const std::string root_path = "/" + a.name;
        if (m) any |= diff_block(m1.p, a, m2.p, *m, root_path, os, ctx);
        else if (path_selected(pathFilter, root_path) &&
                 block_matches_domain(m1.p, a, domain)) {
          os << "- " << root_path << " (deleted)\n";
          any = true;
        }
      }
      for (size_t j = 0; j < t2.size(); ++j)
        if (!used[j]) {
          const std::string root_path = "/" + t2[j].name;
          if (path_selected(pathFilter, root_path) &&
              block_matches_domain(m2.p, t2[j], domain)) {
            os << "+ " << root_path << " (added)\n";
            any = true;
          }
        }
      rc = any ? 1 : 0;
    }
  }
  mmap_close(m1);
  mmap_close(m2);
  return rc;
}

void print_usage() {
  std::cout << "lusddiff — USD Layer Diff Tool\n";
  std::cout << "\n";
  std::cout << "USAGE:\n";
  std::cout << "  lusddiff [OPTIONS] <file1> <file2>\n";
  std::cout << "\n";
  std::cout << "OPTIONS:\n";
  std::cout << "  --json      Output diff in JSON format\n";
  std::cout << "  --quiet     Suppress diff output, exit code only\n";
  std::cout << "  -q, --brief Alias for --quiet (OpenUSD usddiff compatibility).\n";
  std::cout << "  -r, --recursive  Compare USDZ entries recursively; directories recurse automatically.\n";
  std::cout << "  -f, --flatten\n";
  std::cout << "              Compose both inputs before comparing them. Uses the\n";
  std::cout << "              dependency-free next PCP path and loads payloads.\n";
  std::cout << "  -n, --noeffect\n";
  std::cout << "              Accepted for OpenUSD usddiff compatibility. lusddiff\n";
  std::cout << "              never edits its inputs, so this is always the default.\n";
  std::cout << "  --ulps N    Floating-point tolerance in ULPs (default 1).\n";
  std::cout << "              Absorbs ~1 ULP rounding (e.g. pxr quaternion/xform).\n";
  std::cout << "              Use 0 for bitwise-exact comparison.\n";
  std::cout << "  --eps F     Absolute floating-point tolerance (overrides/ORs ULP).\n";
  std::cout << "  --no-meta   Do not compare metadata (attr/prim/layer).\n";
  std::cout << "  --no-canonicalize-instances\n";
  std::cout << "              Disable content-matching of flatten prototypes.\n";
  std::cout << "              (Default ON: /Flattened_Prototype_N prims are matched\n";
  std::cout << "              by content, so non-deterministic numbering does not\n";
  std::cout << "              produce spurious added/deleted/modified diffs.)\n";
  std::cout << "  --low-mem   Strip large arrays to content fingerprints before\n";
  std::cout << "              diffing: each file is processed before the next is\n";
  std::cout << "              loaded, so peak memory ~halves and big-array compares\n";
  std::cout << "              are faster. Large arrays compare EXACTLY (no ULP);\n";
  std::cout << "              scalars / small vectors keep ULP tolerance.\n";
  std::cout << "  --fast      Structural byte-level diff: memory-map both files,\n";
  std::cout << "              skip byte-identical prim subtrees, report only the\n";
  std::cout << "              prim paths that differ (with a line-level diff). No\n";
  std::cout << "              value parsing -> fastest + lowest memory, but\n";
  std::cout << "              text-level (no ULP tolerance, no instance\n";
  std::cout << "              canonicalization). Best for same-tool/same-format\n";
  std::cout << "              comparisons; falls back to the full diff if the USDA\n";
  std::cout << "              structure is unexpected.\n";
  std::cout << "  --faster    Middle tier: same structural byte-skip, but the prim\n";
  std::cout << "              blocks that DIFFER are compared SEMANTICALLY --\n";
  std::cout << "              whitespace/indent-insensitive, ULP-tolerant numbers\n";
  std::cout << "              (--ulps/--eps), and instance-prototype canonicalization.\n";
  std::cout << "              Faster than default (identical subtrees never parsed),\n";
  std::cout << "              slower than --fast. Metadata/properties are matched by\n";
  std::cout << "              NAME (order-insensitive); blank/comment lines ignored.\n";
  std::cout << "  --fuzzy-assets  (--faster) Compare @asset@ paths by leaf/suffix,\n";
  std::cout << "              so differing path prefixes do not show as diffs.\n";
  std::cout << "  --path=PATTERN  Limit --fast/--faster reports to matching prim paths.\n";
  std::cout << "              Glob syntax matches lusdcat --inspect (* and **).\n";
  std::cout << "  --material-diff Compare/report only material, shader, binding, and\n";
  std::cout << "              texture-like prim/property blocks. Implies --faster.\n";
  std::cout << "  --geom-diff Compare/report only geometry topology, subset, and\n";
  std::cout << "              primvar-related prim/property blocks. Implies --faster.\n";
  std::cout << "  --skinning-diff Compare/report only UsdSkel and skinning-related\n";
  std::cout << "              prim/property blocks. Implies --faster.\n";
  std::cout << "  --help      Show this help message\n";
  std::cout << "  -h          Show this help message\n";
  std::cout << "\n";
  std::cout << "EXIT CODES:\n";
  std::cout << "  0  No differences found\n";
  std::cout << "  1  Differences found\n";
  std::cout << "  2  Error (file not found, parse failure, etc.)\n";
  std::cout << "\n";
  std::cout << "EXAMPLES:\n";
  std::cout << "  lusddiff old.usd new.usd\n";
  std::cout << "  lusddiff --json scene1.usda scene2.usda\n";
  std::cout << "  lusddiff --quiet model.usda model.usdc\n";
  std::cout << "\n";
  std::cout << "SUPPORTED FORMATS:\n";
  std::cout << "  .usd, .usda, .usdc, .usdz\n";
}

} // namespace

int main(int argc, char **argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; i++) {
    args.push_back(std::string(argv[i]));
  }

  bool json_output = false;
  bool recursive = false;
  bool quiet = false;
  bool flatten = false;
  // Match flatten prototypes by CONTENT (default on) so non-deterministic
  // /Flattened_Prototype_N numbering does not produce spurious diffs.
  bool canonicalize_instances = true;
  // Strip large arrays to content fingerprints (lower peak RSS + faster). Big
  // arrays then compare EXACTLY (no ULP), so it is opt-in.
  bool low_mem = false;
  // Structural byte-level diff (mmap, no value parsing): fast + low memory.
  // Text-level (no ULP, no instance canonicalization).
  bool fast = false;
  // Middle tier: structural skip + semantic compare of differing blocks
  // (whitespace/indent-insensitive + ULP-tolerant + instance canonicalization).
  bool faster = false;
  // --fuzzy-assets (--faster): compare @asset@ paths by leaf/suffix.
  bool fuzzy_assets = false;
  std::string path_filter;
  DiffCtx::Domain diff_domain = DiffCtx::Domain::All;
  std::string file1, file2;
  lightusd::tydra::DiffOptions diff_opts;

  // Parse command line arguments
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == "--help" || args[i] == "-h") {
      print_usage();
      return 0;
    } else if (args[i] == "--json") {
      json_output = true;
    } else if (args[i] == "--recursive" || args[i] == "-r") {
      recursive = true;
    } else if (args[i] == "--quiet" || args[i] == "--brief" ||
               args[i] == "-q") {
      quiet = true;
    } else if (args[i] == "--flatten" || args[i] == "-f") {
      flatten = true;
    } else if (args[i] == "--noeffect" || args[i] == "-n") {
      // OpenUSD usddiff may write externally edited temporary USDA back to an
      // input. lusddiff is a read-only semantic comparator, so no-effect is
      // already its only behavior.
    } else if (args[i] == "--no-meta") {
      diff_opts.compareMetadata = false;
    } else if (args[i] == "--canonicalize-instances") {
      canonicalize_instances = true;
    } else if (args[i] == "--no-canonicalize-instances") {
      canonicalize_instances = false;
    } else if (args[i] == "--low-mem") {
      low_mem = true;
    } else if (args[i] == "--fast") {
      fast = true;
    } else if (args[i] == "--faster") {
      faster = true;
    } else if (args[i] == "--fuzzy-assets") {
      fuzzy_assets = true;
    } else if (args[i].rfind("--path=", 0) == 0) {
      path_filter = args[i].substr(std::strlen("--path="));
    } else if (args[i] == "--material-diff") {
      faster = true;
      diff_domain = DiffCtx::Domain::Material;
    } else if (args[i] == "--geom-diff") {
      faster = true;
      diff_domain = DiffCtx::Domain::Geometry;
    } else if (args[i] == "--skinning-diff") {
      faster = true;
      diff_domain = DiffCtx::Domain::Skinning;
    } else if (args[i] == "--ulps") {
      if (i + 1 >= args.size()) {
        std::cerr << "Error: --ulps requires a value\n";
        return 2;
      }
      try {
        unsigned long u = std::stoul(args[++i]);
        diff_opts.floatUlps = static_cast<uint32_t>(u);
        diff_opts.doubleUlps = static_cast<uint64_t>(u);
      } catch (...) {
        std::cerr << "Error: --ulps value must be a non-negative integer\n";
        return 2;
      }
    } else if (args[i] == "--eps") {
      if (i + 1 >= args.size()) {
        std::cerr << "Error: --eps requires a value\n";
        return 2;
      }
      try {
        diff_opts.absEps = std::stod(args[++i]);
      } catch (...) {
        std::cerr << "Error: --eps value must be a number\n";
        return 2;
      }
    } else if (file1.empty()) {
      file1 = args[i];
    } else if (file2.empty()) {
      file2 = args[i];
    } else {
      std::cerr << "Error: Too many arguments\n";
      print_usage();
      return 2;
    }
  }

  if (file1.empty() || file2.empty()) {
    std::cerr << "Error: Please specify two USD files to compare\n";
    print_usage();
    return 2;
  }

  if (flatten && (fast || faster)) {
    std::cerr << "Error: --flatten cannot be combined with --fast/--faster\n";
    return 2;
  }

  if (recursive || IsDiffDirectory(file1) || IsDiffDirectory(file2)) {
    if (IsDiffDirectory(file1) != IsDiffDirectory(file2) || flatten || fast || faster ||
        low_mem || fuzzy_assets || !path_filter.empty()) {
      std::cerr << "Collection diff requires matching input kinds and supports authored semantic comparison only\n";
      return 2;
    }
    lightusd::next::DiffOptions opts;
    opts.floatUlps = diff_opts.floatUlps; opts.doubleUlps = diff_opts.doubleUlps;
    opts.absEps = diff_opts.absEps; opts.compareMetadata = diff_opts.compareMetadata;
    return DiffCollections(file1, file2, opts, json_output, quiet);
  }

  if (flatten) {
    lightusd::next::Stage stage1;
    lightusd::next::Stage stage2;
    std::string next_warn;
    std::string next_err;
    if (!lightusd::next::LoadUSDComposed(file1, &stage1, &next_warn,
                                         &next_err)) {
      std::cerr << "Error loading/composing " << file1 << ": " << next_err
                << std::endl;
      return 2;
    }
    if (!next_warn.empty()) {
      std::cerr << "Warning loading/composing " << file1 << ": " << next_warn
                << std::endl;
    }
    next_warn.clear();
    next_err.clear();
    if (!lightusd::next::LoadUSDComposed(file2, &stage2, &next_warn,
                                         &next_err)) {
      std::cerr << "Error loading/composing " << file2 << ": " << next_err
                << std::endl;
      return 2;
    }
    if (!next_warn.empty()) {
      std::cerr << "Warning loading/composing " << file2 << ": " << next_warn
                << std::endl;
    }

    lightusd::next::Layer layer1 = stage1.Flatten();
    lightusd::next::Layer layer2 = stage2.Flatten();
    lightusd::next::DiffOptions next_opts;
    next_opts.floatUlps = diff_opts.floatUlps;
    next_opts.doubleUlps = diff_opts.doubleUlps;
    next_opts.timeUlps = diff_opts.timeUlps;
    next_opts.absEps = diff_opts.absEps;
    next_opts.compareMetadata = diff_opts.compareMetadata;
    next_opts.fuzzyAssetPaths = diff_opts.fuzzyAssetPaths;

    std::unordered_map<std::string, lightusd::next::PrimSpecDiff> ps_diffs;
    std::unordered_map<std::string, lightusd::next::PropDiff> prop_diffs;
    lightusd::next::LayerMetaDiff meta_diff;
    lightusd::next::Diff(layer1, layer2, ps_diffs, prop_diffs, next_opts,
                         &meta_diff);
    const bool has_diffs =
        !ps_diffs.empty() || !prop_diffs.empty() || meta_diff.changed();
    if (!quiet) {
      if (json_output) {
        std::cout << lightusd::next::DiffToJSON(layer1, layer2, file1, file2,
                                                next_opts);
      } else if (has_diffs) {
        std::cout << lightusd::next::DiffToText(layer1, layer2, file1, file2,
                                                next_opts);
      } else {
        std::cout << "No differences found." << std::endl;
      }
    }
    return has_diffs ? 1 : 0;
  }

  // --fast / --faster: structural byte-level diff. --faster adds whitespace /
  // ULP / instance-canonicalization semantics on the differing blocks. Both
  // fall back to the full semantic diff on a structural surprise / unreadable
  // file.
  if (fast || faster) {
    std::ostringstream ss;
    int rc = struct_diff(file1, file2, ss, /*semantic=*/faster, fuzzy_assets,
                         diff_opts, path_filter, diff_domain);
    if (rc >= 0) {
      if (!quiet) {
        if (rc == 1) std::cout << ss.str();
        else std::cout << "No differences found." << std::endl;
      }
      return rc;
    }
    std::cerr << "[lusddiff] " << (faster ? "--faster" : "--fast")
              << " inconclusive (structural surprise); using full diff.\n";
  }

  if (!path_filter.empty()) {
    std::cerr << "[lusddiff] --path currently applies to --fast/--faster; "
                 "default semantic diff will compare full layers.\n";
  }

  // Load both USD files as Layers (preserves full PrimSpec tree). Each file is
  // canonicalized (and, in --low-mem, its big arrays stripped to fingerprints)
  // right after load, BEFORE the next file is loaded — so the two full layers
  // never coexist and peak RSS roughly halves.
  lightusd::Layer layer1, layer2;
  std::string warn, err;

  auto prepare = [&](lightusd::Layer &layer) {
    // Canonicalize instance-flatten prototypes by content so non-deterministic
    // /Flattened_Prototype_N numbering does not show up as spurious diffs.
    if (canonicalize_instances) lightusd::tydra::CanonicalizeInstances(layer);
    if (low_mem) lightusd::tydra::StripLargeArrays(layer);
  };

  if (!lightusd::LoadLayerFromFile(file1, &layer1, &warn, &err)) {
    std::cerr << "Error loading " << file1 << ": " << err << std::endl;
    return 2;
  }
  if (!warn.empty()) {
    std::cerr << "Warning loading " << file1 << ": " << warn << std::endl;
  }
  prepare(layer1);

  warn.clear();
  err.clear();

  if (!lightusd::LoadLayerFromFile(file2, &layer2, &warn, &err)) {
    std::cerr << "Error loading " << file2 << ": " << err << std::endl;
    return 2;
  }
  if (!warn.empty()) {
    std::cerr << "Warning loading " << file2 << ": " << warn << std::endl;
  }
  prepare(layer2);

  // Perform diff
  lightusd::HashMap<std::string, lightusd::tydra::PrimSpecDiff> psDiffs;
  lightusd::HashMap<std::string, lightusd::tydra::PropDiff> propDiffs;
  lightusd::tydra::LayerMetaDiff layerMetaDiff;
  lightusd::tydra::Diff(layer1, layer2, psDiffs, propDiffs, diff_opts,
                        &layerMetaDiff);

  bool has_diffs =
      !psDiffs.empty() || !propDiffs.empty() || layerMetaDiff.changed();

  if (!quiet) {
    if (json_output) {
      std::string jsonDiff =
          lightusd::tydra::DiffToJSON(layer1, layer2, file1, file2, diff_opts);
      std::cout << jsonDiff;
    } else {
      if (has_diffs) {
        std::string textDiff =
            lightusd::tydra::DiffToText(layer1, layer2, file1, file2, diff_opts);
        std::cout << textDiff;
      } else {
        std::cout << "No differences found." << std::endl;
      }
    }
  }

  return has_diffs ? 1 : 0;
}
