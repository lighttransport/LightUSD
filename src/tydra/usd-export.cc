// SPDX-License-Identifier: Apache-2.0
#include "usd-export.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <locale>
#include <ostream>
#include <string>
#include <vector>

#include "export-text.hh"

namespace lightusd {
namespace tydra {
namespace {
std::string Quote(const std::string& text) {
  std::string result = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') {
      result += '\\';
      result += static_cast<char>(c);
    } else if (c == '\n')
      result += "\\n";
    else if (c == '\r')
      result += "\\r";
    else if (c == '\t')
      result += "\\t";
    else if (c == 0)
      result += "\\x00";
    else
      result += static_cast<char>(c);
  }
  return result + '"';
}
const char* Interp(Interpolation i) {
  switch (i) {
    case Interpolation::Constant:
      return "constant";
    case Interpolation::Uniform:
      return "uniform";
    case Interpolation::Vertex:
      return "vertex";
    case Interpolation::Varying:
      return "varying";
    case Interpolation::FaceVarying:
      return "faceVarying";
  }
  return "invalid";
}
struct Exporter {
  const RenderScene& scene;
  std::ostream& out;
  std::string* warning;
  std::string* error;
  bool fail(const char* message) {
    if (error) *error += std::string(message) + '\n';
    return false;
  }
  void warn(const char* message) {
    if (warning) *warning += std::string(message) + '\n';
  }
  template <class T>
  bool number(T value) {
    if (!std::isfinite(static_cast<double>(value)))
      return fail("Non-finite render value");
    out << value;
    return static_cast<bool>(out);
  }
  template <class T>
  bool matrix(const T* data) {
    out << '(';
    for (size_t r = 0; r < 4; r++) {
      if (r) out << ", ";
      out << '(';
      for (size_t c = 0; c < 4; c++) {
        if (c) out << ", ";
        if (!number(data[r * 4 + c])) return false;
      }
      out << ')';
    }
    out << ')';
    return static_cast<bool>(out);
  }
  bool tuple(const Float4& value) {
    out << '(';
    if (!number(value.x)) return false;
    out << ", ";
    if (!number(value.y)) return false;
    out << ", ";
    if (!number(value.z)) return false;
    out << ", ";
    if (!number(value.w)) return false;
    out << ')';
    return static_cast<bool>(out);
  }
  bool material_path(int32_t id) {
    if (id < 0 || static_cast<size_t>(id) >= scene.materials.size())
      return fail("Material index out of range");
    out << "</World/Materials/Material_" << id << '>';
    return true;
  }
  bool parameter(const char* name, const ShaderParam& p, bool color) {
    const char* type =
        color ? (std::string(name) == "normal" ? "normal3f " : "color3f ")
              : "float ";
    out << type << "inputs:" << name << " = ";
    if (color) out << '(';
    if (!number(p.value.x)) return false;
    if (color) {
      out << ", ";
      if (!number(p.value.y)) return false;
      out << ", ";
      if (!number(p.value.z)) return false;
      out << ')';
    }
    out << '\n';
    if (p.is_texture()) {
      if (static_cast<size_t>(p.texture_id) >= scene.textures.size())
        return fail("Texture index out of range");
      const auto channel =
          scene.textures[static_cast<size_t>(p.texture_id)].output_channel;
      const char* scalar = channel == RenderTexture::Channel::G   ? "g"
                           : channel == RenderTexture::Channel::B ? "b"
                           : channel == RenderTexture::Channel::A ? "a"
                                                                  : "r";
      out << type << "inputs:" << name
          << ".connect = </World/Materials/Material_" << active_material
          << "/Texture_" << p.texture_id << ".outputs:"
          << ((color && (channel == RenderTexture::Channel::RGB ||
                         channel == RenderTexture::Channel::RGBA))
                  ? "rgb"
                  : scalar)
          << ">\n";
    }
    return static_cast<bool>(out);
  }
  struct Topology {
    bool triangles = false;
    size_t faces = 0, corners = 0;
    std::vector<size_t> source_faces;
  };
  bool mesh(const RenderMesh& value, size_t id, const Matrix4& transform);
  bool skeletons();
  template <class Array>
  bool array(const Array& values, size_t components = 1) {
    if (values.size() % components) return fail("Incomplete array element");
    out << '[';
    for (size_t i = 0; i < values.size(); i += components) {
      if (i) out << ", ";
      if (components > 1) out << '(';
      for (size_t c = 0; c < components; c++) {
        if (c) out << ", ";
        if (!number(values[i + c])) return false;
      }
      if (components > 1) out << ')';
    }
    out << ']';
    return static_cast<bool>(out);
  }
  template <class Array>
  bool attribute(const char* type, const std::string& name, const Array& values,
                 size_t components, Interpolation interpolation,
                 const RenderMesh& mesh, const Topology& topology,
                 const UInt32Chunked* indices = nullptr,
                 size_t source_components = 0, size_t component_offset = 0) {
    if (values.empty()) return true;
    const size_t stride = source_components ? source_components : components;
    if (!components || !stride || values.size() % stride ||
        component_offset > stride || components > stride - component_offset)
      return fail("Incomplete vertex attribute");
    if (static_cast<unsigned>(interpolation) >
        static_cast<unsigned>(Interpolation::Varying))
      return fail("Invalid vertex interpolation");
    size_t count = interpolation == Interpolation::Constant  ? 1
                   : interpolation == Interpolation::Uniform ? topology.faces
                   : interpolation == Interpolation::FaceVarying
                       ? topology.corners
                       : mesh.point_count();
    out << type << "[] " << name << " = [";
    for (size_t element = 0; element < count; element++) {
      size_t source = element;
      if (interpolation == Interpolation::Uniform)
        source = topology.source_faces[element];
      if (interpolation == Interpolation::FaceVarying && topology.triangles) {
        if (mesh.triangulated_face_vertex_indices.size() == topology.corners)
          source = mesh.triangulated_face_vertex_indices[element];
        else if (!mesh.face_vertex_indices.empty())
          return fail("Missing source corner mapping");
      }
      if (indices && !indices->empty()) {
        if (source >= indices->size()) return fail("Invalid primvar index");
        source = (*indices)[source];
      }
      if (source >= values.size() / stride)
        return fail("Vertex attribute index out of range");
      if (element) out << ", ";
      if (components > 1) out << '(';
      for (size_t c = 0; c < components; c++) {
        if (c) out << ", ";
        const size_t row = components == 9 ? 3 : 4;
        if (components > 4 && c % row == 0) out << '(';
        if (!number(values[source * stride + component_offset + c]))
          return false;
        if (components > 4 && c % row == row - 1) out << ')';
      }
      if (components > 1) out << ')';
    }
    out << "] (interpolation=" << Quote(Interp(interpolation)) << ")\n";
    return static_cast<bool>(out);
  }
  size_t active_material = 0;
  bool materials() {
    out << "def Scope \"Materials\" {\n";
    for (size_t i = 0; i < scene.materials.size(); i++) {
      active_material = i;
      const auto& material = scene.materials[i];
      out << "def Material \"Material_" << i
          << "\" (displayName=" << Quote(material.name) << ") {\n"
          << "token outputs:surface.connect = </World/Materials/Material_" << i
          << "/Surface.outputs:surface>\n"
          << "def Shader \"Surface\" {\nuniform token info:id = "
             "\"UsdPreviewSurface\"\n";
      PreviewSurfaceShader shader;
      if (material.preview_surface)
        shader = *material.preview_surface;
      else if (material.openpbr) {
        const auto& p = *material.openpbr;
        shader.diffuse_color = p.base_color;
        shader.metallic = p.base_metalness;
        shader.roughness = p.specular_roughness;
        shader.ior = p.specular_ior;
        shader.clearcoat = p.coat_weight;
        shader.clearcoat_roughness = p.coat_roughness;
        shader.opacity = p.opacity;
        shader.normal = p.normal;
        shader.emissive_color = p.emission_color;
        if (!p.emission_luminance.is_texture() &&
            !shader.emissive_color.is_texture()) {
          shader.emissive_color.value.x *= p.emission_luminance.value.x;
          shader.emissive_color.value.y *= p.emission_luminance.value.x;
          shader.emissive_color.value.z *= p.emission_luminance.value.x;
        }
        warn("OpenPBR material approximated by UsdPreviewSurface");
      }
      if (!parameter("diffuseColor", shader.diffuse_color, true) ||
          !parameter("emissiveColor", shader.emissive_color, true) ||
          !parameter("specularColor", shader.specular_color, true) ||
          !parameter("metallic", shader.metallic, false) ||
          !parameter("roughness", shader.roughness, false) ||
          !parameter("opacity", shader.opacity, false) ||
          !parameter("opacityThreshold", shader.opacity_threshold, false) ||
          !parameter("clearcoat", shader.clearcoat, false) ||
          !parameter("clearcoatRoughness", shader.clearcoat_roughness, false) ||
          !parameter("ior", shader.ior, false) ||
          !parameter("occlusion", shader.occlusion, false) ||
          !parameter("normal", shader.normal, true) ||
          !parameter("displacement", shader.displacement, false))
        return false;
      out << "int inputs:useSpecularWorkflow = "
          << (shader.use_specular_workflow ? 1 : 0)
          << "\ntoken outputs:surface\n}\n";
      std::vector<int32_t> textures;
      for (const auto* p :
           {&shader.diffuse_color, &shader.emissive_color,
            &shader.specular_color, &shader.metallic, &shader.roughness,
            &shader.opacity, &shader.opacity_threshold, &shader.clearcoat,
            &shader.clearcoat_roughness, &shader.ior, &shader.occlusion,
            &shader.normal, &shader.displacement})
        if (p->is_texture() && std::find(textures.begin(), textures.end(),
                                         p->texture_id) == textures.end())
          textures.push_back(p->texture_id);
      for (int32_t id : textures) {
        const auto& texture = scene.textures[static_cast<size_t>(id)];
        std::string file = texture.asset_path;
        if (file.empty() && texture.image_id >= 0 &&
            static_cast<size_t>(texture.image_id) < scene.images.size())
          file = scene.images[static_cast<size_t>(texture.image_id)]
                     .asset_identifier;
        if (file.empty() ||
            (file.find_first_of("\r\n") != std::string::npos ||
             file.find("@@@") != std::string::npos) ||
            file.find('\0') != std::string::npos)
          return fail("Invalid external texture asset path");
        const auto wrap = [](WrapMode mode) {
          switch (mode) {
            case WrapMode::Repeat:
              return "repeat";
            case WrapMode::Clamp:
              return "clamp";
            case WrapMode::Mirror:
              return "mirror";
            case WrapMode::Black:
              return "black";
          }
          return "clamp";
        };
        out << "def Shader \"Texture_" << id
            << "\" {\nuniform token info:id = \"UsdUVTexture\"\nasset "
               "inputs:file = @@@"
            << file << "@@@\n"
            << "token inputs:sourceColorSpace = "
            << Quote(texture.source_color_space)
            << "\ntoken inputs:wrapS = " << Quote(wrap(texture.wrap_s))
            << "\ntoken inputs:wrapT = " << Quote(wrap(texture.wrap_t)) << '\n'
            << "float2 inputs:st.connect = </World/Materials/Material_" << i
            << "/UVTransform_" << id
            << ".outputs:result>\nfloat3 outputs:rgb\nfloat outputs:r\nfloat "
               "outputs:g\nfloat outputs:b\nfloat outputs:a\n";
        out << "float4 inputs:scale=";
        if (!tuple(texture.scale_value)) return false;
        out << "\nfloat4 inputs:bias=";
        if (!tuple(texture.bias)) return false;
        out << "\n}\n";
        out << "def Shader \"UV_" << id
            << "\" {\nuniform token info:id = "
               "\"UsdPrimvarReader_float2\"\nstring inputs:varname = "
            << Quote(texture.uv_primvar.empty() ? "st" : texture.uv_primvar)
            << "\nfloat2 outputs:result\n}\n"
            << "def Shader \"UVTransform_" << id
            << "\" {\nuniform token info:id = \"UsdTransform2d\"\nfloat2 "
               "inputs:in.connect = </World/Materials/Material_"
            << i << "/UV_" << id << ".outputs:result>\nfloat2 inputs:scale = (";
        if (!number(texture.scale.x)) return false;
        out << ", ";
        if (!number(texture.scale.y)) return false;
        out << ")\nfloat2 inputs:translation = (";
        if (!number(texture.offset.x)) return false;
        out << ", ";
        if (!number(texture.offset.y)) return false;
        out << ")\nfloat inputs:rotation = ";
        if (!number(texture.rotation * 57.29577951308232)) return false;
        out << "\nfloat2 outputs:result\n}\n";
      }
      out << "}\n";
    }
    out << "}\n";
    return static_cast<bool>(out);
  }
};
}  // namespace
namespace {
bool Identifier(const std::string& value, bool namespaces = false) {
  if (value.empty()) return false;
  bool first = true;
  for (unsigned char c : value) {
    if (c == ':' && namespaces) {
      if (first) return false;
      first = true;
      continue;
    }
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
          (!first && c >= '0' && c <= '9')))
      return false;
    first = false;
  }
  return !first;
}
bool Exporter::mesh(const RenderMesh& mesh, size_t id,
                    const Matrix4& transform) {
  Topology t;
  t.triangles = !mesh.triangulated_indices.empty();
  const auto& indices =
      t.triangles ? mesh.triangulated_indices : mesh.face_vertex_indices;
  t.corners = indices.size();
  t.faces = t.triangles ? t.corners / 3 : mesh.face_vertex_counts.size();
  if (mesh.points.size() % 3 || (t.triangles && t.corners % 3))
    return fail("Invalid mesh topology");
  t.source_faces.resize(t.faces);
  size_t count = 0;
  for (size_t f = 0; f < t.faces; f++) {
    t.source_faces[f] = f;
    const size_t corners = t.triangles ? 3 : mesh.face_vertex_counts[f];
    if (corners < 3 || corners > indices.size() - count)
      return fail("Invalid face counts");
    count += corners;
  }
  if (count != indices.size()) return fail("Face counts do not match indices");
  for (uint32_t vertex : indices)
    if (vertex >= mesh.point_count()) return fail("Mesh vertex out of range");
  if (t.triangles && !mesh.face_triangle_offsets.empty()) {
    const auto& ranges = mesh.face_triangle_offsets;
    if (ranges.size() != mesh.face_count() + 1 || ranges.front() != 0 ||
        ranges.back() != t.faces)
      return fail("Invalid triangle face mapping");
    for (size_t f = 0; f + 1 < ranges.size(); f++) {
      if (ranges[f] > ranges[f + 1] || ranges[f + 1] > t.faces)
        return fail("Invalid triangle face mapping");
      for (size_t i = ranges[f]; i < ranges[f + 1]; i++) t.source_faces[i] = f;
    }
  }
  out << "def Mesh \"Mesh_" << id << "\" (displayName=" << Quote(mesh.name);
  if (mesh.skin || !mesh.blend_shapes.empty())
    out << "; prepend apiSchemas=[\"SkelBindingAPI\"]";
  out << ") {\nuniform token subdivisionScheme=\"none\"\n"
      << "uniform token orientation="
      << Quote(t.triangles || !mesh.left_handed ? "rightHanded" : "leftHanded")
      << "\nbool doubleSided=" << (mesh.double_sided ? "true" : "false") << '\n'
      << "matrix4d xformOp:transform = ";
  if (!matrix(transform.m)) return false;
  out << "\nuniform token[] xformOpOrder=[\"xformOp:transform\"]\npoint3f[] "
         "points=";
  if (!array(mesh.points, 3)) return false;
  out << "\nint[] faceVertexCounts=[";
  for (size_t f = 0; f < t.faces; f++) {
    if (f) out << ", ";
    out << (t.triangles ? 3 : mesh.face_vertex_counts[f]);
  }
  out << "]\nint[] faceVertexIndices=";
  if (!array(indices)) return false;
  out << '\n';
  if (!t.triangles && !mesh.hole_faces.empty()) {
    for (uint32_t f : mesh.hole_faces)
      if (f >= t.faces) return fail("Invalid hole face");
    out << "int[] holeIndices=";
    if (!array(mesh.hole_faces)) return false;
    out << '\n';
  }
  if (!attribute("normal3f", "normals", mesh.normals, 3, mesh.normals_interp,
                 mesh, t))
    return false;
  const std::string uv0 =
      mesh.texcoords_0_name.empty() ? "st" : mesh.texcoords_0_name;
  const std::string uv1 =
      mesh.texcoords_1_name.empty() ? "st1" : mesh.texcoords_1_name;
  if (!Identifier(uv0, true) || !Identifier(uv1, true))
    return fail("Invalid UV primvar name");
  if (!attribute("texCoord2f", "primvars:" + uv0, mesh.texcoords_0, 2,
                 mesh.texcoords_0_interp, mesh, t) ||
      !attribute("texCoord2f", "primvars:" + uv1, mesh.texcoords_1, 2,
                 mesh.texcoords_1_interp, mesh, t))
    return false;
  const size_t color_elements =
      mesh.colors_interp == Interpolation::Constant ? 1
      : mesh.colors_interp == Interpolation::Uniform
          ? (mesh.face_count() ? mesh.face_count() : t.faces)
      : mesh.colors_interp == Interpolation::FaceVarying
          ? (mesh.face_vertex_indices.empty() ? t.corners
                                              : mesh.face_vertex_indices.size())
          : mesh.point_count();
  const size_t color_stride =
      color_elements <= std::numeric_limits<size_t>::max() / 4 &&
              mesh.colors.size() == color_elements * 4
          ? 4
          : 3;
  if (!attribute("color3f", "primvars:displayColor", mesh.colors, 3,
                 mesh.colors_interp, mesh, t, nullptr, color_stride))
    return false;
  if (color_stride == 4 && !mesh.colors.empty()) {
    // Combine RGBA alpha with separately authored opacity in output-corner
    // order, so differing interpolation modes retain their rendered product.
    out << "float[] primvars:displayOpacity=[";
    size_t corner = 0;
    const auto source_index = [&](Interpolation interpolation, size_t face,
                                  size_t c, size_t vertex) {
      if (interpolation == Interpolation::Constant) return size_t{0};
      if (interpolation == Interpolation::Uniform) return t.source_faces[face];
      if (interpolation == Interpolation::FaceVarying)
        return t.triangles && !mesh.triangulated_face_vertex_indices.empty()
                   ? static_cast<size_t>(
                         mesh.triangulated_face_vertex_indices[c])
                   : c;
      return vertex;
    };
    for (size_t f = 0; f < t.faces; f++)
      for (size_t k = 0; k < (t.triangles ? 3 : mesh.face_vertex_counts[f]);
           k++, corner++) {
        const size_t vertex = indices[corner];
        const size_t color =
            source_index(mesh.colors_interp, f, corner, vertex);
        if (color >= mesh.colors.size() / 4)
          return fail("Color alpha index out of range");
        float opacity = 1;
        if (!mesh.opacities.empty()) {
          const size_t index =
              source_index(mesh.opacities_interp, f, corner, vertex);
          if (index >= mesh.opacities.size())
            return fail("Opacity index out of range");
          opacity = mesh.opacities[index];
        }
        if (corner) out << ", ";
        if (!number(mesh.colors[color * 4 + 3] * opacity)) return false;
      }
    out << "] (interpolation=\"faceVarying\")\n";
  } else if (!attribute("float", "primvars:displayOpacity", mesh.opacities, 1,
                        mesh.opacities_interp, mesh, t))
    return false;
  for (const auto& primvar : mesh.primvars) {
    if (!Identifier(primvar.name, true))
      return fail("Invalid custom primvar name");
    // VertexFormat groups are scalar, vec2/3/4 for float, signed and unsigned.
    const char* type = nullptr;
    size_t components = 0;
    bool floats = false, signed_values = false;
    switch (primvar.format) {
      case VertexFormat::Float:
        type = "float";
        components = 1;
        floats = true;
        break;
      case VertexFormat::Vec2:
        type = "float2";
        components = 2;
        floats = true;
        break;
      case VertexFormat::Vec3:
        type = "float3";
        components = 3;
        floats = true;
        break;
      case VertexFormat::Vec4:
        type = "float4";
        components = 4;
        floats = true;
        break;
      case VertexFormat::Int:
        type = "int";
        components = 1;
        signed_values = true;
        break;
      case VertexFormat::IVec2:
        type = "int2";
        components = 2;
        signed_values = true;
        break;
      case VertexFormat::IVec3:
        type = "int3";
        components = 3;
        signed_values = true;
        break;
      case VertexFormat::IVec4:
        type = "int4";
        components = 4;
        signed_values = true;
        break;
      case VertexFormat::UInt:
        type = "uint";
        components = 1;
        break;
      case VertexFormat::UVec2:
        type = "uint2";
        components = 2;
        break;
      case VertexFormat::UVec3:
        type = "uint3";
        components = 3;
        break;
      case VertexFormat::UVec4:
        type = "uint4";
        components = 4;
        break;
      case VertexFormat::Matrix33:
        type = "matrix3d";
        components = 9;
        floats = true;
        break;
      case VertexFormat::Matrix44:
        type = "matrix4d";
        components = 16;
        floats = true;
        break;
      default:
        return fail("Invalid custom primvar format");
    }
    const std::string name = "primvars:" + primvar.name;
    if (floats) {
      if (!attribute(type, name, primvar.float_data, components,
                     primvar.interpolation, mesh, t, &primvar.indices))
        return false;
    } else if (signed_values) {
      if (!attribute(type, name, primvar.int_data, components,
                     primvar.interpolation, mesh, t, &primvar.indices))
        return false;
    } else if (!attribute(type, name, primvar.uint_data, components,
                          primvar.interpolation, mesh, t, &primvar.indices))
      return false;
  }
  if (mesh.material_id >= 0) {
    out << "rel material:binding=";
    if (!material_path(mesh.material_id)) return false;
    out << '\n';
  }
  for (size_t s = 0; s < mesh.material_subsets.size(); s++) {
    const auto& subset = mesh.material_subsets[s];
    const bool tri_ranges = t.triangles && !mesh.face_triangle_offsets.empty();
    const size_t limit = tri_ranges ? t.faces : mesh.face_count();
    if (subset.face_start > limit ||
        subset.face_count > limit - subset.face_start)
      return fail("Invalid subset face range");
    out << "def GeomSubset \"Subset_" << s
        << "\" (displayName=" << Quote(subset.name)
        << ") {\nuniform token elementType=\"face\"\nuniform token "
           "familyName=\"materialBind\"\nint[] indices=[";
    bool first = true;
    for (size_t f = 0; f < t.faces; f++) {
      const size_t source = tri_ranges ? f : t.source_faces[f];
      if (source >= subset.face_start &&
          source - subset.face_start < subset.face_count) {
        if (!first) out << ", ";
        out << f;
        first = false;
      }
    }
    out << "]\nrel material:binding=";
    if (!material_path(subset.material_id)) return false;
    out << "\n}\n";
  }
  if (mesh.skin) {
    const auto& skin = *mesh.skin;
    if (!skin.influences_per_vertex ||
        skin.joint_indices.size() != skin.joint_weights.size() ||
        skin.joint_weights.size() / skin.influences_per_vertex !=
            mesh.point_count() ||
        skin.joint_weights.size() % skin.influences_per_vertex)
      return fail("Invalid skin weights");
    if (skin.skeleton_id >= 0) {
      if (static_cast<size_t>(skin.skeleton_id) >= scene.skeletons.size())
        return fail("Invalid skeleton binding");
      out << "rel skel:skeleton=</World/Skeleton_" << skin.skeleton_id << ">\n";
    }
    out << "int[] primvars:skel:jointIndices=";
    if (!array(skin.joint_indices)) return false;
    out << " (interpolation=\"vertex\"; elementSize="
        << skin.influences_per_vertex
        << ")\nfloat[] primvars:skel:jointWeights=";
    if (!array(skin.joint_weights)) return false;
    out << " (interpolation=\"vertex\"; elementSize="
        << skin.influences_per_vertex
        << ")\nmatrix4d primvars:skel:geomBindTransform=";
    if (!matrix(skin.geom_bind_transform.m)) return false;
    out << '\n';
  }
  if (!mesh.blend_shapes.empty()) {
    out << "uniform token[] skel:blendShapes=[";
    for (size_t b = 0; b < mesh.blend_shapes.size(); b++) {
      if (b) out << ", ";
      out << Quote(mesh.blend_shapes[b].name);
    }
    out << "]\nrel skel:blendShapeTargets=[";
    for (size_t b = 0; b < mesh.blend_shapes.size(); b++) {
      if (b) out << ", ";
      out << "</World/Mesh_" << id << "/Shape_" << b << '>';
    }
    out << "]\n";
    for (size_t b = 0; b < mesh.blend_shapes.size(); b++) {
      const auto& shape = mesh.blend_shapes[b];
      const size_t elements = shape.point_indices.empty()
                                  ? mesh.point_count()
                                  : shape.point_indices.size();
      if (shape.point_offsets.size() != elements * 3 ||
          (!shape.normal_offsets.empty() &&
           shape.normal_offsets.size() != elements * 3))
        return fail("Invalid blend shape offsets");
      for (uint32_t point : shape.point_indices)
        if (point >= mesh.point_count())
          return fail("Invalid blend shape point index");
      out << "def BlendShape \"Shape_" << b
          << "\" (displayName=" << Quote(shape.name)
          << ") {\nvector3f[] offsets=";
      if (!array(shape.point_offsets, 3)) return false;
      out << '\n';
      if (!shape.normal_offsets.empty()) {
        out << "vector3f[] normalOffsets=";
        if (!array(shape.normal_offsets, 3)) return false;
        out << '\n';
      }
      if (!shape.point_indices.empty()) {
        out << "int[] pointIndices=";
        if (!array(shape.point_indices)) return false;
        out << '\n';
      }
      for (size_t j = 0; j < shape.inbetweens.size(); j++) {
        const auto& inbetween = shape.inbetweens[j];
        if (inbetween.point_offsets.size() != elements * 3)
          return fail("Invalid inbetween offsets");
        out << "vector3f[] inbetweens:Inbetween_" << j << '=';
        if (!array(inbetween.point_offsets, 3)) return false;
        out << " (weight=";
        if (!number(inbetween.weight)) return false;
        out << ")\n";
      }
      out << "}\n";
    }
  }
  out << "}\n";
  return static_cast<bool>(out);
}
bool Exporter::skeletons() {
  for (size_t s = 0; s < scene.skeletons.size(); s++) {
    const auto& skeleton = scene.skeletons[s];
    out << "def Skeleton \"Skeleton_" << s
        << "\" (displayName=" << Quote(skeleton.name)
        << ") {\nuniform token[] joints=[";
    for (size_t j = 0; j < skeleton.joints.size(); j++) {
      if (j) out << ", ";
      out << Quote(skeleton.joints[j].path);
    }
    out << "]\nuniform token[] jointNames=[";
    for (size_t j = 0; j < skeleton.joints.size(); j++) {
      if (j) out << ", ";
      out << Quote(skeleton.joints[j].name);
    }
    out << "]\nuniform matrix4d[] bindTransforms=[";
    for (size_t j = 0; j < skeleton.joints.size(); j++) {
      if (j) out << ", ";
      if (!matrix(skeleton.joints[j].bind_transform.m)) return false;
    }
    out << "]\nuniform matrix4d[] restTransforms=[";
    for (size_t j = 0; j < skeleton.joints.size(); j++) {
      if (j) out << ", ";
      if (!matrix(skeleton.joints[j].rest_transform.m)) return false;
    }
    out << "]\n}\n";
  }
  return static_cast<bool>(out);
}
}  // namespace
bool export_to_usda(const RenderScene& scene, std::string& output,
                    std::string* warning, std::string* error,
                    size_t max_output_bytes) {
  export_detail::TextBuffer buffer(max_output_bytes);
  std::ostream out(&buffer);
  out.imbue(std::locale::classic());
  out.precision(std::numeric_limits<double>::max_digits10);
  Exporter e{scene, out, warning, error};
  // The previous exporter explicitly rejected animation clips as unsupported.
  if (!scene.animations.empty())
    return e.fail("RenderScene animation export is not implemented");
  out << "#usda 1.0\n(defaultPrim=\"World\"; upAxis="
      << Quote(scene.up_axis == RenderScene::UpAxis::Z ? "Z" : "Y")
      << "; metersPerUnit=";
  if (!e.number(scene.meters_per_unit)) return false;
  out << ")\ndef SkelRoot \"World\" {\n";
  if (!e.materials() || !e.skeletons())
    return e.fail("USDA output limit exceeded or invalid render data");
  std::vector<bool> used(scene.meshes.size());
  size_t instance = 0;
  for (const auto& node : scene.nodes)
    if (node.type == NodeType::Mesh) {
      if (node.data_id < 0 ||
          static_cast<size_t>(node.data_id) >= scene.meshes.size())
        return e.fail("Invalid mesh node");
      const size_t mesh = static_cast<size_t>(node.data_id);
      used[mesh] = true;
      if (!e.mesh(scene.meshes[mesh], instance++, node.world_transform))
        return e.fail("USDA output limit exceeded or invalid render data");
    }
  for (size_t m = 0; m < scene.meshes.size(); m++)
    if (!used[m] && !e.mesh(scene.meshes[m], instance++, Matrix4::Identity()))
      return e.fail("USDA output limit exceeded or invalid render data");
  out << "}\n";
  if (!out) return e.fail("USDA output limit exceeded");
  if (!scene.points.empty() || !scene.curves.empty() || !scene.lights.empty() ||
      !scene.cameras.empty() || !scene.volumes.empty())
    e.warn(
        "Non-mesh render resources are omitted from the diagnostic mesh "
        "export");
  output.swap(buffer.text);
  return true;
}
}  // namespace tydra
}  // namespace lightusd
