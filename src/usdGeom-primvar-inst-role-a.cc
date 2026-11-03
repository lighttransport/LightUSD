// SPDX-License-Identifier: Apache 2.0
// Copyright 2022 - Present, Light Transport Entertainment Inc.
//
// GeomPrimvar flatten_with_indices/get_value explicit instantiations (ROLE_A group).
// Split from usdGeom.cc; shares bodies via usdGeom-primvar-impl.inc.

#include <cstring>
#include <sstream>
#include <type_traits>
#include "value-types.hh"
#include "core/prim.hh"
#include "str-util.hh"
#include "tiny-format.hh"
#include "xform.hh"
#include "usdGeom.hh"
#include "common-macros.inc"
#include "math-util.inc"
#include "str-util.hh"
#include "value-pprint.hh"
#include "core/safe-arithmetic.hh"

namespace lightusd {

#include "usdGeom-primvar-impl.inc"

#define INSTANCIATE_FLATTEN_WITH_INDICES(__ty) \
  template bool GeomPrimvar::flatten_with_indices(std::vector<__ty> *dest, std::string *err) const; \
  template bool GeomPrimvar::flatten_with_indices(const double t, std::vector<__ty> *dest, const value::TimeSampleInterpolationType tinterp, std::string *err) const;

#define INSTANCIATE_GET_VALUE(__ty) \
  template bool GeomPrimvar::get_value(__ty *dest, std::string *err) const; \
  template bool GeomPrimvar::get_value(double, __ty *dest, value::TimeSampleInterpolationType, std::string *err) const; \
  template bool GeomPrimvar::get_value(std::vector<__ty> *dest, std::string *err) const; \
  template bool GeomPrimvar::get_value(double, std::vector<__ty> *dest, value::TimeSampleInterpolationType, std::string *err) const;

APPLY_GEOMPRIVAR_TYPE_ROLE_A(INSTANCIATE_FLATTEN_WITH_INDICES)
APPLY_GEOMPRIVAR_TYPE_ROLE_A(INSTANCIATE_GET_VALUE)

#undef INSTANCIATE_FLATTEN_WITH_INDICES
#undef INSTANCIATE_GET_VALUE

}  // namespace lightusd
