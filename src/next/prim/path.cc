// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - Path implementation

#include "path.hh"
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
  if (path_.empty() || is_root()) {
    return Path();
  }

  // Find property separator first
  size_t dot_pos = path_.find('.');
  std::string prim_part = (dot_pos != std::string::npos) ? path_.substr(0, dot_pos) : path_;

  // Find last '/'
  size_t last_slash = prim_part.rfind('/');
  if (last_slash == std::string::npos) {
    return Path();
  }
  if (last_slash == 0) {
    return Path("/");
  }
  return Path(prim_part.substr(0, last_slash));
}

std::string Path::name() const {
  if (path_.empty()) {
    return "";
  }

  // Get prim part (before any property)
  size_t dot_pos = path_.find('.');
  std::string prim_part = (dot_pos != std::string::npos) ? path_.substr(0, dot_pos) : path_;

  size_t last_slash = prim_part.rfind('/');
  if (last_slash == std::string::npos) {
    return prim_part;
  }
  return prim_part.substr(last_slash + 1);
}

std::string Path::property_name() const {
  size_t dot_pos = path_.find('.');
  if (dot_pos == std::string::npos) {
    return "";
  }
  return path_.substr(dot_pos + 1);
}

Path Path::prim_path() const {
  size_t dot_pos = path_.find('.');
  if (dot_pos == std::string::npos) {
    return *this;
  }
  return Path(path_.substr(0, dot_pos));
}

std::vector<std::string> Path::elements() const {
  std::vector<std::string> result;
  if (path_.empty()) {
    return result;
  }

  // Get prim part only
  std::string prim_part = prim_path().str();

  size_t start = 0;
  if (prim_part[0] == '/') {
    start = 1;
  }

  while (start < prim_part.size()) {
    size_t end = prim_part.find('/', start);
    if (end == std::string::npos) {
      result.push_back(prim_part.substr(start));
      break;
    }
    result.push_back(prim_part.substr(start, end - start));
    start = end + 1;
  }

  return result;
}

Path Path::append_child(const std::string& child_name) const {
  // Authoring boundary: an appended component must be a valid identifier
  // (strict UTF-8 + XID). Invalid input yields an empty Path.
  if (!IsValidIdentifier(child_name)) {
    return Path();
  }
  if (path_.empty()) {
    return Path("/" + child_name);
  }
  if (path_.back() == '/') {
    return Path(path_ + child_name);
  }
  return Path(path_ + "/" + child_name);
}

Path Path::append_property(const std::string& prop_name) const {
  // Authoring boundary: property names must be valid (possibly namespaced)
  // identifiers. Invalid input yields an empty Path.
  if (!IsValidNamespacedIdentifier(prop_name)) {
    return Path();
  }
  if (has_property()) {
    // Already has property - replace it
    return Path(prim_path().str() + "." + prop_name);
  }
  return Path(path_ + "." + prop_name);
}

Path Path::make_relative_to(const Path& base) const {
  if (!is_absolute() || !base.is_absolute()) {
    return *this;  // Can't make relative
  }

  const std::string& base_str = base.str();
  if (path_.find(base_str) != 0) {
    return *this;  // Not a descendant
  }

  if (path_.size() == base_str.size()) {
    return Path(".");
  }

  size_t offset = base_str.size();
  if (path_[offset] == '/') {
    offset++;
  }
  return Path(path_.substr(offset));
}

const Path& Path::root() {
  static Path root_path("/");
  return root_path;
}

const Path& Path::empty_path() {
  static Path empty;
  return empty;
}

}  // namespace next
}  // namespace lightusd
