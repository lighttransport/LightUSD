// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdMtlx Schema Implementation

#include "usd-mtlx.hh"
#include "schema-predicates.hh"
#include <cstring>

namespace lightusd {
namespace next {

// ============================================================
// Typed prims
// ============================================================

namespace {


} // namespace

bool IsMaterialXShader(const UsdPrim& prim) {
  return IsType(prim, "MaterialXShader");
}

bool IsMaterialXNodeGraph(const UsdPrim& prim) {
  return IsType(prim, "MaterialXNodeGraph");
}

bool IsMaterialXLightNode(const UsdPrim& prim) {
  return IsType(prim, "MaterialXLightNode");
}

bool IsMaterialXNodeIO(const UsdPrim& prim) {
  return IsType(prim, "MaterialXNodeIO");
}

// ============================================================
// MaterialXShader
// ============================================================

bool GetMaterialXShaderData(const Stage& stage, const UsdPrim& prim,
                             MaterialXShaderData* out) {
  if (!IsMaterialXShader(prim) || !out) return false;

  (void)stage;

  {
    const Value* val = prim.GetPropertyValue("inputs:nodeUri");
    if (val) {
      auto s = val->as_string();
      if (s) out->nodeUri = *s;
    }
  }
  {
    const Value* val = prim.GetPropertyValue("inputs:nodeDefId");
    if (val) {
      auto s = val->as_string();
      if (s) out->nodeDefId = *s;
    }
  }

  // output
  {
    const Value* val = prim.GetPropertyValue("outputs:mtlx:output");
    if (!val) val = prim.GetPropertyValue("info:mtlx:output");
    if (val) {
      auto s = val->as_string();
      if (s) out->output = *s;
    }
  }

  // Iterate over all properties to find inputs:*
  // (In practice, input discovery would iterate prim.GetPropertyNames()
  //  and filter for "inputs:" prefix. For now we expose the most common.)
  //
  // We'll leave input* maps as read-on-demand via a dedicated helper.

  // nodeUri / nodeDefId connection detection from relationships
  {
    const std::vector<Path>* targets =
        prim.GetRelationship("inputs:nodeUri");
    if (targets && !targets->empty()) {
      out->inputConnections["nodeUri"] = (*targets)[0].str();
    }
  }

  {
    const std::vector<Path>* targets =
        prim.GetRelationship("inputs:nodeDefId");
    if (targets && !targets->empty()) {
      out->inputConnections["nodeDefId"] = (*targets)[0].str();
    }
  }

  return true;
}

// ============================================================
// MaterialXNodeGraph
// ============================================================

bool GetMaterialXNodeGraphData(const Stage& stage, const UsdPrim& prim,
                                MaterialXNodeGraphData* out) {
  if (!IsMaterialXNodeGraph(prim) || !out) return false;

  (void)stage;

  {
    const Value* val = prim.GetPropertyValue("info:mtlx:docUri");
    if (!val) val = prim.GetPropertyValue("strings:mtlx:docUri");
    if (val) {
      auto s = val->as_string();
      if (s) out->docUri = *s;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("outputs:mtlx:output");
    if (val) {
      auto s = val->as_string();
      if (s) out->output = *s;
    }
  }

  return true;
}

// ============================================================
// MaterialXLightNode
// ============================================================

bool GetMaterialXLightNodeData(const Stage& stage, const UsdPrim& prim,
                                MaterialXLightNodeData* out) {
  if (!IsMaterialXLightNode(prim) || !out) return false;

  (void)stage;

  {
    const Value* val = nullptr;
    val = prim.GetPropertyValue("inputs:nodeUri");
    if (val) {
      auto s = val->as_string();
      if (s) out->nodeUri = *s;
    }
  }
  {
    const Value* val = nullptr;
    val = prim.GetPropertyValue("inputs:nodeDefId");
    if (val) {
      auto s = val->as_string();
      if (s) out->nodeDefId = *s;
    }
  }

  // color
  {
    const Value* val = prim.GetPropertyValue("inputs:color");
    if (val && val->is_array()) {
      auto arr = val->as_float_array();
      if (arr) out->color.assign(arr->begin(), arr->end());
    }
  }

  // intensity
  {
    const Value* val = prim.GetPropertyValue("inputs:intensity");
    if (val) {
      const float* f = val->as_float();
      if (f) out->intensity.push_back(*f);
    }
  }

  // exposure
  {
    const Value* val = prim.GetPropertyValue("inputs:exposure");
    if (val) {
      const float* f = val->as_float();
      if (f) out->exposure.push_back(*f);
    }
  }

  return true;
}

// ============================================================
// MaterialXNodeIO
// ============================================================

bool GetMaterialXNodeIOData(const Stage& stage, const UsdPrim& prim,
                             MaterialXNodeIOData* out) {
  if (!IsMaterialXNodeIO(prim) || !out) return false;

  (void)stage;

  {
    const Value* val = prim.GetPropertyValue("strings:mtlx:NodedefInput:nodeUri");
    if (val) {
      auto s = val->as_string();
      if (s) out->nodeUri = *s;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("strings:mtlx:NodedefInput:nodeDefId");
    if (val) {
      auto s = val->as_string();
      if (s) out->nodeDefId = *s;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("outputs:mtlx:output");
    if (val) {
      auto s = val->as_string();
      if (s) out->output = *s;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("inputs:mtlx:input");
    if (val) {
      auto s = val->as_string();
      if (s) out->input = *s;
    }
  }

  {
    const Value* val = prim.GetPropertyValue("info:mtlx:colorspace");
    if (val) {
      auto s = val->as_string();
      if (s) out->colorspace = *s;
    }
  }

  return true;
}

} // namespace next
} // namespace lightusd
