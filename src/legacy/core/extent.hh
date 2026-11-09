// SPDX-License-Identifier: Apache 2.0
// Copyright 2021 - Present, Syoyo Fujita.
//
// extent.hh - Extent (bounding box) structure
//
#pragma once

#include <array>
#include <cmath>
#include <limits>
#include <algorithm>

#include "value-types.hh"

namespace lightusd {

struct Extent {
  value::float3 lower{{std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::infinity()}};

  value::float3 upper{{-std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity()}};

  Extent() = default;

  Extent(const value::float3 &l, const value::float3 &u) : lower(l), upper(u) {}

  bool is_valid() const {
    if (lower[0] > upper[0]) return false;
    if (lower[1] > upper[1]) return false;
    if (lower[2] > upper[2]) return false;

    return std::isfinite(lower[0]) && std::isfinite(lower[1]) &&
           std::isfinite(lower[2]) && std::isfinite(upper[0]) &&
           std::isfinite(upper[1]) && std::isfinite(upper[2]);
  }

  std::array<std::array<float, 3>, 2> to_array() const {
    std::array<std::array<float, 3>, 2> ret;
    ret[0][0] = lower[0];
    ret[0][1] = lower[1];
    ret[0][2] = lower[2];
    ret[1][0] = upper[0];
    ret[1][1] = upper[1];
    ret[1][2] = upper[2];

    return ret;
  }

  const Extent &union_with(const value::float3 &p) LIGHTUSD_LIFETIMEBOUND {
    lower[0] = (std::min)(lower[0], p[0]);
    lower[1] = (std::min)(lower[1], p[1]);
    lower[2] = (std::min)(lower[2], p[2]);

    upper[0] = (std::max)(upper[0], p[0]);
    upper[1] = (std::max)(upper[1], p[1]);
    upper[2] = (std::max)(upper[2], p[2]);

    return *this;
  }

  const Extent &union_with(const value::point3f &p) LIGHTUSD_LIFETIMEBOUND {
    union_with(value::float3{p.x, p.y, p.z});

    return *this;
  }

  const Extent &union_with(const Extent &box) LIGHTUSD_LIFETIMEBOUND {
    lower[0] = (std::min)(lower[0], box.lower[0]);
    lower[1] = (std::min)(lower[1], box.lower[1]);
    lower[2] = (std::min)(lower[2], box.lower[2]);

    upper[0] = (std::max)(upper[0], box.upper[0]);
    upper[1] = (std::max)(upper[1], box.upper[1]);
    upper[2] = (std::max)(upper[2], box.upper[2]);

    return *this;
  }
};

namespace value {

#include "define-type-trait.inc"

DEFINE_TYPE_TRAIT(Extent, "float3[]", TYPE_ID_EXTENT, 2);  // float3[2]

#undef DEFINE_TYPE_TRAIT
#undef DEFINE_ROLE_TYPE_TRAIT

}  // namespace value

}  // namespace lightusd
