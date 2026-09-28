// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdAR Schema Implementation

#include "usd-ar.hh"
#include "../layer/prim-spec.hh"

namespace lightusd {
namespace next {

// ============================================================
// Helpers
// ============================================================

static bool HasAPISchema(const UsdPrim& prim, const std::string& name) {
  for (const auto& s : prim.GetMeta().apiSchemas()) {
    if (s == name) return true;
  }
  return false;
}

static bool IsType(const UsdPrim& prim, const std::string& type) {
  return prim.IsValid() && prim.GetTypeName() == type;
}

// ============================================================
// Typed prims
// ============================================================

bool IsARAnchor(const UsdPrim& prim) { return IsType(prim, "ARAnchor"); }
bool IsARImage(const UsdPrim& prim) { return IsType(prim, "ARImage"); }
bool IsARFaceGeometry(const UsdPrim& prim) { return IsType(prim, "ARFaceGeometry"); }
bool IsARPlane(const UsdPrim& prim) { return IsType(prim, "ARPlane"); }
bool IsARPointCloud(const UsdPrim& prim) { return IsType(prim, "ARPointCloud"); }
bool IsARDevice(const UsdPrim& prim) { return IsType(prim, "ARDevice"); }
bool IsARScene(const UsdPrim& prim) { return IsType(prim, "ARScene"); }

// ============================================================
// ARAnchor
// ============================================================

bool GetARAnchorData(const Stage& stage, const UsdPrim& prim,
                     ARAnchorData* out, double time) {
  if (!IsARAnchor(prim) || !out) return false;

  AttributeEval eval(&stage);
  eval.SetTime(time);

  float loc[3];
  if (eval.EvalFloat3(prim, "ar:location", loc)) {
    out->location = {loc[0], loc[1], loc[2]};
  }

  float orient[4];
  if (eval.EvalFloat4(prim, "ar:orientation", orient)) {
    out->orientation = {orient[0], orient[1], orient[2], orient[3]};
  }

  float scl[3];
  if (eval.EvalFloat3(prim, "ar:scale", scl)) {
    out->scale = {scl[0], scl[1], scl[2]};
  }

  out->tracked = eval.EvalOr(prim, "ar:tracked", true);

  return true;
}

// ============================================================
// ARImage
// ============================================================

bool GetARImageData(const Stage& stage, const UsdPrim& prim,
                    ARImageData* out) {
  if (!IsARImage(prim) || !out) return false;

  (void)stage;

  {
    const Value* val = prim.GetPropertyValue("ar:imageAsset");
    if (val) {
      const std::string* s = val->as_string();
      if (s) out->imageAsset = *s;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("ar:physicalWidth");
    if (val) {
      const float* f = val->as_float();
      if (f) out->physicalWidth = *f;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("ar:physicalHeight");
    if (val) {
      const float* f = val->as_float();
      if (f) out->physicalHeight = *f;
    }
  }

  return true;
}

// ============================================================
// ARFaceGeometry
// ============================================================

bool GetARFaceGeometryData(const Stage& stage, const UsdPrim& prim,
                            ARFaceGeometryData* out, double time) {
  if (!IsARFaceGeometry(prim) || !out) return false;

  AttributeEval eval(&stage);
  eval.SetTime(time);

  {
    EvalResult result = eval.Eval(prim, "ar:blendShapeCoefficients");
    if (result.success && result.value.is_array()) {
      const std::vector<float>* arr = result.value.as_float_array();
      if (arr) {
        out->blendShapeCoefficients = *arr;
      }
    }
  }

  // blendShapeNames: uniform token[] (partial - single token/string fallback)
  {
    const Value* val = prim.GetPropertyValue("ar:blendShapeNames");
    if (val) {
      const std::string* s = val->as_string();
      if (s) out->blendShapeNames.push_back(*s);
      const std::string* tok = val->as_token();
      if (tok) out->blendShapeNames.push_back(*tok);
    }
  }

  return true;
}

// ============================================================
// ARPlane
// ============================================================

bool GetARPlaneData(const Stage& stage, const UsdPrim& prim,
                    ARPlaneData* out) {
  if (!IsARPlane(prim) || !out) return false;

  (void)stage;

  {
    const Value* val = prim.GetPropertyValue("ar:semanticType");
    if (val) {
      const std::string* s = val->as_string();
      if (s) out->semanticType = *s;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("ar:extent");
    if (val && val->is_array()) {
      const std::vector<float>* arr = val->as_float_array();
      if (arr) {
        out->extent = *arr;
      }
    }
  }

  {
    const Value* val = prim.GetPropertyValue("ar:boundary");
    if (val && val->is_array()) {
      const std::vector<float>* arr = val->as_float_array();
      if (arr) {
        out->boundary = *arr;
      }
    }
  }

  {
    const std::vector<Path>* targets =
        prim.GetRelationship("ar:parentPlane");
    if (targets && !targets->empty()) {
      out->parentPlane = (*targets)[0].str();
    }
  }

  return true;
}

// ============================================================
// ARPointCloud
// ============================================================

bool GetARPointCloudData(const Stage& stage, const UsdPrim& prim,
                          ARPointCloudData* out, double time) {
  if (!IsARPointCloud(prim) || !out) return false;

  AttributeEval eval(&stage);
  eval.SetTime(time);

  {
    EvalResult result = eval.Eval(prim, "ar:points");
    if (result.success && result.value.is_array()) {
      const std::vector<float>* arr = result.value.as_float_array();
      if (arr) out->points = *arr;
    }
  }

  {
    EvalResult result = eval.Eval(prim, "ar:normals");
    if (result.success && result.value.is_array()) {
      const std::vector<float>* arr = result.value.as_float_array();
      if (arr) out->normals = *arr;
    }
  }

  {
    EvalResult result = eval.Eval(prim, "ar:confidences");
    if (result.success && result.value.is_array()) {
      const std::vector<float>* arr = result.value.as_float_array();
      if (arr) out->confidences = *arr;
    }
  }

  // timestamp (int64 or uint64)
  {
    const Value* val = prim.GetPropertyValue("ar:timestamp");
    if (val) {
      const uint64_t* u = val->as_uint64();
      if (u) out->timestamp = *u;
    }
  }

  return true;
}

// ============================================================
// ARDevice
// ============================================================

bool GetARDeviceData(const Stage& stage, const UsdPrim& prim,
                     ARDeviceData* out) {
  if (!IsARDevice(prim) || !out) return false;

  (void)stage;

  {
    const Value* val = prim.GetPropertyValue("ar:fieldOfView");
    if (val) {
      const float* f = val->as_float();
      if (f) out->fieldOfView = *f;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("ar:aspectRatio");
    if (val) {
      const float* f = val->as_float();
      if (f) out->aspectRatio = *f;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("ar:nearPlane");
    if (val) {
      const float* f = val->as_float();
      if (f) out->nearPlane = *f;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("ar:farPlane");
    if (val) {
      const float* f = val->as_float();
      if (f) out->farPlane = *f;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("ar:resolution");
    if (val) {
      const int32_t* i32 = val->as_int();
      if (i32) {
        out->resolution = {static_cast<float>(*i32), 0.0f};
      }
      const std::vector<int32_t>* arr = val->as_int_array();
      if (arr && arr->size() >= 2) {
        if (arr->size() >= 2) {
          out->resolution = {
            static_cast<float>((*arr)[0]),
            static_cast<float>((*arr)[1])
          };
        }
      }
    }
  }

  return true;
}

// ============================================================
// Applied API schemas
// ============================================================

bool HasARAnchorAPI(const UsdPrim& prim) {
  return HasAPISchema(prim, "ARAnchorAPI");
}

bool HasARImagingAPI(const UsdPrim& prim) {
  return HasAPISchema(prim, "ARImagingAPI");
}

bool GetARAnchorAPIData(const UsdPrim& prim, ARAnchorAPIData* out) {
  if (!HasARAnchorAPI(prim) || !out) return false;

  {
    const Value* val = prim.GetPropertyValue("ar:tracked");
    if (val) {
      const bool* b = val->as_bool();
      if (b) out->tracked = *b;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("ar:anchorName");
    if (val) {
      const std::string* s = val->as_string();
      if (s) out->anchorName = *s;
    }
  }

  return true;
}

bool GetARImagingAPIData(const UsdPrim& prim, ARImagingAPIData* out) {
  if (!HasARImagingAPI(prim) || !out) return false;

  {
    const Value* val = prim.GetPropertyValue("ar:imageAsset");
    if (val) {
      const std::string* s = val->as_string();
      if (s) out->imageAsset = *s;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("ar:physicalWidth");
    if (val) {
      const float* f = val->as_float();
      if (f) out->physicalWidth = *f;
    }
  }

  return true;
}

} // namespace next
} // namespace lightusd
