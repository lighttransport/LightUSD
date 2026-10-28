// SPDX-License-Identifier: MIT
#include "usdObj.hh"

#include <algorithm>

#include "core/composition-types.hh"
#include "lightusd.hh"
#include "security-policy.hh"
#include "tydra/asset-import/import.hh"
#include "tydra/urdf-to-usd.hh"
#include "usdGeom.hh"
#include "usda-writer.hh"

namespace lightusd {
namespace usdObj {
namespace {
template <class T>
void Set(GPrim* prim, const char* name, T value,
         Interpolation interpolation = Interpolation::Constant) {
  Attribute attr;
  attr.set_value(std::move(value));
  attr.metas().set_interpolation_enum(interpolation);
  prim->props[name] = Property(std::move(attr), false);
}
[[maybe_unused]] bool Geometry(const std::string& text, GPrim* prim,
                               std::string* err) {
  if (!prim) {
    if (err) *err = "OBJ output prim is null";
    return false;
  }
  tydra::import_asset::Options options;
  options.max_memory = security_policy::GetMaxAssetReadBytes();
  options.max_input = options.max_memory;
  tydra::import_asset::Reader reader(options);
  reader.allow_unanchored = false;
  tydra::import_asset::OBJ obj;
  if (!reader.Charge(text.size(), err) ||
      !tydra::import_asset::ReadOBJ(text, "", reader, false, &obj, err))
    return false;
  std::vector<value::point3f> points;
  std::vector<value::normal3f> normals;
  std::vector<value::texcoord2f> uv;
  std::vector<int32_t> counts, indices;
  bool all_uv = true;
  int32_t influences = 0;
  std::vector<int32_t> joint_indices;
  std::vector<float> joint_weights;
  for (const auto& mesh : obj.meshes) {
    if (mesh.type != "Mesh") continue;
    influences = std::max(influences, mesh.influences);
    joint_indices.insert(joint_indices.end(), mesh.joint_indices.begin(),
                         mesh.joint_indices.end());
    joint_weights.insert(joint_weights.end(), mesh.joint_weights.begin(),
                         mesh.joint_weights.end());
    int32_t base = static_cast<int32_t>(points.size());
    for (size_t i = 0; i < mesh.positions.size(); i += 3)
      points.push_back(
          {mesh.positions[i], mesh.positions[i + 1], mesh.positions[i + 2]});
    for (size_t i = 0; i < mesh.normals.size(); i += 3)
      normals.push_back(
          {mesh.normals[i], mesh.normals[i + 1], mesh.normals[i + 2]});
    for (size_t i = 0; i < mesh.uvs.size(); i += 2)
      uv.push_back({mesh.uvs[i], mesh.uvs[i + 1]});
    all_uv &= mesh.uvs.size() == mesh.positions.size() / 3 * 2;
    counts.insert(counts.end(), mesh.counts.begin(), mesh.counts.end());
    for (int32_t i : mesh.indices) indices.push_back(base + i);
  }
  if (counts.empty()) {
    if (err) *err = "Geometry-only OBJ API requires polygon faces";
    return false;
  }
  if (influences > 0) {
    Set(prim, "primvars:skel:jointIndices", std::move(joint_indices),
        Interpolation::Vertex);
    Set(prim, "primvars:skel:jointWeights", std::move(joint_weights),
        Interpolation::Vertex);
    for (const char* name :
         {"primvars:skel:jointIndices", "primvars:skel:jointWeights"})
      prim->props[name].attribute().metas().set_elementSize(
          static_cast<uint32_t>(influences));
    APISchemas apis = prim->metas().has_apiSchemas()
                          ? prim->metas().get_apiSchemas()
                          : APISchemas{};
    apis.names.push_back({APISchemas::APIName::SkelBindingAPI, ""});
    prim->metas().set_apiSchemas(apis);
  }
  Set(prim, "points", std::move(points));
  Set(prim, "faceVertexCounts", std::move(counts));
  Set(prim, "faceVertexIndices", std::move(indices));
  Set(prim, "normals", std::move(normals), Interpolation::Vertex);
  if (all_uv)
    Set(prim, "primvars:st", std::move(uv), Interpolation::Vertex);
  else
    prim->props.erase("primvars:st");
  Set(prim, "subdivisionScheme", value::token("none"));
  return true;
}
}  // namespace
bool ReadObjFromString(const std::string& text, GPrim* prim, std::string* err) {
#if defined(LIGHTUSD_USE_USDOBJ)
  return Geometry(text, prim, err);
#else
  (void)text;
  (void)prim;
  if (err) *err = "usdObj is disabled in this build";
  return false;
#endif
}
bool ReadObjFromFile(const std::string& file, GPrim* prim, std::string* err) {
  tydra::import_asset::Options options;
  options.max_memory = security_policy::GetMaxAssetReadBytes();
  options.max_input = options.max_memory;
  tydra::import_asset::Reader reader(options);
  std::vector<uint8_t> bytes;
  std::string resolved;
  if (!reader.Read(file, "", &bytes, &resolved, err)) return false;
  return ReadObjFromString(
      std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
      prim, err);
}
bool ReadObjStageFromFile(const std::string& file, Stage* stage,
                          std::string* warn, std::string* err) {
#if defined(LIGHTUSD_WITH_TYDRA) && defined(LIGHTUSD_USE_USDOBJ)
  tydra::import_asset::Options options;
  options.format = "obj";
  tydra::import_asset::Result result;
  if (!tydra::import_asset::BuildOBJPayloadFile(file, options, &result, err))
    return false;
  std::string converter_warn;
  bool ok = tydra::ConvertURDFPayloadToUSDStage(result.payload, stage,
                                                &converter_warn, err);
  if (warn) *warn = result.warnings + converter_warn;
  return ok;
#else
  (void)file;
  (void)stage;
  (void)warn;
  if (err) *err = "OBJ stage conversion requires usdObj and Tydra";
  return false;
#endif
}
bool ReadObjStageFromMemory(const std::string& text, const std::string& source,
                            Stage* stage,
                            const tydra::import_asset::Options& options,
                            std::string* warn, std::string* err) {
#if defined(LIGHTUSD_WITH_TYDRA) && defined(LIGHTUSD_USE_USDOBJ)
  tydra::import_asset::Result result;
  if (!tydra::import_asset::BuildOBJPayload(text, source, options, &result,
                                            err))
    return false;
  std::string extra;
  const bool ok =
      tydra::ConvertURDFPayloadToUSDStage(result.payload, stage, &extra, err);
  if (warn) *warn += result.warnings + extra;
  return ok;
#else
  (void)text;
  (void)source;
  (void)stage;
  (void)options;
  (void)warn;
  if (err) *err = "OBJ stage conversion requires usdObj and Tydra";
  return false;
#endif
}
bool ReadObjLayerFromMemory(const std::string& text, const std::string& source,
                            Layer* layer,
                            const tydra::import_asset::Options& options,
                            std::string* warn, std::string* err) {
  Stage stage;
  if (!ReadObjStageFromMemory(text, source, &stage, options, warn, err))
    return false;
  std::string usda;
  if (!usda::ExportToUSDAString(stage, &usda, warn, err)) return false;
  USDLoadOptions load_options;
  load_options.max_memory_limit_in_mb = static_cast<int32_t>(std::min(
      size_t(INT32_MAX), std::max(size_t(1), options.max_memory / (1024 * 1024))));
  return LoadLayerFromMemory(reinterpret_cast<const uint8_t*>(usda.data()),
                             usda.size(), source, layer, warn, err,
                             load_options);
}

}  // namespace usdObj
}  // namespace lightusd
