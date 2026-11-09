// SPDX-License-Identifier: Apache-2.0
// Shared interned property-name accessors for the render converter TUs. Each is
// a three-line intern-once lookup; previously copy-pasted across the converter
// translation units (and kIdInputsTextureFile was defined twice).
#pragma once
#include "layer/property-index.hh"

namespace lightusd {
namespace tydra {

inline const ::lightusd::PropNameId& kIdFaceVertexCounts() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("faceVertexCounts");
  return id;
}
inline const ::lightusd::PropNameId& kIdFaceVertexIndices() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("faceVertexIndices");
  return id;
}
inline const ::lightusd::PropNameId& kIdHoleIndices() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("holeIndices");
  return id;
}
inline const ::lightusd::PropNameId& kIdPoints() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("points");
  return id;
}
inline const ::lightusd::PropNameId& kIdTetVertexIndices() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("tetVertexIndices");
  return id;
}
inline const ::lightusd::PropNameId& kIdNormals() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("normals");
  return id;
}
inline const ::lightusd::PropNameId& kIdDisplayColor() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("primvars:displayColor");
  return id;
}
inline const ::lightusd::PropNameId& kIdDisplayOpacity() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("primvars:displayOpacity");
  return id;
}
inline const ::lightusd::PropNameId& kIdPrimvarsSt() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("primvars:st");
  return id;
}
inline const ::lightusd::PropNameId& kIdExtent() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("extent");
  return id;
}
inline const ::lightusd::PropNameId& kIdCurveVertexCounts() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("curveVertexCounts");
  return id;
}
inline const ::lightusd::PropNameId& kIdWidths() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("widths");
  return id;
}
inline const ::lightusd::PropNameId& kIdInputsTextureFile() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("inputs:texture:file");
  return id;
}
inline const ::lightusd::PropNameId& kIdOutputsOut() {
  static const ::lightusd::PropNameId id =
      ::lightusd::GetPropNameTable().intern("outputs:out");
  return id;
}

}  // namespace tydra
}  // namespace lightusd
