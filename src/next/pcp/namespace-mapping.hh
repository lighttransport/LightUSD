// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - PCP namespace mapping + layer offset (header-only)
//
// Prefix remap (analogue of OpenUSD PcpMapFunction) used to translate a
// referenced/inherited prim's namespace into the referencing prim's namespace.
// A mapping is a SET of (source prefix -> target prefix) pairs, matched
// longest-source-prefix first: one pair per arc, plus one pair per relocate
// that renames a subtree underneath that arc. A single pair could not express
// both (a relocate authored in a referenced layer stack renames a subtree of
// the arc's namespace, e.g. {/CharRig -> /Char, /CharRig/Anim -> /Char/Anim2}).
// Standalone, C++14.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lightusd {
namespace next {
namespace pcp {

/// An immutable, refcounted path string: one pointer, shared by copies.
/// Mapping pairs repeat the same few paths millions of times (full-LOD
/// benchmark scene: 20.6M pair strings, 3.0M distinct) -- every composed mapping
/// carries its inner pairs' sources and its outer pairs' targets -- so copies
/// share one node instead of each owning a std::string. Empty == null.
class PathStr {
 public:
  PathStr() = default;
  PathStr(const std::string &s) : n_(s.empty() ? nullptr : new Node(s)) {}  // NOLINT
  PathStr(std::string &&s)  // NOLINT
      : n_(s.empty() ? nullptr : new Node(std::move(s))) {}
  PathStr(const char *s) : PathStr(std::string(s)) {}  // NOLINT
  PathStr(const PathStr &o) noexcept : n_(o.n_) { retain(); }
  PathStr(PathStr &&o) noexcept : n_(o.n_) { o.n_ = nullptr; }
  PathStr &operator=(const PathStr &o) noexcept {
    if (n_ != o.n_) {
      release();
      n_ = o.n_;
      retain();
    }
    return *this;
  }
  PathStr &operator=(PathStr &&o) noexcept {
    if (this != &o) {
      release();
      n_ = o.n_;
      o.n_ = nullptr;
    }
    return *this;
  }
  ~PathStr() { release(); }

  const std::string &str() const { return n_ ? n_->s : Empty(); }
  operator const std::string &() const { return str(); }  // NOLINT
  bool empty() const { return !n_; }
  size_t size() const { return n_ ? n_->s.size() : 0; }
  std::string substr(size_t pos, size_t n = std::string::npos) const {
    return str().substr(pos, n);
  }

  friend bool operator==(const PathStr &a, const PathStr &b) {
    return a.n_ == b.n_ || a.str() == b.str();
  }
  friend bool operator!=(const PathStr &a, const PathStr &b) { return !(a == b); }
  friend bool operator==(const PathStr &a, const std::string &b) { return a.str() == b; }
  friend bool operator==(const std::string &a, const PathStr &b) { return a == b.str(); }
  friend bool operator!=(const PathStr &a, const std::string &b) { return a.str() != b; }
  friend bool operator!=(const std::string &a, const PathStr &b) { return a != b.str(); }
  friend bool operator==(const PathStr &a, const char *b) { return a.str() == b; }
  friend bool operator!=(const PathStr &a, const char *b) { return a.str() != b; }
  friend bool operator<(const PathStr &a, const PathStr &b) { return a.str() < b.str(); }
  friend std::string operator+(const PathStr &a, const std::string &b) { return a.str() + b; }
  friend std::string operator+(const std::string &a, const PathStr &b) { return a + b.str(); }
  friend std::string operator+(const PathStr &a, const char *b) { return a.str() + b; }

 private:
  struct Node {
    explicit Node(const std::string &v) : s(v) {}
    explicit Node(std::string &&v) : s(std::move(v)) {}
    std::atomic<uint32_t> refs{1};
    std::string s;
  };
  static const std::string &Empty() {
    static const std::string kEmpty;
    return kEmpty;
  }
  void retain() const {
    if (n_) n_->refs.fetch_add(1, std::memory_order_relaxed);
  }
  void release() {
    if (n_ && n_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete n_;
    n_ = nullptr;
  }

  Node *n_ = nullptr;
};

/// Time offset/scale applied to time samples coming through an arc.
struct LayerOffset {
  double offset = 0.0;
  double scale = 1.0;

  /// Compose: `child` applied underneath `this` (this ∘ child).
  LayerOffset Compose(const LayerOffset &child) const {
    return LayerOffset{offset + scale * child.offset, scale * child.scale};
  }
  double Apply(double t) const { return offset + scale * t; }
  bool is_identity() const { return offset == 0.0 && scale == 1.0; }
};

/// Prefix remap: paths under a pair's source prefix are rewritten under its
/// target prefix. Empty == identity. Stores the mapping from a node's namespace
/// to ROOT (composed once when the child arc is created).
struct NamespaceMapping {
  using Pair = std::pair<PathStr, PathStr>;  // (source, target)

  std::vector<Pair> pairs;
  /// True once this mapping crosses a composition arc (reference/payload/
  /// class/variant). A relationship/connection target outside the arc's
  /// namespace is then unmappable and dropped (pxr behavior). A mapping made
  /// only of relocate renames within one layer stack keeps identity fallback:
  /// paths it does not rename still address the same composed prims.
  bool crosses_arc = false;
  /// True for a LOCAL class-based arc (inherit/specialize whose class and
  /// instance live in the SAME layer stack). pxr gives such arcs an identity-
  /// plus-pair map function: target paths outside the class namespace pass
  /// through unchanged, EXCEPT paths at/under the arc's destination (the
  /// inheriting instance) — those are "invalid instance target" errors
  /// (non-invertible path translation) and are dropped.
  bool intra_stack = false;

  NamespaceMapping() = default;
  NamespaceMapping(std::string source_prefix, std::string target_prefix)
      : crosses_arc(true) {
    pairs.emplace_back(std::move(source_prefix), std::move(target_prefix));
  }

  bool is_identity() const { return pairs.empty(); }

  // Compatibility accessors: the ARC pair is always the first one (relocate
  // pairs are appended underneath it).
  const std::string &source_prefix() const {
    static const std::string kEmpty;
    return pairs.empty() ? kEmpty : pairs.front().first.str();
  }
  const std::string &target_prefix() const {
    static const std::string kEmpty;
    return pairs.empty() ? kEmpty : pairs.front().second.str();
  }

  /// Is `site` the ROOT of one of this mapping's renamed namespaces, i.e. the
  /// prim an arc (or relocate) targeted directly? Ancestral/positional sources
  /// -- a child re-rooted under an arc -- map from an ANCESTOR prefix instead.
  bool MapsFrom(const std::string &site) const {
    for (const Pair &p : pairs) {
      if (p.first == site) return true;
    }
    return false;
  }

  /// Does `path` lie at or under `prefix` (namespace boundary aware)? The char
  /// after the prefix is '/' for a descendant prim ("/Ref/Child") or '.' for a
  /// property on the prefix prim itself ("/Ref.attr").
  static bool AtOrUnder(const std::string &path, const std::string &prefix) {
    if (path == prefix) return true;
    return path.size() > prefix.size() &&
           path.compare(0, prefix.size(), prefix) == 0 &&
           (path[prefix.size()] == '/' || path[prefix.size()] == '.');
  }

  /// Index of the longest source prefix matching `path`, or -1.
  int MatchSource(const std::string &path) const {
    int best = -1;
    size_t best_len = 0;
    for (size_t i = 0; i < pairs.size(); ++i) {
      const std::string &src = pairs[i].first;
      if (!AtOrUnder(path, src)) continue;
      if (best < 0 || src.size() > best_len) {
        best = static_cast<int>(i);
        best_len = src.size();
      }
    }
    return best;
  }

  int MatchTarget(const std::string &path) const {
    int best = -1;
    size_t best_len = 0;
    for (size_t i = 0; i < pairs.size(); ++i) {
      const std::string &tgt = pairs[i].second;
      if (tgt.empty()) continue;  // DROP-marker pair: matches nothing
      if (!AtOrUnder(path, tgt)) continue;
      if (best < 0 || tgt.size() > best_len) {
        best = static_cast<int>(i);
        best_len = tgt.size();
      }
    }
    return best;
  }

  /// Map a path string from source namespace to target namespace. A path
  /// outside every source prefix is global and passes through unchanged.
  std::string Apply(const std::string &path) const {
    const int i = MatchSource(path);
    if (i < 0) return path;
    const Pair &p = pairs[static_cast<size_t>(i)];
    // DROP-marker pair (see ApplyTarget): sites are never dropped — keep the
    // identity fallback for non-target path mapping.
    if (p.second.empty()) return path;
    return p.second + path.substr(p.first.size());
  }

  /// Apply() for a pair string, sharing the input (unmapped path) or the
  /// pair target (exact prefix match) instead of building a new string.
  PathStr ApplyShared(const PathStr &path) const {
    const int i = MatchSource(path);
    if (i < 0) return path;
    const Pair &p = pairs[static_cast<size_t>(i)];
    if (p.second.empty()) return path;
    if (p.first.size() == path.size()) return p.second;
    return PathStr(p.second + path.substr(p.first.size()));
  }

  /// Map a target-namespace path back into this mapping's source namespace.
  std::string ReverseApply(const std::string &path) const {
    const int i = MatchTarget(path);
    if (i < 0) return path;
    const Pair &p = pairs[static_cast<size_t>(i)];
    return p.first + path.substr(p.second.size());
  }

  /// Map a relationship/connection TARGET path. Unlike Apply(), a path that
  /// falls OUTSIDE an arc-crossing mapping's namespace returns "" -- such
  /// targets cannot be expressed in the composed namespace (pxr warns "path
  /// outside the scope of the reference" and drops them). Leaking them verbatim
  /// silently aliased unrelated composed prims.
  std::string ApplyTarget(const std::string &path) const {
    const int i = MatchSource(path);
    if (i < 0) {
      if (intra_stack) {
        // Local class arc: identity outside the class namespace, except
        // under the destination (invalid instance target, dropped).
        return MatchTarget(path) >= 0 ? std::string() : path;
      }
      return crosses_arc ? std::string() : path;
    }
    const Pair &p = pairs[static_cast<size_t>(i)];
    // An empty pair target is a DROP marker: content under this source is
    // not addressable through the arc (e.g. a relocate whose destination
    // lies outside the arc's namespace) — the target is unmappable.
    if (p.second.empty()) return std::string();
    return p.second + path.substr(p.first.size());
  }

  /// Add a prefix rename applied UNDERNEATH this mapping's namespace, i.e. in
  /// the same namespace as its sources (a relocate authored by the layer stack
  /// this mapping maps from). `source` and `target` are both source-namespace
  /// paths; the resulting pair maps `source` to wherever `target` composes to.
  void AddRename(const std::string &source, const std::string &composed_target) {
    pairs.emplace_back(source, composed_target);
  }

  /// Compose two mappings. `outer` maps mid->root, `inner` maps deep->mid;
  /// the result maps deep->root (so a nested reference resolves straight to
  /// root space). Prefix-map composition (pxr PcpMapFunction::Compose): every
  /// inner pair is carried through `outer`, and every outer pair whose source
  /// lies INSIDE an inner pair's target is pulled back into the inner (deep)
  /// namespace -- that is how a relocate authored closer to the root renames a
  /// subtree that a deeper reference delivers.
  static NamespaceMapping Compose(const NamespaceMapping &outer,
                                  const NamespaceMapping &inner) {
    if (inner.is_identity()) return outer;
    if (outer.is_identity()) return inner;
    NamespaceMapping out;
    out.crosses_arc = outer.crosses_arc || inner.crosses_arc;
    // An intra-stack class map stays intra-stack when composed with a mapping
    // that does not itself cross an arc (identity or relocate renames within
    // the same stack). Crossing a real arc (reference/payload) demotes it to
    // strict cross-arc target dropping.
    out.intra_stack =
        (inner.intra_stack && (outer.intra_stack || !outer.crosses_arc)) ||
        (outer.intra_stack && (inner.intra_stack || !inner.crosses_arc));
    for (const Pair &in : inner.pairs) {
      out.pairs.emplace_back(in.first, outer.ApplyShared(in.second));
      for (const Pair &o : outer.pairs) {
        // An outer rename authored strictly below this inner pair's target maps
        // a deeper slice of the inner namespace.
        if (o.first.size() > in.second.size() && AtOrUnder(o.first, in.second)) {
          out.pairs.emplace_back(in.first + o.first.substr(in.second.size()),
                                 o.second);
        }
      }
    }
    // A path the inner mapping does not rename passes through it unchanged, so it
    // is ALSO an intermediate-namespace path and is then mapped by `outer`. Carry
    // an outer pair through only when it addresses such a genuine pass-through
    // region: its source must lie OUTSIDE every inner mapped subtree (neither
    // under an inner source, which the first loop already handles, nor under an
    // inner TARGET -- a mid path produced by inner from a DIFFERENT deep path,
    // where reusing it would alias an unrelated deep prim of the same spelling,
    // e.g. a referenced layer's own `/B` vs. the outer arc's `/B`). This is what
    // lets a class arc reached through a reference still map a SIBLING path of
    // its own namespace (`/CharRig/_Class_ToesRig`, the implied-class site)
    // through that reference, without leaking cross-arc target aliases.
    for (const Pair &o : outer.pairs) {
      bool reachable = true;
      for (const Pair &in : inner.pairs) {
        // A variant-strip inner pair maps the variant CONTENT site onto its
        // own host namespace: either the crate holder child (source under its
        // own target, "/X/{s=v}" -> "/X") or the USDA content-root sentinel
        // ("/__self__" -> "/X", which matches no real host path). Host-
        // namespace paths authored in the variant body (connection/rel
        // targets) pass through such a pair UNCHANGED and must still be
        // mapped by `outer`; longest-prefix matching keeps content-site paths
        // on the inner pair, so carrying the outer pair cannot alias. Only an
        // outer source under the inner SOURCE is already covered above.
        // An intra-stack class pair (local inherit/specialize) likewise maps
        // between SIBLING sites of one namespace: paths outside the class
        // pass through it unchanged and remain addressable by `outer`.
        if (in.first == "/__self__" || AtOrUnder(in.first, in.second) ||
            inner.intra_stack) {
          if (AtOrUnder(o.first, in.first)) {
            reachable = false;
            break;
          }
          continue;
        }
        if (AtOrUnder(o.first, in.first) || AtOrUnder(o.first, in.second) ||
            AtOrUnder(in.first, o.first)) {
          reachable = false;
          break;
        }
      }
      if (reachable) out.pairs.push_back(o);
    }
    return out;
  }
};

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
