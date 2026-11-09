// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// path.hh - Path class for USD scene paths
//
#pragma once

#include <memory>
#include <string>
#include <utility>

#include "nonstd/optional.hpp"
#include "value-types.hh"
#include "tiny-string.hh"

namespace lightusd {

// Return false when invalid character(e.g. '%') exists in a given string.
// This function only validates `elementName` of a Prim(e.g. "dora", "xform1").
// If you want to validate a Prim path(e.g. "/root/xform1"),
// Use ValidatePrimPath() in path-util.hh
bool ValidatePrimElementName(const std::string &tok);

///
/// Simlar to SdfPath.
/// NOTE: We are doging refactoring of Path class, so the following comment may
/// not be correct.
///
/// We don't need the performance for USDZ, so use naiive implementation
/// to represent Path.
/// Path is something like Unix path, delimited by `/`, ':' and '.'
/// Square brackets('<', '>' is not included)
///
/// Root path is represented as prim path "/" and elementPath ""(empty).
///
/// Example:
///
/// - `/muda/bora.dora` : prim_part is `/muda/bora`, prop_part is `.dora`.
/// - `bora` : Could be Element(leaf) path or Relative path
///
/// ':' is a namespce delimiter(example `input:muda`).
///
/// Limitations:
///
/// - Relational attribute path(`[` `]`. e.g. `/muda/bora[/ari].dora`) is not
/// supported.
/// - Variant chars('{' '}') is not supported(yet).
/// - Relative path(e.g. '../') is not yet supported(TODO)
///
/// and have more limitatons.
///
namespace crate {
struct PathHasher;  // forward decl: defined in src/crate-format.hh
struct PathKeyEqual;
}  // namespace crate

class Path {
 public:
  // PathHasher and PathKeyEqual need to read the private _variant_part /
  // _variant_selection_part fields directly (see concern 5 in review.md),
  // so we friend them here. variant_part() is the public formatter but
  // allocates a std::string on every call, which is too expensive for a
  // hot-path hasher.
  friend struct crate::PathHasher;
  friend struct crate::PathKeyEqual;
  // Similar to SdfPathNode
  enum class PathType {
    Prim,
    PrimProperty,
    RelationalAttribute,
    MapperArg,
    Target,
    Mapper,
    PrimVariantSelection,
    Expression,
    Root,
  };

  Path() : _valid(false) {}

  static Path make_root_path() {
    // _assemble() sets _full="/", _prim_len=1, empty element, _valid=true.
    return Path("/", "");
  }

  // Create Path both from Prim Path and Prop
  // If `prim` starts
  // "/aaa", "bora" => /aaa.bora
  // "/aaa", "" => /aaa (prim only)
  // "", "bora" => .bora (property only)
  //
  // Note: This constructor may fail to extract elementName from given `prim`
  // and `prop`. It is highly recommended to use AppendPrim() and AppendProperty
  // to. construct Path hierarchy(e.g. `/aaa/xform/geom.points`) so that
  // elementName is set correctly.
  Path(const std::string &prim, const std::string &prop);

  // : prim_part(prim), valid(true) {}
  // Path(const std::string &prim, const std::string &prop)
  //    : prim_part(prim), prop_part(prop) {}

  Path(const Path &rhs)
      : _full(rhs._full),
        _prim_len(rhs._prim_len),
        _prop_len(rhs._prop_len),
        _elem_off(rhs._elem_off),
        _elem_len(rhs._elem_len),
        _variant(rhs._variant ? new VariantTokens(*rhs._variant) : nullptr),
        _path_type(rhs._path_type),
        _valid(rhs._valid) {}
  Path(Path &&rhs) noexcept = default;

  Path &operator=(const Path &rhs) {
    if (this != &rhs) {
      _full = rhs._full;
      _prim_len = rhs._prim_len;
      _prop_len = rhs._prop_len;
      _elem_off = rhs._elem_off;
      _elem_len = rhs._elem_len;
      _variant.reset(rhs._variant ? new VariantTokens(*rhs._variant) : nullptr);
      _path_type = rhs._path_type;
      _valid = rhs._valid;
    }
    return *this;
  }
  Path &operator=(Path &&rhs) noexcept = default;

  // The canonical path text ("prim" or "prim.prop") is stored ONCE in `_full`;
  // every part accessor returns a non-allocating tstring_view slice into it.
  //
  // NOTE: prim_part() / element_name() are *interior* slices and are therefore
  // NOT NUL-terminated at view.size(); use size-aware operations (==, <, ...)
  // and to_string(view) if you need an owned std::string. prop_part() and
  // full_path_view() ARE NUL-terminated (suffix / whole buffer).
  //
  // Lifetime: the returned view borrows from `_full`. The view is valid only
  // as long as this Path is alive and not mutated via _assemble() / _update().
  // See concern 3 in review.md.
  tstring_view full_path_view() const {
    return tstring_view(_full.data(), _full.size());
  }
  tstring_view prim_part_view() const {
    return tstring_view(_full.data(), _prim_len);
  }
  tstring_view prop_part_view() const {
    return _prop_len ? tstring_view(_full.data() + _prim_len + 1, _prop_len)
                     : tstring_view(_full.data() + _full.size(), size_t(0));
  }
  tstring_view element_name_view() const {
    return tstring_view(_full.data() + _elem_off, _elem_len);
  }

  // Back-compatible names, now returning views (return-type change).
  tstring_view prim_part() const { return prim_part_view(); }
  tstring_view prop_part() const { return prop_part_view(); }
  // element_name() is declared below and defined out-of-line (prim-types.cc).

  // Owning full path name ("prim" or "prim.prop"). Allocates; prefer
  // full_path_view() / append_full_path_name() on hot paths.
  std::string full_path_name() const {
    if (_valid) {
      return _full;
    }
    return "#INVALID#" + _full;
  }

  // Append the full path name into `out` with no intermediate allocation; reuse
  // one `out` buffer across a loop to avoid per-call allocation.
  void append_full_path_name(std::string *out) const {
    if (!out) return;
    if (!_valid) {
      out->append("#INVALID#");
    }
    out->append(_full);
  }

  // Formatted variant string "{var=sel}". Returns by value (no cached mutable
  // state -> safe to call on a shared const Path from multiple threads).
  std::string variant_part() const {
    return "{" + variant_part_raw() + "=" + variant_selection_raw() + "}";
  }

  // Raw (unformatted) variant tokens; empty strings when no variant authored.
  const std::string &variant_part_raw() const {
    return _variant ? _variant->part : empty_string();
  }
  const std::string &variant_selection_raw() const {
    return _variant ? _variant->selection : empty_string();
  }

  void set_path_type(const PathType ty) { _path_type = ty; }

  bool get_path_type(PathType &ty) {
    if (_path_type) {
      ty = _path_type.value();
      return true;
    }
    return false;
  }

  // IsPropertyPath: PrimProperty or RelationalAttribute
  bool is_property_path() const {
    if (_path_type) {
      if ((_path_type.value() == PathType::PrimProperty ||
           (_path_type.value() == PathType::RelationalAttribute))) {
        return true;
      }
    }

    // TODO: RelationalAttribute
    if (_prim_len == 0) {
      return false;
    }

    if (_prop_len) {
      return true;
    }

    return false;
  }

  // Is Prim path?
  bool is_prim_path() const {
    if (_prop_len) {
      return false;
    }

    if (_prim_len) {
      return true;
    }

    return false;
  }

  // Is Prim's property path?
  // True when both PrimPart and PropPart are not empty.
  bool is_prim_property_path() const {
    if (_prim_len == 0) {
      return false;
    }
    if (_prop_len) {
      return true;
    }
    return false;
  }

  bool is_valid() const { return _valid; }

  bool is_empty() {
    return (_prim_len == 0 && variant_part_raw().empty() && _prop_len == 0);
  }

  // static Path RelativePath() { return Path("."); }

  // Append property path(change internal state)
  Path append_property(const std::string &elem);

  // Append prim or variantSelection path(change internal state)
  Path append_element(const std::string &elem);
  Path append_prim(const std::string &elem) {
    return append_element(elem);
  }  // for legacy

  // Const version. Does not change internal state.
  const Path AppendProperty(const std::string &elem) const;
  const Path AppendPrim(const std::string &elem) const;
  const Path AppendElement(const std::string &elem) const;

  // Get element name(the last element of Path. i.e. Prim's name, Property's
  // name)
  tstring_view element_name() const;

  ///
  /// Split a path to the root(common ancestor) and its siblings
  ///
  /// example:
  ///
  /// - / -> [/, Empty]
  /// - /bora -> [/bora, Empty]
  /// - /bora/dora -> [/bora, /dora]
  /// - /bora/dora/muda -> [/bora, /dora/muda]
  /// - bora -> [Empty, bora]
  /// - .muda -> [Empty, .muda]
  ///
  std::pair<Path, Path> split_at_root() const;

  ///
  /// TODO: Deprecate(use get_parent_path() instead)
  ///
  /// Get parent Prim path.
  /// If the given path is a root Prim path(e.g. "/bora"), same Path is
  /// returned.
  ///
  /// example:
  ///
  /// - / -> invalid Path
  /// - /bora -> /bora
  /// - /bora/dora -> /bora
  /// - /bora/dora.prop -> /bora/dora
  /// - dora/bora -> dora
  /// - dora -> invalid Path
  /// - .dora -> invalid Path(path is property path)
  Path get_parent_prim_path() const;

  ///
  /// Get parent Path.
  /// If the given path is the root path("/") same Path is returned.
  ///
  /// example:
  ///
  /// - / -> invalid Path
  /// - /bora -> /
  /// - /bora/dora -> /bora
  /// - /bora/dora.prop -> /bora/dora
  /// - dora/bora -> dora
  /// - dora -> invalid Path
  /// - .dora -> invalid Path(path is property path)
  Path get_parent_path() const;

  ///
  /// Check if this Path has same prefix for given Path
  ///
  /// example.
  /// rhs path: /bora/dora
  ///
  /// /bora/dora/muda -> true
  /// /bora/dora2 -> fase
  ///
  /// If the prefix path contains prop part, compare it with ==
  /// (assume no hierarchy in property part)
  ///
  bool has_prefix(const Path &rhs) const;

  ///
  /// Replace Prim path prefix.
  /// example.
  /// srcPrefix = /bora/dora
  /// dstPrefix = /bora2/dora2
  ///
  /// /bora/dora/muda -> /bora2/dora2/muda
  ///
  bool replace_prefix(const Path &srcPrefix, const Path &dstPrefix);

  ///
  /// @returns true if a path is '/' only
  ///
  bool is_root_path() const {
    if (!_valid) {
      return false;
    }

    if ((_prim_len == 1) && (_full[0] == '/')) {
      return true;
    }

    return false;
  }

  ///
  /// @returns true if a path is root prim: e.g. '/bora'
  ///
  bool is_root_prim() const {
    if (!_valid) {
      return false;
    }

    if (is_root_path()) {
      return false;
    }

    if ((_prim_len > 1) && (_full[0] == '/')) {
      // no other '/' in the prim region except for the first one
      if (_full.rfind('/', size_t(_prim_len) - 1) == 0) {
        return true;
      }
    }

    return false;
  }

  bool is_absolute_path() const {
    if (_prim_len && _full[0] == '/') {
      return true;
    }

    return false;
  }

  bool is_relative_path() const {
    if (_prim_len) {
      return !is_absolute_path();
    }

    return true;  // prop part only
  }

  // Strip '/'
  Path &make_relative() LIGHTUSD_LIFETIMEBOUND {
    if (is_absolute_path() && (_prim_len > 1)) {
      // Remove leading '/' from the single buffer and shift offsets left by 1.
      _full.erase(0, 1);
      _prim_len -= 1;
      if (_elem_off > 0) _elem_off -= 1;
    }
    return *this;
  }

  const Path make_relative(Path &&rhs) {
    (*this) = std::move(rhs);

    return make_relative();
  }

  static const Path make_relative(const Path &rhs) {
    Path p = rhs;  // copy
    return p.make_relative();
  }

  static bool LessThan(const Path &lhs, const Path &rhs);

  // To sort paths lexicographically.
  // TODO: consider abs and relative path correctly
  bool operator<(const Path &rhs) const {
    // Equal iff both parts match (no full_path_name() allocation).
    if (prim_part_view() == rhs.prim_part_view() &&
        prop_part_view() == rhs.prop_part_view()) {
      return false;
    }

    if (_prim_len == 0 || rhs._prim_len == 0) {
      return _prim_len == 0 && rhs._prim_len != 0;
    }

    return LessThan(*this, rhs);
  }

  ///
  /// Estimate memory usage of this Path in bytes
  ///
  size_t estimate_memory_usage() const {
    size_t total = sizeof(Path);
    total += _full.capacity();
    if (_variant) {
      total += sizeof(VariantTokens);
      total += _variant->part.capacity();
      total += _variant->selection.capacity();
    }
    return total;
  }

 private:
  void _update(const std::string &p, const std::string &prop);

  // Single canonical owning buffer "prim" or "prim.prop" (e.g.
  // "/Model/MyMesh.visibility"); all string parts are views into this. Built
  // eagerly at construction/mutation, immutable afterwards -> lock-free reads.
  std::string _full;
  uint32_t _prim_len{0};   // prim_part = _full[0 .. _prim_len)
  uint32_t _prop_len{0};   // prop_part = _full[_prim_len+1 .. +_prop_len); 0 = no prop
  uint32_t _elem_off{0};   // element_name = _full[_elem_off .. +_elem_len)
  uint32_t _elem_len{0};

  // Variant tokens are rare: stored out-of-line so the common no-variant Path
  // pays one null pointer instead of two inline strings (64 bytes).
  // `{var=sel}` is also embedded in the prim region of `_full`.
  struct VariantTokens {
    std::string part;       // e.g. `variantColor` for {variantColor=green}
    std::string selection;  // e.g. `green`; could be empty.
  };
  VariantTokens &mutable_variant() LIGHTUSD_LIFETIMEBOUND {
    if (!_variant) {
      _variant.reset(new VariantTokens());
    }
    return *_variant;
  }
  static const std::string &empty_string() {
    static const std::string s_empty;
    return s_empty;
  }
  std::unique_ptr<VariantTokens> _variant;

  nonstd::optional<PathType> _path_type;  // Currently optional.

  bool _valid{false};

  // Assemble `_full` + offsets from prim/prop parts (eager, no mutable state).
  // Defined in prim-types.cc.
  void _assemble(const std::string &prim, const std::string &prop);

  // Slice form of _assemble: builds `_full` from caller-owned slices without
  // copying either part into an intermediate string first. The slices need not
  // be NUL-terminated. Defined in prim-types.cc.
  void _assemble_slices(const char *prim, size_t prim_len, const char *prop,
                        size_t prop_len);
};

bool operator==(const Path &lhs, const Path &rhs);

// SdfRelocates: an ordered list of (source, target) path pairs (layer- or
// prim-level namespace relocations). Aliased so it can be passed to the
// single-argument-aware DEFINE_TYPE_TRAIT macro (the bare type contains a
// comma).
using SdfRelocates = std::vector<std::pair<Path, Path>>;

namespace value {

#include "define-type-trait.inc"

DEFINE_TYPE_TRAIT(Path, "Path", TYPE_ID_PATH, 1);
// TODO(syoyo): Define as 1D array?
DEFINE_TYPE_TRAIT(std::vector<Path>, "PathVector", TYPE_ID_PATH_VECTOR, 1);
// SdfRelocates: an ordered list of (source, target) path pairs. Crate type 58
// (crate >= 0.11.0). Not array-capable in Crate (the vector IS the value).
DEFINE_TYPE_TRAIT(SdfRelocates, "Relocates", TYPE_ID_RELOCATES, 1);

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

}  // namespace lightusd
