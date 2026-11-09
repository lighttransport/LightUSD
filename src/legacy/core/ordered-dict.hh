// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// ordered-dict.hh - Simple Python-like OrderedDict implementation
//
#pragma once

#include <map>
#include <string>
#include <vector>
#include <cstddef>

#include "compiler-features.hh"

namespace lightusd {

///
/// Simple Python-like OrderedDict implementation.
/// Preserves insertion order while providing O(1) key-based lookup.
/// Used throughout USD data structures where order matters.
///
template <typename T>
class ordered_dict {
 public:

  bool at(const size_t idx, T *dst) const {
    if (idx >= _keys.size()) {
      return false;
    }

    if (!_m.count(_keys[idx])) {
      // This should not happen though.
      return false;
    }

    (*dst) = _m.at(_keys[idx]);

    return true;
  }

  bool at(const size_t idx, const T **dst) const {
    if (idx >= _keys.size()) {
      return false;
    }

    if (!_m.count(_keys[idx])) {
      // This should not happen though.
      return false;
    }

    (*dst) = &(_m.at(_keys[idx]));

    return true;
  }

  bool count(const std::string &key) const {
    return _m.count(key) > 0;
  }

  void insert(const std::string &key, const T &value) {
    if (_m.count(key)) {
      // overwrite existing value
    } else {
      _keys.push_back(key);
    }

    _m[key] = value;
  }

  T &get_or_add(const std::string &key) LIGHTUSD_LIFETIMEBOUND {
    if (!_m.count(key)) {
      _keys.push_back(key);
    }

    return _m[key];
  }


  void insert(const std::string &key, T &&value) {
    if (_m.count(key)) {
      // overwrite existing value
    } else {
      _keys.push_back(key);
    }

    _m[key] = std::move(value);
  }

  bool erase(const std::string &key) {

    if (!_m.count(key)) {
      return false;
    }

    // linear search
    bool erased = false;
    size_t idx = 0;
    for (size_t i = 0; i < _keys.size(); i++) {
      if (key == _keys[i]) {
        idx = i;
        erased = true;
        break;
      }
    }

    if (!erased) {
      return false;
    }

    _keys.erase(_keys.begin() + std::ptrdiff_t(idx));
    _m.erase(key);

    return true;
  }

  bool at(const std::string &key, T **dst) {
    if (!_m.count(key)) {
      // This should not happen though.
      return false;
    }

    (*dst) = &_m.at(key);

    return true;
  }


  bool at(const std::string &key, T *dst) const {
    if (!_m.count(key)) {
      // This should not happen though.
      return false;
    }

    (*dst) = _m.at(key);

    return true;
  }

  bool at(const std::string &key, const T **dst) const {
    if (!_m.count(key)) {
      // This should not happen though.
      return false;
    }

    (*dst) = &_m.at(key);

    return true;
  }

  const std::vector<std::string> &keys() const LIGHTUSD_LIFETIMEBOUND {
    return _keys;
  }

  size_t size() const { return _m.size(); }

  // No operator[] for safety.

 private:
  std::vector<std::string> _keys;
  std::map<std::string, T> _m;
};

}  // namespace lightusd
