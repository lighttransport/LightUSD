// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - USD Path representation
// Lightweight path class for USD scene hierarchy

#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace lightusd {
namespace next {

/// Path - represents a USD scene path
/// Examples: "/World/Cube", "/Materials/Metal.outputs:surface"
class Path {
public:
  /// Default constructor - creates empty path
  Path() = default;

  /// Construct from string
  explicit Path(const std::string& path_str);
  explicit Path(std::string&& path_str);
  explicit Path(const char* path_str);

  /// Copy and move
  Path(const Path&) = default;
  Path(Path&&) = default;
  Path& operator=(const Path&) = default;
  Path& operator=(Path&&) = default;

  // ============================================================
  // Path queries
  // ============================================================

  /// Check if path is empty
  bool empty() const { return path_.empty(); }

  /// Check if this is an absolute path (starts with '/')
  bool is_absolute() const;

  /// Check if this is the root path ("/")
  bool is_root() const;

  /// Check if path contains a property component (has '.')
  bool has_property() const;

  /// Validate this path against the AOUSD §8.3 textual path grammar
  /// (strict UTF-8 + Unicode XID identifiers). Empty paths are invalid.
  /// Construction stays unvalidated (hot composition paths build Paths from
  /// already-validated parser output); call this at authoring boundaries or
  /// use Parse() for untrusted input.
  bool is_valid() const;

  /// Validating factory for untrusted input: returns an empty Path when
  /// `path_str` fails the AOUSD §8.3 path grammar.
  static Path Parse(const std::string& path_str);

  /// Get the full path string
  const std::string& str() const { return path_; }
  const char* c_str() const { return path_.c_str(); }

  // ============================================================
  // Path components
  // ============================================================

  /// Get parent path (e.g., "/World/Cube" -> "/World")
  Path parent() const;

  /// Get the final element name (e.g., "/World/Cube" -> "Cube")
  std::string name() const;

  /// Get property name if present (e.g., "/Cube.xformOp:translate" -> "xformOp:translate")
  std::string property_name() const;

  /// Get prim path without property (e.g., "/Cube.xformOp:translate" -> "/Cube")
  Path prim_path() const;

  /// Split into individual path elements
  std::vector<std::string> elements() const;

  // ============================================================
  // Path manipulation
  // ============================================================

  /// Append a child path element
  Path append_child(const std::string& child_name) const;

  /// Append a property
  Path append_property(const std::string& property_name) const;

  /// Make path relative to another path
  Path make_relative_to(const Path& base) const;

  // ============================================================
  // Comparison
  // ============================================================

  bool operator==(const Path& other) const { return path_ == other.path_; }
  bool operator!=(const Path& other) const { return path_ != other.path_; }
  bool operator<(const Path& other) const { return path_ < other.path_; }

  // ============================================================
  // Static factory methods
  // ============================================================

  /// Get the root path singleton
  static const Path& root();

  /// Get an empty path singleton
  static const Path& empty_path();

private:
  std::string path_;
};

/// Stream output for debugging
std::ostream& operator<<(std::ostream& os, const Path& path);

}  // namespace next
}  // namespace lightusd

// Hash specialization for use in unordered containers
namespace std {
template <>
struct hash<lightusd::next::Path> {
  size_t operator()(const lightusd::next::Path& path) const {
    return hash<string>()(path.str());
  }
};
}  // namespace std
