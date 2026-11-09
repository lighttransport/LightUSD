// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// xform-op.hh - XformOp struct for transformation operations
//
#pragma once

#include <string>
#include <vector>

#include "nonstd/optional.hpp"
#include "path.hh"
#include "primvar.hh"
#include "value-types.hh"

namespace lightusd {

struct XformOp {
  enum class OpType {
    // matrix
    Transform,

    // vector3
    Translate,
    Scale,

    // scalar
    RotateX,
    RotateY,
    RotateZ,

    // vector3
    RotateXYZ,
    RotateXZY,
    RotateYXZ,
    RotateYZX,
    RotateZXY,
    RotateZYX,

    // quaternion
    Orient,

    // Special token
    ResetXformStack,  // !resetXformStack!
  };

  // OpType op;
  OpType op_type;
  bool inverted{false};  // true when `!inverted!` prefix
  std::string
      suffix;  // may contain nested namespaces. e.g. suffix will be
               // ":blender:pivot" for "xformOp:translate:blender:pivot". Suffix
               // will be empty for "xformOp:translate"

  primvar::PrimVar _var;
  // const value::TimeSamples &get_ts() const { return _var.ts_raw(); }

  std::string get_value_type_name() const { return _var.type_name(); }

  uint32_t get_value_type_id() const { return _var.type_id(); }

  // TODO: Check if T is valid type.
  template <class T>
  void set_value(const T &v) {
    _var.set_value(v);
  }

  template <class T>
  void set_default(const T &v) {
    _var.set_value(v);
  }

  template <class T>
  void set_timesample(const float t, const T &v) {
    _var.set_timesample(t, v);
  }

  void set_timesamples(const value::TimeSamples &v) { _var.set_timesamples(v); }

  void set_timesamples(value::TimeSamples &&v) { _var.set_timesamples(std::move(v)); }

  bool is_timesamples() const { return !_var.has_value() && _var.has_timesamples(); }
  bool has_timesamples() const { return _var.has_timesamples(); }

  void set_blocked(bool onoff) { _is_blocked = onoff; }
  void clear_blocked() { _is_blocked = false; }

  // check if 'default' value is ValueBlock.
  bool is_blocked() const { return _is_blocked || _var.is_blocked(); }

  bool is_default() const { return _var.has_value() && !_var.has_timesamples(); }
  bool has_default() const { return _var.has_default(); }

  nonstd::optional<value::TimeSamples> get_timesamples() const {
    if (has_timesamples()) {
      return _var.ts_raw();
    }
    return nonstd::nullopt;
  }

  nonstd::optional<value::Value> get_scalar() const {
    if (has_default()) {
      return _var.value_raw();
    }
    return nonstd::nullopt;
  }

  nonstd::optional<value::Value> get_default() const {
    return get_scalar();
  }

  template <class T>
  nonstd::optional<T> get_value(double t = value::TimeCode::Default(),
          value::TimeSampleInterpolationType interp =
               value::TimeSampleInterpolationType::Linear) const {
    if (is_timesamples()) {
      T value{};
      if (get_interpolated_value(&value, t, interp)) {
        return value;
      }
      return nonstd::nullopt;
    }

    return _var.get_value<T>();
  }

  template <class T>
  bool get_interpolated_value(T *dst, double t = value::TimeCode::Default(),
           value::TimeSampleInterpolationType interp =
               value::TimeSampleInterpolationType::Linear) const {
    return _var.get_interpolated_value<T>(t, interp, dst);
  }

  const primvar::PrimVar &get_var() const LIGHTUSD_LIFETIMEBOUND {
    return _var;
  }

  primvar::PrimVar &var() LIGHTUSD_LIFETIMEBOUND { return _var; }

  // Connection support
  bool has_connections() const { return !_connections.empty(); }

  void set_connection(const Path &path) {
    _connections.clear();
    _connections.push_back(path);
  }

  void set_connections(const std::vector<Path> &paths) {
    _connections = paths;
  }

  nonstd::optional<Path> get_connection() const {
    if (_connections.size() == 1) {
      return _connections[0];
    }
    return nonstd::nullopt;
  }

  const std::vector<Path> &connections() const LIGHTUSD_LIFETIMEBOUND {
    return _connections;
  }
  std::vector<Path> &connections() LIGHTUSD_LIFETIMEBOUND {
    return _connections;
  }

  // Check if this xformOp is connection-only (no default value or timeSamples)
  bool is_connection_only() const {
    return has_connections() && !has_default() && !has_timesamples();
  }

 private:

  bool _is_blocked{false};
  std::vector<Path> _connections;  // Connection targets for this xformOp
};

}  // namespace lightusd
