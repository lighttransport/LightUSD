// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - UsdMtlx Schema
// MaterialX node graph support: NodedefInput, MaterialXNodeGraph, MaterialXShader,
// UsdMtlxNodeIO, UsdMtlxLightNode, etc.

#pragma once

#include "stage/stage.hh"
#include "eval/attribute-eval.hh"
#include <string>
#include <vector>
#include <map>

namespace lightusd {
namespace next {

// ============================================================
// Typed prims
// ============================================================

bool IsMaterialXShader(const UsdPrim& prim);
bool IsMaterialXNodeGraph(const UsdPrim& prim);
bool IsMaterialXLightNode(const UsdPrim& prim);
bool IsMaterialXNodeIO(const UsdPrim& prim);

// ============================================================
// MaterialXShader data
// ============================================================

struct MaterialXShaderData {
  std::string nodeUri;         // strings:mtlx:NodedefInput:nodeUri
  std::string nodeDefId;       // strings:mtlx:NodedefInput:nodeDefId
  std::string output;          // strings:mtlx:output (or "outputs:mtlx:output")

  std::map<std::string, int> inputInts;
  std::map<std::string, float> inputFloats;
  std::map<std::string, std::string> inputStrings;
  std::map<std::string, std::vector<float>> inputFloatArrays;
  std::map<std::string, std::vector<int>> inputIntArrays;
  std::map<std::string, std::vector<float>> inputColor3s;   // float3
  std::map<std::string, std::vector<float>> inputColor4s;   // float4
  std::map<std::string, std::vector<float>> inputVector2s;  // float2
  std::map<std::string, std::vector<float>> inputVector3s;  // float3
  std::map<std::string, std::vector<float>> inputVector4s;  // float4
  std::map<std::string, std::string> inputFilenames;        // asset

  // Connection inputs: inputName -> sourceNodePath:outputName
  std::map<std::string, std::string> inputConnections;
};

bool GetMaterialXShaderData(const Stage& stage, const UsdPrim& prim,
                             MaterialXShaderData* out);

// ============================================================
// MaterialXNodeGraph data
// ============================================================

struct MaterialXNodeGraphData {
  std::string docUri;
  std::string output; // outputs:mtlx:output
};

bool GetMaterialXNodeGraphData(const Stage& stage, const UsdPrim& prim,
                                MaterialXNodeGraphData* out);

// ============================================================
// MaterialXLightNode data
// ============================================================

struct MaterialXLightNodeData {
  std::string nodeUri;
  std::string nodeDefId;

  std::vector<float> color;       // color3f
  std::vector<float> intensity;   // float
  std::vector<float> exposure;    // float
};

bool GetMaterialXLightNodeData(const Stage& stage, const UsdPrim& prim,
                                MaterialXLightNodeData* out);

// ============================================================
// MaterialXNodeIO data (shader output/input ports)
// ============================================================

struct MaterialXNodeIOData {
  std::string nodeUri;
  std::string nodeDefId;
  std::string output;   // outputs:mtlx:output
  std::string input;    // inputs:mtlx:input
  std::string colorspace; // input/output colorspace
};

bool GetMaterialXNodeIOData(const Stage& stage, const UsdPrim& prim,
                             MaterialXNodeIOData* out);

} // namespace next
} // namespace lightusd
