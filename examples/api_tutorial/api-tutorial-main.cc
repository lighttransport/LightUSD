#include <iostream>

// All-in-one LightUSD core, but pprint feature is not included to speed up compilation.
#include "lightusd.hh"

// Import to_string() and operator<< features
#include "pprinter.hh"
#include "prim-pprint.hh"
#include "value-pprint.hh"

#if defined(LIGHTUSD_WITH_TYDRA)

// Tydra is a collection of APIs to access/convert USD Prim data
// (e.g. Get Prim's attribute by name)
// See <lightusd>/examples/tydra_api for more info about Tydra API.
#include "tydra/scene-access.hh"

#endif

//
// create a Scene
//
void CreateScene(lightusd::legacy::Stage *stage) {
  // LightUSD API does not use mutex, smart pointers(e.g. unique_ptr/shared_ptr) and C++
  // exception. Also threading is opional in LightUSD and currently
  // multi-threading is not used. `value::token` implementation is simply copy
  // string for each instances(like C++11 ABI std::string). (on the contrary,
  // pxrUSD's token implementation uses string-to-id, which requires mutex lock
  // to make stringId unique. But we think this is inefficient on modern
  // multi-core CPU, since they locks the mutex every `token` value
  // instanciation)
  //
  // LightUSD's API is not fully multi-thread safe, thus if you want to
  // manipulate a scene(Stage) in multi-threaded context, The app must take care
  // of resource locks in the app layer.

  std::string err;

  //
  // Create simple material with UsdPrevieSurface.
  //
  lightusd::legacy::Material mat;
  mat.name = "mat";

  lightusd::legacy::Shader shader;  // Shader container
  shader.name = "defaultPBR";
  {
    lightusd::legacy::UsdPreviewSurface surfaceShader;  // Concrete Shader node object

    //
    // Asssign actual shader object to Shader::value.
    // Also do not forget set its shader node type name through Shader::info_id
    //
    shader.info_id = lightusd::legacy::kUsdPreviewSurface;  // "UsdPreviewSurface" token

    //
    // Currently no shader network/connection API.
    // Manually construct it.
    //
    surfaceShader.outputsSurface.set_authored(
        true);  // Author `token outputs:surface`

    surfaceShader.metallic = 0.3f;
    // TODO: UsdUVTexture, UsdPrimvarReader***, UsdTransform2d

    // Connect to UsdPreviewSurface's outputs:surface by setting targetPath.
    //
    // token outputs:surface = </mat/defaultPBR.outputs:surface>
    mat.surface.set(lightusd::legacy::Path(/* prim path */ "/mat/defaultPBR",
                                   /* prop path */ "outputs:surface"));

    //
    // Shaer::value is `value::Value` type, so can use '=' to assign Shader
    // object.
    //
    shader.value = std::move(surfaceShader);
  }

  lightusd::legacy::Prim shaderPrim(shader);
  lightusd::legacy::Prim matPrim(mat);

  // matPrim.children().emplace_back(std::move(shaderPrim)); // no uniqueness
  // check

  // Use add_child() to ensure child Prim has unique name.
  if (!matPrim.add_child(std::move(shaderPrim),
                         /* rename Prim name if_required */ true, &err)) {
    std::cerr << "Failed to constrcut Scene: " << err << "\n";
    exit(-1);
  }

  //
  // To construct Prim, first create concrete Prim object(e.g. Xform, GeomMesh),
  // then add it to lightusd::legacy::Prim.
  //
  //
  lightusd::legacy::Xform xform;
  {
    xform.name = "root";  // Prim's name(elementPath)

    {
      lightusd::legacy::XformOp op;
      op.op_type = lightusd::legacy::XformOp::OpType::Transform;
      lightusd::legacy::value::matrix4d a0;
      lightusd::legacy::value::matrix4d b0;

      lightusd::legacy::Identity(&a0);
      lightusd::legacy::Identity(&b0);

      a0.m[1][1] = 2.1;

      // column major, so [3][0], [3][1], [3][2] = translate X, Y, Z
      b0.m[3][0] = 1.0;
      b0.m[3][1] = 3.1;
      b0.m[3][2] = 5.1;

      lightusd::legacy::value::matrix4d transform = a0 * b0;

      op.set_value(std::move(transform));

      // `xformOpOrder`(token[]) is represented as std::vector<XformOp>
      xform.xformOps.push_back(op);
    }

    {
      // `xformOp:***` attribute is represented as XformOp class
      lightusd::legacy::XformOp op;
      op.op_type = lightusd::legacy::XformOp::OpType::Translate;
      lightusd::legacy::value::double3 translate;
      translate[0] = 1.0;
      translate[1] = 2.0;
      translate[2] = 3.0;
      op.set_value(std::move(translate));

      // `xformOpOrder`(token[]) is represented as std::vector<XformOp>
      xform.xformOps.push_back(op);
    }

    {
      // .suffix will be prepended to `xformOp:translate`
      // 'xformOp:translate:move'
      lightusd::legacy::XformOp op;
      op.op_type = lightusd::legacy::XformOp::OpType::Translate;
      op.suffix = "move";
      lightusd::legacy::value::double3 translate;

      // TimeSamples value can be added with `set_timesample`
      // NOTE: TimeSamples data will be automatically sorted by time when using
      // it.
      translate[0] = 0.0;
      translate[1] = 0.0;
      translate[2] = 0.0;
      op.set_timesample(0.0, translate);

      translate[0] = 1.0;
      translate[1] = 0.1;
      translate[2] = 0.3;
      op.set_timesample(1.0, translate);

      xform.xformOps.push_back(op);
    }
  }

  lightusd::legacy::GeomMesh mesh;
  {
    mesh.name = "quad";

    {
      std::vector<lightusd::legacy::value::point3f> pts;
      pts.push_back({0.0f, 0.0f, 0.0f});
      pts.push_back({1.0f, 0.0f, 0.0f});
      pts.push_back({1.0f, 1.0f, 0.0f});
      pts.push_back({0.0f, 1.0f, 0.0f});

      mesh.points.set_value(std::move(pts));
    }

    {
      // quad plane composed of 2 triangles.
      std::vector<int> indices;
      std::vector<int> counts;
      counts.push_back(3);
      counts.push_back(3);
      mesh.faceVertexCounts.set_value(std::move(counts));

      indices.push_back(0);
      indices.push_back(1);
      indices.push_back(2);

      indices.push_back(0);
      indices.push_back(2);
      indices.push_back(3);

      mesh.faceVertexIndices.set_value(std::move(indices));
    }

    // primvar and custom attribute can be added to generic Property container
    // `props`(map<std::string, Property>)
    {
      // primvar is simply an attribute with prefix `primvars:`
      //
      // texCoord2f[] primvars:uv = [ ... ] ( interpolation = "vertex" )
      // int[] primvars:uv:indices = [ ... ]
      //
      {
        lightusd::legacy::Attribute uvAttr;
        std::vector<lightusd::legacy::value::texcoord2f> uvs;

        uvs.push_back({0.0f, 0.0f});
        uvs.push_back({1.0f, 0.0f});
        uvs.push_back({1.0f, 1.0f});
        uvs.push_back({0.0f, 1.0f});

        // Fast path. Set the value directly to Attribute.
        uvAttr.set_value(std::move(uvs));

        // or we can first build primvar::PrimVar
        // lightusd::legacy::primvar::PrimVar uvVar;
        // uvVar.set_value(uvs);
        // uvAttr.set_var(std::move(uvVar));

        // Currently `interpolation` is described in Attribute metadataum.
        // You can set builtin(predefined) Attribute Metadatum(e.g.
        // `interpolation`, `hidden`) through `metas()`.
        uvAttr.metas().set_interpolation_enum(lightusd::legacy::Interpolation::Vertex);

        lightusd::legacy::Property uvProp(uvAttr);

        mesh.props.emplace("primvars:uv", uvProp);

        // ----------------------

        lightusd::legacy::Attribute uvIndexAttr;
        std::vector<int> uvIndices;

        // FIXME: Validate
        uvIndices.push_back(0);
        uvIndices.push_back(1);
        uvIndices.push_back(3);
        uvIndices.push_back(2);

        lightusd::legacy::primvar::PrimVar uvIndexVar;
        uvIndexVar.set_value(std::move(uvIndices));
        uvIndexAttr.set_var(std::move(uvIndexVar));
        // Or you can use this approach(if you want to keep a copy of PrimVar
        // data)
        // uvIndexAttr.set_var(uvIndexVar);

        lightusd::legacy::Property uvIndexProp(uvIndexAttr);
        mesh.props.emplace("primvars:uv:indices", uvIndexProp);
      }

      // `custom uniform double myvalue = 3.0 ( hidden = 0 )`
      {
        lightusd::legacy::Attribute attrib;
        double myvalue = 3.0;
        lightusd::legacy::primvar::PrimVar var;
        var.set_value(myvalue);
        attrib.set_var(std::move(var));
        attrib.variability() = lightusd::legacy::Variability::Uniform;

        attrib.metas().set_hidden(false);

        // NOTE: `custom` keyword would be deprecated in the future USD syntax,
        // so you can set it false.
        lightusd::legacy::Property prop(attrib, /* custom*/ true);

        mesh.props.emplace("myvalue", prop);
      }

      // Add Primvar through GeomPrimvar;
      {
        lightusd::legacy::GeomPrimvar uvPrimvar;

        uvPrimvar.set_name("my_uv");
        std::vector<lightusd::legacy::value::texcoord2f> uvs;

        uvs.push_back({0.0f, 0.0f});
        uvs.push_back({1.0f, 0.0f});
        uvs.push_back({1.0f, 1.0f});
        uvs.push_back({0.0f, 1.0f});

        uvPrimvar.set_value(std::move(uvs)); // value at 'default' time
        uvPrimvar.set_interpolation(lightusd::legacy::Interpolation::Vertex);

        std::vector<int> uvIndices;
        uvIndices.push_back(0);
        uvIndices.push_back(1);
        uvIndices.push_back(3);
        uvIndices.push_back(2);

        // set indices at 'default' time.
        uvPrimvar.set_default_indices(uvIndices);

        // NOTE: You can use `uvPrimvar.set_timesampled_indices` to set time-varying indices
        // {
        //   lightusd::TypedTimeSamples<std::vector<int>> uvIndicesTimeSampled;
        //   std::vector<int> uvIndices3;
        //   uvIndices3.push_back(1);
        //   uvIndices3.push_back(0);
        //   uvIndices3.push_back(2);
        //   uvIndices3.push_back(3);
        //   uvIndicesTimeSampled.add_sample(1.0, uvIndices);
        //   uvIndicesTimeSampled.add_sample(3.0, uvIndices3);
        //   uvPrimvar.set_timesampled_indices(uvIndicesTimeSampled);
        // }

        // primvar name is extracted from Primvar::name
        std::string err;
        if (!mesh.set_primvar(uvPrimvar, &err)) {
          std::cerr << "Failed to add Primar: " << err << "\n";
        }
      }
    }

    // Material binding(`rel material:binding` is done by manually setup
    // targetPath.
    lightusd::legacy::Relationship materialBinding;
    materialBinding.set(
        lightusd::legacy::Path(/* prim path */ "/mat/defaultPBR", /* prop path */ ""));

    mesh.materialBinding = materialBinding;

    // TODO: Explicitly author MaterialBindingAPI apiSchema in Mesh Prim.
  }

  lightusd::legacy::GeomSphere sphere1;
  {
    sphere1.name = "sphere";

    sphere1.radius = 3.14;
  }

  lightusd::legacy::GeomSphere sphere2;
  {
    sphere2.name =
        "sphere";  // name will be modified to be unique at add_child().
    sphere2.radius = 1.05;
  }

  //
  // Create Scene(Stage) hierarchy.
  // You can add child Prim to parent Prim's `children()`.
  //
  // [Xform]
  //  |
  //  +- [Mesh]
  //  +- [Sphere0]
  //  +- [Sphere1]
  //
  // [Material]
  //  |
  //  +- [Shader]
  //

  // Prim's elementName is read from concrete Prim class(GeomMesh::name,
  // Xform::name, ...)
  lightusd::legacy::Prim meshPrim(mesh);
  {
    // `references`, `paylaod`

    {
      lightusd::legacy::Reference ref;
      ref.asset_path = lightusd::legacy::value::AssetPath("submesh-000.usd");

      std::vector<lightusd::legacy::Reference> referencesList;
      referencesList.push_back(ref);

      meshPrim.metas().references = std::vector<std::pair<lightusd::legacy::ListEditQual, std::vector<lightusd::legacy::Reference>>>{
          std::make_pair(lightusd::legacy::ListEditQual::Append, referencesList)};
    }

    {
      lightusd::legacy::Payload pl;
      pl.asset_path = lightusd::legacy::value::AssetPath("submesh-payload-000.usd");

      std::vector<lightusd::legacy::Payload> payloadList;
      payloadList.push_back(pl);

      meshPrim.metas().payload = std::vector<std::pair<lightusd::legacy::ListEditQual, std::vector<lightusd::legacy::Payload>>>{
          std::make_pair(lightusd::legacy::ListEditQual::Append, payloadList)};
    }
  }

  lightusd::legacy::Prim spherePrim(sphere1);
  {
    // variantSet is maniuplated in Prim.
    // Currently we don't provide easy API for variantSet.
    // Need to add variantSet information manually for a while.
    lightusd::legacy::VariantSelectionMap vsmap;

    // List of variantSet in the Prim.
    std::vector<std::string> variantSetList;
    variantSetList.push_back("colorVariant");

    // key = variantSet name, value = default Variant selection
    vsmap.emplace("colorVariant", "red");

    spherePrim.metas().variants = vsmap;
    spherePrim.metas().variantSets = std::vector<std::pair<lightusd::legacy::ListEditQual, std::vector<std::string>>>{
        std::make_pair(lightusd::legacy::ListEditQual::Append, variantSetList)};

    // VariantSet is composed of metas + properties + childPrims
    lightusd::legacy::VariantSet variantSet;

    lightusd::legacy::Variant redVariant;
    redVariant.metas().set_comment("red color");
    lightusd::legacy::value::color3f redColor({1.0f, 0.0f, 0.0f});
    lightusd::legacy::Attribute redColorAttr;
    redColorAttr.set_value(std::move(redColor));
    redVariant.properties().emplace("mycolor", redColorAttr);
    // TODO: Add example to add childPrims under Variant
    // redVariant.primChildren().emplace(...)

    lightusd::legacy::Variant greenVariant;
    greenVariant.metas().set_comment("green color");
    lightusd::legacy::value::color3f greenColor({0.0f, 1.0f, 0.0f});
    lightusd::legacy::Attribute greenColorAttr;
    greenColorAttr.set_value(std::move(greenColor));
    greenVariant.properties().emplace("mycolor", greenColorAttr);

    variantSet.name = "red";
    variantSet.variantSet.emplace("red", redVariant);

    variantSet.name = "green";
    variantSet.variantSet.emplace("green", greenVariant);

    spherePrim.variantSets().emplace("colorVariant", variantSet);
  }

  lightusd::legacy::Prim spherePrim2(sphere2);

  lightusd::legacy::Prim xformPrim(xform);

  // xformPrim.children().emplace_back(std::move(meshPrim));
  // xformPrim.children().emplace_back(std::move(spherePrim));

  // Use add_child() to ensure child Prim has unique name.
  if (!xformPrim.add_child(std::move(meshPrim),
                           /* rename Prim name if_required */ true, &err)) {
    std::cerr << "Failed to constrcut Scene: " << err << "\n";
    exit(-1);
  }

  if (!xformPrim.add_child(std::move(spherePrim),
                           /* rename Prim name if_required */ true, &err)) {
    std::cerr << "Failed to constrcut Scene: " << err << "\n";
  }

#if 1
  // Must set rename Prim arg `true`, otherwise `add_child` fails since
  // spherePrim2 does not have valid & unique Prim name.
  if (!xformPrim.add_child(std::move(spherePrim2),
                           /* rename Prim name if_required */ true, &err)) {
    std::cerr << "Failed to constrcut Scene: " << err << "\n";
    exit(-1);
  }

  if (xformPrim.children().size() != 3) {
    std::cerr << "Internal error. num child Prims must be 3, but got "
              << xformPrim.children().size() << "\n";
    exit(-1);
  }

  // If you want to specify the appearance/traversal order of child Prim(e.g.
  // Showing Prim tree in GUI, Ascii output), set "primChildren"(token[])
  // metadata xfromPrim.metas().primChildren.size() must be identical to
  // xformPrim.children().size()
  xformPrim.metas().primChildren.push_back(
      lightusd::legacy::value::token(xformPrim.children()[1].element_name()));
  xformPrim.metas().primChildren.push_back(
      lightusd::legacy::value::token(xformPrim.children()[0].element_name()));
  xformPrim.metas().primChildren.push_back(
      lightusd::legacy::value::token(xformPrim.children()[2].element_name()));
#else
  // You can replace(or add if corresponding Prim does not exist) existing child
  // Prim using replace_child()
  if (!xformPrim.replace_child("sphere", std::move(spherePrim2), &err)) {
    std::cerr << "Failed to constrcut Scene: " << err << "\n";
    exit(-1);
  }

  if (xformPrim.children().size() != 2) {
    std::cerr << "Internal error. num child Prims must be 2, but got "
              << xformPrim.children().size() << "\n";
    exit(-1);
  }

#endif

  ///
  /// Add subLayers
  ///
  std::vector<lightusd::legacy::SubLayer> sublayers;
  lightusd::legacy::SubLayer slayer0;
  slayer0.assetPath = lightusd::legacy::value::AssetPath("sublayer-000.usd");
  sublayers.push_back(slayer0);
  stage->metas().subLayers = sublayers;
  stage->metas().defaultPrim =
      lightusd::legacy::value::token(xformPrim.element_name());  // token

  if (!stage->add_root_prim(std::move(xformPrim))) {
    std::cerr << "Failed to add Prim to Stage root: " << stage->get_error()
              << "\n";
    exit(-1);
  }

  if (!stage->add_root_prim(std::move(matPrim))) {
    std::cerr << "Failed to add Prim to Stage root: " << stage->get_error()
              << "\n";
    exit(-1);
  }

  // You can replace(or add if given Prim name does not exist in the Stage) root
  // Prim using `replace_root_prim`.
#if 0
  {
    lightusd::legacy::Scope scope;
    lightusd::legacy::Prim scopePrim(scope);
    if (!stage->replace_root_prim(/* root Prim name */"root", std::move(scopePrim))) {
      std::cerr << "Failed to replace Prim to Stage root: " << stage->get_error() << "\n";
      exit(-1);
    }
  }
#endif

  // You can add Stage metadatum through Stage::metas()
  stage->metas().comment = "Generated by LightUSD api_tutorial.";

  {
    // Dictionary(alias to CustomDataType) is similar to VtDictionary.
    // it is a map<string, MetaVariable>
    // MetaVariable is similar to Value, but accepts limited variation of
    // types(double, token, string, float3[], ...)
    lightusd::legacy::Dictionary customData;

    lightusd::legacy::MetaVariable metavar;
    double mycustom = 1.3;
    metavar.set_value("mycustom", mycustom);

    std::string mystring = "hello";
    lightusd::legacy::MetaVariable metavar2("mystring", mystring);

    customData.emplace("mycustom", metavar);
    customData.emplace("mystring", metavar2);
    customData.emplace("myvalue", 2.45);  // Construct MetaVariables implicitly

    // You can also use SetCustomDataByKey to set custom value with key having
    // namespaces(':')

    lightusd::legacy::MetaVariable intval = int(5);
    lightusd::legacy::SetCustomDataByKey("mydict:myval", intval,
                                 /* inout */ customData);

    stage->metas().customLayerData = customData;
  }

  // Commit Stage.
  // Internally, it calls Stage::compute_absolute_prim_path_and_assign_prim_id()
  // to compute absolute Prim path and assign unique Prim id to each Prims in
  // the Stage. NOTE: Stage::metas() is not affected by `commit` API, so you can
  // call `commit` before manipulating StageMetas through `Stage::metas()`
  if (!stage->commit()) {
    std::cerr << "Failed to commit Stage. ERR: " << stage->get_error() << "\n";
    exit(-1);
  }
}

int main(int argc, char **argv) {
  lightusd::legacy::Stage stage;  // empty scene

  CreateScene(&stage);

  if (stage.get_warning().size()) {
    std::cout << "WARN in Stage: " << stage.get_warning() << "\n";
  }

  // Print USD scene as Ascii.
  std::cout << to_string(stage) << "\n";
  // std::cout << stage.ExportToString() << "\n"; // you can also use pxrUSD
  // compatible ExportToString().

  // Dump Prim tree info.
  std::cout << stage.dump_prim_tree() << "\n";

  {
    lightusd::legacy::Path path(/* absolute prim path */ "/root",
                        /* property path */ "");

    const lightusd::legacy::Prim *prim{nullptr};
    std::string err;
    bool ret = stage.find_prim_at_path(path, prim, &err);
    if (ret) {
      std::cout << "Found Prim at path: " << lightusd::legacy::to_string(path) << "\n";
      std::cout << "Prim ID: " << prim->prim_id() << "\n";
      std::cout << "Prim's absolute_path: "
                << lightusd::legacy::to_string(prim->absolute_path()) << "\n";
    } else {
      std::cerr << err << "\n";
    }

    if (!prim) {
      // This should not be happen though.
      std::cerr << "Prim is null\n";
      return -1;
    }

    if (!prim->is<lightusd::legacy::Xform>()) {
      std::cerr << "Expected Xform prim."
                << "\n";
      return -1;
    }

    // Cast to Xform
    const lightusd::legacy::Xform *xform = prim->as<lightusd::legacy::Xform>();
    if (!xform) {
      std::cerr << "Expected Xform prim."
                << "\n";
      return -1;
    }
  }

  // find Prim by prim_id
  {
    uint64_t prim_id = 2;

    const lightusd::legacy::Prim *prim{nullptr};
    std::string err;
    bool ret = stage.find_prim_by_prim_id(prim_id, prim, &err);
    if (ret && prim) {
      std::cout << "Found Prim by ID: " << prim_id << "\n";
      std::cout << "Prim's absolute_path: "
                << lightusd::legacy::to_string(prim->absolute_path()) << "\n";
    } else {
      std::cerr << err << "\n";
    }
  }

  // GetAttribute and GeomPrimvar
  {
    lightusd::legacy::Path path(/* absolute prim path */ "/root/quad",
                        /* property path */ "");

    const lightusd::legacy::Prim *prim{nullptr};
    std::string err;
    bool ret = stage.find_prim_at_path(path, prim, &err);
    if (ret && prim) {
      std::cout << "Found Prim at path: " << lightusd::legacy::to_string(path) << "\n";
      std::cout << "Prim ID: " << prim->prim_id() << "\n";
      std::cout << "Prim's absolute_path: "
                << lightusd::legacy::to_string(prim->absolute_path()) << "\n";
    } else {
      std::cerr << err << "\n";
    }

    if (!prim) {
      // This should not be happen though.
      std::cerr << "Prim is null\n";
      return -1;
    }

    const lightusd::legacy::GeomMesh *mesh = prim->as<lightusd::legacy::GeomMesh>();
    if (!mesh) {
      std::cerr << "Expected GeomMesh.\n";
      return -1;
    }

#if defined(LIGHTUSD_WITH_TYDRA)
    lightusd::legacy::Attribute attr;
    // TODO: Use EvaluateAttribute
    if (lightusd::tydra::GetAttribute(*prim, "points", &attr, &err)) {
      std::cout << "point attribute type = " << attr.type_name() << "\n";

      // Ensure Attribute has a value(not Attribute connection)
      if (attr.is_value()) {
        std::vector<lightusd::legacy::value::point3f> pts;
        if (attr.is_timesamples()) {
          // TODO: timesamples
        } else {
          if (attr.get_value(&pts)) {
            std::cout << "point attribute value = " << pts << "\n";
          }
        }
      }

    } else {
      std::cerr << err << "\n";
    }
#else
    // limitation: `points` attribute cannot be attribute connection
    std::cout << "point attribute value = " << mesh->get_points() << "\n";
#endif

    // GeomPrimvar
    // Access GeomPrimvar
    {
      std::cout << "uv is primvar? " << mesh->has_primvar("uv") << "\n";
      lightusd::legacy::GeomPrimvar primvar;
      std::string err;
      if (mesh->get_primvar("uv", &primvar, &err)) {
        std::cout << "uv primvar has default indices? " << primvar.has_default_indices()
                  << "\n";
        std::cout << "uv primvar has timesampled indices? " << primvar.has_timesampled_indices()
                  << "\n";
      } else {
        std::cerr << "get_primvar(\"uv\") failed. err = " << err << "\n";
      }

      // Equivalent to pxr::UsdGeomPrimvar::ComputeFlattened().
      // elems[i] = values[indices[i]]
      lightusd::legacy::value::Value value;
      if (primvar.flatten_with_indices(&value, &err)) {
        // value;:Value can contain any types, but value.array_size() should
        // work well only for primvar types(e.g. `float[]`, `color3f[]`) It
        // would report 0 for non-primvar types(e.g.`std::vector<Xform>`)
        std::cout << "uv primvars. array size = " << value.array_size() << "\n";
        std::cout << "uv primvars. expand_by_indices result = "
                  << lightusd::legacy::value::pprint_value(value) << "\n";
      } else {
        std::cerr << "expand_by_indices failed. err = " << err << "\n";
      }

      // Typed version
      std::vector<lightusd::legacy::value::texcoord2f> uvs;
      if (primvar.flatten_with_indices(&uvs, &err)) {
        // value;:Value can contain any types, but value.array_size() should
        // work well only for primvar types(e.g. `float[]`, `color3f[]`) It
        // would report 0 for non-primvar types(e.g.`std::vector<Xform>`)
        std::cout << "uv primvars. array size = " << uvs.size() << "\n";
        std::cout << "uv primvars. expand_by_indices result = "
                  << lightusd::legacy::value::pprint_value(uvs) << "\n";
      } else {
        std::cerr << "expand_by_indices failed. err = " << err << "\n";
      }

      std::vector<lightusd::legacy::GeomPrimvar> gpvars = mesh->get_primvars();
      std::cout << "# of primvars = " << gpvars.size();
      for (const auto &item : gpvars) {
        std::cout << "  primvar = " << item.name() << "\n";
      }
    }
  }

  return EXIT_SUCCESS;
}
