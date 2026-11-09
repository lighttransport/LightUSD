#include <algorithm>
#include <iostream>
#include <sstream>

#include "lightusd.hh"
#include "tydra/attribute-eval.hh"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"
#include "tydra/shader-network.hh"
#include "usdShade.hh"
#include "pprinter.hh"
#include "prim-pprint.hh"
#include "value-pprint.hh"
#include "value-types.hh"

static std::string GetFileExtension(const std::string &filename) {
  if (filename.find_last_of('.') != std::string::npos)
    return filename.substr(filename.find_last_of('.') + 1);
  return "";
}

static std::string str_tolower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 // static_cast<int(*)(int)>(std::tolower)         // wrong
                 // [](int c){ return std::tolower(c); }           // wrong
                 // [](char c){ return std::tolower(c); }          // wrong
                 [](unsigned char c) { return std::tolower(c); }  // correct
  );
  return s;
}

// key = Full absolute prim path(e.g. `/bora/dora`)
using XformMap = lightusd::legacy::tydra::PathPrimMap<lightusd::legacy::Xform>;
using MeshMap = lightusd::legacy::tydra::PathPrimMap<lightusd::legacy::GeomMesh>;
using MaterialMap = lightusd::legacy::tydra::PathPrimMap<lightusd::legacy::Material>;
using PreviewSurfaceMap =
    std::map<std::string, std::pair<const lightusd::legacy::Shader *, const lightusd::legacy::UsdPreviewSurface *>>;
using UVTextureMap = std::map<std::string, std::pair<const lightusd::legacy::Shader *, const lightusd::legacy::UsdUVTexture *>>;
using PrimvarReader_float2Map =
    std::map<std::string, std::pair<const lightusd::legacy::Shader *, const lightusd::legacy::UsdPrimvarReader_float2 *>>;

#if 0
template <typename T>
static bool TraverseRec(const std::string &path_prefix,
                        const lightusd::legacy::Prim &prim, uint32_t depth,
                        std::map<std::string, const T *> &itemmap) {
  if (depth > 1024 * 128) {
    // Too deep
    return false;
  }

  std::string prim_abs_path = path_prefix + "/" + prim.local_path().full_path_name();

  if (prim.is<lightusd::legacy::Material>()) {
    if (const T *pv = prim.as<T>()) {
      std::cout << "Path : <" << prim_abs_path << "> is "
                << lightusd::legacy::value::TypeTraits<T>::type_name() << ".\n";
      itemmap[prim_abs_path] = pv;
    }
  }

  for (const auto &child : prim.children()) {
    if (!TraverseRec(prim_abs_path, child, depth + 1, itemmap)) {
      return false;
    }
  }

  return true;
}

template <typename T>
static bool TraverseShaderRec(const std::string &path_prefix,
                        const lightusd::legacy::Prim &prim, uint32_t depth,
                        std::map<std::string, const T *> &itemmap) {
  if (depth > 1024 * 128) {
    // Too deep
    return false;
  }

  std::string prim_abs_path = path_prefix + "/" + prim.local_path().full_path_name();

  // First test if Shader prim.
  if (const lightusd::legacy::Shader *ps = prim.as<lightusd::legacy::Shader>()) {
    // Concrete Shader object(e.g. UsdUVTexture) is stored in .data.
    if (const T *s = ps->value.as<T>()) {
      std::cout << "Path : <" << prim_abs_path << "> is "
                << lightusd::legacy::value::TypeTraits<T>::type_name() << ".\n";
      itemmap[prim_abs_path] = s;
    }
  }

  for (const auto &child : prim.children()) {
    if (!TraverseShaderRec(prim_abs_path, child, depth + 1, itemmap)) {
      return false;
    }
  }

  return true;
}

static void TraverseMaterial(const lightusd::legacy::Stage &stage, MaterialMap &m) {
  for (const auto &prim : stage.GetRootPrims()) {
    TraverseRec(/* root */ "", prim, 0, m);
  }
}

static void TraversePreviewSurface(const lightusd::legacy::Stage &stage,
                                   PreviewSurfaceMap &m) {
  for (const auto &prim : stage.GetRootPrims()) {
    TraverseShaderRec(/* root */ "", prim, 0, m);
  }
}

static void TraverseUVTexture(const lightusd::legacy::Stage &stage, UVTextureMap &m) {
  for (const auto &prim : stage.GetRootPrims()) {
    TraverseShaderRec(/* root */ "", prim, 0, m);
  }
}

static void TraversePrimvarReader_float2(const lightusd::legacy::Stage &stage,
                                         PrimvarReader_float2Map &m) {
  for (const auto &prim : stage.GetRootPrims()) {
    TraverseShaderRec(/* root */ "", prim, 0, m);
  }
}
#endif

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cout << "Need USD file with Material/Shader(e.g. `<lightusd>/models/texturescube.usda`)\n" << std::endl;
    return EXIT_FAILURE;
  }

  std::string filepath = argv[1];
  std::string warn;
  std::string err;

  std::string ext = str_tolower(GetFileExtension(filepath));

  lightusd::legacy::Stage stage;

  if (!lightusd::legacy::IsUSD(filepath)) {
    std::cerr << "File not found or not a USD format: " << filepath << "\n";
  }

  bool ret = lightusd::legacy::LoadUSDFromFile(filepath, &stage, &warn, &err);
  if (!warn.empty()) {
    std::cerr << "WARN : " << warn << "\n";
  }

  if (!err.empty()) {
    std::cerr << "ERR : " << err << "\n";
  }

  if (!ret) {
    std::cerr << "Failed to load USD file: " << filepath << "\n";
    return EXIT_FAILURE;
  }

  // bool is_usdz = lightusd::legacy::IsUSDZ(filepath);

  std::string s = stage.ExportToString();
  std::cout << s << "\n";
  std::cout << "--------------------------------------"
            << "\n";

  // Visit all Prims in the Stage.
  auto prim_visit_fun = [](const lightusd::legacy::Path &abs_path, const lightusd::legacy::Prim &prim, const int32_t level, void *userdata, std::string *err) -> bool {
    (void)err;
    std::cout << lightusd::legacy::pprint::Indent(level) << "[" << level << "] (" << prim.data().type_name() << ") " << prim.local_path().prim_part() << " : AbsPath " << lightusd::legacy::to_string(abs_path) << "\n";

    // Use as() or is() for Prim specific processing.
    if (const lightusd::legacy::Material *pm = prim.as<lightusd::legacy::Material>()) {
      (void)pm;
      std::cout << lightusd::legacy::pprint::Indent(level) << "  Got Material!\n";
      // return false + `err` empty if you want to terminate traversal earlier.
      //return false;
    }

    return true;
  };

  void *userdata = nullptr;

  lightusd::legacy::tydra::VisitPrims(stage, prim_visit_fun, userdata);


  std::cout << "--------------------------------------"
            << "\n";

  // Compute Xform of each Prim at time t.
  {
    lightusd::legacy::tydra::XformNode xformnode;
    double t = lightusd::legacy::value::TimeCode::Default();
    lightusd::legacy::value::TimeSampleInterpolationType tinterp = lightusd::legacy::value::TimeSampleInterpolationType::Held; // Held or Linear

    if (!lightusd::legacy::tydra::BuildXformNodeFromStage(stage, &xformnode, t, tinterp)) {
      std::cerr << "BuildXformNodeFromStage error.\n";
    } else {
      std::cout << lightusd::legacy::tydra::DumpXformNode(xformnode) << "\n";
    }
  }


  // Mapping hold the pointer to concrete Prim object,
  // So stage content should not be changed(no Prim addition/deletion).
  XformMap xformmap;
  MeshMap meshmap;
  MaterialMap matmap;
  PreviewSurfaceMap surfacemap;
  UVTextureMap texmap;
  PrimvarReader_float2Map preadermap;

  // Collect and make full path <-> Prim/Shader mapping
  lightusd::legacy::tydra::ListPrims(stage, xformmap);
  lightusd::legacy::tydra::ListPrims(stage, meshmap);
  lightusd::legacy::tydra::ListPrims(stage, matmap);
  lightusd::legacy::tydra::ListShaders(stage, surfacemap);
  lightusd::legacy::tydra::ListShaders(stage, texmap);
  lightusd::legacy::tydra::ListShaders(stage, preadermap);

  //
  // Query example
  //
  for (const auto &item : matmap) {
    nonstd::expected<const lightusd::legacy::Prim*, std::string> mat = stage.GetPrimAtPath(lightusd::legacy::Path(item.first, /* prop name */""));
    if (mat) {
      std::cout << "Found Material <" << item.first << "> from Stage:\n";
      if (const lightusd::legacy::Material *mp = mat.value()->as<lightusd::legacy::Material>()) { // this should be true though.
        std::cout << lightusd::legacy::to_string(*mp) << "\n";
      }
    } else {
      std::cerr << "Err: " << mat.error() << "\n";
    }
  }

  for (const auto &item : surfacemap) {
    // Returned Prim is Shader class
    nonstd::expected<const lightusd::legacy::Prim*, std::string> shader = stage.GetPrimAtPath(lightusd::legacy::Path(item.first, /* prop name */""));
    if (shader) {
      std::cout << "Found Shader(UsdPreviewSurface) <" << item.first << "> from Stage:\n";

      const lightusd::legacy::Shader *sp = shader.value()->as<lightusd::legacy::Shader>();
      if (sp) { // this should be true though.

        if (const lightusd::legacy::UsdPreviewSurface *surf = sp->value.as<lightusd::legacy::UsdPreviewSurface>()) {
          // Print Shader
          std::cout << lightusd::legacy::to_string(*sp) << "\n";
        }
      }

    } else {
      std::cerr << "Err: " << shader.error() << "\n";
    }
  }

  for (const auto &item : texmap) {
    // Returned Prim is Shader class
    nonstd::expected<const lightusd::legacy::Prim*, std::string> shader = stage.GetPrimAtPath(lightusd::legacy::Path(item.first, /* prop name */""));
    if (shader) {
      std::cout << "Found Shader(UsdUVTexture) <" << item.first << "> from Stage:\n";

      const lightusd::legacy::Shader *sp = shader.value()->as<lightusd::legacy::Shader>();
      if (sp) { // this should be true though.
        if (const lightusd::legacy::UsdUVTexture *tex = sp->value.as<lightusd::legacy::UsdUVTexture>()) {
          std::cout << lightusd::legacy::to_string(*sp);
        }
      }

    } else {
      std::cerr << "Err: " << shader.error() << "\n";
    }
  }

  for (const auto &item : preadermap) {
    // Returned Prim is Shader class
    nonstd::expected<const lightusd::legacy::Prim*, std::string> shader = stage.GetPrimAtPath(lightusd::legacy::Path(item.first, /* prop name */""));
    if (shader) {
      std::cout << "Found Shader(UsdPrimvarReader_float2) <" << item.first << "> from Stage:\n";

      const lightusd::legacy::Shader *sp = shader.value()->as<lightusd::legacy::Shader>();
      if (sp) { // this should be true though.

        if (const lightusd::legacy::UsdPrimvarReader_float2 *preader = sp->value.as<lightusd::legacy::UsdPrimvarReader_float2>()) {
          std::cout << lightusd::legacy::to_string(*sp) << "\n";
        }
      }

    } else {
      std::cerr << "Err: " << shader.error() << "\n";
    }
  }

  //
  // -- Querying Parent Prim
  //
  for (const auto &item : surfacemap) {
    // Usually Parent is Material
    std::string err;
    if (const lightusd::legacy::Prim *p = lightusd::legacy::tydra::GetParentPrim(stage, lightusd::legacy::Path(item.first, /* property */""), &err)) {
      std::cout << "Input path = " << lightusd::legacy::to_string(item.first) << "\n";
      std::cout << "Parent prim = " << lightusd::legacy::prim::print_prim(*p) << "\n";
    } else {
      std::cerr << err;
    }
  }

  std::cout << "GetProperty example -------------\n";
  for (const auto &item : xformmap) {
    std::string err;
    lightusd::legacy::Property prop;
    if (lightusd::legacy::tydra::GetProperty(*item.second, "xformOp:transform", &prop, &err)) {
      std::cout << "Property value = " << print_prop(prop, "xformOp:transform", 0) << "\n";
    } else {
      std::cerr << err;
    }
  }


  //
  // Get bound Material
  //
  // - GetBoundMaterial:
  //   - Look Directly boundMaterial to the Prim.
  //   - If no material is asigned to the Prim, look into parent's BoundMaterial settings.
  //   - Also considers BindMaterial strength `bindMaterialAs` Prim metadatum
  //   - [ ] TODO: Consider materialBindingCollection
  // - GetDirectlyBoundMaterial: Get BoundMaterial assigned to the Prim being queried.
  //
  // Usually GetBoundMaterial is what you want.
  //
  // See https://openusd.org/release/wp_usdshade.html for more details about Material assignment.
  //
  std::cout << "GetBoundMaterial example -------------\n";
  for (const auto &item : meshmap) {

    lightusd::legacy::Path matPath;
    const lightusd::legacy::Material *material{nullptr};
    std::string err;

    bool ret = lightusd::legacy::tydra::GetDirectlyBoundMaterial(stage, lightusd::legacy::Path(item.first, ""), /* purpose */"", &matPath, &material, &err);
    std::cout << "Prim : " << item.first << "\n";
    if (ret) {
      std::cout << "has directlyBoundMaterial: " << matPath.full_path_name() << "\n";
    } else if (err.empty()) {
      std::cout << "no directlyBoundMaterial" << "\n";
    } else {
      std::cout << "GetDirectlyBoundMaterial failed: " << err << "\n";
    }

    ret = lightusd::legacy::tydra::GetBoundMaterial(stage, lightusd::legacy::Path(item.first, ""), /* purpose */"", &matPath, &material, &err);

    if (ret) {
      std::cout << item.first << " has bound Material. Material Path = " << matPath << "\n";
      std::cout << "mat = " << material << "\n";
      if (material) {
        std::cout << to_string(*material, /* indent */1) << "\n";
      }
    } else {
      std::cout << "Bound material not found for Prim path : " << item.first << "\n";
    }
  }



  //
  // Shader attribute evaluation example.
  //
  std::cout << "EvaluateAttribute example -------------\n";
  for (const auto &item : preadermap) {
    // Returned Prim is Shader class
    nonstd::expected<const lightusd::legacy::Prim*, std::string> shader = stage.GetPrimAtPath(lightusd::legacy::Path(item.first, /* prop name */""));
    if (shader) {
      std::cout << "Shader(UsdPrimvarReader_float2) <" << item.first << "> from Stage:\n";

      const lightusd::legacy::Shader *sp = shader.value()->as<lightusd::legacy::Shader>();
      if (sp) { // this should be true though.

        if (const lightusd::legacy::UsdPrimvarReader_float2 *preader = sp->value.as<lightusd::legacy::UsdPrimvarReader_float2>()) {

          lightusd::legacy::tydra::TerminalAttributeValue tav;
          std::string err;
          bool ret = lightusd::legacy::tydra::EvaluateAttribute(stage, *shader.value(), "inputs:varname", &tav, &err);

          if (!ret) {
            std::cout << "Resolving `inputs:varname` failed: " << err << "\n";
          }

          std::cout << "type = " << tav.type_name() << "\n";

          if (auto pv = tav.as<lightusd::legacy::value::token>()) {
            std::cout << "inputs:varname = " << (*pv) << "\n";
          }
        }
      }

    } else {
      std::cerr << "Err: " << shader.error() << "\n";
    }
  }



  return 0;
}
