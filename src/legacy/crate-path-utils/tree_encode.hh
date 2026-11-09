//
// Tree encoding for USD Crate format v0.4.0+
// SPDX-License-Identifier: Apache 2.0
//
// Implements the compressed PATHS encoding used in Crate v0.4.0+.
// Works with any implementation of the IPath interface.
//
#pragma once

#include "path_interface.hh"
#include <vector>
#include <cstdint>
#include <map>
#include <string>
#include <memory>

namespace crate {

///
/// Token index type for element names
/// Negative values indicate property paths
///
using TokenIndex = int32_t;

///
/// Index into the original paths vector
///
using PathIndex = uint64_t;

///
/// Token table for mapping strings to indices
///
class TokenTable {
public:
  TokenTable() : next_index_(0) {}

  /// Get or create token index for a string
  /// Properties use negative indices
  TokenIndex GetOrCreateToken(const std::string& str, bool is_property);

  /// Get token string from index
  std::string GetToken(TokenIndex index) const;

  /// Get all tokens for serialization
  const std::map<std::string, TokenIndex>& GetTokens() const
      LIGHTUSD_LIFETIMEBOUND {
    return tokens_;
  }
  const std::map<TokenIndex, std::string>& GetReverseTokens() const
      LIGHTUSD_LIFETIMEBOUND {
    return reverse_tokens_;
  }

  /// Clear all tokens
  void Clear();

private:
  std::map<std::string, TokenIndex> tokens_;
  std::map<TokenIndex, std::string> reverse_tokens_;
  TokenIndex next_index_;
};

///
/// Compressed path tree representation (Crate v0.4.0+ format)
///
/// This is the output of tree encoding, consisting of three parallel arrays:
///
struct CompressedPathTree {
  /// Index into original paths vector for each node
  std::vector<PathIndex> path_indexes;

  /// Token index for element name (negative = property)
  std::vector<TokenIndex> element_token_indexes;

  /// Navigation information:
  /// -2 = leaf node
  /// -1 = only child follows
  ///  0 = only sibling follows
  /// >0 = both child and sibling (value is offset to sibling)
  std::vector<int32_t> jumps;

  /// Token table used for encoding
  TokenTable token_table;

  size_t size() const { return path_indexes.size(); }
  bool empty() const { return path_indexes.empty(); }

  void clear() {
    path_indexes.clear();
    element_token_indexes.clear();
    jumps.clear();
    token_table.Clear();
  }
};

///
/// Encode sorted paths into compressed tree format
///
/// Input paths MUST be sorted using SortPaths() before calling this.
///
/// @param sorted_paths Vector of paths in sorted order
/// @return Compressed tree representation
///
/// Example:
///   std::vector<SimplePath> paths = {...};
///   SortSimplePaths(paths);
///   CompressedPathTree tree = EncodePaths(paths);
///
CompressedPathTree EncodePaths(const std::vector<SimplePath>& sorted_paths);

///
/// Encode sorted paths (generic interface version)
///
/// Works with any path type implementing IPath interface.
/// Paths are accessed via pointers/references.
///
template<typename PathPtr>
CompressedPathTree EncodePathsGeneric(const std::vector<PathPtr>& sorted_paths) {
  // Convert to SimplePath for encoding
  std::vector<SimplePath> simple_paths;
  simple_paths.reserve(sorted_paths.size());

  for (const auto& path_ptr : sorted_paths) {
    const IPath& path = *path_ptr;
    simple_paths.emplace_back(path.GetPrimPart(), path.GetPropertyPart());
  }

  return EncodePaths(simple_paths);
}

///
/// Decode compressed tree back to paths
///
/// @param compressed Compressed tree representation
/// @return Vector of paths in original order
///
std::vector<SimplePath> DecodePaths(const CompressedPathTree& compressed);

///
/// Validate that encode/decode round-trip preserves paths
///
/// @param original Original sorted paths
/// @param compressed Compressed representation
/// @param errors Output vector for error messages
/// @return true if validation passes, false otherwise
///
bool ValidateRoundTrip(
  const std::vector<SimplePath>& original,
  const CompressedPathTree& compressed,
  std::vector<std::string>* errors = nullptr
);

} // namespace crate
