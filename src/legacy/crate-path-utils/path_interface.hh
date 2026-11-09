//
// Path interface for Crate format encoding
// SPDX-License-Identifier: Apache 2.0
//
// This provides an abstract interface for paths, allowing the sorting
// and tree encoding algorithms to work with any path implementation.
//
#pragma once

#include <string>
#include <vector>

#include "compiler-features.hh"

// Disable weak-vtables warning for interface classes
// These are header-only interfaces, vtable duplication is acceptable
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wweak-vtables"
#endif

namespace crate {

///
/// Abstract interface for USD-like paths
///
/// This allows the sorting and encoding algorithms to work with
/// different path implementations (LightUSD Path, OpenUSD SdfPath, etc.)
///
class IPath {
public:
  virtual ~IPath() = default;
  IPath() = default;
  IPath(const IPath&) = default;
  IPath(IPath&&) = default;
  IPath& operator=(const IPath&) = default;
  IPath& operator=(IPath&&) = default;

  /// Get the full path as a string (e.g., "/World/Geom.points")
  virtual std::string GetString() const = 0;

  /// Get the prim part of the path (e.g., "/World/Geom")
  virtual std::string GetPrimPart() const = 0;

  /// Get the property part of the path (e.g., "points", empty if no property)
  virtual std::string GetPropertyPart() const = 0;

  /// Is this an absolute path (starts with '/')?
  virtual bool IsAbsolute() const = 0;

  /// Is this a prim path (no property part)?
  virtual bool IsPrimPath() const = 0;

  /// Is this a property path (has both prim and property parts)?
  virtual bool IsPropertyPath() const = 0;

  /// Clone this path
  virtual IPath* Clone() const = 0;
};

///
/// Simple concrete implementation of IPath for standalone use
///
class SimplePath : public IPath {
public:
  SimplePath() = default;
  ~SimplePath() override = default;
  SimplePath(const SimplePath&) = default;
  SimplePath(SimplePath&&) noexcept = default;
  SimplePath& operator=(const SimplePath&) = default;
  SimplePath& operator=(SimplePath&&) noexcept = default;

  SimplePath(const std::string& prim, const std::string& prop = "")
    : prim_part_(prim), prop_part_(prop) {}

  std::string GetString() const override {
    if (prop_part_.empty()) {
      return prim_part_;
    }
    return prim_part_ + "." + prop_part_;
  }

  std::string GetPrimPart() const override { return prim_part_; }
  std::string GetPropertyPart() const override { return prop_part_; }

  bool IsAbsolute() const override {
    return !prim_part_.empty() && prim_part_[0] == '/';
  }

  bool IsPrimPath() const override {
    return !prim_part_.empty() && prop_part_.empty();
  }

  bool IsPropertyPath() const override {
    return !prim_part_.empty() && !prop_part_.empty();
  }

  IPath* Clone() const override {
    return new SimplePath(prim_part_, prop_part_);
  }

  // Direct accessors for SimplePath
  const std::string& prim_part() const LIGHTUSD_LIFETIMEBOUND {
    return prim_part_;
  }
  const std::string& prop_part() const LIGHTUSD_LIFETIMEBOUND {
    return prop_part_;
  }

private:
  std::string prim_part_;
  std::string prop_part_;
};

} // namespace crate

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
