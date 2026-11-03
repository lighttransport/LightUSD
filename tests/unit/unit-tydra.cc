#ifdef _MSC_VER
#define NOMINMAX
#endif

#define TEST_NO_MAIN
#include "acutest.h"

#include "unit-tydra.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "layer.hh"
#include "io-util.hh"
#include "core/prim.hh"
#include "core/prim-spec.hh"
#include "mtlx-dom.hh"
#include "value-clip-utils.hh"
#include "lightusd.hh"
#include "tydra/openpbr-params.hh"
#include "image-types.hh"
#include "image-writer.hh"
#include "tydra/attribute-eval.hh"
#include "tydra/layer-to-renderscene.hh"
#include "tydra/materialx-to-json.hh"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"
#include "usdGeom.hh"
#include "usdMtlx.hh"

using namespace lightusd;

namespace {

std::string ToPosixPath(std::string path) {
  for (char &ch : path) {
    if (ch == '\\') {
      ch = '/';
    }
  }
  return path;
}

std::string MakeTempUSDFilePath(const std::string &prefix) {
  static int counter = 0;
  return lightusd::io::JoinPath(
      lightusd::io::GetTempDir(),
      prefix + "_" + std::to_string(counter++) + ".usda");
}

bool WriteTextFile(const std::string &path, const std::string &content) {
  std::ofstream ofs(path, std::ios::out | std::ios::trunc);
  if (!ofs) {
    return false;
  }
  ofs << content;
  return ofs.good();
}

void CleanupTempFiles(const std::vector<std::string> &paths) {
  for (const auto &path : paths) {
    if (path.empty()) {
      continue;
    }
    (void)lightusd::io::RemoveFile(path);
  }
}

bool NearlyEqual(float lhs, float rhs, float eps = 1e-5f) {
  return std::fabs(lhs - rhs) <= eps;
}

std::string FormatDouble2Pairs(const std::vector<std::pair<double, double>> &pairs) {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < pairs.size(); i++) {
    if (i > 0) {
      oss << ", ";
    }
    oss << "(" << pairs[i].first << ", " << pairs[i].second << ")";
  }
  oss << "]";
  return oss.str();
}

std::string FormatActivePairs(
    const std::vector<std::pair<double, int>> &pairs) {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < pairs.size(); i++) {
    if (i > 0) {
      oss << ", ";
    }
    oss << "(" << pairs[i].first << ", " << pairs[i].second << ")";
  }
  oss << "]";
  return oss.str();
}

std::string MakeValueClipAssetUSD(
    const std::array<float, 3> &translate,
    const std::array<float, 3> &scale,
    const std::vector<std::pair<std::string, std::string>> &extra_props = {}) {
  std::ostringstream oss;
  oss << "#usda 1.0\n";
  oss << "def Xform \"Root\"\n";
  oss << "{\n";
  oss << "    double3 xformOp:translate = (" << translate[0] << ", "
      << translate[1] << ", " << translate[2] << ")\n";
  oss << "    double3 xformOp:scale = (" << scale[0] << ", " << scale[1]
      << ", " << scale[2] << ")\n";
  oss << "    uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:scale\"]\n";
  for (const auto &prop : extra_props) {
    oss << "    " << prop.first << " = " << prop.second << "\n";
  }
  oss << "}\n";
  return oss.str();
}

std::string MakeValueClipSceneUSD(const std::string &clip0_path,
                                 const std::string &clip1_path,
                                 const std::vector<std::pair<double, int>> &active,
                                 const std::vector<std::pair<double, double>> &times,
                                 bool interpolate_missing) {
  std::ostringstream oss;
  oss << "#usda 1.0\n";
  oss << "def Xform \"Root\" (\n";
  oss << "    clips = {\n";
  oss << "        dictionary default_clip = {\n";
  oss << "            double2[] active = " << FormatActivePairs(active) << "\n";
  oss << "            asset[] assetPaths = [@" << ToPosixPath(clip0_path) << "@, @"
      << ToPosixPath(clip1_path) << "@]\n";
  oss << "            string primPath = \"/Root\"\n";
  oss << "            double2[] times = " << FormatDouble2Pairs(times) << "\n";
  if (interpolate_missing) {
    oss << "            bool interpolateMissingClipValues = true\n";
  }
  oss << "        }\n";
  oss << "    }\n";
  oss << ")\n";
  oss << "{}\n";
  return oss.str();
}

std::vector<Path> MakeMultiTargetConnections() {
  return {Path("/ShaderA", "outputs:out"), Path("/ShaderB", "outputs:out")};
}

bool ContainsName(const std::vector<std::string> &names,
                  const std::string &name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

size_t CountName(const std::vector<std::string> &names,
                 const std::string &name) {
  return size_t(std::count(names.begin(), names.end(), name));
}

const std::array<value::texcoord2f, 3> kTexcoordsVec2 = {
    value::texcoord2f{0.0f, 0.0f}, value::texcoord2f{1.0f, 0.0f},
    value::texcoord2f{0.0f, 1.0f}};

const std::array<value::float3, 3> kTexcoordsVec3 = {
    value::float3{0.0f, 0.0f, 0.0f}, value::float3{1.0f, 0.0f, 0.0f},
    value::float3{0.0f, 1.0f, 0.0f}};

struct CancelOnMeshProgressState {
  size_t mesh_progress_calls{0};
};

bool FailingTextureImageLoader(
    const value::AssetPath &assetPath, const AssetInfo &assetInfo,
    const AssetResolutionResolver &assetResolver, tydra::TextureImage *imageOut,
    std::vector<uint8_t> *imageData, void *userdata, std::string *warn,
    std::string *err) {
  (void)assetPath;
  (void)assetInfo;
  (void)assetResolver;
  (void)imageOut;
  (void)imageData;
  (void)userdata;
  (void)warn;

  if (err) {
    *err = "synthetic texture loader failure";
  }
  return false;
}

bool SingleChannelTextureImageLoader(
    const value::AssetPath &assetPath, const AssetInfo &assetInfo,
    const AssetResolutionResolver &assetResolver, tydra::TextureImage *imageOut,
    std::vector<uint8_t> *imageData, void *userdata, std::string *warn,
    std::string *err) {
  (void)assetInfo;
  (void)assetResolver;
  (void)userdata;
  (void)warn;

  if (!imageOut || !imageData) {
    if (err) {
      *err = "output buffer is null";
    }
    return false;
  }

  imageOut->asset_identifier = assetPath.GetAssetPath();
  imageOut->width = 1;
  imageOut->height = 1;
  imageOut->channels = 1;
  imageOut->assetTexelComponentType = tydra::ComponentType::UInt8;

  imageData->assign(1, uint8_t(128));
  return true;
}

bool CancelOnFirstMeshProgress(const tydra::DetailedProgressInfo &info,
                               void *userptr) {
  auto *state = reinterpret_cast<CancelOnMeshProgressState *>(userptr);
  if (!state) {
    return false;
  }

  if (info.stage == tydra::DetailedProgressInfo::Stage::ConvertingMeshes) {
    state->mesh_progress_calls++;
    return false;
  }

  return true;
}

}  // namespace

void tydra_connection_validation_test(void) {
  std::string err;
  Path target_path;

  TEST_CHECK(!tydra::detail::ResolveSingleConnectionTargetPath(
      {}, "inputs:test", &target_path, &err));
  TEST_CHECK(err.find("empty") != std::string::npos);

  err.clear();
  const std::vector<Path> multi_targets = MakeMultiTargetConnections();
  TEST_CHECK(!tydra::detail::ResolveSingleConnectionTargetPath(
      multi_targets, "inputs:test", &target_path, &err));
  TEST_CHECK(err.find("Multiple targetPaths") != std::string::npos);

  Stage stage;
  tydra::TerminalAttributeValue resolved;

  {
    Attribute attr;
    attr.set_connections(multi_targets);

    err.clear();
    TEST_CHECK(
        !tydra::EvaluateAttribute(stage, attr, "inputs:test", &resolved, &err));
    TEST_CHECK(err.find("Multiple targetPaths") != std::string::npos);
  }

  {
    TypedAttribute<float> attr;
    attr.set_connections(multi_targets);

    float value = 0.0f;
    err.clear();
    TEST_CHECK(
        !tydra::EvaluateTypedAttribute(stage, attr, "inputs:test", &value, &err));
    TEST_CHECK(err.find("Multiple targetPaths") != std::string::npos);
  }

  {
    TypedAttribute<Animatable<float>> attr;
    attr.set_connections(multi_targets);

    float value = 0.0f;
    err.clear();
    TEST_CHECK(!tydra::EvaluateTypedAnimatableAttribute(
        stage, attr, "inputs:test", &value, &err, 1.0,
        value::TimeSampleInterpolationType::Held));
    TEST_CHECK(err.find("Multiple targetPaths") != std::string::npos);
  }

  {
    TypedAttributeWithFallback<float> attr{0.5f};
    attr.set_connections(multi_targets);
    attr.set_value_empty();

    float value = 0.0f;
    err.clear();
    TEST_CHECK(
        !tydra::EvaluateTypedAttribute(stage, attr, "inputs:test", &value, &err));
    TEST_CHECK(err.find("Multiple targetPaths") != std::string::npos);
  }

  {
    TypedAttributeWithFallback<Animatable<float>> attr{Animatable<float>(0.5f)};
    attr.set_connections(multi_targets);
    attr.set_value_empty();

    float value = 0.0f;
    err.clear();
    TEST_CHECK(!tydra::EvaluateTypedAnimatableAttribute(
        stage, attr, "inputs:test", &value, &err, 1.0,
        value::TimeSampleInterpolationType::Held));
    TEST_CHECK(err.find("Multiple targetPaths") != std::string::npos);
  }
}

void tydra_inplace_conversion_guard_test(void) {
  tydra::LayerToRenderSceneConverter converter;
  tydra::RenderScene render_scene;
  std::string warn;
  std::string err;

  {
    auto layer = std::make_unique<Layer>();
    PrimSpec xform_ps;
    xform_ps.specifier() = Specifier::Def;
    xform_ps.typeName() = "Xform";
    xform_ps.name() = "Root";
    TEST_CHECK(layer->add_primspec("Root", xform_ps));

    TEST_CHECK(!converter.ConvertLayerInPlace(std::move(layer), &render_scene,
                                              &warn, &err));
    TEST_CHECK(err.find("temporarily disabled") != std::string::npos);
    TEST_CHECK(render_scene.nodes.empty());
    TEST_CHECK(render_scene.meshes.empty());
    TEST_CHECK(render_scene.materials.empty());
  }

  {
    Layer layer;
    PrimSpec xform_ps;
    xform_ps.specifier() = Specifier::Def;
    xform_ps.typeName() = "Xform";
    xform_ps.name() = "Root";
    TEST_CHECK(layer.add_primspec("Root", xform_ps));

    tydra::RenderScene normal_scene;
    warn.clear();
    err.clear();
    TEST_CHECK(converter.ConvertLayer(&layer, &normal_scene, &warn, &err));
    TEST_CHECK(normal_scene.nodes.size() == 1);
  }

  {
    auto prim_spec = std::make_unique<PrimSpec>();
    prim_spec->specifier() = Specifier::Def;
    prim_spec->typeName() = "Mesh";
    prim_spec->name() = "MeshPrim";

    tydra::RenderMesh mesh;
    warn.clear();
    err.clear();
    TEST_CHECK(!converter.ConvertPrimSpecInPlace(std::move(prim_spec), &mesh,
                                                 &warn, &err));
    TEST_CHECK(err.find("temporarily disabled") != std::string::npos);
  }
}

void tydra_geommesh_property_accessor_test(void) {
  GeomMesh mesh;
  std::string err;
  Prim prim("MeshPrim", mesh);

  mesh.faceVertexCounts = Animatable<std::vector<int32_t>>(
      std::vector<int32_t>{4, 4});
  mesh.faceVertexIndices = Animatable<std::vector<int32_t>>(
      std::vector<int32_t>{0, 1, 2, 3, 3, 2, 1, 0});
  prim.set_primdata("MeshPrim", mesh);

  {
    Property prop;
    err.clear();
    TEST_CHECK(tydra::GetProperty(prim, "faceVertexCounts", &prop, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(prop.is_attribute());

    const Attribute *attr = prop.get_attribute_or_null();
    TEST_CHECK(attr != nullptr);
    auto value = attr->get_value<std::vector<int32_t>>();
    TEST_CHECK(value.has_value());
    TEST_CHECK(value.value() == std::vector<int32_t>({4, 4}));
  }

  {
    Property prop;
    err.clear();
    TEST_CHECK(tydra::GetProperty(prim, "faceVertexIndices", &prop, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(prop.is_attribute());

    const Attribute *attr = prop.get_attribute_or_null();
    TEST_CHECK(attr != nullptr);
    auto value = attr->get_value<std::vector<int32_t>>();
    TEST_CHECK(value.has_value());
    TEST_CHECK(value.value() ==
               std::vector<int32_t>({0, 1, 2, 3, 3, 2, 1, 0}));
    TEST_CHECK(value.value() != std::vector<int32_t>({4, 4}));
  }
}

void tydra_analytic_shape_facevarying_attrs_test(void) {
  const std::string path = MakeTempUSDFilePath("tydra_analytic_shapes");
  auto cleanup = [&]() { CleanupTempFiles({path}); };

  const char *usda = R"USD(#usda 1.0

def Xform "World"
{
    def Capsule "Capsule"
    {
        double radius = 0.05
        double height = 0.4
    }

    def Cylinder "Cylinder"
    {
        double radius = 0.05
        double height = 0.4
    }

    def Cone "Cone"
    {
        double radius = 0.05
        double height = 0.4
    }

    def Plane "Plane"
    {
        double width = 1
        double length = 1
    }
}
)USD";

  std::string err;
  TEST_CHECK(WriteTextFile(path, usda));

  Stage stage;
  TEST_CHECK(LoadUSDFromFile(path, &stage, nullptr, &err));
  TEST_MSG("LoadUSDFromFile failed: %s", err.c_str());

  tydra::RenderScene scene;
  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderSceneConverter converter;

  TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());
  TEST_CHECK(scene.meshes.size() == 4);

  cleanup();
}

void tydra_memory_tracking_test(void) {
  tydra::detail::MemoryUsageState state;
  std::vector<std::string> progress_messages;
  std::vector<size_t> freed_bytes;

  state.current_memory_usage = 128;
  state.peak_memory_usage = 128;

  tydra::detail::ApplyMemoryUsageDelta(
      0, 256, 1024, &state,
      [&progress_messages](const std::string &message) {
        progress_messages.push_back(message);
      },
      [&freed_bytes](size_t bytes_freed) { freed_bytes.push_back(bytes_freed); });

  TEST_CHECK(state.current_memory_usage == 0);
  TEST_CHECK(state.peak_memory_usage == 128);
  TEST_CHECK(freed_bytes.size() == 1);
  TEST_CHECK(freed_bytes[0] == 128);
  TEST_CHECK(progress_messages.empty());

  tydra::detail::ApplyMemoryUsageDelta(
      2 * 1024 * 1024, 0, 1, &state,
      [&progress_messages](const std::string &message) {
        progress_messages.push_back(message);
      },
      [&freed_bytes](size_t bytes_freed) { freed_bytes.push_back(bytes_freed); });

  TEST_CHECK(state.current_memory_usage == 2 * 1024 * 1024);
  TEST_CHECK(state.peak_memory_usage == 2 * 1024 * 1024);
  TEST_CHECK(freed_bytes.size() == 1);
  TEST_CHECK(progress_messages.size() == 1);
  TEST_CHECK(progress_messages[0].find("Memory limit exceeded") !=
             std::string::npos);
}

void tydra_scene_access_helper_test(void) {
  GeomMesh mesh;
  mesh.points = Animatable<std::vector<value::point3f>>(
      std::vector<value::point3f>{{0.0f, 0.0f, 0.0f},
                                  {1.0f, 0.0f, 0.0f},
                                  {0.0f, 1.0f, 0.0f}});
  mesh.faceVertexCounts = Animatable<std::vector<int32_t>>(
      std::vector<int32_t>{3});
  mesh.faceVertexIndices = Animatable<std::vector<int32_t>>(
      std::vector<int32_t>{0, 1, 2});

  Relationship skeleton_rel;
  skeleton_rel.set(Path("/Skeleton", ""));
  mesh.skeleton = skeleton_rel;

  Prim prim("MeshPrim", mesh);
  std::string err;

  {
    std::vector<std::string> attr_names;
    TEST_CHECK(tydra::GetAttributeNames(prim, &attr_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(std::find(attr_names.begin(), attr_names.end(), "points") !=
               attr_names.end());
    TEST_CHECK(std::find(attr_names.begin(), attr_names.end(),
                         "faceVertexCounts") != attr_names.end());
    TEST_CHECK(std::find(attr_names.begin(), attr_names.end(),
                         "faceVertexIndices") != attr_names.end());
    TEST_CHECK(std::find(attr_names.begin(), attr_names.end(), "skeleton") ==
               attr_names.end());
  }

  {
    std::vector<std::string> rel_names;
    err.clear();
    TEST_CHECK(tydra::GetRelationshipNames(prim, &rel_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(std::find(rel_names.begin(), rel_names.end(), "skeleton") !=
               rel_names.end());
    TEST_CHECK(std::find(rel_names.begin(), rel_names.end(),
                         "faceVertexCounts") == rel_names.end());
  }

  {
    Relationship rel;
    err.clear();
    TEST_CHECK(tydra::GetRelationship(prim, "skeleton", &rel, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(rel.is_path());
    TEST_CHECK(rel.targetPath.prim_part() == "/Skeleton");
  }
}

void tydra_shader_scene_access_test(void) {
  std::string err;

  {
    UsdPreviewSurface surface;
    surface.diffuseColor.set_value(
        Animatable<value::color3f>(value::color3f({0.8f, 0.2f, 0.1f})));
    surface.outputsSurface.set_authored(true);

    Shader shader;
    shader.info_id = kUsdPreviewSurface;
    shader.value = surface;

    Prim prim("PreviewSurface", shader);

    Property prop;
    TEST_CHECK(tydra::GetProperty(prim, "info:id", &prop, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(prop.is_attribute());
    {
      const Attribute *attr = prop.get_attribute_or_null();
      TEST_CHECK(attr != nullptr);
      auto info_id = attr->get_value<value::token>();
      TEST_CHECK(info_id.has_value());
      TEST_CHECK(info_id.value().str() == kUsdPreviewSurface);
    }

    err.clear();
    TEST_CHECK(tydra::GetProperty(prim, "inputs:diffuseColor", &prop, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(prop.is_attribute());
    {
      const Attribute *attr = prop.get_attribute_or_null();
      TEST_CHECK(attr != nullptr);
      auto diffuse = attr->get_value<value::color3f>();
      TEST_CHECK(diffuse.has_value());
      TEST_CHECK(diffuse.value()[0] == 0.8f);
      TEST_CHECK(diffuse.value()[1] == 0.2f);
      TEST_CHECK(diffuse.value()[2] == 0.1f);
    }

    err.clear();
    TEST_CHECK(tydra::GetProperty(prim, "outputs:surface", &prop, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(prop.is_attribute());

    std::vector<std::string> prop_names;
    TEST_CHECK(tydra::GetPropertyNames(prim, &prop_names, &err));
    TEST_CHECK(std::find(prop_names.begin(), prop_names.end(), "info:id") !=
               prop_names.end());
    TEST_CHECK(std::find(prop_names.begin(), prop_names.end(),
                         "inputs:diffuseColor") != prop_names.end());
    TEST_CHECK(std::find(prop_names.begin(), prop_names.end(),
                         "outputs:surface") != prop_names.end());

    std::vector<std::string> attr_names;
    err.clear();
    TEST_CHECK(tydra::GetAttributeNames(prim, &attr_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(std::find(attr_names.begin(), attr_names.end(), "info:id") !=
               attr_names.end());
    TEST_CHECK(std::find(attr_names.begin(), attr_names.end(),
                         "inputs:diffuseColor") != attr_names.end());

    std::vector<std::string> rel_names;
    err.clear();
    TEST_CHECK(tydra::GetRelationshipNames(prim, &rel_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(rel_names.empty());
  }

  {
    UsdTransform2d tx;
    tx.rotation.set_value(Animatable<float>(45.0f));
    tx.result.set_authored(true);

    Shader shader;
    shader.info_id = kUsdTransform2d;
    shader.value = tx;

    Prim prim("Transform2d", shader);
    Property prop;

    err.clear();
    TEST_CHECK(tydra::GetProperty(prim, "inputs:rotation", &prop, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(prop.is_attribute());

    const Attribute *attr = prop.get_attribute_or_null();
    TEST_CHECK(attr != nullptr);
    auto rotation = attr->get_value<float>();
    TEST_CHECK(rotation.has_value());
    TEST_CHECK(rotation.value() == 45.0f);
  }

  {
    Material material;
    material.surface.set(Path("/PreviewSurface", "outputs:surface"));

    Prim prim("Material", material);
    std::vector<std::string> prop_names;
    err.clear();
    TEST_CHECK(tydra::GetPropertyNames(prim, &prop_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(std::find(prop_names.begin(), prop_names.end(),
                         "outputs:surface") != prop_names.end());

    std::vector<std::string> attr_names;
    err.clear();
    TEST_CHECK(tydra::GetAttributeNames(prim, &attr_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(std::find(attr_names.begin(), attr_names.end(),
                         "outputs:surface") != attr_names.end());
  }
}

void tydra_skel_scene_access_test(void) {
  std::string err;

  {
    SkelRoot skelroot;
    skelroot.purpose = Purpose::Render;
    skelroot.visibility = Animatable<Visibility>(Visibility::Invisible);

    Relationship proxy_rel;
    proxy_rel.set(Path("/Proxy", ""));
    skelroot.proxyPrim = proxy_rel;

    Relationship anim_rel;
    anim_rel.set(Path("/Anim", ""));
    skelroot.animationSource = anim_rel;

    Relationship skeleton_rel;
    skeleton_rel.set(Path("/Skeleton", ""));
    skelroot.skeleton = skeleton_rel;

    XformOp translate_op;
    translate_op.op_type = XformOp::OpType::Translate;
    translate_op.set_value(value::double3{1.0, 2.0, 3.0});
    skelroot.xformOps.push_back(translate_op);

    Prim prim("SkelRootPrim", skelroot);

    std::vector<std::string> prop_names;
    TEST_CHECK(tydra::GetPropertyNames(prim, &prop_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(ContainsName(prop_names, "purpose"));
    TEST_CHECK(ContainsName(prop_names, "visibility"));
    TEST_CHECK(ContainsName(prop_names, "proxyPrim"));
    TEST_CHECK(ContainsName(prop_names, "animationSource"));
    TEST_CHECK(ContainsName(prop_names, "skeleton"));
    TEST_CHECK(ContainsName(prop_names, "xformOp:translate"));
    TEST_CHECK(ContainsName(prop_names, "xformOpOrder"));

    std::vector<std::string> attr_names;
    err.clear();
    TEST_CHECK(tydra::GetAttributeNames(prim, &attr_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(ContainsName(attr_names, "purpose"));
    TEST_CHECK(ContainsName(attr_names, "visibility"));
    TEST_CHECK(ContainsName(attr_names, "xformOp:translate"));
    TEST_CHECK(ContainsName(attr_names, "xformOpOrder"));
    TEST_CHECK(!ContainsName(attr_names, "skeleton"));

    std::vector<std::string> rel_names;
    err.clear();
    TEST_CHECK(tydra::GetRelationshipNames(prim, &rel_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(ContainsName(rel_names, "proxyPrim"));
    TEST_CHECK(ContainsName(rel_names, "animationSource"));
    TEST_CHECK(ContainsName(rel_names, "skeleton"));
    TEST_CHECK(!ContainsName(rel_names, "purpose"));

    Property prop;
    err.clear();
    TEST_CHECK(tydra::GetProperty(prim, "purpose", &prop, &err));
    TEST_CHECK(err.empty());
    {
      const Attribute *attr = prop.get_attribute_or_null();
      TEST_CHECK(attr != nullptr);
      auto purpose = attr->get_value<value::token>();
      TEST_CHECK(purpose.has_value());
      TEST_CHECK(purpose.value().str() == "render");
    }

    err.clear();
    TEST_CHECK(tydra::GetProperty(prim, "xformOpOrder", &prop, &err));
    TEST_CHECK(err.empty());
    {
      const Attribute *attr = prop.get_attribute_or_null();
      TEST_CHECK(attr != nullptr);
      auto xform_op_order = attr->get_value<std::vector<value::token>>();
      TEST_CHECK(xform_op_order.has_value());
      TEST_CHECK(xform_op_order.value().size() == 1);
      TEST_CHECK(xform_op_order.value()[0].str() == "xformOp:translate");
    }

    Relationship rel;
    err.clear();
    TEST_CHECK(tydra::GetRelationship(prim, "skeleton", &rel, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(rel.is_path());
    TEST_CHECK(rel.targetPath.prim_part() == "/Skeleton");
  }

  {
    Skeleton skeleton;
    skeleton.bindTransforms = std::vector<value::matrix4d>{
        value::matrix4d::identity()};
    skeleton.jointNames = std::vector<value::token>{value::token("Root")};
    skeleton.joints = std::vector<value::token>{value::token("Root")};
    skeleton.restTransforms = std::vector<value::matrix4d>{
        value::matrix4d::identity()};
    skeleton.purpose = Purpose::Guide;

    Relationship proxy_rel;
    proxy_rel.set(Path("/Proxy", ""));
    skeleton.proxyPrim = proxy_rel;

    Relationship anim_rel;
    anim_rel.set(Path("/Anim", ""));
    skeleton.animationSource = anim_rel;

    XformOp scale_op;
    scale_op.op_type = XformOp::OpType::Scale;
    scale_op.set_value(value::double3{2.0, 2.0, 2.0});
    skeleton.xformOps.push_back(scale_op);

    Prim prim("SkeletonPrim", skeleton);

    std::vector<std::string> prop_names;
    err.clear();
    TEST_CHECK(tydra::GetPropertyNames(prim, &prop_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(ContainsName(prop_names, "bindTransforms"));
    TEST_CHECK(ContainsName(prop_names, "jointNames"));
    TEST_CHECK(ContainsName(prop_names, "joints"));
    TEST_CHECK(ContainsName(prop_names, "restTransforms"));
    TEST_CHECK(ContainsName(prop_names, "purpose"));
    TEST_CHECK(ContainsName(prop_names, "proxyPrim"));
    TEST_CHECK(ContainsName(prop_names, "animationSource"));
    TEST_CHECK(ContainsName(prop_names, "xformOp:scale"));
    TEST_CHECK(ContainsName(prop_names, "xformOpOrder"));

    std::vector<std::string> rel_names;
    err.clear();
    TEST_CHECK(tydra::GetRelationshipNames(prim, &rel_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(ContainsName(rel_names, "proxyPrim"));
    TEST_CHECK(ContainsName(rel_names, "animationSource"));
    TEST_CHECK(!ContainsName(rel_names, "bindTransforms"));

    Property prop;
    err.clear();
    TEST_CHECK(tydra::GetProperty(prim, "purpose", &prop, &err));
    TEST_CHECK(err.empty());
    {
      const Attribute *attr = prop.get_attribute_or_null();
      TEST_CHECK(attr != nullptr);
      auto purpose = attr->get_value<value::token>();
      TEST_CHECK(purpose.has_value());
      TEST_CHECK(purpose.value().str() == "guide");
    }
  }

  {
    BlendShape blendshape;
    blendshape.offsets = std::vector<value::vector3f>{
        value::vector3f{0.1f, 0.0f, 0.0f}};
    blendshape.normalOffsets = std::vector<value::vector3f>{
        value::vector3f{0.0f, 0.1f, 0.0f}};
    blendshape.pointIndices = std::vector<int>{0};

    Prim prim("BlendShapePrim", blendshape);

    std::vector<std::string> attr_names;
    err.clear();
    TEST_CHECK(tydra::GetAttributeNames(prim, &attr_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(ContainsName(attr_names, "offsets"));
    TEST_CHECK(ContainsName(attr_names, "normalOffsets"));
    TEST_CHECK(ContainsName(attr_names, "pointIndices"));

    std::vector<std::string> rel_names;
    err.clear();
    TEST_CHECK(tydra::GetRelationshipNames(prim, &rel_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(rel_names.empty());
  }

  {
    SkelAnimation anim;
    anim.blendShapes = std::vector<value::token>{value::token("shapeA")};
    anim.blendShapeWeights = Animatable<std::vector<float>>(
        std::vector<float>{0.5f});
    anim.joints = std::vector<value::token>{value::token("Root")};
    anim.translations = Animatable<std::vector<value::float3>>(
        std::vector<value::float3>{value::float3{1.0f, 2.0f, 3.0f}});

    Prim prim("SkelAnimPrim", anim);

    std::vector<std::string> prop_names;
    err.clear();
    TEST_CHECK(tydra::GetPropertyNames(prim, &prop_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(ContainsName(prop_names, "blendShapes"));
    TEST_CHECK(ContainsName(prop_names, "blendShapeWeights"));
    TEST_CHECK(ContainsName(prop_names, "joints"));
    TEST_CHECK(ContainsName(prop_names, "translations"));

    Property prop;
    err.clear();
    TEST_CHECK(tydra::GetProperty(prim, "blendShapeWeights", &prop, &err));
    TEST_CHECK(err.empty());
    {
      const Attribute *attr = prop.get_attribute_or_null();
      TEST_CHECK(attr != nullptr);
      auto weights = attr->get_value<std::vector<float>>();
      TEST_CHECK(weights.has_value());
      TEST_CHECK(weights.value().size() == 1);
      TEST_CHECK(weights.value()[0] == 0.5f);
    }
  }
}

void tydra_blendshape_resolution_test(void) {
  std::string err;

  {
    Xform xform;
    XformOp translate_op;
    translate_op.op_type = XformOp::OpType::Translate;
    translate_op.set_value(value::double3{1.0, 2.0, 3.0});
    xform.xformOps.push_back(translate_op);

    Prim prim("XformPrim", xform);
    std::vector<std::string> prop_names;
    TEST_CHECK(tydra::GetPropertyNames(prim, &prop_names, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(CountName(prop_names, "xformOp:translate") == 1);
    TEST_CHECK(CountName(prop_names, "xformOpOrder") == 1);
  }

  {
    Stage stage;

    BlendShape shape_a;
    shape_a.name = "ShapeA";
    Prim shape_a_prim("ShapeA", shape_a);

    BlendShape shape_b;
    shape_b.name = "ShapeB";
    Prim shape_b_prim("ShapeB", shape_b);

    GeomMesh mesh;
    mesh.points = Animatable<std::vector<value::point3f>>(
        std::vector<value::point3f>{{0.0f, 0.0f, 0.0f},
                                    {1.0f, 0.0f, 0.0f},
                                    {0.0f, 1.0f, 0.0f}});
    mesh.faceVertexCounts = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{3});
    mesh.faceVertexIndices = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{0, 1, 2});
    mesh.blendShapes = std::vector<value::token>{value::token("shapeA"),
                                                 value::token("shapeB")};

    Relationship targets_rel;
    targets_rel.set(std::vector<Path>{Path("/ShapeA", ""), Path("/ShapeB", "")});
    mesh.blendShapeTargets = targets_rel;

    Prim mesh_prim("MeshPrim", mesh);

    stage.add_root_prim(std::move(mesh_prim));
    stage.add_root_prim(std::move(shape_a_prim));
    stage.add_root_prim(std::move(shape_b_prim));

    auto mesh_result = stage.GetPrimAtPath(Path("/MeshPrim", ""));
    TEST_CHECK(mesh_result.has_value());
    if (mesh_result) {
      err.clear();
      auto blendshapes = tydra::GetBlendShapes(stage, *mesh_result.value(), &err);
      TEST_CHECK(err.empty());
      TEST_CHECK(blendshapes.size() == 2);
      if (blendshapes.size() == 2) {
        TEST_CHECK(blendshapes[0].first == "shapeA");
        TEST_CHECK(blendshapes[1].first == "shapeB");
        TEST_CHECK(blendshapes[0].second != nullptr);
        TEST_CHECK(blendshapes[1].second != nullptr);
      }
    }
  }

  {
    Stage stage;

    GeomMesh mesh;
    mesh.points = Animatable<std::vector<value::point3f>>(
        std::vector<value::point3f>{{0.0f, 0.0f, 0.0f},
                                    {1.0f, 0.0f, 0.0f},
                                    {0.0f, 1.0f, 0.0f}});
    mesh.faceVertexCounts = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{3});
    mesh.faceVertexIndices = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{0, 1, 2});
    mesh.blendShapes = std::vector<value::token>{value::token("shapeA")};

    Relationship invalid_targets_rel;
    invalid_targets_rel.set(Path("/MissingBlendShape", ""));
    mesh.blendShapeTargets = invalid_targets_rel;

    Prim mesh_prim("MeshPrim", mesh);
    stage.add_root_prim(std::move(mesh_prim));

    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find("Failed to get BlendShapes prims") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("/MissingBlendShape") !=
               std::string::npos);
  }
}

// In-between BlendShape offsets must be carried into RenderMesh::targets and,
// crucially, splatted alongside the primary offsets when a facevarying mesh is
// de-indexed (single-indexable build duplicates shared points). Regression for
// the converter populating ShapeTarget::inbetweens + reordering them in
// ReorderVertexVaryingAttributes.
void tydra_blendshape_inbetween_test(void) {
  // Two triangles share points 0 and 2, but their facevarying `st` differs at
  // those corners, so the single-indexable build duplicates them: 4 -> 6
  // points. The in-between (parallel to the 4 authored pointIndices) must be
  // remapped to the same 6-entry space as the primary.
  const std::string usda = R"(#usda 1.0
(
    defaultPrim = "Root"
)
def Xform "Root"
{
    def Mesh "Mesh" (
        prepend apiSchemas = ["SkelBindingAPI"]
    )
    {
        int[] faceVertexCounts = [3, 3]
        int[] faceVertexIndices = [0, 1, 2, 0, 2, 3]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
        texCoord2f[] primvars:st = [
            (0, 0), (1, 0), (1, 1),
            (0.5, 0.5), (0.7, 0.7), (0, 1)
        ] (
            interpolation = "faceVarying"
        )
        uniform token[] skel:blendShapes = ["Key"]
        rel skel:blendShapeTargets = </Root/Mesh/Key>

        def BlendShape "Key"
        {
            uniform vector3f[] offsets = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)]
            uniform int[] pointIndices = [0, 1, 2, 3]
            uniform vector3f[] inbetweens:half = [(5, 0, 0), (5, 0, 0), (5, 0, 0), (5, 0, 0)] (
                weight = 0.5
            )
        }
    }
}
)";

  Stage stage;
  std::string warn;
  std::string err;
  TEST_CHECK(LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "blendshape_inbetween.usda", &stage, &warn, &err));
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;
  TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  TEST_CHECK(scene.meshes.size() == 1);
  if (scene.meshes.empty()) {
    return;
  }
  const tydra::RenderMesh &mesh = scene.meshes[0];

  // Facevarying `st` forced the shared points 0 and 2 to duplicate.
  TEST_CHECK(mesh.points.size() == 6);
  TEST_CHECK(mesh.targets.size() == 1);
  if (mesh.targets.empty()) {
    return;
  }

  const tydra::ShapeTarget &tgt = mesh.targets.begin()->second;
  // Primary offsets were splatted to the duplicated points.
  TEST_CHECK(tgt.pointIndices.size() == 6);
  TEST_CHECK(tgt.pointOffsets.size() == 6);

  // The in-between survived conversion and was splatted in lockstep with the
  // primary (same count, same indexing).
  TEST_CHECK(tgt.inbetweens.size() == 1);
  if (tgt.inbetweens.empty()) {
    return;
  }
  const tydra::InbetweenShapeTarget &ib = tgt.inbetweens.begin()->second;
  TEST_CHECK(std::fabs(ib.weight - 0.5f) < 1e-6f);
  TEST_CHECK(ib.pointOffsets.size() == tgt.pointIndices.size());
  TEST_CHECK(ib.pointOffsets.size() == 6);
  // Every authored in-between offset was (5,0,0); it must be preserved verbatim
  // through the splat (no aliasing with the primary's (0,0,1)).
  bool all_x5 = !ib.pointOffsets.empty();
  for (size_t i = 0; i < ib.pointOffsets.size(); i++) {
    all_x5 &= (std::fabs(ib.pointOffsets[i][0] - 5.0f) < 1e-6f) &&
              (std::fabs(ib.pointOffsets[i][1]) < 1e-6f) &&
              (std::fabs(ib.pointOffsets[i][2]) < 1e-6f);
  }
  TEST_CHECK(all_x5);
}

void tydra_blendshape_weight_animation_target_test(void) {
  const std::string usda = R"(#usda 1.0
(
    defaultPrim = "Root"
    startTimeCode = 1
    endTimeCode = 3
)
def SkelRoot "Root"
{
    def Skeleton "Skeleton"
    {
        uniform token[] joints = ["Root"]
        uniform matrix4d[] bindTransforms = [
            ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1))
        ]
        uniform matrix4d[] restTransforms = [
            ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1))
        ]
        rel skel:animationSource = </Root/Anim>
    }

    def SkelAnimation "Anim"
    {
        uniform token[] joints = ["Root"]
        float3[] translations = [(0, 0, 0)]
        quatf[] rotations = [(1, 0, 0, 0)]
        half3[] scales = [(1, 1, 1)]
        uniform token[] blendShapes = ["Smile"]
        float[] blendShapeWeights.timeSamples = {
            1: [0.0],
            2: [1.0],
            3: [0.25],
        }
    }

    def Mesh "Face" (
        prepend apiSchemas = ["SkelBindingAPI"]
    )
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
        rel skel:skeleton = </Root/Skeleton>
        int[] primvars:skel:jointIndices = [0, 0, 0] (
            interpolation = "vertex"
            elementSize = 1
        )
        float[] primvars:skel:jointWeights = [1, 1, 1] (
            interpolation = "vertex"
            elementSize = 1
        )
        uniform token[] skel:blendShapes = ["Smile"]
        rel skel:blendShapeTargets = </Root/Smile>
    }

    def BlendShape "Smile"
    {
        uniform vector3f[] offsets = [(0, 0, 1), (0, 0, 1), (0, 0, 1)]
        uniform int[] pointIndices = [0, 1, 2]
    }
}
)";

  Stage stage;
  std::string warn;
  std::string err;
  TEST_CHECK(LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "blendshape_weight_animation.usda", &stage, &warn, &err));
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;
  TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  const tydra::AnimationChannel *weight_channel = nullptr;
  const tydra::KeyframeSampler *weight_sampler = nullptr;
  for (const tydra::AnimationClip &clip : scene.animations) {
    for (const tydra::AnimationChannel &channel : clip.channels) {
      if (channel.path != tydra::AnimationPath::Weights) {
        continue;
      }
      if (channel.sampler < 0 ||
          size_t(channel.sampler) >= clip.samplers.size()) {
        continue;
      }
      weight_channel = &channel;
      weight_sampler = &clip.samplers[size_t(channel.sampler)];
      break;
    }
    if (weight_channel) {
      break;
    }
  }

  TEST_CHECK(weight_channel != nullptr);
  TEST_CHECK(weight_sampler != nullptr);
  if (!weight_channel || !weight_sampler) {
    return;
  }

  TEST_CHECK(weight_channel->target_node >= 0);
  TEST_CHECK(weight_channel->target_prim_path == "/Root/Face");
  TEST_CHECK(weight_channel->property_name == "blendShapeWeights");
  TEST_CHECK(weight_channel->blendshape_target_names.size() == 1);
  if (!weight_channel->blendshape_target_names.empty()) {
    TEST_CHECK(weight_channel->blendshape_target_names[0] == "Smile");
  }
  TEST_CHECK(weight_sampler->times.size() == 3);
  TEST_CHECK(weight_sampler->values.size() == 3);
  if (weight_sampler->values.size() == 3) {
    TEST_CHECK(NearlyEqual(weight_sampler->values[0], 0.0f));
    TEST_CHECK(NearlyEqual(weight_sampler->values[1], 1.0f));
    TEST_CHECK(NearlyEqual(weight_sampler->values[2], 0.25f));
  }
}

void tydra_material_binding_validation_test(void) {
  auto make_mesh = []() {
    GeomMesh mesh;
    mesh.points = Animatable<std::vector<value::point3f>>(
        std::vector<value::point3f>{{0.0f, 0.0f, 0.0f},
                                    {1.0f, 0.0f, 0.0f},
                                    {0.0f, 1.0f, 0.0f}});
    mesh.faceVertexCounts = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{3});
    mesh.faceVertexIndices = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{0, 1, 2});
    return mesh;
  };

  auto make_mtlx_openpbr_stage = [&](const Path &nodegraph_output_target) {
    Stage stage;

    Material material;
    material.surface.set(Path("/MaterialPrim/OpenPBRShader",
                              "outputs:surface"));

    OpenPBRSurface openpbr_surface;
    openpbr_surface.surface.set_authored(true);
    openpbr_surface.base_color.set_connection(
        Path("/MaterialPrim/NodeGraphs", "outputs:out"));
    openpbr_surface.base_color.set_value_empty();

    Shader openpbr_shader;
    openpbr_shader.info_id = kOpenPBRSurface;
    openpbr_shader.value = openpbr_surface;

    NodeGraph nodegraph;
    nodegraph.props["outputs:out.connect"] = Property(
        nodegraph_output_target, value::TypeTraits<value::color3f>::type_name());

    Shader image_shader;
    image_shader.info_id = "ND_image_color3";
    image_shader.value = ShaderNode{};

    Prim nodegraph_prim("NodeGraphs", nodegraph);
    TEST_CHECK(nodegraph_prim.add_child(Prim("Image", image_shader)));

    Prim material_prim("MaterialPrim", material);
    TEST_CHECK(material_prim.add_child(Prim("OpenPBRShader", openpbr_shader)));
    TEST_CHECK(material_prim.add_child(std::move(nodegraph_prim)));

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(std::move(material_prim)));

    return stage;
  };

  {
    Stage stage;

    Xform not_material;
    Prim not_material_prim("NotMaterial", not_material);

    GeomMesh mesh = make_mesh();

    Relationship invalid_material_rel;
    invalid_material_rel.set(Path("/NotMaterial", ""));
    mesh.set_materialBinding(invalid_material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(std::move(not_material_prim)));

    tydra::RenderSceneConverter converter;
    Path material_path;
    const Material *material{nullptr};
    std::string err;

    TEST_CHECK(!converter.GetBoundMaterialCached(stage, Path("/MeshPrim", ""), "",
                                                 &material_path, &material,
                                                 &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(material == nullptr);

    std::string cached_err;
    material_path = Path();
    material = nullptr;
    TEST_CHECK(!converter.GetBoundMaterialCached(stage, Path("/MeshPrim", ""), "",
                                                 &material_path, &material,
                                                 &cached_err));
    TEST_CHECK(cached_err.empty());
    TEST_CHECK(material == nullptr);

    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderScene scene;
    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().empty());
    TEST_CHECK(scene.meshes.size() == 1);
    if (scene.meshes.size() == 1) {
      TEST_CHECK(scene.meshes[0].material_id < 0);
    }
  }

  {
    Stage stage;

    Material invalid_material;
    Prim material_prim("MaterialPrim", invalid_material);

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(std::move(material_prim)));

    // outputs:surface unauthored is demoted to a warning by default;
    // opt into strict mode so this test exercises the error path.
    tydra::RenderSceneConverterEnv env(stage);
    env.material_config.strict_material_check = true;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find("/MaterialPrim") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("outputs:surface isn't authored") !=
               std::string::npos);
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/UnsupportedSurface", "outputs:surface"));

    Shader unsupported_shader;
    unsupported_shader.info_id = "ND_unimplemented_surface_surfaceshader";
    unsupported_shader.value = ShaderNode{};

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(Prim("MaterialPrim", material)));
    TEST_CHECK(stage.add_root_prim(Prim("UnsupportedSurface", unsupported_shader)));

    tydra::RenderSceneConverterEnv env(stage);
    env.material_config.assign_default_material = true;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().empty());
    TEST_CHECK(scene.meshes.size() == 1);
    TEST_CHECK(scene.materials.size() == 1);
    if (scene.meshes.size() == 1) {
      TEST_CHECK(scene.meshes[0].material_id == 0);
    }
    if (scene.materials.size() == 1) {
      TEST_CHECK(scene.materials[0].name == "defaultMaterial");
      TEST_CHECK(scene.materials[0].surfaceShader.has_value());
    }
    TEST_CHECK(converter.GetWarning().find("Material conversion failed") !=
               std::string::npos);
    TEST_CHECK(converter.GetWarning().find("using default material") !=
               std::string::npos);
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/PreviewSurface", "outputs:surface"));

    UsdPreviewSurface preview_surface;
    preview_surface.outputsSurface.set_authored(true);
    preview_surface.diffuseColor.set_connection(Path("/Tex", "outputs:rgb"));
    preview_surface.diffuseColor.set_value_empty();

    Shader preview_shader;
    preview_shader.info_id = kUsdPreviewSurface;
    preview_shader.value = preview_surface;

    UsdUVTexture uv_texture;
    uv_texture.outputsRGB.set_authored(true);

    Shader tex_shader;
    tex_shader.info_id = kUsdUVTexture;
    tex_shader.value = uv_texture;

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(Prim("MaterialPrim", material)));
    TEST_CHECK(stage.add_root_prim(Prim("PreviewSurface", preview_shader)));
    TEST_CHECK(stage.add_root_prim(Prim("Tex", tex_shader)));

    tydra::RenderSceneConverterEnv env(stage);
    // Strict mode: unauthored `asset:file` should propagate an error.
    env.material_config.allow_missing_asset = false;
    // Strict mode: 73330b10b demoted non-strict material failures to
    // warnings + default material; opt in to keep testing the error path.
    env.material_config.strict_material_check = true;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find("/Tex") != std::string::npos);
    TEST_CHECK(converter.GetError().find("`asset:file` is not authored") !=
               std::string::npos);
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/MaterialPrim/PreviewSurface",
                              "outputs:surface"));
    material.materialXConfig = MaterialXConfigAPI{};

    Relationship mtlx_surface_rel;
    mtlx_surface_rel.set(Path("/MaterialPrim/MtlxPreviewSurface",
                              "outputs:out"));
    material.props["outputs:mtlx:surface.connect"] = Property(
        mtlx_surface_rel);

    UsdPreviewSurface preview_surface;
    preview_surface.outputsSurface.set_authored(true);
    preview_surface.diffuseColor.set_value(
        Animatable<value::color3f>(value::color3f({0.1f, 0.2f, 0.3f})));

    Shader preview_shader;
    preview_shader.info_id = kUsdPreviewSurface;
    preview_shader.value = preview_surface;

    UsdPreviewSurface mtlx_preview_surface;
    mtlx_preview_surface.outputsSurface.set_authored(true);
    mtlx_preview_surface.diffuseColor.set_value(
        Animatable<value::color3f>(value::color3f({0.7f, 0.4f, 0.2f})));

    Shader mtlx_preview_shader;
    mtlx_preview_shader.info_id = kNdUsdPreviewSurfaceSurfaceshader;
    mtlx_preview_shader.value = mtlx_preview_surface;

    Prim material_prim("MaterialPrim", material);
    TEST_CHECK(material_prim.add_child(Prim("PreviewSurface", preview_shader)));
    TEST_CHECK(material_prim.add_child(
        Prim("MtlxPreviewSurface", mtlx_preview_shader)));

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(std::move(material_prim)));

    tydra::RenderSceneConverterEnv env(stage);
    env.material_config.strict_material_check = true;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.materials.size() == 1);
    if (scene.materials.size() == 1) {
      TEST_CHECK(scene.materials[0].surfaceShader.has_value());
      TEST_CHECK(!scene.materials[0].openPBRShader.has_value());
      if (scene.materials[0].surfaceShader.has_value()) {
        const auto &color =
            scene.materials[0].surfaceShader->diffuseColor.value;
        TEST_CHECK(color[0] == 0.7f);
        TEST_CHECK(color[1] == 0.4f);
        TEST_CHECK(color[2] == 0.2f);
      }
    }
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/MaterialPrim/MtlxSurface",
                              "outputs:surface"));

    MtlxOpenPBRSurface mtlx_surface;
    mtlx_surface.surface.set_authored(true);
    mtlx_surface.geometry_normal.set_connection(
        Path("/MaterialPrim/NodeGraphs", "outputs:normal"));
    mtlx_surface.geometry_normal.set_value_empty();

    Shader mtlx_surface_shader;
    mtlx_surface_shader.info_id = kNdOpenPbrSurfaceSurfaceshader;
    mtlx_surface_shader.value = mtlx_surface;

    NodeGraph nodegraph;
    nodegraph.props["outputs:normal"] = Property(
        Path("/MaterialPrim/NodeGraphs/NormalMap", "outputs:out"),
        value::TypeTraits<value::vector3f>::type_name());

    Shader normalmap_shader;
    normalmap_shader.info_id = "ND_normalmap";
    ShaderNode normalmap_node;
    normalmap_node.props["inputs:scale"] =
        Property(Attribute::Uniform(0.5f));
    normalmap_node.props["inputs:in"] = Property(
        Path("/MaterialPrim/NodeGraphs/Image", "outputs:out"),
        value::TypeTraits<value::vector3f>::type_name());
    normalmap_shader.value = normalmap_node;

    Shader image_shader;
    image_shader.info_id = "ND_tiledimage_vector3";
    ShaderNode image_node;
    image_node.props["inputs:file"] =
        Property(Attribute::Uniform(value::AssetPath("normal.png")));
    image_node.props["inputs:texcoord"] = Property(
        Path("/MaterialPrim/NodeGraphs/Texcoord", "outputs:out"),
        value::TypeTraits<value::float2>::type_name());
    {
      value::float2 uv_tiling;
      uv_tiling[0] = 2.0f;
      uv_tiling[1] = 3.0f;
      value::float2 uv_offset;
      uv_offset[0] = 0.25f;
      uv_offset[1] = 0.5f;
      image_node.props["inputs:uvtiling"] =
          Property(Attribute::Uniform(uv_tiling));
      image_node.props["inputs:uvoffset"] =
          Property(Attribute::Uniform(uv_offset));
    }
    image_shader.value = image_node;

    Shader texcoord_shader;
    texcoord_shader.info_id = "ND_texcoord_vector2";
    ShaderNode texcoord_node;
    texcoord_node.props["inputs:index"] = Property(Attribute::Uniform(1));
    texcoord_shader.value = texcoord_node;

    Prim nodegraph_prim("NodeGraphs", nodegraph);
    TEST_CHECK(nodegraph_prim.add_child(Prim("NormalMap", normalmap_shader)));
    TEST_CHECK(nodegraph_prim.add_child(Prim("Image", image_shader)));
    TEST_CHECK(nodegraph_prim.add_child(Prim("Texcoord", texcoord_shader)));

    Prim material_prim("MaterialPrim", material);
    TEST_CHECK(
        material_prim.add_child(Prim("MtlxSurface", mtlx_surface_shader)));
    TEST_CHECK(material_prim.add_child(std::move(nodegraph_prim)));

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(std::move(material_prim)));

    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.load_texture_assets = false;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.images.size() == 1);
    if (scene.images.size() == 1) {
      TEST_CHECK(scene.images[0].asset_identifier == "normal.png");
      TEST_CHECK(scene.images[0].colorSpace == tydra::ColorSpace::Raw);
    }
    TEST_CHECK(scene.textures.size() == 1);
    if (scene.textures.size() == 1) {
      TEST_CHECK(scene.textures[0].varname_uv == "st1");
      TEST_CHECK(scene.textures[0].connectedOutputChannel ==
                 tydra::UVTexture::Channel::RGB);
      TEST_CHECK(scene.textures[0].has_transform2d);
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_scale[0], 2.0f));
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_scale[1], 3.0f));
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_translation[0], 0.25f));
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_translation[1], 0.5f));
    }
    TEST_CHECK(scene.materials.size() == 1);
    if (scene.materials.size() == 1) {
      TEST_CHECK(scene.materials[0].openPBRShader.has_value());
      if (scene.materials[0].openPBRShader.has_value()) {
        const auto &shader = scene.materials[0].openPBRShader.value();
        TEST_CHECK(shader.normal.texture_id == 0);
        TEST_CHECK(NearlyEqual(shader.normal_map_scale, 0.5f));
      }
    }
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/MaterialPrim/OpenPBRShader",
                              "outputs:surface"));

    OpenPBRSurface openpbr_surface;
    openpbr_surface.surface.set_authored(true);
    openpbr_surface.base_color.set_connection(
        Path("/MaterialPrim/NodeGraphs", "outputs:baseColor"));
    openpbr_surface.base_color.set_value_empty();

    Shader openpbr_shader;
    openpbr_shader.info_id = kOpenPBRSurface;
    openpbr_shader.value = openpbr_surface;

    NodeGraph nodegraph;
    nodegraph.props["outputs:baseColor.connect"] = Property(
        Path("/MaterialPrim/NodeGraphs/Image", "outputs:out"),
        value::TypeTraits<value::color3f>::type_name());

    Shader image_shader;
    image_shader.info_id = "ND_image_color3";
    ShaderNode image_node;
    image_node.props["inputs:file"] =
        Property(Attribute::Uniform(value::AssetPath("basecolor-uv1.png")));
    image_node.props["inputs:texcoord"] = Property(
        Path("/MaterialPrim/NodeGraphs/Texcoord", "outputs:out"),
        value::TypeTraits<value::float2>::type_name());
    image_shader.value = image_node;

    Shader texcoord_shader;
    texcoord_shader.info_id = "ND_texcoord_vector2";
    ShaderNode texcoord_node;
    texcoord_node.props["inputs:index"] = Property(Attribute::Uniform(1));
    texcoord_shader.value = texcoord_node;

    Prim nodegraph_prim("NodeGraphs", nodegraph);
    TEST_CHECK(nodegraph_prim.add_child(Prim("Image", image_shader)));
    TEST_CHECK(nodegraph_prim.add_child(Prim("Texcoord", texcoord_shader)));

    Prim material_prim("MaterialPrim", material);
    TEST_CHECK(material_prim.add_child(Prim("OpenPBRShader", openpbr_shader)));
    TEST_CHECK(material_prim.add_child(std::move(nodegraph_prim)));

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(std::move(material_prim)));

    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.load_texture_assets = false;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.textures.size() == 1);
    if (scene.textures.size() == 1) {
      TEST_CHECK(scene.textures[0].varname_uv == "st1");
    }
  }

  {
    Stage stage;

    Material material;
    material.materialXConfig = MaterialXConfigAPI{};

    MtlxAutodeskStandardSurface standard_surface;
    standard_surface.out.set_authored(true);
    standard_surface.base_color.set_value(
        Animatable<value::color3f>(value::color3f({0.25f, 0.5f, 0.75f})));
    standard_surface.metalness.set_value(Animatable<float>(0.4f));
    standard_surface.specular_roughness.set_value(Animatable<float>(0.32f));
    standard_surface.transmission.set_value(Animatable<float>(0.8f));
    standard_surface.transmission_color.set_value(
        Animatable<value::color3f>(value::color3f({0.3f, 0.5f, 0.7f})));
    standard_surface.subsurface.set_value(Animatable<float>(0.22f));
    standard_surface.subsurface_color.set_value(
        Animatable<value::color3f>(value::color3f({0.6f, 0.7f, 0.8f})));
    standard_surface.subsurface_radius.set_value(
        Animatable<value::color3f>(value::color3f({0.7f, 0.5f, 0.3f})));
    standard_surface.thin_film_thickness.set_value(Animatable<float>(420.0f));
    standard_surface.emission.set_value(Animatable<float>(1.75f));
    standard_surface.emission_color.set_value(
        Animatable<value::color3f>(value::color3f({0.2f, 0.3f, 0.4f})));
    standard_surface.opacity.set_value(
        Animatable<value::color3f>(value::color3f({0.5f, 0.25f, 0.125f})));

    Shader standard_shader;
    standard_shader.info_id = kNdStandardSurfaceSurfaceshader;
    standard_shader.value = standard_surface;

    Prim material_prim("MaterialPrim", material);
    TEST_CHECK(material_prim.add_child(Prim("StandardSurface", standard_shader)));

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(std::move(material_prim)));

    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.meshes.size() == 1);
    TEST_CHECK(scene.materials.size() == 1);
    if (scene.meshes.size() == 1) {
      TEST_CHECK(scene.meshes[0].material_id == 0);
    }
    if (scene.materials.size() == 1) {
      TEST_CHECK(scene.materials[0].openPBRShader.has_value());
      TEST_CHECK(!scene.materials[0].surfaceShader.has_value());
      TEST_CHECK(scene.materials[0].materialTag ==
                 tydra::MaterialTag::Translucent);
      if (scene.materials[0].openPBRShader.has_value()) {
        const auto &shader = *scene.materials[0].openPBRShader;
        TEST_CHECK(NearlyEqual(shader.base_color.value[0], 0.25f));
        TEST_CHECK(NearlyEqual(shader.base_color.value[1], 0.5f));
        TEST_CHECK(NearlyEqual(shader.base_color.value[2], 0.75f));
        TEST_CHECK(NearlyEqual(shader.base_metalness.value, 0.4f));
        TEST_CHECK(NearlyEqual(shader.specular_roughness.value, 0.32f));
        TEST_CHECK(NearlyEqual(shader.transmission_weight.value, 0.8f));
        TEST_CHECK(NearlyEqual(shader.transmission_color.value[0], 0.3f));
        TEST_CHECK(NearlyEqual(shader.transmission_color.value[1], 0.5f));
        TEST_CHECK(NearlyEqual(shader.transmission_color.value[2], 0.7f));
        TEST_CHECK(NearlyEqual(shader.subsurface_weight.value, 0.22f));
        TEST_CHECK(NearlyEqual(shader.subsurface_color.value[0], 0.6f));
        TEST_CHECK(NearlyEqual(shader.subsurface_color.value[1], 0.7f));
        TEST_CHECK(NearlyEqual(shader.subsurface_color.value[2], 0.8f));
        TEST_CHECK(NearlyEqual(shader.subsurface_radius_scale.value[0], 0.7f));
        TEST_CHECK(NearlyEqual(shader.subsurface_radius_scale.value[1], 0.5f));
        TEST_CHECK(NearlyEqual(shader.subsurface_radius_scale.value[2], 0.3f));
        TEST_CHECK(NearlyEqual(shader.subsurface_radius.value, 1.0f));
        TEST_CHECK(NearlyEqual(shader.thin_film_thickness.value, 420.0f));
        TEST_CHECK(NearlyEqual(shader.thin_film_weight.value, 1.0f));
        TEST_CHECK(NearlyEqual(shader.emission_luminance.value, 1.75f));
        TEST_CHECK(NearlyEqual(shader.emission_color.value[0], 0.2f));
        const float expected_alpha =
            0.2126f * 0.5f + 0.7152f * 0.25f + 0.0722f * 0.125f;
        TEST_CHECK(NearlyEqual(shader.opacity.value, expected_alpha));
      }
    }
    TEST_CHECK(converter.GetError().empty());
    TEST_CHECK(converter.GetWarning().find("using default material appearance") ==
               std::string::npos);
  }

  {
    Stage stage;

    Material material;
    material.materialXConfig = MaterialXConfigAPI{};

    MtlxAutodeskStandardSurface standard_surface;
    standard_surface.out.set_authored(true);
    standard_surface.opacity.set_connection(
        Path("/MaterialPrim/OpacityTex", "outputs:r"));
    standard_surface.normal.set_connection(
        Path("/MaterialPrim/NormalTex", "outputs:rgb"));
    standard_surface.coat_normal.set_connection(
        Path("/MaterialPrim/CoatNormalTex", "outputs:rgb"));
    standard_surface.displacement.set_connection(
        Path("/MaterialPrim/DisplacementTex", "outputs:r"));

    Shader standard_shader;
    standard_shader.info_id = kNdStandardSurfaceSurfaceshader;
    standard_shader.value = standard_surface;

    UsdUVTexture opacity_texture;
    opacity_texture.outputsR.set_authored(true);
    opacity_texture.file.set_value(
        Animatable<value::AssetPath>(value::AssetPath("opacity.png")));
    Shader opacity_shader;
    opacity_shader.info_id = kUsdUVTexture;
    opacity_shader.value = opacity_texture;

    UsdUVTexture normal_texture;
    normal_texture.outputsRGB.set_authored(true);
    normal_texture.file.set_value(
        Animatable<value::AssetPath>(value::AssetPath("normal.png")));
    Shader normal_shader;
    normal_shader.info_id = kUsdUVTexture;
    normal_shader.value = normal_texture;

    UsdUVTexture coat_normal_texture;
    coat_normal_texture.outputsRGB.set_authored(true);
    coat_normal_texture.file.set_value(
        Animatable<value::AssetPath>(value::AssetPath("coat-normal.png")));
    Shader coat_normal_shader;
    coat_normal_shader.info_id = kUsdUVTexture;
    coat_normal_shader.value = coat_normal_texture;

    UsdUVTexture displacement_texture;
    displacement_texture.outputsR.set_authored(true);
    displacement_texture.file.set_value(
        Animatable<value::AssetPath>(value::AssetPath("height.png")));
    Shader displacement_shader;
    displacement_shader.info_id = kUsdUVTexture;
    displacement_shader.value = displacement_texture;

    Prim material_prim("MaterialPrim", material);
    TEST_CHECK(material_prim.add_child(Prim("StandardSurface", standard_shader)));
    TEST_CHECK(material_prim.add_child(Prim("OpacityTex", opacity_shader)));
    TEST_CHECK(material_prim.add_child(Prim("NormalTex", normal_shader)));
    TEST_CHECK(material_prim.add_child(
        Prim("CoatNormalTex", coat_normal_shader)));
    TEST_CHECK(material_prim.add_child(
        Prim("DisplacementTex", displacement_shader)));

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(std::move(material_prim)));

    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.load_texture_assets = false;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.materials.size() == 1);
    if (scene.materials.size() == 1) {
      TEST_CHECK(scene.materials[0].openPBRShader.has_value());
      if (scene.materials[0].openPBRShader.has_value()) {
        TEST_CHECK(scene.materials[0].openPBRShader->opacity.texture_id >= 0);
        TEST_CHECK(scene.materials[0].openPBRShader->normal.texture_id >= 0);
        TEST_CHECK(
            scene.materials[0].openPBRShader->coat_normal.texture_id >= 0);
        TEST_CHECK(
            scene.materials[0].openPBRShader->displacement.texture_id >= 0);
      }
    }
    TEST_CHECK(converter.GetError().empty());
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/PreviewSurface", "outputs:surface"));

    UsdPreviewSurface preview_surface;
    preview_surface.outputsSurface.set_authored(true);
    preview_surface.diffuseColor.set_connection(Path("/Tex", "outputs:rgb"));
    preview_surface.diffuseColor.set_value_empty();

    Shader preview_shader;
    preview_shader.info_id = kUsdPreviewSurface;
    preview_shader.value = preview_surface;

    UsdUVTexture uv_texture;
    uv_texture.outputsRGB.set_authored(true);
    uv_texture.file.set_value(
        Animatable<value::AssetPath>(value::AssetPath("dummy.png")));
    uv_texture.st.set_value(Animatable<value::texcoord2f>());

    Shader tex_shader;
    tex_shader.info_id = kUsdUVTexture;
    tex_shader.value = uv_texture;

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(Prim("MaterialPrim", material)));
    TEST_CHECK(stage.add_root_prim(Prim("PreviewSurface", preview_shader)));
    TEST_CHECK(stage.add_root_prim(Prim("Tex", tex_shader)));

    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.load_texture_assets = false;
    // Strict mode: 73330b10b demoted non-strict material failures to
    // warnings + default material; opt in to keep testing the error path.
    env.material_config.strict_material_check = true;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find(
                   "Failed to get fallback `st` texcoord attribute") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("/Tex") != std::string::npos);
  }

  {
    Stage stage = make_mtlx_openpbr_stage(
        Path("/MaterialPrim/NodeGraphs/Image", "outputs:out"));

    tydra::RenderSceneConverterEnv env(stage);
    // Strict mode: 73330b10b demoted non-strict material failures to
    // warnings + default material; opt in to keep testing the error path.
    env.material_config.strict_material_check = true;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().empty());
  }

  {
    Stage stage = make_mtlx_openpbr_stage(
        Path("/MaterialPrim/NodeGraphs/Missing", "outputs:out"));

    tydra::RenderSceneConverterEnv env(stage);
    // Strict mode: 73330b10b demoted non-strict material failures to
    // warnings + default material; opt in to keep testing the error path.
    env.material_config.strict_material_check = true;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find(
                   "Failed to find MaterialX texture for base_color") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("/MaterialPrim/NodeGraphs/Missing") !=
               std::string::npos);
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/MaterialPrim/OpenPBRShader",
                              "outputs:surface"));

    OpenPBRSurface openpbr_surface;
    openpbr_surface.surface.set_authored(true);
    openpbr_surface.base_roughness.set_connection(
        Path("/MaterialPrim/NodeGraphs", "outputs:rough"));
    openpbr_surface.base_roughness.set_value_empty();

    Shader openpbr_shader;
    openpbr_shader.info_id = kOpenPBRSurface;
    openpbr_shader.value = openpbr_surface;

    NodeGraph nodegraph;
    nodegraph.props["outputs:rough.connect"] = Property(
        Path("/MaterialPrim/NodeGraphs/Image", "outputs:g"),
        value::TypeTraits<float>::type_name());

    Shader image_shader;
    image_shader.info_id = "ND_image_color4";
    ShaderNode image_node;
    image_node.props["inputs:file"] =
        Property(Attribute::Uniform(value::AssetPath("packed.png")));
    {
      value::float2 uv_tiling;
      uv_tiling[0] = 2.0f;
      uv_tiling[1] = 3.0f;
      value::float2 uv_offset;
      uv_offset[0] = 0.25f;
      uv_offset[1] = 0.5f;
      image_node.props["inputs:uvtiling"] =
          Property(Attribute::Uniform(uv_tiling));
      image_node.props["inputs:uvoffset"] =
          Property(Attribute::Uniform(uv_offset));
      image_node.props["inputs:rotate"] =
          Property(Attribute::Uniform(15.0f));
      image_node.props["inputs:uaddressmode"] =
          Property(Attribute::Uniform(value::token("mirror")));
      image_node.props["inputs:vaddressmode"] =
          Property(Attribute::Uniform(value::token("periodic")));
    }
    image_shader.value = image_node;

    Prim nodegraph_prim("NodeGraphs", nodegraph);
    TEST_CHECK(nodegraph_prim.add_child(Prim("Image", image_shader)));

    Prim material_prim("MaterialPrim", material);
    TEST_CHECK(material_prim.add_child(Prim("OpenPBRShader", openpbr_shader)));
    TEST_CHECK(material_prim.add_child(std::move(nodegraph_prim)));

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(std::move(material_prim)));

    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.load_texture_assets = false;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.textures.size() == 1);
    if (scene.textures.size() == 1) {
      TEST_CHECK(scene.textures[0].connectedOutputChannel ==
                 tydra::UVTexture::Channel::G);
      TEST_CHECK(scene.textures[0].has_transform2d);
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_scale[0], 2.0f));
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_scale[1], 3.0f));
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_translation[0], 0.25f));
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_translation[1], 0.5f));
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_rotation, 15.0f));
      TEST_CHECK(scene.textures[0].wrapS ==
                 tydra::UVTexture::WrapMode::MIRROR);
      TEST_CHECK(scene.textures[0].wrapT ==
                 tydra::UVTexture::WrapMode::REPEAT);
    }
    TEST_CHECK(scene.materials.size() == 1);
    if (scene.materials.size() == 1) {
      TEST_CHECK(scene.materials[0].openPBRShader.has_value());
      if (scene.materials[0].openPBRShader.has_value()) {
        TEST_CHECK(scene.materials[0]
                       .openPBRShader->base_roughness.texture_id == 0);
      }
    }
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/MaterialPrim/OpenPBRShader",
                              "outputs:surface"));

    OpenPBRSurface openpbr_surface;
    openpbr_surface.surface.set_authored(true);
    openpbr_surface.base_color.set_connection(
        Path("/MaterialPrim/NodeGraphs", "outputs:baseColor"));
    openpbr_surface.base_color.set_value_empty();

    Shader openpbr_shader;
    openpbr_shader.info_id = kOpenPBRSurface;
    openpbr_shader.value = openpbr_surface;

    NodeGraph nodegraph;
    nodegraph.props["outputs:baseColor.connect"] = Property(
        Path("/MaterialPrim/NodeGraphs/Image", "outputs:out"),
        value::TypeTraits<value::color3f>::type_name());

    Shader image_shader;
    image_shader.info_id = "ND_image_color3";
    ShaderNode image_node;
    image_node.props["inputs:file"] =
        Property(Attribute::Uniform(value::AssetPath("basecolor.png")));
    image_node.props["inputs:texcoord"] = Property(
        Path("/MaterialPrim/NodeGraphs/Place2d", "outputs:out"),
        value::TypeTraits<value::float2>::type_name());
    image_shader.value = image_node;

    Shader place2d_shader;
    place2d_shader.info_id = "ND_place2d_vector2";
    ShaderNode place2d_node;
    place2d_node.props["inputs:texcoord"] = Property(
        Path("/MaterialPrim/NodeGraphs/GeomProp", "outputs:out"),
        value::TypeTraits<value::float2>::type_name());
    {
      value::float2 scale;
      scale[0] = 4.0f;
      scale[1] = 5.0f;
      value::float2 translation;
      translation[0] = 0.125f;
      translation[1] = 0.25f;
      place2d_node.props["inputs:scale"] =
          Property(Attribute::Uniform(scale));
      place2d_node.props["inputs:translation"] =
          Property(Attribute::Uniform(translation));
      place2d_node.props["inputs:rotation"] =
          Property(Attribute::Uniform(30.0f));
    }
    place2d_shader.value = place2d_node;

    Shader geomprop_shader;
    geomprop_shader.info_id = "ND_geompropvalue_vector2";
    ShaderNode geomprop_node;
    geomprop_node.props["inputs:geomprop"] =
        Property(Attribute::Uniform(value::token("st1")));
    geomprop_shader.value = geomprop_node;

    Prim nodegraph_prim("NodeGraphs", nodegraph);
    TEST_CHECK(nodegraph_prim.add_child(Prim("Image", image_shader)));
    TEST_CHECK(nodegraph_prim.add_child(Prim("Place2d", place2d_shader)));
    TEST_CHECK(nodegraph_prim.add_child(Prim("GeomProp", geomprop_shader)));

    Prim material_prim("MaterialPrim", material);
    TEST_CHECK(material_prim.add_child(Prim("OpenPBRShader", openpbr_shader)));
    TEST_CHECK(material_prim.add_child(std::move(nodegraph_prim)));

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(std::move(material_prim)));

    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.load_texture_assets = false;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.textures.size() == 1);
    if (scene.textures.size() == 1) {
      TEST_CHECK(scene.textures[0].varname_uv == "st1");
      TEST_CHECK(scene.textures[0].has_transform2d);
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_scale[0], 4.0f));
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_scale[1], 5.0f));
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_translation[0], 0.125f));
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_translation[1], 0.25f));
      TEST_CHECK(NearlyEqual(scene.textures[0].tx_rotation, 30.0f));
    }
    TEST_CHECK(scene.materials.size() == 1);
    if (scene.materials.size() == 1) {
      TEST_CHECK(scene.materials[0].openPBRShader.has_value());
      if (scene.materials[0].openPBRShader.has_value()) {
        TEST_CHECK(scene.materials[0]
                       .openPBRShader->base_color.texture_id == 0);
      }
    }
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/MaterialPrim/PreviewSurface",
                              "outputs:surface"));
    material.materialXConfig = MaterialXConfigAPI{};

    Relationship mtlx_surface_rel;
    mtlx_surface_rel.set(Path("/MaterialPrim/MissingMtlx", ""));
    material.props["outputs:mtlx:surface.connect"] = Property(
        mtlx_surface_rel);

    UsdPreviewSurface preview_surface;
    preview_surface.outputsSurface.set_authored(true);

    Shader preview_shader;
    preview_shader.info_id = kUsdPreviewSurface;
    preview_shader.value = preview_surface;

    Prim material_prim("MaterialPrim", material);
    TEST_CHECK(material_prim.add_child(Prim("PreviewSurface", preview_shader)));

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(std::move(material_prim)));

    tydra::RenderSceneConverterEnv env(stage);
    // Strict mode: 73330b10b demoted non-strict material failures to
    // warnings + default material; opt in to keep testing the error path.
    env.material_config.strict_material_check = true;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find("MaterialX shader path") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("/MaterialPrim/MissingMtlx") !=
               std::string::npos);
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/MaterialPrim/PreviewSurface",
                              "outputs:surface"));
    material.materialXConfig = MaterialXConfigAPI{};

    Relationship mtlx_surface_rel;
    mtlx_surface_rel.set(Path("/MaterialPrim/BadMtlx", ""));
    material.props["outputs:mtlx:surface.connect"] = Property(
        mtlx_surface_rel);

    UsdPreviewSurface preview_surface;
    preview_surface.outputsSurface.set_authored(true);

    Shader preview_shader;
    preview_shader.info_id = kUsdPreviewSurface;
    preview_shader.value = preview_surface;

    Shader bad_mtlx_shader;
    bad_mtlx_shader.info_id = kUsdPreviewSurface;
    bad_mtlx_shader.value = preview_surface;

    Prim material_prim("MaterialPrim", material);
    TEST_CHECK(material_prim.add_child(Prim("PreviewSurface", preview_shader)));
    TEST_CHECK(material_prim.add_child(Prim("BadMtlx", bad_mtlx_shader)));

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(std::move(material_prim)));

    tydra::RenderSceneConverterEnv env(stage);
    // Strict mode: 73330b10b demoted non-strict material failures to
    // warnings + default material; opt in to keep testing the error path.
    env.material_config.strict_material_check = true;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find(
                   "ND_open_pbr_surface_surfaceshader") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("/MaterialPrim/BadMtlx") !=
               std::string::npos);
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/MaterialPrim/PreviewSurface",
                              "outputs:surface"));
    material.materialXConfig = MaterialXConfigAPI{};

    Relationship mtlx_surface_rel;
    mtlx_surface_rel.set(Path("/MaterialPrim/MtlxSurface", ""));
    material.props["outputs:mtlx:surface.connect"] = Property(
        mtlx_surface_rel);

    UsdPreviewSurface preview_surface;
    preview_surface.outputsSurface.set_authored(true);

    Shader preview_shader;
    preview_shader.info_id = kUsdPreviewSurface;
    preview_shader.value = preview_surface;

    MtlxOpenPBRSurface mtlx_surface;
    mtlx_surface.surface.set_authored(true);
    mtlx_surface.base_color.set_connection(
        Path("/MaterialPrim/NodeGraphs", "outputs:out"));
    mtlx_surface.base_color.set_value_empty();

    Shader mtlx_surface_shader;
    mtlx_surface_shader.info_id = kNdOpenPbrSurfaceSurfaceshader;
    mtlx_surface_shader.value = mtlx_surface;

    NodeGraph nodegraph;
    nodegraph.props["outputs:out.connect"] = Property(
        Path("/MaterialPrim/NodeGraphs/Image", "outputs:out"),
        value::TypeTraits<value::color3f>::type_name());

    Shader image_shader;
    image_shader.info_id = "ND_image_color3";
    image_shader.value = ShaderNode{};

    Prim nodegraph_prim("NodeGraphs", nodegraph);
    TEST_CHECK(nodegraph_prim.add_child(Prim("Image", image_shader)));

    Prim material_prim("MaterialPrim", material);
    TEST_CHECK(material_prim.add_child(Prim("PreviewSurface", preview_shader)));
    TEST_CHECK(
        material_prim.add_child(Prim("MtlxSurface", mtlx_surface_shader)));
    TEST_CHECK(material_prim.add_child(std::move(nodegraph_prim)));

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(std::move(material_prim)));

    tydra::RenderSceneConverterEnv env(stage);
    // Strict mode: 73330b10b demoted non-strict material failures to
    // warnings + default material; opt in to keep testing the error path.
    env.material_config.strict_material_check = true;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().empty());
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/MaterialPrim/MtlxSurface",
                              "outputs:surface"));

    MtlxOpenPBRSurface mtlx_surface;
    mtlx_surface.surface.set_authored(true);
    mtlx_surface.base_color.set_connection(
        Path("/MaterialPrim/NodeGraphs", "outputs:out"));
    mtlx_surface.base_color.set_value_empty();

    Shader mtlx_surface_shader;
    mtlx_surface_shader.info_id = kNdOpenPbrSurfaceSurfaceshader;
    mtlx_surface_shader.value = mtlx_surface;

    NodeGraph nodegraph;
    value::color3f tint;
    tint[0] = 0.25f;
    tint[1] = 0.5f;
    tint[2] = 0.75f;
    nodegraph.props["inputs:tint"] = Property(Attribute::Uniform(tint));
    nodegraph.props["outputs:out.connect"] = Property(
        Path("/MaterialPrim/NodeGraphs/Multiply", "outputs:out"),
        value::TypeTraits<value::color3f>::type_name());

    Shader multiply_shader;
    multiply_shader.info_id = "ND_multiply_color3";
    ShaderNode multiply_node;
    multiply_node.props["inputs:in1"] = Property(
        Path("/MaterialPrim/NodeGraphs", "inputs:tint"),
        value::TypeTraits<value::color3f>::type_name());
    value::color3f scale;
    scale[0] = 2.0f;
    scale[1] = 0.5f;
    scale[2] = 0.25f;
    multiply_node.props["inputs:in2"] =
        Property(Attribute::Uniform(scale));
    multiply_shader.value = multiply_node;

    Prim nodegraph_prim("NodeGraphs", nodegraph);
    TEST_CHECK(nodegraph_prim.add_child(Prim("Multiply", multiply_shader)));

    Prim material_prim("MaterialPrim", material);
    TEST_CHECK(
        material_prim.add_child(Prim("MtlxSurface", mtlx_surface_shader)));
    TEST_CHECK(material_prim.add_child(std::move(nodegraph_prim)));

    TEST_CHECK(stage.add_root_prim(std::move(material_prim)));

    const Prim *shader_prim = nullptr;
    std::string find_err;
    TEST_CHECK(stage.find_prim_at_path(Path("/MaterialPrim/MtlxSurface", ""),
                                       shader_prim, &find_err));
    TEST_CHECK(shader_prim != nullptr);

    std::string json;
    std::string conv_err;
    TEST_CHECK(shader_prim &&
               tydra::ConvertShaderWithNodeGraphToJson(
                   *shader_prim, Path("/MaterialPrim/MtlxSurface", ""), stage,
                   &json, &conv_err));
    TEST_MSG("ConvertShaderWithNodeGraphToJson failed: %s",
             conv_err.c_str());
    TEST_CHECK(json.find("\"inputs\"") != std::string::npos);
    TEST_CHECK(json.find("\"name\": \"tint\"") != std::string::npos);
    TEST_CHECK(json.find("\"interfacename\": \"tint\"") !=
               std::string::npos);
    TEST_CHECK(json.find("\"input\": \"base_color\"") != std::string::npos);
  }

  {
    mtlx::MtlxNodeGraph dom_graph;
    dom_graph.SetName("NG_dom_interface");

    auto tint = std::make_shared<mtlx::MtlxInput>();
    tint->SetName("tint");
    tint->SetType("color3");
    tint->SetValue(mtlx::MtlxValue(std::vector<float>{0.25f, 0.5f, 0.75f}));
    dom_graph.AddInput(tint);

    auto multiply = std::make_shared<mtlx::MtlxNode>();
    multiply->SetName("mul");
    multiply->SetCategory("multiply");
    multiply->SetType("color3");
    auto in1 = std::make_shared<mtlx::MtlxInput>();
    in1->SetName("in1");
    in1->SetType("color3");
    in1->SetInterfaceName("tint");
    in1->SetChannels("rgb");
    multiply->AddInput(in1);
    auto in2 = std::make_shared<mtlx::MtlxInput>();
    in2->SetName("in2");
    in2->SetType("color3");
    in2->SetValue(mtlx::MtlxValue(std::vector<float>{2.0f, 0.5f, 0.25f}));
    multiply->AddInput(in2);
    dom_graph.AddNode(multiply);

    auto out = std::make_shared<mtlx::MtlxOutput>();
    out->SetName("out");
    out->SetType("color3");
    out->SetNodeName("mul");
    dom_graph.AddOutput(out);

    std::string json;
    std::string conv_err;
    TEST_CHECK(tydra::ConvertMtlxNodeGraphToJson(dom_graph, &json,
                                                 &conv_err));
    TEST_MSG("ConvertMtlxNodeGraphToJson failed: %s", conv_err.c_str());
    TEST_CHECK(json.find("\"name\": \"tint\"") != std::string::npos);
    TEST_CHECK(json.find("\"value\": [0.25, 0.5, 0.75]") !=
               std::string::npos);
    TEST_CHECK(json.find("\"interfacename\": \"tint\"") !=
               std::string::npos);
    TEST_CHECK(json.find("\"channels\": \"rgb\"") != std::string::npos);
    TEST_CHECK(json.find("\"nodename\": \"mul\"") != std::string::npos);
  }
}

void tydra_material_colorspace_token_test(void) {
  auto expect_colorspace = [](const char *token,
                              tydra::ColorSpace expected) {
    tydra::ColorSpace actual = tydra::ColorSpace::Unknown;
    TEST_CHECK(tydra::InferColorSpace(value::token(token), &actual));
    TEST_CHECK(actual == expected);
  };

  expect_colorspace("acescg", tydra::ColorSpace::Lin_ACEScg);
  expect_colorspace("lin_ap1", tydra::ColorSpace::Lin_ACEScg);
  expect_colorspace("lin_ap1_scene", tydra::ColorSpace::Lin_ACEScg);
  expect_colorspace("ACES - ACEScg", tydra::ColorSpace::Lin_ACEScg);
  expect_colorspace("aces2065-1", tydra::ColorSpace::ACES2065_1);
  expect_colorspace("lin_ap0_scene", tydra::ColorSpace::ACES2065_1);
  expect_colorspace("lin_rec2020", tydra::ColorSpace::Lin_Rec2020);
  expect_colorspace("lin_rec2020_scene", tydra::ColorSpace::Lin_Rec2020);
  expect_colorspace("srgb_rec709_scene", tydra::ColorSpace::sRGB);
  expect_colorspace("lin_rec709_scene", tydra::ColorSpace::Lin_Rec709);
  expect_colorspace("g22_rec709_scene", tydra::ColorSpace::g22_Rec709);
  expect_colorspace("g18_rec709_scene", tydra::ColorSpace::g18_Rec709);
  expect_colorspace("lin_displayp3", tydra::ColorSpace::Lin_DisplayP3);
  expect_colorspace("lin_p3d65_scene", tydra::ColorSpace::Lin_DisplayP3);
  expect_colorspace("srgb_displayp3", tydra::ColorSpace::sRGB_DisplayP3);
  expect_colorspace("srgb_p3d65_scene", tydra::ColorSpace::sRGB_DisplayP3);
  expect_colorspace("Input - Texture - sRGB - Display P3",
                    tydra::ColorSpace::sRGB_DisplayP3);

  tydra::ColorSpace actual = tydra::ColorSpace::Unknown;
  TEST_CHECK(!tydra::InferColorSpace(value::token("not_a_colorspace"),
                                     &actual));
  TEST_CHECK(actual == tydra::ColorSpace::Unknown);
}

void tydra_lightrt_opacity_mode_test(void) {
  using lightusd::tydra::PackRealtimePbrMaterial;
  using lightusd::tydra::RealtimePbrMaterial;
  constexpr int kFloats = lightusd::tydra::kLightRtOpenPBRFloats;

  // UsdPreviewSurface inputs:opacityMode="presence" keeps the packed coverage
  // flag clear so the RT shader may drop a hit; the default "transparent"
  // mode sets it so only the diffuse lobe is attenuated.
  RealtimePbrMaterial presence;
  presence.baseColor[0] = 0.25f;
  presence.baseColor[1] = 0.5f;
  presence.baseColor[2] = 0.75f;
  presence.specularRoughness = 0.4f;
  presence.diffuseRoughness = 0.2f;
  presence.opacityTransparent = false;
  std::vector<float> packed(size_t(kFloats), -1.0f);
  PackRealtimePbrMaterial(presence, true, 2.0f, 0.0f, packed.data());
  TEST_CHECK(packed[56] == 0.0f);
  TEST_CHECK(packed[0] == 0.25f);
  TEST_CHECK(packed[42] == 0.4f);
  TEST_CHECK(packed[41] == 0.2f);  // diffuseRoughness keeps its own slot

  RealtimePbrMaterial transparent = presence;
  transparent.opacityTransparent = true;
  PackRealtimePbrMaterial(transparent, true, 2.0f, 0.0f, packed.data());
  TEST_CHECK(packed[56] == 1.0f);
  TEST_CHECK(packed[42] == 0.4f);
  TEST_CHECK(packed[0] == 0.25f);
}

void tydra_progress_cancellation_test(void) {
  Stage stage;

  GeomMesh mesh;
  mesh.points = Animatable<std::vector<value::point3f>>(
      std::vector<value::point3f>{{0.0f, 0.0f, 0.0f},
                                  {1.0f, 0.0f, 0.0f},
                                  {0.0f, 1.0f, 0.0f}});
  mesh.faceVertexCounts = Animatable<std::vector<int32_t>>(
      std::vector<int32_t>{3});
  mesh.faceVertexIndices = Animatable<std::vector<int32_t>>(
      std::vector<int32_t>{0, 1, 2});

  Prim mesh_prim("MeshPrim", mesh);
  TEST_CHECK(stage.add_root_prim(std::move(mesh_prim)));

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;
  CancelOnMeshProgressState state;

  converter.SetDetailedProgressCallback(CancelOnFirstMeshProgress, &state);

  TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
  TEST_CHECK(state.mesh_progress_calls == 1);
  TEST_CHECK(converter.GetError().find("Conversion cancelled by user") !=
             std::string::npos);
}

void tydra_default_material_assignment_test(void) {
  auto make_mesh = []() {
    GeomMesh mesh;
    mesh.points = Animatable<std::vector<value::point3f>>(
        std::vector<value::point3f>{{0.0f, 0.0f, 0.0f},
                                    {1.0f, 0.0f, 0.0f},
                                    {0.0f, 1.0f, 0.0f}});
    mesh.faceVertexCounts = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{3});
    mesh.faceVertexIndices = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{0, 1, 2});
    return mesh;
  };

  auto make_stage = [&]() {
    Stage stage;

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", make_mesh())));
    return stage;
  };

  {
    Stage stage = make_stage();
    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.meshes.size() == 1);
    if (scene.meshes.size() == 1) {
      TEST_CHECK(scene.meshes[0].material_id < 0);
    }
    TEST_CHECK(scene.materials.empty());
  }

  {
    Stage stage = make_stage();
    tydra::RenderSceneConverterEnv env(stage);
    env.material_config.assign_default_material = true;
    env.material_config.default_material_name = "FallbackPreview";
    env.material_config.default_material_diffuse_color =
        tydra::vec3{0.25f, 0.5f, 0.75f};

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.meshes.size() == 1);
    TEST_CHECK(scene.materials.size() == 1);
    if (scene.meshes.size() == 1 && scene.materials.size() == 1) {
      TEST_CHECK(scene.meshes[0].material_id == 0);
      TEST_CHECK(scene.materials[0].name == "FallbackPreview");
      TEST_CHECK(scene.materials[0].surfaceShader.has_value());
      if (scene.materials[0].surfaceShader) {
        const auto &color = scene.materials[0].surfaceShader->diffuseColor.value;
        TEST_CHECK(color[0] == 0.25f);
        TEST_CHECK(color[1] == 0.5f);
        TEST_CHECK(color[2] == 0.75f);
      }
    }
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/PreviewSurface", "outputs:surface"));

    UsdPreviewSurface preview_surface;
    preview_surface.outputsSurface.set_authored(true);
    preview_surface.diffuseColor.set_connection(Path("/Tex", "outputs:rgb"));
    preview_surface.diffuseColor.set_value_empty();

    Shader preview_shader;
    preview_shader.info_id = kUsdPreviewSurface;
    preview_shader.value = preview_surface;

    UsdUVTexture uv_texture;
    uv_texture.outputsRGB.set_authored(true);

    Shader tex_shader;
    tex_shader.info_id = kUsdUVTexture;
    tex_shader.value = uv_texture;

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(Prim("MaterialPrim", material)));
    TEST_CHECK(stage.add_root_prim(Prim("PreviewSurface", preview_shader)));
    TEST_CHECK(stage.add_root_prim(Prim("Tex", tex_shader)));

    tydra::RenderSceneConverterEnv env(stage);
    env.material_config.assign_default_material = true;
    env.material_config.allow_missing_asset = false;

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().empty());
    TEST_CHECK(converter.GetWarning().find("Material conversion failed") !=
               std::string::npos);
    TEST_CHECK(scene.meshes.size() == 1);
    TEST_CHECK(scene.materials.size() == 1);
    if (scene.meshes.size() == 1) {
      TEST_CHECK(scene.meshes[0].material_id == 0);
    }
  }
}

void tydra_texture_loader_policy_test(void) {
  auto make_mesh = []() {
    GeomMesh mesh;
    mesh.points = Animatable<std::vector<value::point3f>>(
        std::vector<value::point3f>{{0.0f, 0.0f, 0.0f},
                                    {1.0f, 0.0f, 0.0f},
                                    {0.0f, 1.0f, 0.0f}});
    mesh.faceVertexCounts = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{3});
    mesh.faceVertexIndices = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{0, 1, 2});
    return mesh;
  };

  auto make_stage_with_texture = [&](const UsdUVTexture &uv_texture) {
    Stage stage;

    Material material;
    material.surface.set(Path("/PreviewSurface", "outputs:surface"));

    UsdPreviewSurface preview_surface;
    preview_surface.outputsSurface.set_authored(true);
    preview_surface.diffuseColor.set_connection(Path("/Tex", "outputs:rgb"));
    preview_surface.diffuseColor.set_value_empty();

    Shader preview_shader;
    preview_shader.info_id = kUsdPreviewSurface;
    preview_shader.value = preview_surface;

    Shader tex_shader;
    tex_shader.info_id = kUsdUVTexture;
    tex_shader.value = uv_texture;

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(Prim("MaterialPrim", material)));
    TEST_CHECK(stage.add_root_prim(Prim("PreviewSurface", preview_shader)));
    TEST_CHECK(stage.add_root_prim(Prim("Tex", tex_shader)));

    return stage;
  };

  {
    UsdUVTexture uv_texture;
    uv_texture.outputsRGB.set_authored(true);
    uv_texture.file.set_value(
        Animatable<value::AssetPath>(value::AssetPath("missing.png")));

    Stage stage = make_stage_with_texture(uv_texture);

    tydra::RenderSceneConverterEnv env(stage);
    env.material_config.texture_image_loader_function =
        FailingTextureImageLoader;
    env.material_config.allow_texture_load_failure = true;

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.images.size() == 1);
    if (scene.images.size() == 1) {
      TEST_CHECK(scene.images[0].asset_identifier == "missing.png");
      TEST_CHECK(!scene.images[0].decoded);
    }
    TEST_CHECK(converter.GetWarning().find("missing.png") !=
               std::string::npos);
    TEST_CHECK(converter.GetWarning().find("synthetic texture loader failure") !=
               std::string::npos);
  }

  {
    UsdUVTexture uv_texture;
    uv_texture.outputsRGB.set_authored(true);
    uv_texture.file.set_value(
        Animatable<value::AssetPath>(value::AssetPath("missing.png")));

    Stage stage = make_stage_with_texture(uv_texture);

    tydra::RenderSceneConverterEnv env(stage);
    env.material_config.texture_image_loader_function =
        FailingTextureImageLoader;
    env.material_config.allow_texture_load_failure = false;
    env.material_config.strict_material_check = true;

    // Strict mode: 73330b10b demoted non-strict material failures to
    // warnings + default material; opt in to keep testing the error path.
    env.material_config.strict_material_check = true;
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find("missing.png") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("synthetic texture loader failure") !=
               std::string::npos);
  }

  {
    UsdUVTexture uv_texture;
    uv_texture.outputsRGB.set_authored(true);
    uv_texture.file.set_value(
        Animatable<value::AssetPath>(value::AssetPath("mask.png")));
    uv_texture.sourceColorSpace.set_value(
        Animatable<UsdUVTexture::SourceColorSpace>(
            UsdUVTexture::SourceColorSpace::Auto));

    Stage stage = make_stage_with_texture(uv_texture);

    tydra::RenderSceneConverterEnv env(stage);
    env.material_config.texture_image_loader_function =
        SingleChannelTextureImageLoader;
    env.material_config.allow_texture_load_failure = false;

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.images.size() == 1);
    if (scene.images.size() == 1) {
      TEST_CHECK(scene.images[0].decoded);
      TEST_CHECK(scene.images[0].usdColorSpace == tydra::ColorSpace::Raw);
      TEST_CHECK(scene.images[0].asset_identifier == "mask.png");
    }
    TEST_CHECK(converter.GetWarning().find("Infer colorSpace failed") ==
               std::string::npos);
  }

  {
    Stage stage;

    Material material;
    material.surface.set(Path("/PreviewSurface", "outputs:surface"));

    UsdPreviewSurface preview_surface;
    preview_surface.outputsSurface.set_authored(true);
    preview_surface.diffuseColor.set_connection(Path("/Tex", "outputs:rgb"));
    preview_surface.diffuseColor.set_value_empty();

    Attribute source_color_space_attr;
    source_color_space_attr.set_type_name(value::kToken);
    source_color_space_attr.variability() = Variability::Uniform;
    primvar::PrimVar source_color_space_pvar;
    source_color_space_pvar.set_value(value::token("raw"));
    source_color_space_attr.set_var(std::move(source_color_space_pvar));
    preview_surface.props["inputs:sourceColorSpace"] =
        Property(std::move(source_color_space_attr), false);

    Shader preview_shader;
    preview_shader.info_id = kUsdPreviewSurface;
    preview_shader.value = preview_surface;

    UsdUVTexture uv_texture;
    uv_texture.outputsRGB.set_authored(true);
    uv_texture.file.set_value(
        Animatable<value::AssetPath>(value::AssetPath("mask.png")));
    uv_texture.sourceColorSpace.set_connection(
        Path("/PreviewSurface", "inputs:sourceColorSpace"));
    uv_texture.sourceColorSpace.set_value_empty();

    Shader tex_shader;
    tex_shader.info_id = kUsdUVTexture;
    tex_shader.value = uv_texture;

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(Prim("MaterialPrim", material)));
    TEST_CHECK(stage.add_root_prim(Prim("PreviewSurface", preview_shader)));
    TEST_CHECK(stage.add_root_prim(Prim("Tex", tex_shader)));

    tydra::RenderSceneConverterEnv env(stage);
    env.material_config.texture_image_loader_function =
        SingleChannelTextureImageLoader;
    env.material_config.allow_texture_load_failure = false;

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.images.size() == 1);
    if (scene.images.size() == 1) {
      TEST_CHECK(scene.images[0].decoded);
      TEST_CHECK(scene.images[0].usdColorSpace == tydra::ColorSpace::Raw);
      TEST_CHECK(scene.images[0].asset_identifier == "mask.png");
    }
    TEST_CHECK(converter.GetWarning().find("inputs:sourceColorSpace") ==
               std::string::npos);
  }

  {
    Stage stage;

    auto make_uniform_property = [](const value::Value &val,
                                    const std::string &type_name) {
      Attribute attr;
      attr.set_type_name(type_name);
      attr.variability() = Variability::Uniform;
      primvar::PrimVar pvar;
      pvar.set_value(val);
      attr.set_var(std::move(pvar));
      return Property(std::move(attr), false);
    };

    Material material;
    material.surface.set(Path("/PreviewSurface", "outputs:surface"));

    UsdPreviewSurface preview_surface;
    preview_surface.outputsSurface.set_authored(true);
    preview_surface.diffuseColor.set_connection(Path("/Tex", "outputs:rgb"));
    preview_surface.diffuseColor.set_value_empty();
    preview_surface.useSpecularWorkflow.set_connection(
        Path("/PreviewSurface", "inputs:useSpecularWorkflowDriver"));
    preview_surface.useSpecularWorkflow.set_value_empty();
    preview_surface.props["inputs:sourceColorSpace"] = make_uniform_property(
        value::Value(value::token("raw")), value::kToken);
    preview_surface.props["inputs:fileDriver"] = make_uniform_property(
        value::Value(value::AssetPath("mask.png")),
        value::TypeTraits<value::AssetPath>::type_name());
    preview_surface.props["inputs:wrapSDriver"] = make_uniform_property(
        value::Value(value::token("mirror")), value::kToken);
    preview_surface.props["inputs:wrapTDriver"] = make_uniform_property(
        value::Value(value::token("repeat")), value::kToken);
    preview_surface.props["inputs:useSpecularWorkflowDriver"] =
        make_uniform_property(value::Value(int32_t(1)),
                              value::TypeTraits<int>::type_name());

    Shader preview_shader;
    preview_shader.info_id = kUsdPreviewSurface;
    preview_shader.value = preview_surface;

    UsdUVTexture uv_texture;
    uv_texture.outputsRGB.set_authored(true);
    uv_texture.file.set_connection(Path("/PreviewSurface", "inputs:fileDriver"));
    uv_texture.sourceColorSpace.set_connection(
        Path("/PreviewSurface", "inputs:sourceColorSpace"));
    uv_texture.sourceColorSpace.set_value_empty();
    uv_texture.wrapS.set_connection(Path("/PreviewSurface", "inputs:wrapSDriver"));
    uv_texture.wrapS.set_value_empty();
    uv_texture.wrapT.set_connection(Path("/PreviewSurface", "inputs:wrapTDriver"));
    uv_texture.wrapT.set_value_empty();

    Shader tex_shader;
    tex_shader.info_id = kUsdUVTexture;
    tex_shader.value = uv_texture;

    GeomMesh mesh = make_mesh();
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(Prim("MaterialPrim", material)));
    TEST_CHECK(stage.add_root_prim(Prim("PreviewSurface", preview_shader)));
    TEST_CHECK(stage.add_root_prim(Prim("Tex", tex_shader)));

    tydra::RenderSceneConverterEnv env(stage);
    env.material_config.texture_image_loader_function =
        SingleChannelTextureImageLoader;
    env.material_config.allow_texture_load_failure = false;

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.materials.size() == 1);
    TEST_CHECK(scene.textures.size() == 1);
    TEST_CHECK(scene.images.size() == 1);
    if (scene.materials.size() == 1) {
      TEST_CHECK(scene.materials[0].surfaceShader.has_value());
      if (scene.materials[0].surfaceShader.has_value()) {
        TEST_CHECK(scene.materials[0].surfaceShader->useSpecularWorkflow);
      }
    }
    if (scene.textures.size() == 1) {
      TEST_CHECK(scene.textures[0].wrapS == tydra::UVTexture::WrapMode::MIRROR);
      TEST_CHECK(scene.textures[0].wrapT == tydra::UVTexture::WrapMode::REPEAT);
    }
    if (scene.images.size() == 1) {
      TEST_CHECK(scene.images[0].asset_identifier == "mask.png");
      TEST_CHECK(scene.images[0].usdColorSpace == tydra::ColorSpace::Raw);
    }
    TEST_CHECK(converter.GetWarning().find("Failed to resolve") ==
               std::string::npos);
  }
}

void tydra_udim_texture_test(void) {
  // --- Set up a temp directory with 3 UDIM tiles: 1001, 1002, 1011 ---
  // Tiles: 1001 -> (u0,v0), 1002 -> (u1,v0), 1011 -> (u0,v1).
  // Grid bounds => cols=2, rows=2.
  const std::string tmpdir =
      lightusd::io::JoinPath(lightusd::io::GetTempDir(), "lightusd_udim_test");
  lightusd::io::CreateDirectories(tmpdir);

  auto make_tile = [](uint8_t r, uint8_t g, uint8_t b) {
    Image img;
    img.width = 8;
    img.height = 8;
    img.channels = 4;
    img.bpp = 8;
    img.format = Image::PixelFormat::UInt;
    img.data.resize(8 * 8 * 4);
    for (size_t i = 0; i < 8 * 8; i++) {
      img.data[i * 4 + 0] = r;
      img.data[i * 4 + 1] = g;
      img.data[i * 4 + 2] = b;
      img.data[i * 4 + 3] = 255;
    }
    return img;
  };

  struct TileSpec { uint32_t id; uint8_t r, g, b; };
  const std::vector<TileSpec> tile_specs = {
      {1001, 255, 0, 0}, {1002, 0, 255, 0}, {1011, 0, 0, 255}};

  std::vector<std::string> tile_files;
  bool wrote_all = true;
  for (const auto &ts : tile_specs) {
    Image img = make_tile(ts.r, ts.g, ts.b);
    const std::string path =
        lightusd::io::JoinPath(tmpdir, "tile." + std::to_string(ts.id) + ".png");
    auto wret = image::WriteImageToFile(path, img);
    if (!wret) {
      wrote_all = false;
      break;
    }
    tile_files.push_back(path);
  }
  TEST_CHECK(wrote_all);
  if (!wrote_all) {
    return;
  }

  const std::string udim_asset = "tile.<UDIM>.png";

  // --- Direct helper test: ExpandUDIMTiles ---
  {
    AssetResolutionResolver resolver;
    resolver.set_search_paths({tmpdir});

    std::vector<tydra::UDIMTile> tiles;
    std::string warn, err;
    bool ok = tydra::ExpandUDIMTiles(udim_asset, resolver, /*max_tiles*/ 100,
                                     &tiles, &warn, &err);
    TEST_CHECK(ok);
    TEST_CHECK(tiles.size() == 3);
    if (tiles.size() == 3) {
      TEST_CHECK(tiles[0].udim_id == 1001);
      TEST_CHECK(tiles[0].u == 0 && tiles[0].v == 0);
      TEST_CHECK(tiles[1].udim_id == 1002);
      TEST_CHECK(tiles[1].u == 1 && tiles[1].v == 0);
      TEST_CHECK(tiles[2].udim_id == 1011);
      TEST_CHECK(tiles[2].u == 0 && tiles[2].v == 1);
    }

    // --- Direct helper test: BuildUDIMAtlas ---
    if (tiles.size() == 3) {
      tydra::UDIMAtlas atlas;
      std::string awarn, aerr;
      bool aok = tydra::BuildUDIMAtlas(tiles, resolver, /*max_atlas_size*/ 256,
                                       /*srgb*/ true, &atlas, &awarn, &aerr);
      TEST_CHECK(aok);
      if (aok) {
        TEST_CHECK(atlas.cols == 2);
        TEST_CHECK(atlas.rows == 2);
        TEST_CHECK(NearlyEqual(atlas.uv_scale[0], 0.5f));
        TEST_CHECK(NearlyEqual(atlas.uv_scale[1], 0.5f));
        TEST_CHECK(NearlyEqual(atlas.uv_offset[0], 0.0f));
        TEST_CHECK(NearlyEqual(atlas.uv_offset[1], 0.0f));
        // per-tile cell = floor_pow2(256/2) = 128 -> atlas 256x256.
        TEST_CHECK(atlas.image.width == 256);
        TEST_CHECK(atlas.image.height == 256);
        TEST_CHECK(atlas.image.channels == 4);
        TEST_CHECK(atlas.image.width <= 256 && atlas.image.height <= 256);
      }
    }
  }

  // --- Pipeline test helper: stage with a UDIM diffuse texture ---
  auto make_stage = [&]() {
    Stage stage;

    GeomMesh mesh;
    mesh.points = Animatable<std::vector<value::point3f>>(
        std::vector<value::point3f>{{0.0f, 0.0f, 0.0f},
                                    {1.0f, 0.0f, 0.0f},
                                    {0.0f, 1.0f, 0.0f}});
    mesh.faceVertexCounts = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{3});
    mesh.faceVertexIndices = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{0, 1, 2});
    Relationship material_rel;
    material_rel.set(Path("/MaterialPrim", ""));
    mesh.set_materialBinding(material_rel);

    Material material;
    material.surface.set(Path("/PreviewSurface", "outputs:surface"));

    UsdPreviewSurface preview_surface;
    preview_surface.outputsSurface.set_authored(true);
    preview_surface.diffuseColor.set_connection(Path("/Tex", "outputs:rgb"));
    preview_surface.diffuseColor.set_value_empty();

    Shader preview_shader;
    preview_shader.info_id = kUsdPreviewSurface;
    preview_shader.value = preview_surface;

    UsdUVTexture uv_texture;
    uv_texture.outputsRGB.set_authored(true);
    uv_texture.file.set_value(
        Animatable<value::AssetPath>(value::AssetPath(udim_asset)));

    Shader tex_shader;
    tex_shader.info_id = kUsdUVTexture;
    tex_shader.value = uv_texture;

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));
    TEST_CHECK(stage.add_root_prim(Prim("MaterialPrim", material)));
    TEST_CHECK(stage.add_root_prim(Prim("PreviewSurface", preview_shader)));
    TEST_CHECK(stage.add_root_prim(Prim("Tex", tex_shader)));

    return stage;
  };

  // --- Combine mode (default): single atlas image + remap on the texture ---
  {
    Stage stage = make_stage();
    tydra::RenderSceneConverterEnv env(stage);
    env.set_search_paths({tmpdir});
    env.material_config.combine_udim_tiles = true;
    env.material_config.udim_max_atlas_size = 256;

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;
    bool ok = converter.ConvertToRenderScene(env, &scene);
    TEST_CHECK(ok);
    TEST_CHECK(scene.textures.size() == 1);
    TEST_CHECK(scene.images.size() == 1);  // one combined atlas
    TEST_CHECK(scene.udim_textures.empty());
    if (scene.textures.size() == 1) {
      TEST_CHECK(scene.textures[0].is_udim);
      TEST_CHECK(scene.textures[0].udim_texture_id < 0);
      TEST_CHECK(NearlyEqual(scene.textures[0].udim_uv_scale[0], 0.5f));
      TEST_CHECK(NearlyEqual(scene.textures[0].udim_uv_scale[1], 0.5f));
    }
  }

  // --- Keep-as-is mode: sparse UDIMTexture with per-tile images ---
  {
    Stage stage = make_stage();
    tydra::RenderSceneConverterEnv env(stage);
    env.set_search_paths({tmpdir});
    env.material_config.combine_udim_tiles = false;

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;
    bool ok = converter.ConvertToRenderScene(env, &scene);
    TEST_CHECK(ok);
    TEST_CHECK(scene.textures.size() == 1);
    TEST_CHECK(scene.udim_textures.size() == 1);
    TEST_CHECK(scene.images.size() == 3);  // 3 separate tiles
    if (scene.udim_textures.size() == 1) {
      const auto &udim = scene.udim_textures[0];
      TEST_CHECK(udim.imageTileIds.size() == 3);
      TEST_CHECK(udim.imageTileIds.count(1001) == 1);
      TEST_CHECK(udim.imageTileIds.count(1002) == 1);
      TEST_CHECK(udim.imageTileIds.count(1011) == 1);
      TEST_CHECK(udim.asset_identifier == udim_asset);
    }
    if (scene.textures.size() == 1) {
      TEST_CHECK(scene.textures[0].is_udim);
      TEST_CHECK(scene.textures[0].udim_texture_id == 0);
    }
  }

  CleanupTempFiles(tile_files);
  lightusd::io::RemoveAll(tmpdir);
}

void tydra_envmap_loader_policy_test(void) {
  auto make_stage_with_dome = []() {
    Stage stage;
    DomeLight light;
    light.name = "DomeLightPrim";
    light.file.set_value(
        Animatable<value::AssetPath>(value::AssetPath("missing.exr")));
    TEST_CHECK(stage.add_root_prim(Prim("DomeLightPrim", light)));
    return stage;
  };

  {
    Stage stage = make_stage_with_dome();

    tydra::RenderSceneConverterEnv env(stage);
    env.material_config.texture_image_loader_function =
        FailingTextureImageLoader;
    env.material_config.allow_texture_load_failure = true;

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.lights.size() == 1);
    if (scene.lights.size() == 1) {
      TEST_CHECK(scene.lights[0].type == tydra::RenderLight::Type::Dome);
      TEST_CHECK(scene.lights[0].envmap_texture_id == -1);
      TEST_CHECK(scene.lights[0].textureFile == "missing.exr");
    }
    TEST_CHECK(converter.GetWarning().find("missing.exr") !=
               std::string::npos);
    TEST_CHECK(converter.GetWarning().find(
                   "synthetic texture loader failure") !=
               std::string::npos);
  }

  {
    Stage stage = make_stage_with_dome();

    tydra::RenderSceneConverterEnv env(stage);
    env.material_config.texture_image_loader_function =
        FailingTextureImageLoader;
    env.material_config.allow_texture_load_failure = false;

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find("missing.exr") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find(
                   "Failed to load envmap texture") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find(
                   "synthetic texture loader failure") !=
               std::string::npos);
  }
}

void tydra_light_shaping_ies_conversion_test(void) {
  Stage stage;
  RectLight light;
  light.name = "Key";
  light.shapingIesFile.set_value(
      Animatable<value::AssetPath>(value::AssetPath("profiles/key.ies")));
  light.shapingIesAngleScale.set_value(Animatable<float>(1.25f));
  light.shapingIesNormalize.set_value(Animatable<bool>(true));
  TEST_CHECK(stage.add_root_prim(Prim("Key", light)));

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;
  TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  TEST_CHECK(scene.lights.size() == 1);
  if (scene.lights.size() == 1) {
    const tydra::RenderLight &converted = scene.lights[0];
    TEST_CHECK(converted.type == tydra::RenderLight::Type::Rect);
    TEST_CHECK(converted.shapingIesFile == "profiles/key.ies");
    TEST_CHECK(NearlyEqual(converted.shapingIesAngleScale, 1.25f));
    TEST_CHECK(converted.shapingIesNormalize);
  }
}

void tydra_light_spectral_emission_conversion_test(void) {
  Stage stage;
  SphereLight light;
  light.name = "SpectralKey";
  light.spectralEmission.set_value(std::vector<value::float2>{
      value::float2{450.0f, 0.25f},
      value::float2{550.0f, 1.0f},
      value::float2{650.0f, 0.5f},
  });
  TEST_CHECK(stage.add_root_prim(Prim("SpectralKey", light)));

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;
  TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  TEST_CHECK(scene.lights.size() == 1);
  if (scene.lights.size() == 1) {
    const tydra::RenderLight &converted = scene.lights[0];
    TEST_CHECK(converted.type == tydra::RenderLight::Type::Sphere);
    TEST_CHECK(converted.hasSpectralEmission());
    TEST_CHECK(converted.spd_emission.has_value());
    if (converted.spd_emission.has_value()) {
      TEST_CHECK(converted.spd_emission->samples.size() == 3);
      TEST_CHECK(NearlyEqual(converted.spd_emission->samples[1][0], 550.0f));
      TEST_CHECK(NearlyEqual(converted.spd_emission->samples[1][1], 1.0f));
    }
  }
}

void tydra_geometry_light_validation_test(void) {
  auto make_mesh = []() {
    GeomMesh mesh;
    mesh.points = Animatable<std::vector<value::point3f>>(
        std::vector<value::point3f>{{0.0f, 0.0f, 0.0f},
                                    {1.0f, 0.0f, 0.0f},
                                    {0.0f, 1.0f, 0.0f}});
    mesh.faceVertexCounts = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{3});
    mesh.faceVertexIndices = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{0, 1, 2});
    return mesh;
  };

  {
    Stage stage;
    GeometryLight light;
    light.name = "GeomLight";

    TEST_CHECK(stage.add_root_prim(Prim("GeomLight", light)));

    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find("missing geometry relationship") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("/GeomLight") != std::string::npos);
  }

  {
    Stage stage;
    GeometryLight light;
    light.name = "GeomLight";

    Relationship empty_geometry_rel;
    empty_geometry_rel.set(std::vector<Path>{});
    light.geometry = empty_geometry_rel;

    TEST_CHECK(stage.add_root_prim(Prim("GeomLight", light)));

    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find(
                   "must have exactly one geometry target") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("/GeomLight") != std::string::npos);
  }

  {
    Stage stage;
    GeometryLight light;
    light.name = "GeomLight";

    Relationship multi_geometry_rel;
    multi_geometry_rel.set(
        std::vector<Path>{Path("/MeshA", ""), Path("/MeshB", "")});
    light.geometry = multi_geometry_rel;

    TEST_CHECK(stage.add_root_prim(Prim("GeomLight", light)));

    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find(
                   "must have exactly one geometry target") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("/GeomLight") != std::string::npos);
  }

  {
    Stage stage;
    GeometryLight light;
    light.name = "GeomLight";

    Relationship missing_geometry_rel;
    missing_geometry_rel.set(Path("/MissingMesh", ""));
    light.geometry = missing_geometry_rel;

    TEST_CHECK(stage.add_root_prim(Prim("GeomLight", light)));

    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find("references missing geometry target") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("/MissingMesh") !=
               std::string::npos);
  }

  {
    Stage stage;
    GeometryLight light;
    light.name = "GeomLight";

    Relationship geometry_rel;
    geometry_rel.set(Path("/MeshPrim", ""));
    light.geometry = geometry_rel;

    TEST_CHECK(stage.add_root_prim(Prim("GeomLight", light)));
    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", make_mesh())));

    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.lights.size() == 1);
    if (scene.lights.size() == 1) {
      TEST_CHECK(scene.lights[0].type == tydra::RenderLight::Type::Geometry);
      TEST_CHECK(scene.lights[0].geometry_mesh_id == -1);
    }
  }
}

void tydra_mesh_fallback_policy_test(void) {
  {
    tydra::RenderSceneConverter converter;

    tydra::RenderMesh dst;
    dst.abs_path = "/MeshA";
    dst.points = std::vector<tydra::vec3>{{0.0f, 0.0f, 0.0f}};
    dst.usdFaceVertexIndices = std::vector<uint32_t>{0};
    dst.usdFaceVertexCounts = std::vector<uint32_t>{1};

    tydra::VertexAttribute dst_tc;
    dst_tc.format = tydra::VertexAttributeFormat::Vec2;
    dst_tc.data.resize(sizeof(float) * 2);
    dst.texcoords.emplace(0, dst_tc);

    tydra::RenderMesh src;
    src.abs_path = "/MeshB";
    src.points = std::vector<tydra::vec3>{{1.0f, 0.0f, 0.0f}};
    src.usdFaceVertexIndices = std::vector<uint32_t>{0};
    src.usdFaceVertexCounts = std::vector<uint32_t>{1};

    tydra::VertexAttribute src_tc;
    src_tc.format = tydra::VertexAttributeFormat::Vec3;
    src_tc.data.resize(sizeof(float) * 3);
    src.texcoords.emplace(0, src_tc);

    std::string merge_err;
    TEST_CHECK(!converter.MergeMeshData(
        src, value::matrix4d::identity(), dst, &merge_err));
    TEST_CHECK(merge_err.find("Cannot merge texcoords slot 0") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().empty());
  }

  {
    tydra::RenderSceneConverter converter;

    const std::array<tydra::vec3, 3> normals = {
        tydra::vec3{0.70710677f, 0.70710677f, 0.0f},
        tydra::vec3{0.70710677f, 0.70710677f, 0.0f},
        tydra::vec3{0.70710677f, 0.70710677f, 0.0f},
    };

    tydra::RenderMesh src;
    src.abs_path = "/MeshNormal";
    src.points = {{0.0f, 0.0f, 0.0f},
                  {1.0f, 0.0f, 0.0f},
                  {0.0f, 1.0f, 0.0f}};
    src.usdFaceVertexIndices = {0, 1, 2};
    src.usdFaceVertexCounts = {3};
    src.normals.format = tydra::VertexAttributeFormat::Vec3;
    src.normals.set_buffer(reinterpret_cast<const uint8_t *>(normals.data()),
                           normals.size() * sizeof(tydra::vec3));
    src.normals.variability = tydra::VertexVariability::Vertex;

    value::matrix4d scale = value::matrix4d::identity();
    scale.m[0][0] = 2.0;

    tydra::RenderMesh dst;
    std::string merge_err;
    TEST_CHECK(converter.MergeMeshData(src, scale, dst, &merge_err));
    TEST_CHECK(merge_err.empty());
    TEST_CHECK(dst.normals.vertex_count() == 3);
    const tydra::vec3 *baked =
        reinterpret_cast<const tydra::vec3 *>(dst.normals.data.data());
    TEST_CHECK(std::fabs((*baked)[0] - 0.4472136f) < 1.0e-5f);
    TEST_CHECK(std::fabs((*baked)[1] - 0.8944272f) < 1.0e-5f);
    TEST_CHECK(std::fabs((*baked)[2]) < 1.0e-5f);
  }

  {
    tydra::RenderSceneConverter converter;

    const std::array<tydra::vec3, 3> normals = {
        tydra::vec3{0.0f, 0.0f, 1.0f},
        tydra::vec3{0.0f, 0.0f, 1.0f},
        tydra::vec3{0.0f, 0.0f, 1.0f},
    };

    tydra::RenderMesh src;
    src.abs_path = "/MeshSingular";
    src.points = {{0.0f, 0.0f, 0.0f},
                  {1.0f, 0.0f, 0.0f},
                  {0.0f, 1.0f, 0.0f}};
    src.usdFaceVertexIndices = {0, 1, 2};
    src.usdFaceVertexCounts = {3};
    src.normals.format = tydra::VertexAttributeFormat::Vec3;
    src.normals.set_buffer(reinterpret_cast<const uint8_t *>(normals.data()),
                           normals.size() * sizeof(tydra::vec3));
    src.normals.variability = tydra::VertexVariability::Vertex;

    value::matrix4d singular = value::matrix4d::identity();
    singular.m[0][0] = 0.0;

    tydra::RenderMesh dst;
    std::string merge_err;
    TEST_CHECK(!converter.MergeMeshData(src, singular, dst, &merge_err));
    TEST_CHECK(merge_err.find("non-invertible") != std::string::npos);
    TEST_CHECK(dst.points.empty());
    TEST_CHECK(dst.normals.empty());
  }

  {
    tydra::RenderSceneConverter converter;

    tydra::RenderMesh src;
    src.abs_path = "/MeshBadIndex";
    src.points = {{0.0f, 0.0f, 0.0f},
                  {1.0f, 0.0f, 0.0f},
                  {0.0f, 1.0f, 0.0f}};
    src.usdFaceVertexIndices = {0, 1, 99};
    src.usdFaceVertexCounts = {3};

    tydra::RenderMesh dst;
    dst.points = {{10.0f, 0.0f, 0.0f}};
    dst.usdFaceVertexIndices = {0};
    dst.usdFaceVertexCounts = {1};

    std::string merge_err;
    TEST_CHECK(!converter.MergeMeshData(
        src, value::matrix4d::identity(), dst, &merge_err));
    TEST_CHECK(merge_err.find("out of point range") != std::string::npos);
    TEST_CHECK(dst.points.size() == 1);
    TEST_CHECK(dst.usdFaceVertexIndices.size() == 1);
    TEST_CHECK(dst.usdFaceVertexCounts.size() == 1);
  }

  {
    Stage stage;
    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.merge_meshes = true;
    env.scene_config.merge_meshes_bake_transform = true;

    tydra::RenderSceneConverter converter;

    tydra::RenderMesh mesh_a;
    mesh_a.prim_name = "MeshA";
    mesh_a.abs_path = "/MeshA";
    mesh_a.material_id = 7;
    mesh_a.points = {{0.0f, 0.0f, 0.0f},
                     {1.0f, 0.0f, 0.0f},
                     {0.0f, 1.0f, 0.0f}};
    mesh_a.usdFaceVertexIndices = {0, 1, 2};
    mesh_a.usdFaceVertexCounts = {3};
    mesh_a.texcoords[0].format = tydra::VertexAttributeFormat::Vec2;
    mesh_a.texcoords[0].set_buffer(
        reinterpret_cast<const uint8_t *>(kTexcoordsVec2.data()),
        kTexcoordsVec2.size() * sizeof(value::texcoord2f));
    mesh_a.texcoords[0].variability = tydra::VertexVariability::Vertex;

    tydra::RenderMesh mesh_b = mesh_a;
    mesh_b.prim_name = "MeshB";
    mesh_b.abs_path = "/MeshB";
    mesh_b.texcoords[0].format = tydra::VertexAttributeFormat::Vec3;
    mesh_b.texcoords[0].set_buffer(
        reinterpret_cast<const uint8_t *>(kTexcoordsVec3.data()),
        kTexcoordsVec3.size() * sizeof(value::float3));
    mesh_b.texcoords[0].variability = tydra::VertexVariability::Vertex;

    tydra::RenderMesh mesh_c = mesh_a;
    mesh_c.prim_name = "MeshC";
    mesh_c.abs_path = "/MeshC";

    converter.meshes.push_back(mesh_a);
    converter.meshes.push_back(mesh_b);
    converter.meshes.push_back(mesh_c);

    tydra::Node node_a;
    node_a.prim_name = "MeshA";
    node_a.abs_path = "/MeshA";
    node_a.category = tydra::NodeCategory::Geom;
    node_a.nodeType = tydra::NodeType::Mesh;
    node_a.id = 0;
    node_a.local_matrix = value::matrix4d::identity();
    node_a.global_matrix = value::matrix4d::identity();

    tydra::Node node_b = node_a;
    node_b.prim_name = "MeshB";
    node_b.abs_path = "/MeshB";
    node_b.id = 1;

    tydra::Node node_c = node_a;
    node_c.prim_name = "MeshC";
    node_c.abs_path = "/MeshC";
    node_c.id = 2;

    converter.root_nodes.push_back(node_a);
    converter.root_nodes.push_back(node_b);
    converter.root_nodes.push_back(node_c);
    converter.meshMap.add("/MeshA", uint64_t(0));
    converter.meshMap.add("/MeshB", uint64_t(1));
    converter.meshMap.add("/MeshC", uint64_t(2));

    TEST_CHECK(converter.MergeMeshesImpl(env));
    TEST_CHECK(converter.GetError().empty());
    TEST_CHECK(converter.GetInfo().find("Skipping mesh merge for /MeshB") !=
               std::string::npos);
    TEST_CHECK(converter.GetInfo().find("Cannot merge texcoords slot 0") !=
               std::string::npos);
    TEST_CHECK(converter.meshes.size() == 2);
    bool found_merged = false;
    for (const tydra::RenderMesh &mesh : converter.meshes) {
      if (mesh.abs_path == "/merged/merged_material_7") {
        found_merged = true;
        TEST_CHECK(mesh.points.size() == 6);
      }
    }
    TEST_CHECK(found_merged);
  }

  {
    // Heterogeneous vertex-attribute presence: meshes sharing a material but
    // differing in whether they carry normals must not be merged into a single
    // mesh whose normals array is shorter than its point count.
    Stage stage;
    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.merge_meshes = true;
    env.scene_config.merge_meshes_bake_transform = true;

    tydra::RenderSceneConverter converter;

    tydra::RenderMesh mesh_a;
    mesh_a.prim_name = "MeshA";
    mesh_a.abs_path = "/MeshA";
    mesh_a.material_id = 4;
    mesh_a.points = {{0.0f, 0.0f, 0.0f},
                     {1.0f, 0.0f, 0.0f},
                     {0.0f, 1.0f, 0.0f}};
    mesh_a.usdFaceVertexIndices = {0, 1, 2};
    mesh_a.usdFaceVertexCounts = {3};
    mesh_a.normals.format = tydra::VertexAttributeFormat::Vec3;
    mesh_a.normals.set_buffer(
        reinterpret_cast<const uint8_t *>(kTexcoordsVec3.data()),
        kTexcoordsVec3.size() * sizeof(value::float3));
    mesh_a.normals.variability = tydra::VertexVariability::Vertex;

    // mesh_b: same material, same topology, but NO normals.
    tydra::RenderMesh mesh_b = mesh_a;
    mesh_b.prim_name = "MeshB";
    mesh_b.abs_path = "/MeshB";
    mesh_b.normals = tydra::VertexAttribute{};

    // mesh_c: identical to mesh_a (has normals) -> mergeable with mesh_a.
    tydra::RenderMesh mesh_c = mesh_a;
    mesh_c.prim_name = "MeshC";
    mesh_c.abs_path = "/MeshC";

    converter.meshes.push_back(mesh_a);
    converter.meshes.push_back(mesh_b);
    converter.meshes.push_back(mesh_c);

    tydra::Node node_a;
    node_a.prim_name = "MeshA";
    node_a.abs_path = "/MeshA";
    node_a.category = tydra::NodeCategory::Geom;
    node_a.nodeType = tydra::NodeType::Mesh;
    node_a.id = 0;
    node_a.local_matrix = value::matrix4d::identity();
    node_a.global_matrix = value::matrix4d::identity();

    tydra::Node node_b = node_a;
    node_b.prim_name = "MeshB";
    node_b.abs_path = "/MeshB";
    node_b.id = 1;

    tydra::Node node_c = node_a;
    node_c.prim_name = "MeshC";
    node_c.abs_path = "/MeshC";
    node_c.id = 2;

    converter.root_nodes.push_back(node_a);
    converter.root_nodes.push_back(node_b);
    converter.root_nodes.push_back(node_c);
    converter.meshMap.add("/MeshA", uint64_t(0));
    converter.meshMap.add("/MeshB", uint64_t(1));
    converter.meshMap.add("/MeshC", uint64_t(2));

    TEST_CHECK(converter.MergeMeshesImpl(env));
    TEST_CHECK(converter.GetError().empty());
    // mesh_b (no normals) must be refused...
    TEST_CHECK(converter.GetInfo().find("Skipping mesh merge for /MeshB") !=
               std::string::npos);
    TEST_CHECK(converter.GetInfo().find("Cannot merge normals") !=
               std::string::npos);
    // ...while mesh_a and mesh_c merge cleanly.
    bool found_merged = false;
    for (const tydra::RenderMesh &mesh : converter.meshes) {
      // No surviving mesh may have a normals array shorter than its points.
      if (!mesh.normals.empty()) {
        TEST_CHECK(mesh.normals.vertex_count() == mesh.points.size());
      }
      if (mesh.abs_path == "/merged/merged_material_4") {
        found_merged = true;
        TEST_CHECK(mesh.points.size() == 6);
        TEST_CHECK(mesh.normals.vertex_count() == 6);
      }
    }
    TEST_CHECK(found_merged);
  }

  {
    // End-to-end regression for the heterogeneous-attribute mesh-merge bug,
    // mirroring tests/usda/merge-meshes-heterogeneous-attrs.usda. Two meshes
    // share a material (same merge group) and have identity (bakeable)
    // transforms, but only one carries a texcoord (st) set. (Texcoords are
    // used rather than normals because the converter fabricates normals when
    // absent, which would erase the presence difference.) The merge must be
    // refused so no mesh ends up with a UV set shorter than its point count.
    const std::string usda = R"(#usda 1.0
def Xform "Root" {
  def Material "Mat" {
    token outputs:surface.connect = </Root/Mat/Preview.outputs:surface>
    def Shader "Preview" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (0.5, 0.5, 0.5)
      token outputs:surface
    }
  }
  def Mesh "MeshWithUV" {
    rel material:binding = </Root/Mat>
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    texCoord2f[] primvars:st = [(0, 0), (1, 0), (0, 1)] (
      interpolation = "vertex"
    )
  }
  def Mesh "MeshNoUV" {
    rel material:binding = </Root/Mat>
    point3f[] points = [(2, 0, 0), (3, 0, 0), (2, 1, 0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
  }
}
)";

    Stage stage;
    std::string warn;
    std::string err;
    TEST_CHECK(LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
        "merge_meshes_heterogeneous_attrs.usda", &stage, &warn, &err));
    TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.merge_meshes = true;
    env.scene_config.merge_meshes_bake_transform = true;

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;
    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().empty());

    // Heterogeneous attributes must prevent the merge: both meshes survive and
    // no synthetic /merged mesh is produced. Any surviving UV set must cover
    // every vertex of its mesh.
    TEST_CHECK(scene.meshes.size() == 2);
    for (const tydra::RenderMesh &mesh : scene.meshes) {
      TEST_CHECK(mesh.abs_path.find("/merged") == std::string::npos);
      for (const auto &tc : mesh.texcoords) {
        TEST_CHECK(tc.second.vertex_count() >= mesh.points.size());
      }
    }
  }

  {
    // Positive control: when both meshes carry the same attributes they DO
    // merge, and the merged UV set stays consistent with the points.
    const std::string usda = R"(#usda 1.0
def Xform "Root" {
  def Material "Mat" {
    token outputs:surface.connect = </Root/Mat/Preview.outputs:surface>
    def Shader "Preview" {
      uniform token info:id = "UsdPreviewSurface"
      color3f inputs:diffuseColor = (0.5, 0.5, 0.5)
      token outputs:surface
    }
  }
  def Mesh "MeshA" {
    rel material:binding = </Root/Mat>
    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    texCoord2f[] primvars:st = [(0, 0), (1, 0), (0, 1)] (
      interpolation = "vertex"
    )
  }
  def Mesh "MeshB" {
    rel material:binding = </Root/Mat>
    point3f[] points = [(2, 0, 0), (3, 0, 0), (2, 1, 0)]
    int[] faceVertexCounts = [3]
    int[] faceVertexIndices = [0, 1, 2]
    texCoord2f[] primvars:st = [(0, 0), (1, 0), (0, 1)] (
      interpolation = "vertex"
    )
  }
}
)";

    Stage stage;
    std::string warn;
    std::string err;
    TEST_CHECK(LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
        "merge_meshes_homogeneous_attrs.usda", &stage, &warn, &err));
    TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.merge_meshes = true;
    env.scene_config.merge_meshes_bake_transform = true;

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;
    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().empty());

    // The two homogeneous meshes collapse into a single merged mesh whose UV
    // set covers every vertex.
    TEST_CHECK(scene.meshes.size() == 1);
    for (const tydra::RenderMesh &mesh : scene.meshes) {
      for (const auto &tc : mesh.texcoords) {
        TEST_CHECK(tc.second.vertex_count() >= mesh.points.size());
      }
    }
  }

  {
    tydra::RenderSceneConverter converter;

    tydra::UVTexture texture_a;
    texture_a.texture_image_id = 5;
    texture_a.wrapS = tydra::UVTexture::WrapMode::REPEAT;
    texture_a.wrapT = tydra::UVTexture::WrapMode::REPEAT;
    texture_a.varname_uv = "st";

    tydra::UVTexture texture_b = texture_a;

    converter.textures.push_back(texture_a);
    converter.textures.push_back(texture_b);

    tydra::RenderMaterial material;
    material.surfaceShader = tydra::PreviewSurfaceShader{};
    material.surfaceShader->diffuseColor.texture_id = 1;
    converter.materials.push_back(material);

    TEST_CHECK(converter.DeduplicateTexturesByIdentityImpl() == 1);
    TEST_CHECK(converter.textures.size() == 1);
    TEST_CHECK(converter.materials.size() == 1);
    if (converter.materials.size() == 1 &&
        converter.materials[0].surfaceShader.has_value()) {
      TEST_CHECK(converter.materials[0]
                     .surfaceShader->diffuseColor.texture_id == 0);
    }
  }

  {
    Stage stage;
    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.merge_meshes = true;
    env.scene_config.merge_meshes_bake_transform = true;

    tydra::RenderSceneConverter converter;

    tydra::RenderMesh mesh_a;
    mesh_a.prim_name = "MeshA";
    mesh_a.abs_path = "/MeshA";
    mesh_a.material_id = 3;
    mesh_a.points = {{0.0f, 0.0f, 0.0f},
                     {1.0f, 0.0f, 0.0f},
                     {0.0f, 1.0f, 0.0f}};
    mesh_a.usdFaceVertexIndices = {0, 1, 2};
    mesh_a.usdFaceVertexCounts = {3};

    tydra::RenderMesh mesh_b = mesh_a;
    mesh_b.prim_name = "MeshB";
    mesh_b.abs_path = "/MeshB";

    converter.meshes.push_back(mesh_a);
    converter.meshes.push_back(mesh_b);

    // Scene: /Root (Xform) -> { MeshA, MeshB }. Default root is index 0 (no
    // defaultPrim set -> default_node stays -1 -> falls back to root 0).
    tydra::Node node_a;
    node_a.prim_name = "MeshA";
    node_a.abs_path = "/Root/MeshA";
    node_a.category = tydra::NodeCategory::Geom;
    node_a.nodeType = tydra::NodeType::Mesh;
    node_a.id = 0;
    node_a.local_matrix = value::matrix4d::identity();
    node_a.global_matrix = value::matrix4d::identity();

    tydra::Node node_b = node_a;
    node_b.prim_name = "MeshB";
    node_b.abs_path = "/Root/MeshB";
    node_b.id = 1;

    tydra::Node root;
    root.prim_name = "Root";
    root.abs_path = "/Root";
    root.category = tydra::NodeCategory::Group;
    root.nodeType = tydra::NodeType::Xform;
    root.id = -1;
    root.local_matrix = value::matrix4d::identity();
    root.global_matrix = value::matrix4d::identity();
    root.children.push_back(node_a);
    root.children.push_back(node_b);

    converter.root_nodes.push_back(root);

    TEST_CHECK(converter.MergeMeshesImpl(env));
    TEST_CHECK(converter.GetError().empty());
    TEST_CHECK(converter.meshes.size() == 1);
    if (converter.meshes.size() == 1) {
      TEST_CHECK(converter.meshes[0].material_id == 3);
      TEST_CHECK(converter.meshes[0].points.size() == 6);
    }
    // Baked merge: both source nodes become plain groups (transforms intact)
    // and the merged mesh is attached as a child of the default root with an
    // identity transform (root is identity here).
    TEST_CHECK(converter.root_nodes.size() == 1);
    if (converter.root_nodes.size() == 1) {
      const tydra::Node &r = converter.root_nodes[0];
      TEST_CHECK(r.children.size() == 3);
      if (r.children.size() == 3) {
        TEST_CHECK(r.children[0].nodeType == tydra::NodeType::Xform);
        TEST_CHECK(r.children[0].id == -1);
        TEST_CHECK(r.children[1].nodeType == tydra::NodeType::Xform);
        TEST_CHECK(r.children[1].id == -1);
        const tydra::Node &merged = r.children[2];
        TEST_CHECK(merged.nodeType == tydra::NodeType::Mesh);
        TEST_CHECK(merged.id == 0);
        TEST_CHECK(merged.abs_path == "/merged/merged_material_3");
        TEST_CHECK(lightusd::is_identity(merged.local_matrix));
      }
    }
    auto merged_it = converter.meshMap.find("/merged/merged_material_3");
    TEST_CHECK(merged_it != converter.meshMap.s_end());
    if (merged_it != converter.meshMap.s_end()) {
      TEST_CHECK(merged_it->second == 0);
    }
    TEST_CHECK(converter.GetInfo().find(
                   "Mesh merge compacted mesh records: 3 -> 1.") !=
               std::string::npos);
  }

  {
    // Regression (nested mesh-under-mesh): baked merge must NOT rewrite a source
    // node's local transform in place. Source mesh nodes can be nested (e.g. a
    // window mesh parented under a wall mesh); neutralizing an ancestor's local
    // corrupts the world transform of any descendant mesh node — the "floating
    // mesh" artifact. The fix attaches the world-space merged mesh to a fresh
    // root-level node (identity transform) and leaves every source node's local
    // untouched.
    Stage stage;
    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.merge_meshes = true;
    env.scene_config.merge_meshes_bake_transform = true;

    tydra::RenderSceneConverter converter;

    auto translate_x = [](double tx) {
      value::matrix4d m = value::matrix4d::identity();
      m.m[3][0] = tx;
      return m;
    };

    tydra::RenderMesh wall_mesh;
    wall_mesh.prim_name = "Wall";
    wall_mesh.abs_path = "/Root/Wall";
    wall_mesh.material_id = 5;
    wall_mesh.points = {{0.0f, 0.0f, 0.0f},
                        {1.0f, 0.0f, 0.0f},
                        {0.0f, 1.0f, 0.0f}};
    wall_mesh.usdFaceVertexIndices = {0, 1, 2};
    wall_mesh.usdFaceVertexCounts = {3};

    tydra::RenderMesh window_mesh = wall_mesh;
    window_mesh.prim_name = "Window";
    window_mesh.abs_path = "/Root/Wall/Window";

    converter.meshes.push_back(wall_mesh);    // id 0
    converter.meshes.push_back(window_mesh);  // id 1

    // Scene: /Root (Xform, identity) -> Wall (Mesh, local 10) -> Window
    // (Mesh, local 2; global = 10 * 2 = 12). Window is nested under Wall.
    tydra::Node window;
    window.prim_name = "Window";
    window.abs_path = "/Root/Wall/Window";
    window.category = tydra::NodeCategory::Geom;
    window.nodeType = tydra::NodeType::Mesh;
    window.id = 1;
    window.local_matrix = translate_x(2.0);
    window.global_matrix = translate_x(12.0);

    tydra::Node wall;
    wall.prim_name = "Wall";
    wall.abs_path = "/Root/Wall";
    wall.category = tydra::NodeCategory::Geom;
    wall.nodeType = tydra::NodeType::Mesh;
    wall.id = 0;
    wall.local_matrix = translate_x(10.0);
    wall.global_matrix = translate_x(10.0);
    wall.children.push_back(window);

    tydra::Node root;
    root.prim_name = "Root";
    root.abs_path = "/Root";
    root.category = tydra::NodeCategory::Group;
    root.nodeType = tydra::NodeType::Xform;
    root.id = -1;
    root.local_matrix = value::matrix4d::identity();
    root.global_matrix = value::matrix4d::identity();
    root.children.push_back(wall);

    converter.root_nodes.push_back(root);

    TEST_CHECK(converter.MergeMeshesImpl(env));
    TEST_CHECK(converter.GetError().empty());

    // Both sources merged into one world-space mesh.
    bool found_merged = false;
    for (const tydra::RenderMesh &m : converter.meshes) {
      if (m.abs_path == "/merged/merged_material_5") {
        found_merged = true;
        TEST_CHECK(m.points.size() == 6);
      }
    }
    TEST_CHECK(found_merged);

    TEST_CHECK(converter.root_nodes.size() == 1);
    const tydra::Node &root_after = converter.root_nodes[0];
    // Root gains the merged mesh child; the original Wall subtree is retained.
    TEST_CHECK(root_after.children.size() == 2);
    if (root_after.children.size() == 2) {
      // Source nodes keep their ORIGINAL locals (NOT neutralized) and become
      // plain groups, so descendants stay correctly placed.
      const tydra::Node &wall_after = root_after.children[0];
      TEST_CHECK(wall_after.nodeType == tydra::NodeType::Xform);
      TEST_CHECK(wall_after.id == -1);
      TEST_CHECK(std::abs(wall_after.local_matrix.m[3][0] - 10.0) < 1e-9);
      TEST_CHECK(wall_after.children.size() == 1);
      if (wall_after.children.size() == 1) {
        const tydra::Node &window_after = wall_after.children[0];
        TEST_CHECK(window_after.id == -1);
        TEST_CHECK(std::abs(window_after.local_matrix.m[3][0] - 2.0) < 1e-9);
      }

      // Merged mesh attached under the (identity) root with identity transform.
      const tydra::Node &merged_node = root_after.children[1];
      TEST_CHECK(merged_node.nodeType == tydra::NodeType::Mesh);
      TEST_CHECK(merged_node.abs_path == "/merged/merged_material_5");
      TEST_CHECK(lightusd::is_identity(merged_node.local_matrix));
    }
  }

  {
    tydra::RenderSceneConverter converter;

    tydra::Node root;
    root.prim_name = "Root";
    root.abs_path = "/Root";
    root.category = tydra::NodeCategory::Group;
    root.nodeType = tydra::NodeType::Xform;
    root.local_matrix = value::matrix4d::identity();
    root.global_matrix = value::matrix4d::identity();

    tydra::Node group;
    group.prim_name = "Group";
    group.abs_path = "/Root/Group";
    group.category = tydra::NodeCategory::Group;
    group.nodeType = tydra::NodeType::Xform;
    group.local_matrix = value::matrix4d::identity();
    group.global_matrix = value::matrix4d::identity();

    tydra::Node mesh_node;
    mesh_node.prim_name = "Mesh";
    mesh_node.abs_path = "/Root/Group/Mesh";
    mesh_node.category = tydra::NodeCategory::Geom;
    mesh_node.nodeType = tydra::NodeType::Mesh;
    mesh_node.id = 0;
    mesh_node.local_matrix = value::matrix4d::identity();
    mesh_node.global_matrix = value::matrix4d::identity();
    mesh_node.global_matrix.m[3][0] = 2.0;

    group.children.push_back(mesh_node);
    root.children.push_back(group);
    converter.root_nodes.push_back(root);

    TEST_CHECK(converter.FlattenOptimizedRenderTreeImpl() == 1);
    TEST_CHECK(converter.root_nodes.size() == 1);
    if (converter.root_nodes.size() == 1) {
      TEST_CHECK(converter.root_nodes[0].nodeType == tydra::NodeType::Xform);
      TEST_CHECK(converter.root_nodes[0].children.size() == 1);
      if (converter.root_nodes[0].children.size() == 1) {
        const tydra::Node &flat_mesh = converter.root_nodes[0].children[0];
        TEST_CHECK(flat_mesh.nodeType == tydra::NodeType::Mesh);
        TEST_CHECK(flat_mesh.id == 0);
        TEST_CHECK(flat_mesh.children.empty());
        TEST_CHECK(flat_mesh.local_matrix.m[3][0] == 2.0);
      }
    }
  }

  {
    const std::string usda = R"(#usda 1.0
def Xform "Root" {
  def Xform "Instancer" {
    double3 xformOp:translate = (5, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
    def PointInstancer "Points" {
      rel prototypes = </Root/Proto/Mesh>
      int[] protoIndices = [0]
      point3f[] positions = [(1, 0, 0)]
      quatf[] orientations = [(1, 0, 0, 0)]
      float3[] scales = [(1, 1, 1)]
      color3f[] primvars:displayColor = [(0.25, 0.5, 0.75)] (
        interpolation = "vertex"
      )
      float[] primvars:displayOpacity = [0.5] (
        interpolation = "vertex"
      )
    }
  }
  def Xform "Proto" {
    def Mesh "Mesh" {
      point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
      int[] faceVertexCounts = [3]
      int[] faceVertexIndices = [0,1,2]
    }
  }
}
)";

    Stage stage;
    std::string warn;
    std::string err;
    TEST_CHECK(LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
        "flatten_point_instancer.usda", &stage, &warn, &err));
    TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.expand_point_instancers = true;
    env.scene_config.flatten_optimized_render_tree = true;

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;
    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().empty());
    TEST_CHECK(scene.instances.size() == 1);
    if (scene.instances.size() == 1) {
      TEST_CHECK(scene.instances[0].global_matrix.m[3][0] == 6.0);
      TEST_CHECK(scene.instances[0].has_display_color);
      TEST_CHECK(NearlyEqual(scene.instances[0].display_color[0], 0.25f));
      TEST_CHECK(NearlyEqual(scene.instances[0].display_color[1], 0.5f));
      TEST_CHECK(NearlyEqual(scene.instances[0].display_color[2], 0.75f));
      TEST_CHECK(scene.instances[0].has_display_opacity);
      TEST_CHECK(NearlyEqual(scene.instances[0].display_opacity, 0.5f));
    }
    TEST_CHECK(scene.nodes.size() == 1);
    if (scene.nodes.size() == 1) {
      TEST_CHECK(scene.nodes[0].abs_path == "/OptimizedRenderRoot");
    }
  }

  {
    const std::string usda = R"(#usda 1.0
def Xform "Root" {
  def Xform "Group" {
    def Mesh "Mesh" {
      double3 xformOp:translate.timeSamples = {
        1: (0, 0, 0),
        2: (2, 0, 0),
      }
      uniform token[] xformOpOrder = ["xformOp:translate"]
      point3f[] points = [(0,0,0),(1,0,0),(0,1,0)]
      int[] faceVertexCounts = [3]
      int[] faceVertexIndices = [0,1,2]
    }
  }
}
)";

    Stage stage;
    std::string warn;
    std::string err;
    TEST_CHECK(LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
        "flatten_animation_remap.usda", &stage, &warn, &err));
    TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

    tydra::RenderSceneConverterEnv env(stage);
    env.scene_config.flatten_optimized_render_tree = true;

    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;
    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.animations.size() == 1);
    if (scene.animations.size() == 1 &&
        !scene.animations[0].channels.empty()) {
      TEST_CHECK(scene.animations[0].channels[0].target_node == 1);
    }
  }

  {
    const std::string usda = R"(#usda 1.0
def Material "Mat" {
  token outputs:surface.connect = </Mat/Preview.outputs:surface>
  def Shader "Preview" {
    uniform token info:id = "UsdPreviewSurface"
    color3f inputs:diffuseColor.timeSamples = {
      1: (1, 0, 0),
      2: (0, 1, 0),
    }
    token outputs:surface
  }
}
)";

    Stage stage;
    std::string warn;
    std::string err;
    TEST_CHECK(LoadUSDAFromMemory(
        reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
        "timesampled_material.usda", &stage, &warn, &err));
    TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

    const Prim *prim{nullptr};
    TEST_CHECK(stage.find_prim_at_path(Path("/Mat", ""), prim, &err));
    TEST_CHECK(prim != nullptr);
    if (prim) {
      const Material *material = prim->as<Material>();
      TEST_CHECK(material != nullptr);
      if (material) {
        tydra::RenderSceneConverterEnv env(stage);
        tydra::RenderSceneConverter converter;
        std::string signature;
        TEST_CHECK(!converter.BuildMaterialSourceSignature(
            env, Path("/Mat", ""), *material, &signature));
      }
    }
  }
}

void tydra_skel_animation_validation_test(void) {
  {
    SkelAnimation anim;
    anim.name = "Anim";
    anim.joints = std::vector<value::token>{value::token("Root")};
    anim.translations = Animatable<std::vector<value::float3>>(
        std::vector<value::float3>{value::float3{1.0f, 2.0f, 3.0f}});
    // Invalid per USD skel rules: translation is authored, rotation/scale are not.

    Stage stage;
    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderSceneConverter converter;
    tydra::SkelHierarchy hierarchy;
    hierarchy.root_node.joint_name = "Root";
    hierarchy.root_node.joint_path = "Root";
    hierarchy.root_node.joint_id = 0;
    converter.skeletons.push_back(hierarchy);
    tydra::AnimationClip clip;

    TEST_CHECK(!converter.ConvertSkelAnimation(env, Path("/Anim", ""), anim, 0,
                                               &clip));
    TEST_CHECK(converter.GetError().find("must be all authored") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("/Anim") != std::string::npos);
  }

  {
    Stage stage;

    Skeleton fallback_skeleton;
    fallback_skeleton.name = "FallbackSkeleton";
    fallback_skeleton.joints = std::vector<value::token>{value::token("Root")};
    fallback_skeleton.jointNames =
        std::vector<value::token>{value::token("Root")};
    fallback_skeleton.bindTransforms =
        std::vector<value::matrix4d>{};  // Authored empty; treat as missing.
    fallback_skeleton.restTransforms = std::vector<value::matrix4d>{
        value::matrix4d::identity()};

    TEST_CHECK(stage.add_root_prim(Prim("FallbackSkeleton", fallback_skeleton)));

    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.skeletons.size() == 1);
    if (scene.skeletons.size() == 1) {
      TEST_CHECK(scene.skeletons[0].bind_transforms.size() == 1);
      TEST_CHECK(scene.skeletons[0].rest_transforms.size() == 1);
      TEST_CHECK(scene.skeletons[0].bind_transforms[0] ==
                 value::matrix4d::identity());
    }
  }
}

void tydra_prim_parameter_animation_test(void) {
  const std::string usda = R"(#usda 1.0
(
    defaultPrim = "Root"
    startTimeCode = 1
    endTimeCode = 2
)
def Xform "Root"
{
    def Camera "Cam"
    {
        float focalLength.timeSamples = {
            1: 35,
            2: 50,
        }
        custom quatf customOrientation.timeSamples = {
            1: (1, 0, 0, 0),
            2: (0.5, 0.5, 0.5, 0.5),
        }
        custom matrix4d customMatrix.timeSamples = {
            1: ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0), (0, 0, 0, 1)),
            2: ((2, 0, 0, 0), (0, 2, 0, 0), (0, 0, 2, 0), (1, 2, 3, 1)),
        }
    }

    def SphereLight "Key"
    {
        float inputs:intensity.timeSamples = {
            1: 10,
            2: 20,
        }
    }

    def Material "Mat"
    {
        token outputs:surface.connect = </Root/Mat/PreviewSurface.outputs:surface>

        def Shader "PreviewSurface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.timeSamples = {
                1: (0.1, 0.2, 0.3),
                2: (0.4, 0.5, 0.6),
            }
            token outputs:surface
        }
    }
}
)";

  Stage stage;
  std::string warn;
  std::string err;
  TEST_CHECK(LoadUSDAFromMemory(
      reinterpret_cast<const uint8_t *>(usda.data()), usda.size(),
      "prim_parameter_animation.usda", &stage, &warn, &err));
  TEST_MSG("LoadUSDAFromMemory failed: %s", err.c_str());

  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderScene scene;
  tydra::RenderSceneConverter converter;
  TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
  TEST_MSG("ConvertToRenderScene failed: %s", converter.GetError().c_str());

  auto find_custom_channel = [&](const std::string &target_path,
                                 const std::string &property_name,
                                 const tydra::KeyframeSampler **sampler_out) {
    for (const tydra::AnimationClip &clip : scene.animations) {
      for (const tydra::AnimationChannel &channel : clip.channels) {
        if (channel.path != tydra::AnimationPath::CustomProperty ||
            channel.target_prim_path != target_path ||
            channel.property_name != property_name ||
            channel.sampler < 0 ||
            size_t(channel.sampler) >= clip.samplers.size()) {
          continue;
        }
        *sampler_out = &clip.samplers[size_t(channel.sampler)];
        return true;
      }
    }
    return false;
  };

  auto count_custom_channels = [&](const std::string &target_path,
                                   const std::string &property_name) {
    size_t count = 0;
    for (const tydra::AnimationClip &clip : scene.animations) {
      for (const tydra::AnimationChannel &channel : clip.channels) {
        if (channel.path == tydra::AnimationPath::CustomProperty &&
            channel.target_prim_path == target_path &&
            channel.property_name == property_name) {
          count++;
        }
      }
    }
    return count;
  };

  const tydra::KeyframeSampler *camera_sampler = nullptr;
  const tydra::KeyframeSampler *camera_quat_sampler = nullptr;
  const tydra::KeyframeSampler *camera_matrix_sampler = nullptr;
  const tydra::KeyframeSampler *light_sampler = nullptr;
  const tydra::KeyframeSampler *shader_sampler = nullptr;
  TEST_CHECK(find_custom_channel("/Root/Cam", "focalLength", &camera_sampler));
  TEST_CHECK(find_custom_channel("/Root/Cam", "customOrientation",
                                 &camera_quat_sampler));
  TEST_CHECK(find_custom_channel("/Root/Cam", "customMatrix",
                                 &camera_matrix_sampler));
  TEST_CHECK(find_custom_channel("/Root/Key", "inputs:intensity",
                                 &light_sampler));
  TEST_CHECK(find_custom_channel("/Root/Mat/PreviewSurface",
                                 "inputs:diffuseColor", &shader_sampler));
  TEST_CHECK(count_custom_channels("/Root/Cam", "focalLength") == 1);
  TEST_CHECK(count_custom_channels("/Root/Key", "inputs:intensity") == 1);

  if (camera_sampler) {
    TEST_CHECK(camera_sampler->times.size() == 2);
    TEST_CHECK(camera_sampler->values.size() == 2);
    if (camera_sampler->values.size() == 2) {
      TEST_CHECK(NearlyEqual(camera_sampler->values[0], 35.0f));
      TEST_CHECK(NearlyEqual(camera_sampler->values[1], 50.0f));
    }
  }
  if (camera_quat_sampler) {
    TEST_CHECK(camera_quat_sampler->times.size() == 2);
    TEST_CHECK(camera_quat_sampler->values.size() == 8);
    if (camera_quat_sampler->values.size() == 8) {
      TEST_CHECK(NearlyEqual(camera_quat_sampler->values[0], 0.0f));
      TEST_CHECK(NearlyEqual(camera_quat_sampler->values[1], 0.0f));
      TEST_CHECK(NearlyEqual(camera_quat_sampler->values[2], 0.0f));
      TEST_CHECK(NearlyEqual(camera_quat_sampler->values[3], 1.0f));
      TEST_CHECK(NearlyEqual(camera_quat_sampler->values[4], 0.5f));
      TEST_CHECK(NearlyEqual(camera_quat_sampler->values[7], 0.5f));
    }
  }
  if (camera_matrix_sampler) {
    TEST_CHECK(camera_matrix_sampler->times.size() == 2);
    TEST_CHECK(camera_matrix_sampler->values.size() == 32);
    if (camera_matrix_sampler->values.size() == 32) {
      TEST_CHECK(NearlyEqual(camera_matrix_sampler->values[0], 1.0f));
      TEST_CHECK(NearlyEqual(camera_matrix_sampler->values[5], 1.0f));
      TEST_CHECK(NearlyEqual(camera_matrix_sampler->values[10], 1.0f));
      TEST_CHECK(NearlyEqual(camera_matrix_sampler->values[15], 1.0f));
      TEST_CHECK(NearlyEqual(camera_matrix_sampler->values[16], 2.0f));
      TEST_CHECK(NearlyEqual(camera_matrix_sampler->values[31], 1.0f));
    }
  }
  if (light_sampler) {
    TEST_CHECK(light_sampler->times.size() == 2);
    TEST_CHECK(light_sampler->values.size() == 2);
    if (light_sampler->values.size() == 2) {
      TEST_CHECK(NearlyEqual(light_sampler->values[0], 10.0f));
      TEST_CHECK(NearlyEqual(light_sampler->values[1], 20.0f));
    }
  }
  if (shader_sampler) {
    TEST_CHECK(shader_sampler->times.size() == 2);
    TEST_CHECK(shader_sampler->values.size() == 6);
    if (shader_sampler->values.size() == 6) {
      TEST_CHECK(NearlyEqual(shader_sampler->values[0], 0.1f));
      TEST_CHECK(NearlyEqual(shader_sampler->values[1], 0.2f));
      TEST_CHECK(NearlyEqual(shader_sampler->values[2], 0.3f));
      TEST_CHECK(NearlyEqual(shader_sampler->values[3], 0.4f));
      TEST_CHECK(NearlyEqual(shader_sampler->values[4], 0.5f));
      TEST_CHECK(NearlyEqual(shader_sampler->values[5], 0.6f));
    }
  }
}

void tydra_skin_binding_validation_test(void) {
  auto make_skinned_mesh = []() {
    GeomMesh mesh;
    mesh.points = Animatable<std::vector<value::point3f>>(
        std::vector<value::point3f>{{0.0f, 0.0f, 0.0f},
                                    {1.0f, 0.0f, 0.0f},
                                    {0.0f, 1.0f, 0.0f}});
    mesh.faceVertexCounts = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{3});
    mesh.faceVertexIndices = Animatable<std::vector<int32_t>>(
        std::vector<int32_t>{0, 1, 2});

    GeomPrimvar joint_indices;
    joint_indices.set_name("skel:jointIndices");
    joint_indices.set_value(std::vector<int>{0, 0, 0});
    joint_indices.set_interpolation(Interpolation::Vertex);
    joint_indices.set_elementSize(1);

    GeomPrimvar joint_weights;
    joint_weights.set_name("skel:jointWeights");
    joint_weights.set_value(std::vector<float>{1.0f, 1.0f, 1.0f});
    joint_weights.set_interpolation(Interpolation::Vertex);
    joint_weights.set_elementSize(1);

    std::string err;
    TEST_CHECK(mesh.set_primvar(joint_indices, &err));
    TEST_CHECK(err.empty());
    TEST_CHECK(mesh.set_primvar(joint_weights, &err));
    TEST_CHECK(err.empty());

    return mesh;
  };

  {
    Stage stage;
    GeomMesh mesh = make_skinned_mesh();

    Relationship invalid_skeleton_rel;
    invalid_skeleton_rel.set(std::vector<Path>{});
    mesh.skeleton = invalid_skeleton_rel;

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));

    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find(
                   "`skel:skeleton` must have exactly one target") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("/MeshPrim") != std::string::npos);
  }

  {
    Stage stage;
    GeomMesh mesh = make_skinned_mesh();

    Relationship invalid_skeleton_rel;
    invalid_skeleton_rel.set(
        std::vector<Path>{Path("/SkeletonA", ""), Path("/SkeletonB", "")});
    mesh.skeleton = invalid_skeleton_rel;

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));

    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(!converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(converter.GetError().find(
                   "`skel:skeleton` must have exactly one target") !=
               std::string::npos);
    TEST_CHECK(converter.GetError().find("/MeshPrim") != std::string::npos);
  }

  {
    Stage stage;
    GeomMesh mesh = make_skinned_mesh();

    TEST_CHECK(stage.add_root_prim(Prim("MeshPrim", mesh)));

    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderScene scene;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.meshes.size() == 1);
    TEST_CHECK(converter.GetWarning().find(
                   "Mesh has skinning data but no skeleton bound") !=
               std::string::npos);
    TEST_CHECK(converter.GetWarning().find("/MeshPrim") != std::string::npos);
  }
}

void tydra_value_clip_animation_conversion_test(void) {
  std::string err;
  const std::string clip0_path = MakeTempUSDFilePath("value_clip_conversion_0");
  const std::string clip1_path = MakeTempUSDFilePath("value_clip_conversion_1");
  const std::string scene_path = MakeTempUSDFilePath("value_clip_conversion_scene");

  const auto temp_files = std::vector<std::string>{clip0_path, clip1_path, scene_path};
  auto cleanup = [&]() { CleanupTempFiles(temp_files); };
  const std::array<float, 3> clip0_translate{0.0f, 0.0f, 0.0f};
  const std::array<float, 3> clip0_scale{1.0f, 1.0f, 1.0f};
  const std::array<float, 3> clip1_translate{10.0f, 0.0f, 0.0f};
  const std::array<float, 3> clip1_scale{2.0f, 2.0f, 2.0f};

  TEST_CHECK(WriteTextFile(
      clip0_path,
      MakeValueClipAssetUSD(clip0_translate, clip0_scale)));
  TEST_CHECK(WriteTextFile(
      clip1_path,
      MakeValueClipAssetUSD(clip1_translate, clip1_scale)));
  TEST_CHECK(WriteTextFile(
      scene_path,
      MakeValueClipSceneUSD(clip0_path, clip1_path,
                            {{0.0, 0}, {1.0, 0}, {2.0, 1}},
                            {{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}},
                            false)));

  Stage stage;
  TEST_CHECK(LoadUSDFromFile(scene_path, &stage, nullptr, &err));

  tydra::RenderScene scene;
  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderSceneConverter converter;

  TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
  TEST_CHECK(scene.animations.size() == 1);

  const auto &anim = scene.animations[0];
  TEST_CHECK(anim.has_value_clip);
  TEST_CHECK(anim.value_clip_baked);
  TEST_CHECK(anim.value_clip_sample_rate == 0.0f);
  TEST_CHECK(anim.clip_asset_paths.size() == 2);
  TEST_CHECK(anim.samplers.size() == 3);
  TEST_CHECK(anim.channels.size() == 3);

  TEST_CHECK(anim.samplers[0].times.size() == 3);
  TEST_CHECK(anim.samplers[1].times.size() == 3);
  TEST_CHECK(anim.samplers[2].times.size() == 3);
  TEST_CHECK(NearlyEqual(anim.samplers[0].times[0], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].times[1], 1.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].times[2], 2.0f));

  TEST_CHECK(anim.samplers[0].values.size() == 9);
  TEST_CHECK(anim.samplers[1].values.size() == 12);
  TEST_CHECK(anim.samplers[2].values.size() == 9);
  // Clip0 sample values
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[0], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[1], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[2], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[1].values[3], 1.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[2].values[0], 1.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[2].values[1], 1.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[2].values[2], 1.0f));
  // Clip1 sample values
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[6], 10.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[7], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[8], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[1].values[11], 1.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[2].values[6], 2.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[2].values[7], 2.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[2].values[8], 2.0f));

  TEST_CHECK(anim.channels[0].path == tydra::AnimationPath::Translation);
  TEST_CHECK(anim.channels[1].path == tydra::AnimationPath::Rotation);
  TEST_CHECK(anim.channels[2].path == tydra::AnimationPath::Scale);
  TEST_CHECK(anim.channels[0].sampler == 0);
  TEST_CHECK(anim.channels[1].sampler == 1);
  TEST_CHECK(anim.channels[2].sampler == 2);

  cleanup();
}

void tydra_value_clip_animation_resample_test(void) {
  std::string err;
  const std::string clip0_path = MakeTempUSDFilePath("value_clip_resample_0");
  const std::string clip1_path = MakeTempUSDFilePath("value_clip_resample_1");
  const std::string scene_path = MakeTempUSDFilePath("value_clip_resample_scene");

  const auto temp_files = std::vector<std::string>{clip0_path, clip1_path, scene_path};
  auto cleanup = [&]() { CleanupTempFiles(temp_files); };
  const std::array<float, 3> clip0_translate{0.0f, 0.0f, 0.0f};
  const std::array<float, 3> clip0_scale{1.0f, 1.0f, 1.0f};
  const std::array<float, 3> clip1_translate{10.0f, 0.0f, 0.0f};
  const std::array<float, 3> clip1_scale{2.0f, 2.0f, 2.0f};

  TEST_CHECK(WriteTextFile(
      clip0_path,
      MakeValueClipAssetUSD(clip0_translate, clip0_scale)));
  TEST_CHECK(WriteTextFile(
      clip1_path,
      MakeValueClipAssetUSD(clip1_translate, clip1_scale)));
  TEST_CHECK(WriteTextFile(
      scene_path,
      MakeValueClipSceneUSD(clip0_path, clip1_path,
                            {{0.0, 0}, {1.0, 0}, {2.0, 1}},
                            {{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}},
                            false)));

  Stage stage;
  TEST_CHECK(LoadUSDFromFile(scene_path, &stage, nullptr, &err));

  tydra::RenderScene scene;
  tydra::RenderSceneConverterEnv env(stage);
  env.scene_config.value_clip_sample_rate = 4.0f;
  tydra::RenderSceneConverter converter;

  TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
  TEST_CHECK(scene.animations.size() == 1);

  const auto &anim = scene.animations[0];
  TEST_CHECK(anim.has_value_clip);
  TEST_CHECK(anim.value_clip_baked);
  TEST_CHECK(anim.value_clip_sample_rate == 4.0f);
  TEST_CHECK(anim.samplers[0].times.size() == 9);
  TEST_CHECK(anim.samplers[0].values.size() == 27);
  TEST_CHECK(anim.samplers[1].values.size() == 36);
  TEST_CHECK(anim.samplers[2].values.size() == 27);

  TEST_CHECK(NearlyEqual(anim.samplers[0].times[0], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].times[1], 0.25f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].times[4], 1.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].times[8], 2.0f));
  // t=0 from clip0
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[0], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[3], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[6], 0.0f));
  // t=1 from clip0
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[12], 0.0f));
  // t=2 from clip1
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[24], 10.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[2].values[0], 1.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[2].values[26], 2.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[1].values[3], 1.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[1].values.back(), 1.0f));

  cleanup();
}

void tydra_value_clip_animation_retime_test(void) {
  std::string err;
  const std::string clip0_path = MakeTempUSDFilePath("value_clip_retime_0");
  const std::string clip1_path = MakeTempUSDFilePath("value_clip_retime_1");
  const std::string scene_path = MakeTempUSDFilePath("value_clip_retime_scene");

  const auto temp_files = std::vector<std::string>{clip0_path, clip1_path, scene_path};
  auto cleanup = [&]() { CleanupTempFiles(temp_files); };
  const std::array<float, 3> clip0_translate{0.0f, 0.0f, 0.0f};
  const std::array<float, 3> clip0_scale{1.0f, 1.0f, 1.0f};
  const std::array<float, 3> clip1_translate{10.0f, 0.0f, 0.0f};
  const std::array<float, 3> clip1_scale{2.0f, 2.0f, 2.0f};

  TEST_CHECK(WriteTextFile(
      clip0_path,
      MakeValueClipAssetUSD(clip0_translate, clip0_scale)));
  TEST_CHECK(WriteTextFile(
      clip1_path,
      MakeValueClipAssetUSD(clip1_translate, clip1_scale)));
  TEST_CHECK(WriteTextFile(
      scene_path,
      MakeValueClipSceneUSD(clip0_path, clip1_path,
                            {{0.0, 0}, {1.0, 0}, {2.0, 1}},
                            {{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}},
                            false)));

  Stage stage;
  TEST_CHECK(LoadUSDFromFile(scene_path, &stage, nullptr, &err));

  tydra::RenderScene scene;
  tydra::RenderSceneConverterEnv env(stage);
  env.scene_config.value_clip_sample_rate = 2.0f;
  env.scene_config.value_clip_use_time_range = true;
  env.scene_config.value_clip_start_time = 0.5;
  env.scene_config.value_clip_end_time = 1.5;
  tydra::RenderSceneConverter converter;

  TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
  TEST_CHECK(scene.animations.size() == 1);

  const auto &anim = scene.animations[0];
  TEST_CHECK(anim.samplers.size() >= 3);
  TEST_CHECK(anim.samplers[0].times.size() == 3);
  TEST_CHECK(anim.channels.size() == 3);

  TEST_CHECK(NearlyEqual(anim.samplers[0].times[0], 0.5f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].times[1], 1.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].times[2], 1.5f));
  TEST_CHECK(NearlyEqual(anim.value_clip_sample_rate, 2.0f));

  // Retimed samples: active window is [0.5, 1.5], which remains in clip0
  // because clip1 starts at stage time 2.0.
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[0], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[3], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[0].values[6], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[2].values[0], 1.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[2].values[6], 1.0f));

  cleanup();
}

void tydra_value_clip_animation_primpath_test(void) {
  std::string err;
  Dictionary clipset;
  clipset["assetPaths"] = std::vector<value::AssetPath>{
      value::AssetPath("clip_0.usda"), value::AssetPath("clip_1.usda")};
  clipset["times"] = std::vector<value::double2>{
      value::double2{0.0, 0.0}, value::double2{1.0, 1.0}};
  clipset["active"] = std::vector<value::double2>{
      value::double2{0.0, 0.0}, value::double2{1.0, 1.0}};
  clipset["interpolateMissingClipValues"] = true;
  clipset["manifestAssetPath"] = value::AssetPath("manifest.usda");
  clipset["primPath"] = value::StringData("/Root");

  std::map<std::string, MetaVariable> clips_dict;
  clips_dict.emplace("default_clip", clipset);

  ClipSetMetadata meta;
  TEST_CHECK(ParseClipSetMetadataFull(clips_dict, &meta, &err));
  TEST_CHECK(err.empty());
  TEST_CHECK(meta.assetPaths.size() == 2);
  TEST_CHECK(meta.times.size() == 2);
  TEST_CHECK(meta.active.size() == 2);
  TEST_CHECK(meta.assetPaths[0] == "clip_0.usda");
  TEST_CHECK(meta.assetPaths[1] == "clip_1.usda");
  TEST_CHECK(meta.primPath == "/Root");
  TEST_CHECK(meta.times[0].first == 0.0);
  TEST_CHECK(meta.times[1].first == 1.0);
  TEST_CHECK(meta.active[0].first == 0.0);
  TEST_CHECK(meta.active[1].second == 1);
  TEST_CHECK(meta.manifestAssetPath == "manifest.usda");
  TEST_CHECK(meta.interpolateMissingClipValues);

  clipset["primPath"] = std::string("/AnimRoot");
  clips_dict.clear();
  clips_dict.emplace("default_clip", clipset);

  err.clear();
  TEST_CHECK(ParseClipSetMetadataFull(clips_dict, &meta, &err));
  TEST_CHECK(meta.primPath == "/AnimRoot");
}

void tydra_value_clip_animation_fallback_test(void) {
  std::string err;
  const std::string clip0_path = MakeTempUSDFilePath("value_clip_fallback_0");
  const std::string clip1_path = MakeTempUSDFilePath("value_clip_fallback_1");
  const std::string scene_path =
      MakeTempUSDFilePath("value_clip_fallback_scene");
  const std::string scene_fallback_path =
      MakeTempUSDFilePath("value_clip_fallback_with_interp_scene");

  const auto base_scene = MakeValueClipSceneUSD(
      clip0_path, clip1_path, {{0.0, 0}, {1.0, 99}, {2.0, 1}},
      {{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}}, false);
  auto fallback_scene = MakeValueClipSceneUSD(
      clip0_path, clip1_path, {{0.0, 0}, {1.0, 99}, {2.0, 1}},
      {{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}}, true);

  const auto temp_files = std::vector<std::string>{clip0_path, clip1_path, scene_path,
                                                  scene_fallback_path};
  auto cleanup = [&]() { CleanupTempFiles(temp_files); };
  const std::array<float, 3> clip0_translate{0.0f, 0.0f, 0.0f};
  const std::array<float, 3> clip0_scale{1.0f, 1.0f, 1.0f};
  const std::array<float, 3> clip1_translate{5.0f, 0.0f, 0.0f};
  const std::array<float, 3> clip1_scale{2.0f, 2.0f, 2.0f};

  TEST_CHECK(WriteTextFile(
      clip0_path,
      MakeValueClipAssetUSD(clip0_translate, clip0_scale)));
  TEST_CHECK(WriteTextFile(
      clip1_path,
      MakeValueClipAssetUSD(clip1_translate, clip1_scale)));
  TEST_CHECK(WriteTextFile(scene_path, base_scene));
  TEST_CHECK(WriteTextFile(scene_fallback_path, fallback_scene));

  {
    Stage stage;
    TEST_CHECK(LoadUSDFromFile(scene_path, &stage, nullptr, &err));
    tydra::RenderScene scene;
    tydra::RenderSceneConverterEnv env(stage);
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.animations.size() == 1);
    const auto &anim = scene.animations[0];
    // Without fallback we expect only the valid entries at t=0 and t=2.
    TEST_CHECK(anim.samplers[0].times.size() == 2);
    TEST_CHECK(anim.samplers[0].values.size() == 6);
    TEST_CHECK(NearlyEqual(anim.samplers[0].values[0], 0.0f));
    TEST_CHECK(NearlyEqual(anim.samplers[0].values[3], 5.0f));
  }

  {
    Stage stage;
    TEST_CHECK(LoadUSDFromFile(scene_fallback_path, &stage, nullptr, &err));
    tydra::RenderScene scene;
    tydra::RenderSceneConverterEnv env(stage);
    // Keep this test explicit for converter output.
    env.scene_config.value_clip_sample_rate = 0.0f;
    tydra::RenderSceneConverter converter;

    TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
    TEST_CHECK(scene.animations.size() == 1);
    const auto &anim = scene.animations[0];
    // With fallback, we expect all 3 samples to be kept.
    TEST_CHECK(anim.samplers[0].times.size() == 3);
    TEST_CHECK(anim.samplers[0].values.size() == 9);
    TEST_CHECK(NearlyEqual(anim.samplers[0].values[3], 0.0f));
    TEST_CHECK(NearlyEqual(anim.samplers[0].values[4], 0.0f));
    TEST_CHECK(NearlyEqual(anim.samplers[0].values[5], 0.0f));
    TEST_CHECK(NearlyEqual(anim.samplers[0].values[6], 5.0f));
    TEST_CHECK(NearlyEqual(anim.samplers[0].values[7], 0.0f));
    TEST_CHECK(NearlyEqual(anim.samplers[0].values[8], 0.0f));
  }

  cleanup();
}

void tydra_value_clip_animation_custom_property_test(void) {
  std::string err;
  const std::string clip0_path = MakeTempUSDFilePath("value_clip_custom_prop_0");
  const std::string clip1_path = MakeTempUSDFilePath("value_clip_custom_prop_1");
  const std::string scene_path = MakeTempUSDFilePath("value_clip_custom_prop_scene");

  const auto temp_files = std::vector<std::string>{clip0_path, clip1_path,
                                                  scene_path};
  auto cleanup = [&]() { CleanupTempFiles(temp_files); };

  const std::array<float, 3> clip0_translate{0.0f, 0.0f, 0.0f};
  const std::array<float, 3> clip0_scale{1.0f, 1.0f, 1.0f};
  const std::array<float, 3> clip1_translate{8.0f, 2.0f, 0.0f};
  const std::array<float, 3> clip1_scale{2.0f, 2.0f, 2.0f};
  const std::vector<std::pair<std::string, std::string>> custom_0 = {
      {"float physics:radius", "1.0"},
      {"float3 physics:traj", "(0.0, 0.0, 0.0)"}
  };
  const std::vector<std::pair<std::string, std::string>> custom_1 = {
      {"float physics:radius", "4.0"},
      {"float3 physics:traj", "(4.0, 2.0, 0.0)"}
  };

  TEST_CHECK(WriteTextFile(
      clip0_path,
      MakeValueClipAssetUSD(clip0_translate, clip0_scale, custom_0)));
  TEST_CHECK(WriteTextFile(
      clip1_path,
      MakeValueClipAssetUSD(clip1_translate, clip1_scale, custom_1)));
  TEST_CHECK(WriteTextFile(
      scene_path,
      MakeValueClipSceneUSD(clip0_path, clip1_path,
                            {{0.0, 0}, {1.0, 0}, {2.0, 1}},
                            {{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}},
                            false)));

  Stage stage;
  TEST_CHECK(LoadUSDFromFile(scene_path, &stage, nullptr, &err));

  tydra::RenderScene scene;
  tydra::RenderSceneConverterEnv env(stage);
  tydra::RenderSceneConverter converter;

  TEST_CHECK(converter.ConvertToRenderScene(env, &scene));
  TEST_CHECK(scene.animations.size() == 1);
  const auto &anim = scene.animations[0];
  TEST_CHECK(anim.channels.size() == 5);

  TEST_CHECK(anim.samplers.size() >= 5);
  TEST_CHECK(anim.samplers[0].times.size() == 3);
  TEST_CHECK(anim.samplers[0].values.size() == 9);
  TEST_CHECK(anim.has_value_clip);
  TEST_CHECK(anim.value_clip_baked);

  // Ensure transform channels are still available.
  bool found_translation = false;
  bool found_rotation = false;
  bool found_scale = false;
  bool found_radius = false;
  bool found_traj = false;
  size_t radius_sampler = 0;
  size_t traj_sampler = 0;
  for (const auto &ch : anim.channels) {
    if (ch.path == tydra::AnimationPath::Translation) {
      found_translation = true;
    } else if (ch.path == tydra::AnimationPath::Rotation) {
      found_rotation = true;
    } else if (ch.path == tydra::AnimationPath::Scale) {
      found_scale = true;
    } else if (ch.path == tydra::AnimationPath::CustomProperty) {
      if (ch.property_name == "physics:radius") {
        found_radius = true;
        radius_sampler = static_cast<size_t>(ch.sampler);
      } else if (ch.property_name == "physics:traj") {
        found_traj = true;
        traj_sampler = static_cast<size_t>(ch.sampler);
      }
    }
  }
  TEST_CHECK(found_translation);
  TEST_CHECK(found_rotation);
  TEST_CHECK(found_scale);
  TEST_CHECK(found_radius);
  TEST_CHECK(found_traj);

  TEST_CHECK(radius_sampler < anim.samplers.size());
  TEST_CHECK(traj_sampler < anim.samplers.size());
  TEST_CHECK(anim.samplers[radius_sampler].times.size() == 3);
  TEST_CHECK(anim.samplers[radius_sampler].values.size() == 3);
  TEST_CHECK(anim.samplers[traj_sampler].times.size() == 3);
  TEST_CHECK(anim.samplers[traj_sampler].values.size() == 9);

  TEST_CHECK(NearlyEqual(anim.samplers[radius_sampler].values[0], 1.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[radius_sampler].values[1], 1.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[radius_sampler].values[2], 4.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[traj_sampler].values[0], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[traj_sampler].values[1], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[traj_sampler].values[2], 0.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[traj_sampler].values[6], 4.0f));
  TEST_CHECK(NearlyEqual(anim.samplers[traj_sampler].values[7], 2.0f));

  cleanup();
}
