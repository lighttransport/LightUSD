// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Path implementation

#include "path.hh"
#include "../c/lightusd-core-c.h"  // lu_path_*
#include <ostream>

#include "identifier.hh"

namespace lightusd {
namespace next {

std::ostream& operator<<(std::ostream& os, const Path& path) {
  return os << path.str();
}

bool Path::is_valid() const {
  return !path_.empty() && IsValidPathString(path_);
}

Path Path::Parse(const std::string& path_str) {
  if (path_str.empty() || !IsValidPathString(path_str)) return Path();
  return Path(path_str);
}

Path::Path(const std::string& path_str) : path_(path_str) {}

Path::Path(std::string&& path_str) : path_(std::move(path_str)) {}

Path::Path(const char* path_str) : path_(path_str ? path_str : "") {}

bool Path::is_absolute() const {
  return !path_.empty() && path_[0] == '/';
}

bool Path::is_root() const {
  return path_ == "/";
}

bool Path::has_property() const {
  return path_.find('.') != std::string::npos;
}

Path Path::parent() const {
  const size_t n = path_.size();
  std::string out(n ? n : 1, '\0');
  size_t len = 0;
  if (!lu_path_parent(path_.data(), n, &out[0], out.size(), &len)) return Path();
  out.resize(len);
  return Path(std::move(out));
}

std::string Path::name() const {
  const size_t n = path_.size();
  std::string out(n ? n : 1, '\0');
  size_t len = 0;
  lu_path_name(path_.data(), n, &out[0], out.size(), &len);
  out.resize(len);
  return out;
}

std::string Path::property_name() const {
  const size_t n = path_.size();
  std::string out(n ? n : 1, '\0');
  size_t len = 0;
  lu_path_property_name(path_.data(), n, &out[0], out.size(), &len);
  out.resize(len);
  return out;
}

Path Path::prim_path() const {
  const size_t n = path_.size();
  std::string out(n ? n : 1, '\0');
  size_t len = 0;
  lu_path_prim_path(path_.data(), n, &out[0], out.size(), &len);
  out.resize(len);
  return Path(std::move(out));
}

std::vector<std::string> Path::elements() const {
  std::vector<std::string> result;
  const std::string prim = prim_path().str();
  size_t count = 0;
  lu_path_element_count(prim.data(), prim.size(), &count);
  result.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    std::string e(prim.size() ? prim.size() : 1, '\0');
    size_t len = 0;
    lu_path_element(prim.data(), prim.size(), i, &e[0], e.size(), &len);
    e.resize(len);
    result.push_back(std::move(e));
  }
  return result;
}

namespace {
// a + sep + b, allocated at exactly its length: `x + "/" + y` grows its
// temporary by doubling, and a composed stage keeps ~10M of these path
// strings (full-LOD benchmark scene: 2.7 GiB of heap for 1.7 GiB of text).
std::string JoinExact(const std::string& a, const char* sep,
                      const std::string& b) {
  const size_t sl = std::char_traits<char>::length(sep);
  std::string out;
  out.reserve(a.size() + sl + b.size());
  out.append(a).append(sep, sl).append(b);
  return out;
}
}  // namespace

Path Path::append_child(const std::string& child_name) const {
  // Authoring boundary: an appended component must be a valid identifier
  // (strict UTF-8 + XID). Invalid input yields an empty Path.
  if (!IsValidIdentifier(child_name)) {
    return Path();
  }
  if (path_.empty()) {
    return Path(JoinExact(std::string(), "/", child_name));
  }
  if (path_.back() == '/') {
    return Path(JoinExact(path_, "", child_name));
  }
  return Path(JoinExact(path_, "/", child_name));
}

Path Path::append_child_unchecked(const std::string& child_name) const {
  if (path_.empty()) {
    return Path(JoinExact(std::string(), "/", child_name));
  }
  if (path_.back() == '/') {
    return Path(JoinExact(path_, "", child_name));
  }
  return Path(JoinExact(path_, "/", child_name));
}

Path Path::append_property(const std::string& prop_name) const {
  // Authoring boundary: property names must be valid (possibly namespaced)
  // identifiers. Invalid input yields an empty Path.
  if (!IsValidNamespacedIdentifier(prop_name)) {
    return Path();
  }
  if (has_property()) {
    // Already has property - replace it
    return Path(JoinExact(prim_path().str(), ".", prop_name));
  }
  return Path(JoinExact(path_, ".", prop_name));
}

Path Path::make_relative_to(const Path& base) const {
  const size_t n = path_.size();
  const std::string& b = base.str();
  std::string out(n ? n : 1, '\0');
  size_t len = 0;
  lu_path_make_relative_to(path_.data(), n, b.data(), b.size(), &out[0],
                           out.size(), &len);
  out.resize(len);
  return Path(std::move(out));
}

const Path& Path::root() {
  static Path root_path("/");
  return root_path;
}

const Path& Path::empty_path() {
  static Path empty;
  return empty;
}

// ---------------------------------------------------------------------------
// Inline method bodies moved out of the header so units that include it do
// not re-parse them.
// ---------------------------------------------------------------------------
bool Path::empty() const { return path_.empty(); }
const std::string& Path::str() const { return path_; }
const char* Path::c_str() const { return path_.c_str(); }

}  // namespace next
}  // namespace lightusd
