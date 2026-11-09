// SPDX-License-Identifier: Apache-2.0
// Shared interned property-name accessors for the render converter TUs. Each is
// a three-line intern-once lookup; previously copy-pasted across the converter
// translation units (and kIdInputsTextureFile was defined twice).
#pragma once
#include "next/layer/property-index.hh"

namespace lightusd {
namespace tydra {
namespace next {

inline const ::lightusd::next::PropNameId& kIdFaceVertexCounts() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("faceVertexCounts");
  return id;
}
inline const ::lightusd::next::PropNameId& kIdFaceVertexIndices() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("faceVertexIndices");
  return id;
}
inline const ::lightusd::next::PropNameId& kIdHoleIndices() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("holeIndices");
  return id;
}
inline const ::lightusd::next::PropNameId& kIdPoints() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("points");
  return id;
}
inline const ::lightusd::next::PropNameId& kIdTetVertexIndices() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("tetVertexIndices");
  return id;
}
inline const ::lightusd::next::PropNameId& kIdNormals() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("normals");
  return id;
}
inline const ::lightusd::next::PropNameId& kIdDisplayColor() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("primvars:displayColor");
  return id;
}
inline const ::lightusd::next::PropNameId& kIdDisplayOpacity() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("primvars:displayOpacity");
  return id;
}
inline const ::lightusd::next::PropNameId& kIdPrimvarsSt() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("primvars:st");
  return id;
}
inline const ::lightusd::next::PropNameId& kIdExtent() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("extent");
  return id;
}
inline const ::lightusd::next::PropNameId& kIdCurveVertexCounts() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("curveVertexCounts");
  return id;
}
inline const ::lightusd::next::PropNameId& kIdWidths() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("widths");
  return id;
}
inline const ::lightusd::next::PropNameId& kIdInputsTextureFile() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("inputs:texture:file");
  return id;
}
inline const ::lightusd::next::PropNameId& kIdOutputsOut() {
  static const ::lightusd::next::PropNameId id =
      ::lightusd::next::GetPropNameTable().intern("outputs:out");
  return id;
}

}  // namespace next
}  // namespace tydra
}  // namespace lightusd
