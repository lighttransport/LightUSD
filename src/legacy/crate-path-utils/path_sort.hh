//
// Path sorting for USD Crate format
// SPDX-License-Identifier: Apache 2.0
//
// Implements OpenUSD-compatible path sorting for Crate format encoding.
// Works with any implementation of the IPath interface.
//
#pragma once

#include "path_interface.hh"
#include <vector>
#include <memory>
#include <algorithm>

namespace crate {

///
/// Compare two paths following OpenUSD SdfPath comparison rules
///
/// Returns:
///   < 0 if lhs < rhs
///   = 0 if lhs == rhs
///   > 0 if lhs > rhs
///
/// Rules:
/// 1. Absolute paths are less than relative paths
/// 2. For paths at different depths, compare after normalizing to same depth
/// 3. At same depth, compare elements lexicographically
/// 4. Prim parts are compared before property parts
///
int ComparePaths(const IPath& lhs, const IPath& rhs);

///
/// Precomputed (parsed) sort key for a path.
///
/// ComparePaths() re-parses both paths' prim parts into elements on every
/// invocation, which makes N-log-N sorts over large spec/path tables
/// allocation-bound. Parse each path once with MakeParsedSimplePath() and
/// sort with CompareParsedPaths() instead — the ordering is identical
/// (ComparePaths() itself is implemented on top of these).
///
struct PathElement {
  std::string name;
  bool is_absolute = false;
  bool is_property = false;
  int depth = 0;

  PathElement() = default;
  PathElement(const std::string& n, bool abs, bool prop, int d)
    : name(n), is_absolute(abs), is_property(prop), depth(d) {}
};

struct ParsedSimplePath {
  std::vector<PathElement> prim_elements;  // prim part only
  bool is_absolute = false;
  std::string prop;                        // property part ("" if none)
};

ParsedSimplePath MakeParsedSimplePath(const std::string& prim_part,
                                      const std::string& prop_part);

int CompareParsedPaths(const ParsedSimplePath& lhs,
                       const ParsedSimplePath& rhs);

///
/// Sort a vector of paths in-place using OpenUSD-compatible ordering
///
/// This modifies the input vector to be in sorted order.
/// Paths must remain valid for the duration of the sort.
///
template<typename PathPtr>
void SortPaths(std::vector<PathPtr>& paths) {
  std::sort(paths.begin(), paths.end(),
            [](const PathPtr& lhs, const PathPtr& rhs) {
              return ComparePaths(*lhs, *rhs) < 0;
            });
}

///
/// Specialization for SimplePath (direct comparison without pointers)
///
void SortSimplePaths(std::vector<SimplePath>& paths);

} // namespace crate
