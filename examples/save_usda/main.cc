// TODO: merge into api-tutorial example.
#include "usda-writer.hh"
#include "usdGeom.hh"  // Xform/GeomMesh (no longer re-exported by lightusd.hh)

#include <iostream>

//
// create simple scene composed of Xform and Mesh.
//
void SimpleScene(lightusd::legacy::Stage *stage)
{
  //
  // lightusd currently does not provide scene construction API yet, so edit parameters directly.
  //
  // LightUSD does not use mutex, smart pointers(e.g. shared_ptr) and C++ exception.
  // API is not multi-thread safe, thus if you need to manipulate a scene in multi-threaded context,
  // The app must take care of resource locks in app layer.
  //
  lightusd::legacy::Xform xform;
  xform.name = "root";

  lightusd::legacy::XformOp op;
  op.op_type = lightusd::legacy::XformOp::OpType::Translate;
  lightusd::legacy::value::double3 translate;
  translate[0] = 1.0;
  translate[1] = 2.0;
  translate[2] = 3.0;
  op.set_value(std::move(translate));

  xform.xformOps.push_back(op);

  lightusd::legacy::GeomMesh mesh;
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

  // primvar and custom attribute can be added to generic Property container `props`
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
      //lightusd::legacy::primvar::PrimVar uvVar;
      //uvVar.set_value(uvs);
      //uvAttr.set_var(std::move(uvVar));

      // Currently `interpolation` is described in Attribute metadataum.
      uvAttr.metas().set_interpolation_enum(lightusd::legacy::Interpolation::Vertex);

      lightusd::legacy::Property uvProp(uvAttr, /* custom*/false);

      mesh.props.emplace("primvars:uv", uvProp);

      // ----------------------

      lightusd::legacy::Attribute uvIndexAttr;
      std::vector<int> uvIndices;

      // FIXME: Validate
      uvIndices.push_back(0);
      uvIndices.push_back(1);
      uvIndices.push_back(2);
      uvIndices.push_back(3);


      lightusd::legacy::primvar::PrimVar uvIndexVar;
      uvIndexVar.set_value(std::move(uvIndices));
      uvIndexAttr.set_var(std::move(uvIndexVar));

      lightusd::legacy::Property uvIndexProp(uvIndexAttr, /* custom*/false);
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

      lightusd::legacy::Property prop(attrib, /* custom*/true);

      mesh.props.emplace("myvalue", prop);
    }

  }

  lightusd::legacy::Prim meshPrim(mesh);
  lightusd::legacy::Prim xformPrim(xform);

  // [Xform]
  //  |
  //  +- [Mesh]
  //
  xformPrim.children().emplace_back(std::move(meshPrim));

  stage->root_prims().emplace_back(std::move(xformPrim));
}

int main(int argc, char **argv)
{
  lightusd::legacy::Stage stage; // empty scene

  SimpleScene(&stage);

  std::string warn;
  std::string err;
  bool ret = lightusd::legacy::usda::SaveAsUSDA("output.usda", stage, &warn, &err);

  if (warn.size()) {
    std::cout << "WARN: " << warn << "\n";
  }

  if (err.size()) {
    std::cerr << "ERR: " << err << "\n";
  }

  return ret ? EXIT_SUCCESS : -1;
}
