// SPDX-License-Identifier: Apache-2.0
// Shared MaterialX/shader constant-eval + ShaderParam helpers (de-duplicated
// from render-converter-material.cc and render-converter-preview.cc).
#pragma once
#include <array>
#include <functional>
#include <set>
#include <string>

namespace lightusd {
namespace next {
class Value; class Stage; class UsdPrim;
}
namespace tydra {
namespace next {
using ::lightusd::next::Stage; using ::lightusd::next::UsdPrim; using ::lightusd::next::Value;
struct ShaderParam;

constexpr int kMaxMtlxConstantDepth = 64;

struct MtlxConstantValue {
  std::array<float, 4> value{{0.0f, 0.0f, 0.0f, 0.0f}};
  int components = 0;
  bool color_managed = false;

  float component(int i) const {
    return value[static_cast<size_t>(i < components ? i : 0)];
  }
};

std::string SourcePrimPathFromConnection(const std::string& connection_path);
bool SplitConnectionPath(const std::string& connection_path, std::string* prim_path, std::string* prop_name);
void SetParamFloat(ShaderParam* out, float x);
void SetParamFloat3(ShaderParam* out, float x, float y, float z);
void SetParamFloat4(ShaderParam* out, float x, float y, float z, float w);
bool ValueToShaderParam(const Value& value, ShaderParam* out);
bool ValueToMtlxConstant(const Value& value, MtlxConstantValue* out);
MtlxConstantValue MtlxBinary(const MtlxConstantValue& a, const MtlxConstantValue& b, const std::function<float(float, float)>& op);
bool EvalMtlxConstantConnection(const Stage& stage, const std::string& connection, double time_code, const std::string& evaluation_space, MtlxConstantValue* out, std::set<std::string>* visiting, int depth);
bool TransformMtlxColorInput(const UsdPrim& node, const std::string& property, const std::string& evaluation_space, MtlxConstantValue* value);
bool EvalMtlxInput(const Stage& stage, const UsdPrim& node, const std::string& input, double time_code, const std::string& evaluation_space, MtlxConstantValue* out, std::set<std::string>* visiting, int depth);
bool EvalMtlxConstantNode(const Stage& stage, const UsdPrim& node, double time_code, const std::string& evaluation_space, MtlxConstantValue* out, std::set<std::string>* visiting, int depth);
}
}
}
