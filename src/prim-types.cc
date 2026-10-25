// SPDX-License-Identifier: MIT
// Copyright 2021 - Present, Syoyo Fujita.
#include <algorithm>
#include <cstdio>
#include <limits>
#include <numeric>
//
#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "core/model-scope.hh"  // Model, Scope
#include "str-util.hh"
#include "tiny-container.hh"
#include "tiny-format.hh"
//
//
#include "common-macros.inc"
#include "pprint-meta.hh"
#include "value-pprint.hh"
#include "prim-meta-access.hh"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif

//#include "external/pystring.h"

#ifdef __clang__
#pragma clang diagnostic pop
#endif

// PushError macro removed - Layer implementation moved to layer.cc

namespace lightusd {

template<class InputIt1, class InputIt2>
bool lexicographical_compare(InputIt1 first1, InputIt1 last1,
                             InputIt2 first2, InputIt2 last2)
{
    for (; (first1 != last1) && (first2 != last2); ++first1, (void) ++first2)
    {
        if (*first1 < *first2)
            return true;
        if (*first2 < *first1)
            return false;
    }

    return (first1 == last1) && (first2 != last2);
}

nonstd::optional<Interpolation> InterpolationFromString(const std::string &v) {
  if ("faceVarying" == v) {
    return Interpolation::FaceVarying;
  } else if ("constant" == v) {
    return Interpolation::Constant;
  } else if ("uniform" == v) {
    return Interpolation::Uniform;
  } else if ("vertex" == v) {
    return Interpolation::Vertex;
  } else if ("varying" == v) {
    return Interpolation::Varying;
  }
  return nonstd::nullopt;
}

nonstd::optional<Orientation> OrientationFromString(const std::string &v) {
  if ("rightHanded" == v) {
    return Orientation::RightHanded;
  } else if ("leftHanded" == v) {
    return Orientation::LeftHanded;
  }
  return nonstd::nullopt;
}

bool operator==(const Path &lhs, const Path &rhs) {
  if (!lhs.is_valid()) {
    return false;
  }

  if (!rhs.is_valid()) {
    return false;
  }

  // Compare the parts directly (no full_path_name() allocation). Two paths are
  // equal iff prim and prop parts match.
  return lhs.prim_part() == rhs.prim_part() &&
         lhs.prop_part() == rhs.prop_part();
}

bool ConvertTokenAttributeToStringAttribute(
    const TypedAttribute<Animatable<value::token>> &inp,
    TypedAttribute<Animatable<std::string>> &out) {

    out.metas() = inp.metas();
    // The author wrote `token`, the schema field is string-typed. Preserve the
    // authored spelling so printers/writers re-declare it as authored (OpenUSD
    // keeps the authored typeName; usdchecker flags the type on BOTH sides).
    out.set_actual_type_name("token");

    if (inp.is_blocked()) {
      out.set_blocked(true);
    } else if (inp.is_value_empty()) {
      out.set_value_empty();
    } else if (inp.is_connection()) {
      out.set_connections(inp.get_connections());
    } else {
      Animatable<value::token> toks;
      Animatable<std::string> strs;
      if (inp.get_value(&toks)) {
        if (toks.is_scalar()) {
          value::token tok;
          toks.get_scalar(&tok);
          strs.set(tok.str());
        } else if (toks.is_timesamples()) {
          if (const value::TimeSamples *tsp = toks.get_timesamples_ptr()) {
            for (const auto &item : tsp->get_samples()) {
              if (item.blocked) {
                strs.add_blocked_sample(item.t);
              } else if (const value::token *tk =
                             item.value.as<value::token>()) {
                strs.add_sample(item.t, tk->str());
              }
            }
          }
        } else if (toks.is_blocked()) {
          return false;
        }
      }
      out.set_value(strs);
    }

    return true;
  }



//
// -- Path
//

// Build the single canonical buffer `_full` and the part offsets from prim and
// prop strings (eager, lock-free). element = property name (when present) else
// the last '/'-delimited segment of the prim part.
void Path::_assemble(const std::string &prim, const std::string &prop) {
  _full = prim;
  _prim_len = static_cast<uint32_t>(prim.size());
  if (!prop.empty()) {
    _full.push_back('.');
    _full.append(prop);
    _prop_len = static_cast<uint32_t>(prop.size());
    _elem_off = _prim_len + 1u;
    _elem_len = _prop_len;
  } else {
    _prop_len = 0;
    if (_prim_len == 0) {
      _elem_off = 0;
      _elem_len = 0;
    } else {
      const size_t pos = prim.rfind('/');
      if (pos == std::string::npos) {
        _elem_off = 0;
        _elem_len = _prim_len;
      } else {
        _elem_off = static_cast<uint32_t>(pos + 1);
        _elem_len = static_cast<uint32_t>(_prim_len - (pos + 1));
      }
    }
  }
}

void Path::_update(const std::string &p, const std::string &prop) {
  //
  // For absolute path, starts with '/' and no other '/' exists.
  // For property part, '.' exists only once.
  //

  if (p.empty() && prop.empty()) {
    _valid = false;
    return;
  }

  auto slash_fun = [](const char c) { return c == '/'; };
  auto dot_fun = [](const char c) { return c == '.'; };

  if (prop.size()) {
    // prop should not contain slashes
    if (std::count_if(prop.begin(), prop.end(), slash_fun)) {
      _valid = false;
      return;
    }
    // prop does not start with '.'
    if (startsWith(prop, ".")) {
      _valid = false;
      return;
    }
  }

  std::string lp;   // prim part
  std::string lpr;  // prop part

  if (!p.empty() && p[0] == '/') {
    // absolute path
    auto ndots = std::count_if(p.begin(), p.end(), dot_fun);
    if (ndots == 0) {
      lp = p;
      lpr = prop;
    } else if (ndots == 1) {
      // prim_part contains property name; prop must be empty.
      if (prop.size()) {
        _valid = false;
        return;
      }
      if (p.size() < 3) {
        _valid = false;
        return;
      }
      auto loc = p.find_first_of('.');
      if (loc == std::string::npos) {
        _valid = false;
        return;
      }
      lp = p.substr(0, size_t(loc));
      lpr = p.substr(size_t(loc) + 1);  // remove '.'
    } else {
      _valid = false;
      return;
    }
  } else if (!p.empty() && p[0] == '.') {
    // maybe relative(e.g. "./xform", "../xform"). FIXME: Support fully.
    lp = p;
    lpr = prop;
  } else {
    // prim or prim.prop (relative)
    auto ndots = std::count_if(p.begin(), p.end(), dot_fun);
    if (ndots == 0) {
      lp = p;
      lpr = prop;
    } else if (ndots == 1) {
      if (p.size() < 3) {
        _valid = false;
        return;
      }
      auto loc = p.find_first_of('.');
      if (loc == std::string::npos) {
        _valid = false;
        return;
      }
      std::string prop_name = p.substr(size_t(loc) + 1);  // remove '.'
      // Check no '/' in prop part.
      if (std::count_if(prop_name.begin(), prop_name.end(), slash_fun) > 0) {
        _valid = false;
        return;
      }
      lp = p.substr(0, size_t(loc));
      lpr = prop_name;
    } else {
      _valid = false;
      return;
    }
  }

  _assemble(lp, lpr);
  _valid = true;
}

Path::Path(const std::string &p, const std::string &prop) {
  _update(p, prop);
}

Path Path::append_property(const std::string &elem) {
  Path &p = (*this);

  if (elem.empty()) {
    p._valid = false;
    return p;
  }

  if (is_variantElementName(elem)) {
    // variant chars are not supported yet.
    p._valid = false;
    return p;
  }

  if (elem[0] == '[') {
    // relational attrib are not supported
    p._valid = false;
    return p;
  } else if (elem[0] == '.') {
    // Relative
    // std::cerr << "???. elem[0] is '.'\n";
    // For a while, make this valid.
    p._valid = false;
    return p;
  } else {
    // TODO: Validate property path. Keep the prim part, set the property part.
    p._assemble(std::string(p._full.data(), p._prim_len), elem);
    return p;
  }
}

const Path Path::AppendPrim(const std::string &elem) const {
  Path p = (*this);  // copies

  p.append_prim(elem);

  return p;
}

const Path Path::AppendElement(const std::string &elem) const {
  Path p = (*this);  // copies

  p.append_element(elem);

  return p;
}

const Path Path::AppendProperty(const std::string &elem) const {
  Path p = (*this);  // copies

  p.append_property(elem);

  return p;
}

bool Path::replace_prefix(const Path &srcPrefix, const Path &dstPrefix) {
  const std::string &srcPrefixStr = srcPrefix.prim_part();
  const std::string &dstPrefixStr = dstPrefix.prim_part();

  std::string pathStr = prim_part();
  if (startsWith(pathStr, srcPrefixStr)) {
    pathStr = dstPrefixStr + removePrefix(pathStr, srcPrefixStr);

    _update(pathStr, prop_part());

    return true;
  }

  return false;
}

// TODO: Do test more.
// Current implementation may not behave as in pxrUSD's SdfPath's
// _LessThanInternal implementation
bool Path::LessThan(const Path &lhs, const Path &rhs) {
  // DCOUT("LessThan");
  if (lhs.is_valid() && rhs.is_valid()) {
    // ok
  } else {
    // Even though this should not happen,
    // valid paths is less than invalid paths
    return lhs.is_valid();
  }

  // TODO: handle relative path correctly.
  if (lhs.is_absolute_path() && rhs.is_absolute_path()) {
    // ok
  } else {
    // Absolute paths are less than relative paths
    return lhs.is_absolute_path();
  }

  if (lhs.prim_part() == rhs.prim_part()) {
    // compare property (view-based, byte-wise lexicographic)
    const tstring_view lhs_prop_part = lhs.prop_part();
    const tstring_view rhs_prop_part = rhs.prop_part();

    if (lhs_prop_part.empty() || rhs_prop_part.empty()) {
      return lhs_prop_part.empty();
    }

    return lhs_prop_part < rhs_prop_part;
  }

  // Different prim parts: compare using full_path_name with special
  // handling for variant paths to maintain correct tree traversal order.
  //
  // In OpenUSD, SdfPath sorts variant selections as children of their
  // prim, AFTER regular children and properties. The key relationships:
  //   /A < /A.prop < /A/B < /A{v} < /A{v=sel} < /A{v=sel}/C
  //
  // Simple string comparison almost works (. < / < {), but within
  // variants we need {name} (VariantSet) before {name=sel} (Variant).
  // Since '=' (0x3D) < '}' (0x7D), we pad VariantSet paths:
  //   /A{v}  →  /A{v=\x00} for comparison (sorts before /A{v=sel})
  //
  // We also need /A{v=sel}/S to sort AFTER /A{v=\x00} (the VariantSet).
  // /A{v=sel}/S → /A{v=sel}\x7F/S (replace / after } with high char)
  // Actually simpler: just ensure VariantSet comes first by making its
  // comparison key shorter.
  //
  // Build the two full path names into reused thread-local buffers (clear()
  // keeps capacity, so no per-call heap allocation after warm-up; thread_local
  // makes this race-free for concurrent sorts), preserving the exact ordering
  // of the previous `full_path_name() < full_path_name()` comparison.
  thread_local std::string l_buf;
  thread_local std::string r_buf;
  l_buf.clear();
  r_buf.clear();
  lhs.append_full_path_name(&l_buf);
  rhs.append_full_path_name(&r_buf);
  return l_buf < r_buf;
}

std::pair<Path, Path> Path::split_at_root() const {
  if (is_absolute_path()) {
    if (is_root_path()) {
      return std::make_pair(Path("/", ""), Path());
    }

    std::string p = full_path_name();

    if (p.size() < 2) {
      // Never should reach here. just in case
      return std::make_pair(*this, Path());
    }

    // Fine 2nd '/'
    auto ret =
        std::find_if(p.begin() + 1, p.end(), [](char c) { return c == '/'; });

    if (ret != p.end()) {
      auto ndist = std::distance(p.begin(), ret);  // distance from str[0]
      if (ndist < 1) {
        // This should not happen though.
        return std::make_pair(*this, Path());
      }
      size_t n = size_t(ndist);
      std::string root = p.substr(0, n);
      std::string siblings = p.substr(n);

      Path rP(root, "");
      Path sP(siblings, "");

      return std::make_pair(rP, sP);
    }

    return std::make_pair(*this, Path());
  } else {
    return std::make_pair(Path(), *this);
  }
}

bool Path::has_prefix(const Path &prefix) const {
  if (!is_valid() || !prefix.is_valid()) {
    return false;
  }

  if (prefix.is_prim_property_path()) {
    // No hierarchy in Prim's property path, so use == (compare parts directly,
    // no full_path_name() allocation).
    return prim_part() == prefix.prim_part() &&
           prop_part() == prefix.prop_part();
  } else if (prefix.is_prim_path()) {
    // '/', prefix = '/'
    if (is_root_path() && prefix.is_root_path()) {
      // DCOUT("both are root path");
      return true;
    }

    // example:
    // - '/bora', prefix = '/'
    // - '/bora/dora', prefix = '/'
    if (is_absolute_path() && prefix.is_root_path()) {
      // DCOUT("prefix is root path");
      return true;
    }

    // Use string prefix matching on prim_part.
    // A path has prefix P if the prim_part starts with P's prim_part,
    // followed by either '/', '{', '.', or end of string.
    // This correctly handles variant paths: /A{v} has prefix /A.
    const std::string &pp = prim_part();
    const std::string &pfx = prefix.prim_part();

    if (pp.size() < pfx.size()) {
      return false;
    }

    if (pp.compare(0, pfx.size(), pfx) != 0) {
      return false;
    }

    // If exact match, it's a prefix (same path or property of same prim)
    if (pp.size() == pfx.size()) {
      return true;
    }

    // After the prefix, the next char must be '/', '{', or '.'
    // to ensure we matched a complete path component
    char next = pp[pfx.size()];
    return (next == '/' || next == '{' || next == '.');

  } else {
    // TODO: property-only path.
    DCOUT("TODO: Unsupported Path type in has_prefix()");
    return false;
  }
}

Path Path::append_element(const std::string &elem) {
  Path &p = (*this);

  if (elem.empty()) {
    p._valid = false;
    return p;
  }

  // Current prim/prop parts (copied out before _assemble rebuilds _full).
  std::string cur_prim(p._full.data(), p._prim_len);
  std::string cur_prop =
      p._prop_len ? std::string(p._full.data() + p._prim_len + 1, p._prop_len)
                  : std::string();

  // {variant=value}
  if (is_variantElementName(elem)) {
    std::array<std::string, 2> variant;
    if (tokenize_variantElement(elem, &variant)) {
      VariantTokens &vt = mutable_variant();
      vt.part = variant[0];       // variant set name, e.g. `color`
      vt.selection = variant[1];  // selection, e.g. `red` (may be empty)
      p._assemble(cur_prim + elem, cur_prop);
      return p;
    } else {
      p._valid = false;
    }
  }

  if (elem[0] == '[') {
    // relational attrib are not supported
    p._valid = false;
    return p;
  } else if (elem[0] == '.') {
    // Relative path
    // For a while, make this valid.
    p._valid = false;
    return p;
  } else {
    if ((p._prim_len == 1) && (p._full[0] == '/')) {
      p._assemble(cur_prim + elem, cur_prop);
    } else {
      // TODO: Validate element name.
      p._assemble(cur_prim + '/' + elem, cur_prop);
    }
    return p;
  }
}

Path Path::get_parent_path() const {
  if (!_valid) {
    return Path();
  }

  if (is_root_path()) {
    Path p("", "");
    return p;
  }

  // Prim part as an owning string for substring/find operations.
  const std::string prim(_full.data(), _prim_len);

  if (is_prim_property_path()) {
    return Path(prim, "");
  }

  // Handle variant paths where the LAST element is a variant: /A{v} -> /A
  // Only applies when there's no '/' after the last '{'.
  // For /A{v=sel}/C, the normal '/' splitting handles it → parent is /A{v=sel}.
  {
    auto brace_pos = prim.find_last_of('{');
    auto last_slash = prim.find_last_of('/');
    if (brace_pos != std::string::npos && brace_pos > 0 &&
        (last_slash == std::string::npos || last_slash < brace_pos)) {
      // The variant element is the last component (no '/' after it)
      std::string parent_str = prim.substr(0, brace_pos);
      if (parent_str.empty()) {
        return Path("/", "");
      }
      return Path(parent_str, "");
    }
  }

  size_t n = prim.find_last_of('/');
  if (n == std::string::npos) {
    return Path();
  }

  if (n == 0) {
    return Path("/", "");
  }

  return Path(prim.substr(0, n), "");
}

Path Path::get_parent_prim_path() const {
  if (!_valid) {
    return Path();
  }

  // Prim part as an owning string for substring/find operations.
  const std::string prim(_full.data(), _prim_len);

  // Handle variant paths FIRST — /A{v} -> /A, /A{v=sel} -> /A
  // Must check before is_root_prim() because /A{v} passes the root prim
  // test (only one '/') but is actually a sub-element of /A.
  {
    auto brace_pos = prim.find_last_of('{');
    if (brace_pos != std::string::npos && brace_pos > 0) {
      std::string parent_str = prim.substr(0, brace_pos);
      if (parent_str.empty()) {
        return Path("/", "");
      }
      return Path(parent_str, "");
    }
  }

  if (is_root_prim()) {
    return *this;
  }

  if (is_prim_property_path()) {
    return Path(prim, "");
  }

  size_t n = prim.find_last_of('/');
  if (n == std::string::npos) {
    // this should never happen though.
    return Path();
  }

  if (n == 0) {
    // return root
    return Path("/", "");
  }

  return Path(prim.substr(0, n), "");
}

tstring_view Path::element_name() const {
  // The element slice is computed eagerly during construction / append_* (the
  // last '/'-segment of the prim path, or the property name). Lock-free read
  // (no mutable state) -> safe on a shared const Path across threads.
  return element_name_view();
}

nonstd::optional<Kind> KindFromString(const std::string &str) {
  if (str == "model") {
    return Kind::Model;
  } else if (str == "group") {
    return Kind::Group;
  } else if (str == "assembly") {
    return Kind::Assembly;
  } else if (str == "component") {
    return Kind::Component;
  } else if (str == "subcomponent") {
    return Kind::Subcomponent;
  } else if (str == "sceneLibrary") {
    // https://developer.apple.com/documentation/arkit/usdz_schemas_for_ar/scenelibrary
    return Kind::SceneLibrary;
  } else if (str.empty()) {
    return nonstd::nullopt;
  } else {
    return Kind::UserDef;
  }
}

bool ValidatePrimElementName(const std::string &name) {
  if (name.empty()) {
    return false;
  }

  // alphanum + '_'
  // first char must not be number.

  if (std::isdigit(int(name[0]))) {
    return false;
  } else if (std::isalpha(int(name[0]))) {
    // ok
  } else if (name[0] == '_') {
    // ok
  } else {
    return false;
  }

  for (size_t i = 1; i < name.size(); i++) {
    if (std::isalnum(int(name[i])) || (name[i] == '_')) {
      // ok
    } else {
      return false;
    }
  }

  return true;
}

//
// -- Prim
//


Prim::Prim(const value::Value &rhs) {
  // Check if type is Prim(Model(GPrim), usdShade, usdLux, etc.)
  if ((value::TypeId::TYPE_ID_MODEL_BEGIN <= rhs.type_id()) &&
      (value::TypeId::TYPE_ID_MODEL_END > rhs.type_id())) {
    if (auto pv = GetPrimElementName(rhs)) {
      _path = Path(pv.value(), /* prop part*/ "");
      _elementPath = Path(pv.value(), /* prop part */ "");
    }

    _data = rhs;
  } else {
    // TODO: Raise an error if rhs is not an Prim
  }
}

Prim::Prim(value::Value &&rhs) {
  // Check if type is Prim(Model(GPrim), usdShade, usdLux, etc.)
  if ((value::TypeId::TYPE_ID_MODEL_BEGIN <= rhs.type_id()) &&
      (value::TypeId::TYPE_ID_MODEL_END > rhs.type_id())) {
    _data = std::move(rhs);

    if (auto pv = GetPrimElementName(_data)) {
      _path = Path(pv.value(), "");
      _elementPath = Path(pv.value(), "");
    }

  } else {
    // TODO: Raise an error if rhs is not an Prim
  }
}

Prim::Prim(const std::string &elementPath, const value::Value &rhs) {
  // Check if type is Prim(Model(GPrim), usdShade, usdLux, etc.)
  if ((value::TypeId::TYPE_ID_MODEL_BEGIN <= rhs.type_id()) &&
      (value::TypeId::TYPE_ID_MODEL_END > rhs.type_id())) {
    _path = Path(elementPath, /* prop part*/ "");
    _elementPath = Path(elementPath, /* prop part */ "");

    _data = rhs;
    SetPrimElementName(_data, elementPath);
  } else {
    // TODO: Raise an error if rhs is not an Prim
  }
}

Prim::Prim(const std::string &elementPath, value::Value &&rhs) {
  // Check if type is Prim(Model(GPrim), usdShade, usdLux, etc.)
  if ((value::TypeId::TYPE_ID_MODEL_BEGIN <= rhs.type_id()) &&
      (value::TypeId::TYPE_ID_MODEL_END > rhs.type_id())) {
    _path = Path(elementPath, /* prop part */ "");
    _elementPath = Path(elementPath, /* prop part */ "");

    _data = std::move(rhs);
    SetPrimElementName(_data, elementPath);
  } else {
    // TODO: Raise an error if rhs is not an Prim
  }
}

bool Prim::add_child(Prim &&rhs, const bool rename_prim_name,
                     std::string *err) {
  std::string elementName = rhs.element_name();

  if (elementName.empty()) {
    if (rename_prim_name) {
      // assign default name `default`
      elementName = "default";

      if (!SetPrimElementName(rhs.get_data(), elementName)) {
        if (err) {
          (*err) = fmt::format(
              "Internal error. cannot modify Prim's elementName.\n");
        }
        return false;
      }
      rhs.element_path() = Path(elementName, /* prop_part */ "");
    } else {
      if (err) {
        (*err) = "Prim has empty elementName.\n";
      }
      return false;
    }
  }

  if (_children.size() != _childrenNameSet.size()) {
    // Rebuild _childrenNames
    _childrenNameSet.clear();
    for (size_t i = 0; i < _children.size(); i++) {
      if (_children[i].element_name().empty()) {
        if (err) {
          (*err) =
              "Internal error: Existing child Prim's elementName is empty.\n";
        }
        return false;
      }

      if (_childrenNameSet.count(_children[i].element_name())) {
        if (err) {
          (*err) =
              "Internal error: _children contains Prim with same "
              "elementName.\n";
        }
        return false;
      }

      _childrenNameSet.insert(_children[i].element_name());
    }
  }

  DCOUT("elementName = " << elementName);

  if (_childrenNameSet.count(elementName)) {
    if (rename_prim_name) {
      std::string unique_name;
      if (!makeUniqueName(_childrenNameSet, elementName, &unique_name)) {
        if (err) {
          (*err) = fmt::format(
              "Internal error. cannot assign unique name for `{}`.\n",
              elementName);
        }
        return false;
      }

      // Ensure valid Prim name
      if (!ValidatePrimElementName(unique_name)) {
        if (err) {
          (*err) = fmt::format(
              "Internally generated Prim name `{}` is invalid as a Prim "
              "name.\n",
              unique_name);
        }
        return false;
      }

      elementName = unique_name;

      // Need to modify both Prim::data::name and Prim::elementPath
      DCOUT("elementName = " << elementName);
      if (!SetPrimElementName(rhs.get_data(), elementName)) {
        if (err) {
          (*err) = fmt::format(
              "Internal error. cannot modify Prim's elementName.\n");
        }
        return false;
      }
      rhs.element_path() = Path(elementName, /* prop_part */ "");
    } else {
      if (err) {
        (*err) = fmt::format(
            "Prim name(elementName) {} already exists in children.\n",
            rhs.element_name());
      }
      return false;
    }
  }

  DCOUT("rhs.elementName = " << rhs.element_name());

  _childrenNameSet.insert(elementName);
  _children.emplace_back(std::move(rhs));

  return true;
}

bool Prim::replace_child(const std::string &child_prim_name, Prim &&rhs,
                         std::string *err) {
  if (child_prim_name.empty()) {
    if (err) {
      (*err) += "child_prim_name is empty.\n";
    }
    return false;
  }

  if (!ValidatePrimElementName(child_prim_name)) {
    if (err) {
      (*err) +=
          fmt::format("`{}` is not a valid Prim name.\n", child_prim_name);
    }
    return false;
  }

  if (_children.size() != _childrenNameSet.size()) {
    // Rebuild _childrenNames
    _childrenNameSet.clear();
    for (size_t i = 0; i < _children.size(); i++) {
      if (_children[i].element_name().empty()) {
        if (err) {
          (*err) =
              "Internal error: Existing child Prim's elementName is empty.\n";
        }
        return false;
      }

      if (_childrenNameSet.count(_children[i].element_name())) {
        if (err) {
          (*err) =
              "Internal error: _children contains Prim with same "
              "elementName.\n";
        }
        return false;
      }

      _childrenNameSet.insert(_children[i].element_name());
    }
  }

  // Simple linear scan
  auto result = std::find_if(_children.begin(), _children.end(),
                             [child_prim_name](const Prim &p) {
                               return (p.element_name() == child_prim_name);
                             });

  if (result != _children.end()) {
    // Need to modify both Prim::data::name and Prim::elementPath
    if (!SetPrimElementName(rhs.get_data(), child_prim_name)) {
      if (err) {
        (*err) =
            fmt::format("Internal error. cannot modify Prim's elementName.\n");
      }
      return false;
    }
    rhs.element_path() = Path(child_prim_name, /* prop_part */ "");

    (*result) = std::move(rhs);  // replace

  } else {
    // Need to modify both Prim::data::name and Prim::elementPath
    if (!SetPrimElementName(rhs.get_data(), child_prim_name)) {
      if (err) {
        (*err) =
            fmt::format("Internal error. cannot modify Prim's elementName.\n");
      }
      return false;
    }
    rhs.element_path() = Path(child_prim_name, /* prop_part */ "");

    _childrenNameSet.insert(child_prim_name);
    _children.emplace_back(std::move(rhs));  // add
  }

  return true;
}

std::vector<int64_t> Prim::get_child_indices_from_primChildren(
    bool force_update, bool *indices_is_valid) const {
  // Computed on each call and returned by value (no `mutable` cache) so it is
  // safe to call on a shared Prim concurrently. `force_update` is retained for
  // source compatibility but is now a no-op (nothing is cached).
  (void)force_update;

  std::vector<int64_t> indices;

  if (metas().primChildren.empty()) {
    indices.resize(_children.size());
    std::iota(indices.begin(), indices.end(), 0);
    if (indices_is_valid) {
      (*indices_is_valid) = true;
    }
    return indices;
  }

  std::map<std::string, size_t> m;  // name -> children() index map
  for (size_t i = 0; i < _children.size(); i++) {
    m.emplace(_children[i].element_name(), i);
  }
  std::set<size_t> table;  // to check uniqueness

  // Use the length of primChildren.
  indices.resize(metas().primChildren.size());

  bool valid = true;

  for (size_t i = 0; i < indices.size(); i++) {
    std::string tok = metas().primChildren[i].str();
    const auto it = m.find(tok);
    if (it != m.end()) {
      indices[i] = int64_t(it->second);

      table.insert(it->second);
    } else {
      // Prim name not found.
      indices[i] = -1;
      valid = false;
    }
  }

  if (table.size() != indices.size()) {
    // duplicated index exists.
    valid = false;
  }

  if (indices_is_valid) {
    (*indices_is_valid) = valid;
  }

  return indices;
}

//
// To deal with clang's -Wexit-time-destructors, dynamically allocate buffer for
// PrimMeta.
//
// NOTE: not thread-safe.
//
class EmptyStaticMeta {
 private:
  EmptyStaticMeta() = default;

 public:
  static PrimMeta &GetEmptyStaticMeta() {
    // Thread-safe magic-static init (heap-allocated to keep avoiding clang's
    // -Wexit-time-destructors; intentionally never freed). NOTE: this is a
    // last-resort SHARED object for prim types missing from GetPrimMeta() —
    // writes through it alias across prims. Keep GetPrimMeta()'s dispatch
    // complete instead of relying on this fallback.
    static PrimMeta *s_meta = new PrimMeta();
    return *s_meta;
  }
};

PrimMeta &Prim::metas() {
  PrimMeta *p = GetPrimMeta(_data);
  if (p) {
    return *p;
  }

  // TODO: This should not happen. report an error.
  return EmptyStaticMeta::GetEmptyStaticMeta();
}

const PrimMeta &Prim::metas() const {
  const PrimMeta *p = GetPrimMeta(_data);
  if (p) {
    return *p;
  }

  // TODO: This should not happen. report an error.
  return EmptyStaticMeta::GetEmptyStaticMeta();
}


bool SetCustomDataByKey(const std::string &key, const MetaVariable &var,
                        CustomDataType &custom) {
  // split by namespace
  std::vector<std::string> names = split(key, ":");
  DCOUT("names = " << to_string(names));

  if (names.empty()) {
    DCOUT("names is empty");
    return false;
  }

  if (names.size() > 1024) {
    // too deep
    DCOUT("too deep");
    return false;
  }

  CustomDataType *curr = &custom;

  for (size_t i = 0; i < names.size(); i++) {
    const std::string &elemkey = names[i];
    DCOUT("elemkey = " << elemkey);

    if (i == (names.size() - 1)) {
      DCOUT("leaf");
      // leaf
      (*curr)[elemkey] = var;
    } else {
      auto it = curr->find(elemkey);
      if (it != curr->end()) {
        // must be CustomData type
        value::Value &data = it->second.get_raw_value();
        CustomDataType *p = data.as<CustomDataType>();
        if (p) {
          curr = p;
        } else {
          DCOUT("value is not dictionary");
          return false;
        }
      } else {
        // Add empty dictionary.
        CustomDataType customData;
        curr->emplace(elemkey, customData);
        DCOUT("add dict " << elemkey);

        MetaVariable &child = curr->at(elemkey);
        value::Value &data = child.get_raw_value();
        CustomDataType *childp = data.as<CustomDataType>();
        if (!childp) {
          DCOUT("childp is null");
          return false;
        }

        DCOUT("child = " << print_customData(*childp, "child", uint32_t(i)));

        // renew curr
        curr = childp;
      }
    }
  }

  DCOUT("dict = " << print_customData(custom, "custom", 0));

  return true;
}

bool HasCustomDataKey(const CustomDataType &custom, const std::string &key) {
  // split by namespace
  std::vector<std::string> names = split(key, ":");

  DCOUT(print_customData(custom, "customData", 0));

  if (names.empty()) {
    DCOUT("empty");
    return false;
  }

  if (names.size() > 1024) {
    DCOUT("too deep");
    // too deep
    return false;
  }

  const CustomDataType *curr = &custom;

  for (size_t i = 0; i < names.size(); i++) {
    const std::string &elemkey = names[i];
    DCOUT("elemkey = " << elemkey);

    DCOUT("dict = " << print_customData(*curr, "dict", uint32_t(i)));

    auto it = curr->find(elemkey);
    if (it == curr->end()) {
      DCOUT("key not found");
      return false;
    }

    if (i == (names.size() - 1)) {
      // leaf .ok
    } else {
      // must be CustomData type
      const value::Value &data = it->second.get_raw_value();
      const CustomDataType *p = data.as<CustomDataType>();
      if (p) {
        curr = p;
      } else {
        DCOUT("value is not dictionary type.");
        return false;
      }
    }
  }

  return true;
}

bool GetCustomDataByKey(const CustomDataType &custom, const std::string &key,
                        MetaVariable *var) {
  if (!var) {
    return false;
  }

  DCOUT(print_customData(custom, "customData", 0));

  // split by namespace
  std::vector<std::string> names = split(key, ":");

  if (names.empty()) {
    return false;
  }

  if (names.size() > 1024) {
    // too deep
    return false;
  }

  const CustomDataType *curr = &custom;

  for (size_t i = 0; i < names.size(); i++) {
    const std::string &elemkey = names[i];

    auto it = curr->find(elemkey);
    if (it == curr->end()) {
      return false;
    }

    if (i == (names.size() - 1)) {
      // leaf
      (*var) = it->second;
    } else {
      // must be CustomData type
      const value::Value &data = it->second.get_raw_value();
      const CustomDataType *p = data.as<CustomDataType>();
      if (p) {
        curr = p;
      } else {
        return false;
      }
    }
  }

  return true;
}

namespace {

// Iterative version of dictionary override using explicit stack
// Avoids recursion for deeply nested dictionary structures
void OverrideCustomDataIterative(CustomDataType &dst, const CustomDataType &src,
                                 const bool override_existing) {
  // Stack of pairs: (dst_dict pointer, src_dict pointer)
  StackVector<std::pair<CustomDataType *, const CustomDataType *>, 4> stack;
  stack.reserve(16);

  // Start with the root dictionaries
  stack.emplace_back(&dst, &src);

  while (!stack.empty()) {
    auto current = stack.back();
    stack.pop_back();

    CustomDataType *current_dst = current.first;
    const CustomDataType *current_src = current.second;

    for (const auto &item : *current_src) {
      if (current_dst->count(item.first)) {
        if (override_existing) {
          CustomDataType *dst_dict =
              current_dst->at(item.first).get_raw_value().as<CustomDataType>();

          const value::Value &src_data = item.second.get_raw_value();
          const CustomDataType *src_dict = src_data.as<CustomDataType>();

          // If both are dicts, push to stack for later processing
          if (src_dict && dst_dict) {
            stack.emplace_back(dst_dict, src_dict);
          } else {
            (*current_dst)[item.first] = item.second;
          }
        }
      } else {
        // add dict value
        current_dst->emplace(item.first, item.second);
      }
    }
  }
}

}  // namespace

void OverrideDictionary(CustomDataType &dst, const CustomDataType &src, const bool override_existing) {
  OverrideCustomDataIterative(dst, src, override_existing);
}

AssetInfo PrimMetas::get_assetInfo_struct(bool *is_authored) const {
  AssetInfo ainfo;

  bool has_assetinfo = has_assetInfo();
  if (is_authored) {
    (*is_authored) = has_assetinfo;
  }

  if (has_assetinfo) {
    Dictionary asset_dict = get_assetInfo();
    ainfo._fields = asset_dict;

    {
      MetaVariable identifier_var;
      if (GetCustomDataByKey(asset_dict, "identifier", &identifier_var)) {
        std::string identifier;
        if (identifier_var.get_value<std::string>(&identifier)) {
          ainfo.identifier = identifier;
          ainfo._fields.erase("identifier");
        }
      }
    }

    {
      MetaVariable name_var;
      if (GetCustomDataByKey(asset_dict, "name", &name_var)) {
        std::string name;
        if (name_var.get_value<std::string>(&name)) {
          ainfo.name = name;
          ainfo._fields.erase("name");
        }
      }
    }

    {
      MetaVariable payloadDeps_var;
      if (GetCustomDataByKey(asset_dict, "payloadAssetDependencies",
                             &payloadDeps_var)) {
        std::vector<value::AssetPath> assets;
        if (payloadDeps_var.get_value<std::vector<value::AssetPath>>(&assets)) {
          ainfo.payloadAssetDependencies = assets;
          ainfo._fields.erase("payloadAssetDependencies");
        }
      }
    }

    {
      MetaVariable version_var;
      if (GetCustomDataByKey(asset_dict, "version", &version_var)) {
        std::string version;
        if (version_var.get_value<std::string>(&version)) {
          ainfo.version = version;
          ainfo._fields.erase("version");
        }
      }
    }
  }

  return ainfo;
}

// NOTE: PrimMetas::get_kind() is now implemented inline in the header

bool IsXformablePrim(const Prim &prim) {
  uint32_t tyid = prim.type_id();

  // GeomSubset is not xformable

  switch (tyid) {
    case value::TYPE_ID_GPRIM: {
      return true;
    }
    case value::TYPE_ID_GEOM_XFORM: {
      return true;
    }
    case value::TYPE_ID_GEOM_MESH: {
      return true;
    }
    // UsdVol (GPrim / Xformable-derived)
    case value::TYPE_ID_VOLUME:
    case value::TYPE_ID_VOLUME_FIELD_BASE:
    case value::TYPE_ID_FIELD_BASE:
    case value::TYPE_ID_VOLUME_FIELD_ASSET:
    case value::TYPE_ID_FIELD_ASSET:
    case value::TYPE_ID_OPENVDB_ASSET:
    case value::TYPE_ID_FIELD3D_ASSET: {
      return true;
    }
    case value::TYPE_ID_GEOM_BASIS_CURVES: {
      return true;
    }
    case value::TYPE_ID_GEOM_SPHERE: {
      return true;
    }
    case value::TYPE_ID_GEOM_CUBE: {
      return true;
    }
    case value::TYPE_ID_GEOM_CYLINDER: {
      return true;
    }
    case value::TYPE_ID_GEOM_CONE: {
      return true;
    }
    case value::TYPE_ID_GEOM_CAPSULE: {
      return true;
    }
    case value::TYPE_ID_GEOM_POINTS: {
      return true;
    }
    // value::TYPE_ID_GEOM_GEOMSUBSET
    case value::TYPE_ID_GEOM_POINT_INSTANCER: {
      return true;
    }
    case value::TYPE_ID_GEOM_CAMERA: {
      return true;
    }
    case value::TYPE_ID_LUX_DOME: {
      return true;
    }
    case value::TYPE_ID_LUX_CYLINDER: {
      return true;
    }
    case value::TYPE_ID_LUX_SPHERE: {
      return true;
    }
    case value::TYPE_ID_LUX_DISK: {
      return true;
    }
    case value::TYPE_ID_LUX_DISTANT: {
      return true;
    }
    case value::TYPE_ID_LUX_RECT: {
      return true;
    }
    case value::TYPE_ID_LUX_GEOMETRY: {
      return true;
    }
    case value::TYPE_ID_LUX_PORTAL: {
      return true;
    }
    case value::TYPE_ID_LUX_PLUGIN: {
      return true;
    }
    case value::TYPE_ID_SKEL_ROOT: {
      return true;
    }
    case value::TYPE_ID_SKELETON: {
      return true;
    }
    default:
      return false;
  }
}

void PrimMetas::update_from(const PrimMetas &rhs, const bool override_authored) {
  // Simple metadata fields - use MetadataBase merge
  merge_from(rhs, override_authored);

  // apiSchemas (special handling since it's a custom type)
  if (rhs.has_apiSchemas()) {
    if (override_authored || !has_apiSchemas()) {
      set_apiSchemas(rhs.get_apiSchemas());
    }
  }

  // Composition fields
  if (rhs.references) {
    if (override_authored || !references.has_value()) {
      references = rhs.references;
    }
  }
  if (rhs.payload) {
    if (override_authored || !payload.has_value()) {
      payload = rhs.payload;
    }
  }
  if (rhs.inherits) {
    if (override_authored || !inherits.has_value()) {
      inherits = rhs.inherits;
    }
  }
  if (rhs.variantSets) {
    if (override_authored || !variantSets.has_value()) {
      variantSets = rhs.variantSets;
    }
  }
  if (rhs.variants) {
    if (!variants.has_value()) {
      variants = rhs.variants;
    } else {
      for (const auto &item : rhs.variants.value()) {
        auto it = variants->find(item.first);
        if (it == variants->end()) {
          (*variants)[item.first] = item.second;
        } else if (override_authored) {
          it->second = item.second;
        }
      }
    }
  }
  if (rhs.specializes) {
    if (override_authored || !specializes.has_value()) {
      specializes = rhs.specializes;
    }
  }

  // Unregistered metadata
  if (!rhs.unregisteredMetas.empty()) {
    for (const auto &item : rhs.unregisteredMetas) {
      if (unregisteredMetas.count(item.first)) {
        if (override_authored) {
          unregisteredMetas[item.first] = item.second;
        }
      } else {
        unregisteredMetas[item.first] = item.second;
      }
    }
  }

  // Legacy meta dictionary
  OverrideDictionary(meta, rhs.meta, override_authored);
}

// NOTE: AttrMetas accessors (has_colorSpace, get_colorSpace, has_unauthoredValuesIndex,
// get_unauthoredValuesIndex) are now provided by MetadataBase base class

namespace {

// GetPrimSpecAtPathRec function moved to layer.cc

// Helper functions moved to layer.cc

}  // namespace

// All Layer methods moved to layer.cc

size_t Property::estimate_memory_usage() const {
  size_t total = sizeof(Property);

  // Add storage for the active variant member
  if (auto* attr = get_attribute_or_null()) {
    total += attr->estimate_memory_usage();
  } else if (auto* rel = get_relationship_or_null()) {
    total += rel->estimate_memory_usage();
  }

  return total;
}

size_t Property::estimate_actual_usage() const {
  size_t total = sizeof(Property);

  if (auto* attr = get_attribute_or_null()) {
    total += attr->estimate_actual_usage();
  } else if (auto* rel = get_relationship_or_null()) {
    total += rel->estimate_actual_usage();
  }

  return total;
}

size_t Relationship::estimate_memory_usage() const {
  size_t total = sizeof(Relationship);

  total += targetPath.full_path_name().size();
  for (const auto& path : targetPathVector) {
    total += path.full_path_name().size();
  }

  return total;
}

size_t Relationship::estimate_actual_usage() const {
  // Relationship already uses .size() in estimate_memory_usage(),
  // so actual == allocated for this type.
  return estimate_memory_usage();
}

// Memory usage estimation implementation for Attribute
size_t Attribute::estimate_memory_usage() const {
  size_t total = sizeof(Attribute);

  // String storage
  total += _name.capacity();
  total += _type_name.capacity();

  // PrimVar memory - includes value::Value and value::TimeSamples
  total += _var.estimate_memory_usage();

  // Connection paths
  total += _paths.capacity() * sizeof(Path);
  for (const auto& path : _paths) {
    // Path internally contains strings, estimate their capacity
    total += path.full_path_name().capacity();
  }

  // Attribute metadata
  total += sizeof(AttrMeta); // Basic size of metadata structure

  return total;
}

size_t Attribute::estimate_actual_usage() const {
  size_t total = sizeof(Attribute);

  total += _name.size();
  total += _type_name.size();

  total += _var.estimate_actual_usage();

  total += _paths.size() * sizeof(Path);
  for (const auto& path : _paths) {
    total += path.full_path_name().size();
  }

  total += sizeof(AttrMeta);

  return total;
}

}  // namespace lightusd
