// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include "core/attribute.hh"
#include "core/prim.hh"
#include "core/property.hh"

namespace lusdview {

std::string PropertyToString(const lightusd::legacy::Property& prop);

std::string PrimMetaSummary(const lightusd::legacy::Prim& prim);
std::string AttrMetaSummary(const lightusd::legacy::Attribute& attr);
std::string GPrimPropertySummary(const lightusd::legacy::Prim& prim);
std::string SubdivisionSchemeName(const lightusd::legacy::Prim& prim);
std::string VisibilityState(const lightusd::legacy::Prim& prim);
std::string VariantSetDetail(const lightusd::legacy::Prim& prim);
std::string GeometrySummary(const lightusd::legacy::Prim& prim);

}  // namespace lusdview
