// SPDX-License-Identifier: Apache-2.0
// Core USD Stage -> retained RenderScene diagnostics and debug exports.
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

#include "image-loader.hh"
#include "load-usd.hh"
#include "reader/usdz-reader.hh"
#include "render-diagnostics.hh"
#include "tydra/mesh-index.hh"
#include "tydra/obj-export.hh"
#include "tydra/render-converter.hh"
#include "tydra/usd-export.hh"
#include "writer/usda-writer.hh"

namespace {
namespace r = lightusd::tydra;
using render_example::Json;
struct Options {
  std::string file, format = "yaml", tangent = "lengyel";
  bool triangulate = true, fan = false, indices = true, texload = false,
       noar = false, usdprint = false;
  bool obj = false, usd = false, times = false, nodump = false, memstat = false,
       notangent = false, force_tangent = false;
  bool mmap = false, lowmem = false, snorm8 = false, fast = false,
       profile = false, payloads = true, animation = true;
  int threads = 1;
  double time = std::numeric_limits<double>::quiet_NaN();
};
void help(const char* name) {
  std::cout
      << "Usage: " << name << " input.usda/usdc/usdz [OPTIONS]\n"
      << "  --timecode VALUE      Evaluate at a numeric time (default: "
         "authored defaults)\n"
      << "  --noidxbuild          Do not unify vertex indices\n  --notri       "
         "        Preserve polygon topology\n  --trifan              Use "
         "triangle-fan triangulation\n"
      << "  --texload             Decode external/archive textures\n  --noar   "
         "             Disable external asset resolution and texture decoding\n"
      << "  --usdprint            Print the composed USD stage\n  --dumpobj    "
         "         Export OBJ and MTL files per mesh\n  --dumpusd             "
         "Export the RenderScene as USDA\n"
      << "  --dump-timesamples    Print all animation channel samples\n  "
         "--nodump              Suppress the JSON/YAML scene dump\n  --memstat "
         "            Print memory and resource counts\n"
      << "  --notangent           Disable tangent computation\n  --calctangent "
         "        Force tangent computation without a normal map\n  "
         "--tangent-method M    lengyel, mikktspace, fast-mikktspace, hybrid\n"
      << "  --yaml | --json       Diagnostic format (default: YAML)\n  "
         "--mmap-lowmem         Enable mmap for trusted local USDC input\n  "
         "--lowmem              Release the source Stage after conversion\n"
      << "  --snorm8              Retain normals/tangents as packed signed "
         "bytes\n  --fast-index-build    Expand corners without "
         "deduplication\n"
      << "  --compose-threads N   Composition worker count\n  "
         "--compose-threads-auto Use hardware concurrency\n"
      << "  --load-payloads | --defer-payloads\n  --no-animation        Skip "
         "animation extraction\n  --profile             Print load and "
         "conversion timings\n";
}
bool numeric(const char* text, double* value) {
  char* end = nullptr;
  errno = 0;
  *value = std::strtod(text, &end);
  return end != text && end && !*end && !errno && std::isfinite(*value);
}
bool parse(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; i++) {
    const std::string flag = argv[i];
    if (flag == "--help" || flag == "-h") {
      help(argv[0]);
      std::exit(0);
    }
    if (flag == "--timecode" || flag == "--compose-threads" ||
        flag == "--tangent-method") {
      if (++i >= argc) {
        std::cerr << "Missing value for " << flag << '\n';
        return false;
      }
      if (flag == "--tangent-method") {
        o->tangent = argv[i];
        if (o->tangent != "lengyel" && o->tangent != "mikktspace" &&
            o->tangent != "fast-mikktspace" && o->tangent != "hybrid") {
          std::cerr << "Invalid tangent method\n";
          return false;
        }
      } else {
        double value = 0;
        if (!numeric(argv[i], &value)) {
          std::cerr << "Invalid number for " << flag << '\n';
          return false;
        }
        if (flag == "--timecode")
          o->time = value;
        else {
          if (value < 1 || value > std::numeric_limits<int>::max() ||
              std::floor(value) != value) {
            std::cerr << "Invalid composition thread count\n";
            return false;
          }
          o->threads = static_cast<int>(value);
        }
      }
    } else if (flag == "--notri")
      o->triangulate = false;
    else if (flag == "--trifan")
      o->fan = true;
    else if (flag == "--noidxbuild")
      o->indices = false;
    else if (flag == "--texload")
      o->texload = true;
    else if (flag == "--noar")
      o->noar = true;
    else if (flag == "--usdprint")
      o->usdprint = true;
    else if (flag == "--dumpobj")
      o->obj = true;
    else if (flag == "--dumpusd")
      o->usd = true;
    else if (flag == "--dump-timesamples")
      o->times = true;
    else if (flag == "--nodump")
      o->nodump = true;
    else if (flag == "--memstat")
      o->memstat = true;
    else if (flag == "--notangent")
      o->notangent = true;
    else if (flag == "--calctangent")
      o->force_tangent = true;
    else if (flag == "--mmap-lowmem")
      o->mmap = true;
    else if (flag == "--lowmem")
      o->lowmem = true;
    else if (flag == "--snorm8")
      o->snorm8 = true;
    else if (flag == "--fast-index-build")
      o->fast = true;
    else if (flag == "--profile")
      o->profile = true;
    else if (flag == "--yaml")
      o->format = "yaml";
    else if (flag == "--json")
      o->format = "json";
    else if (flag == "--load-payloads")
      o->payloads = true;
    else if (flag == "--defer-payloads")
      o->payloads = false;
    else if (flag == "--no-animation")
      o->animation = false;
    else if (flag == "--compose-threads-auto")
      o->threads = -1;
    else if (!flag.empty() && flag[0] == '-') {
      std::cerr << "Unknown option: " << flag << '\n';
      return false;
    } else if (o->file.empty())
      o->file = flag;
    else {
      std::cerr << "Unexpected argument: " << flag << '\n';
      return false;
    }
  }
  return !o->file.empty();
}
bool write(const std::string& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file) {
    std::cerr << "Failed to write " << path << '\n';
    return false;
  }
  std::cerr << "Wrote " << path << '\n';
  return true;
}
}  // namespace
int main(int argc, char** argv) {
  Options o;
  if (!parse(argc, argv, &o)) {
    help(argv[0]);
    return 1;
  }
  std::string warning, error;
  lightusd::ResolverConfig resolver_config;
  resolver_config.working_directory =
      std::filesystem::path(o.file).parent_path().string();
  resolver_config.allow_filesystem = !o.noar;
  lightusd::AssetResolver resolver(resolver_config);
  lightusd::LoadUSDOptions load;
  load.resolver = &resolver;
  load.usdc_options.crate_options.lazy_arrays = true;
  load.usdc_options.crate_options.use_mmap = o.mmap;
  if (o.mmap) load.input_policy = lightusd::InputPolicy::Trusted;
  lightusd::pcp::CompositionOptions composition;
  composition.load_payloads = o.payloads;
  composition.num_threads = o.threads;
  const auto load_start = std::chrono::steady_clock::now();
  lightusd::Stage stage;
  bool loaded = false;
  if (o.noar) {
    // The explicitly supplied root is authorized; dependent assets remain
    // disabled during composition and rendering.
    auto root_load = load;
    root_load.resolver = nullptr;
    loaded = lightusd::LoadUSD(o.file, &stage, root_load, &warning, &error) &&
             lightusd::ComposeLoadedStage(&stage, resolver, o.file, load,
                                          &warning, &error, &composition);
  } else
    loaded = lightusd::LoadUSDComposed(o.file, &stage, load, &warning, &error,
                                       &composition);
  if (!loaded) {
    std::cerr << warning << error << '\n';
    return 1;
  }
  const auto load_end = std::chrono::steady_clock::now();
  if (!warning.empty()) std::cerr << warning << '\n';
  if (o.usdprint) std::cout << lightusd::WriteUSDAToString(stage) << '\n';
  const auto& meta = stage.GetMeta();
  Json source_metadata{{"upAxis", meta.upAxis},
                       {"metersPerUnit", meta.metersPerUnit},
                       {"framesPerSecond", meta.framesPerSecond},
                       {"timeCodesPerSecond", meta.timeCodesPerSecond},
                       {"autoPlay", meta.autoPlay},
                       {"copyright", meta.copyright},
                       {"comment", meta.comment}};
  if (meta.startTimeCode_set)
    source_metadata["startTimeCode"] = meta.startTimeCode;
  if (meta.endTimeCode_set) source_metadata["endTimeCode"] = meta.endTimeCode;
  r::ConverterConfig config;
  config.time_code = o.time;
  config.asset_resolver = &resolver;
  config.asset_base_dir = resolver_config.working_directory;
  config.mesh.triangulate = o.triangulate;
  config.mesh.triangulation_method =
      o.fan ? r::MeshConfig::TriangulationMethod::Fan
            : r::MeshConfig::TriangulationMethod::Earcut;
  config.mesh.build_vertex_indices = o.indices;
  config.mesh.compute_tangents = false;
  config.mesh.tangent_method =
      o.tangent == "lengyel" ? r::MeshConfig::TangentComputationMethod::Lengyel
      : o.tangent == "mikktspace"
          ? r::MeshConfig::TangentComputationMethod::MikkTSpace
      : o.tangent == "fast-mikktspace"
          ? r::MeshConfig::TangentComputationMethod::FastMikkTSpace
          : r::MeshConfig::TangentComputationMethod::Hybrid;
  config.material.load_textures = o.texload && !o.noar;
  config.animation.enabled = o.animation;
  size_t decoded_bytes = 0;
  if (config.material.load_textures)
    config.material.custom_texture_loader = [&](const std::string& path,
                                                r::TextureImage* out) {
      std::vector<uint8_t> bytes;
      std::string why;
      if (!resolver.ReadAsset(path, &bytes, &why) ||
          bytes.size() > config.limits.max_asset_bytes)
        return false;
      std::string package, entry;
      std::string hint = path;
      if (lightusd::AssetResolver::ParsePackagePath(path, &package, &entry)) {
        bool extracted = false;
        for (size_t depth = 0; depth < 16; depth++) {
          std::string nested, next;
          const bool more =
              lightusd::AssetResolver::ParsePackagePath(entry, &nested, &next);
          const std::string wanted = more ? nested : entry;
          lightusd::USDZReader archive;
          lightusd::USDZReadOptions limits;
          limits.max_archive_size = config.limits.max_asset_bytes;
          limits.max_entry_size = config.limits.max_asset_bytes;
          if (!archive.Open(bytes.data(), bytes.size(), limits)) return false;
          bool found = false;
          for (size_t i = 0; i < archive.NumEntries(); i++)
            if (archive.EntryName(i) == wanted) {
              const auto* data = archive.EntryData(i);
              const size_t size = archive.EntrySize(i);
              if (!data || !size) return false;
              std::vector<uint8_t> content(data, data + size);
              bytes.swap(content);
              found = true;
              break;
            }
          if (!found) return false;
          hint = wanted;
          if (!more) {
            extracted = true;
            break;
          }
          entry = next;
        }
        if (!extracted) return false;
      }
      const size_t cap = config.limits.max_resident_bytes;
      if (decoded_bytes >= cap) return false;
      auto image = lightusd::image::LoadImageFromMemoryBounded(
          bytes.data(), bytes.size(), hint, cap - decoded_bytes);
      if (!image) return false;
      const auto& src = image->image;
      if (src.width <= 0 || src.height <= 0 || src.channels < 1 ||
          src.channels > 4 || src.data.size() > cap - decoded_bytes)
        return false;
      using F = lightusd::Image::PixelFormat;
      r::ComponentType type;
      if (src.format == F::Float) {
        if (src.bpp == 16)
          type = r::ComponentType::Float16;
        else if (src.bpp == 32)
          type = r::ComponentType::Float32;
        else if (src.bpp == 64)
          type = r::ComponentType::Float64;
        else
          return false;
      } else if (src.format == F::UInt) {
        if (src.bpp == 8)
          type = r::ComponentType::UInt8;
        else if (src.bpp == 16)
          type = r::ComponentType::UInt16;
        else if (src.bpp == 32)
          type = r::ComponentType::UInt32;
        else
          return false;
      } else {
        if (src.bpp == 8)
          type = r::ComponentType::Int8;
        else if (src.bpp == 16)
          type = r::ComponentType::Int16;
        else if (src.bpp == 32)
          type = r::ComponentType::Int32;
        else
          return false;
      }
      out->width = static_cast<uint32_t>(src.width);
      out->height = static_cast<uint32_t>(src.height);
      out->channels = static_cast<uint8_t>(src.channels);
      out->component_type = type;
      out->color_space =
          src.format == F::Float ? r::ColorSpace::Linear : r::ColorSpace::sRGB;
      out->asset_identifier = path;
      out->resolved_path = path;
      if (!out->data.append_exact(src.data.data(), src.data.size()))
        return false;
      decoded_bytes += src.data.size();
      return true;
    };
  r::ConvertResult converted;
  {
    r::RenderSceneConverter converter(config);
    converted = converter.Convert(stage);
    if (converted.success && !o.notangent)
      for (auto& mesh : converted.scene.meshes) {
        const auto needs_normal = [&](int32_t id) {
          if (id < 0 ||
              static_cast<size_t>(id) >= converted.scene.materials.size())
            return false;
          const auto& material =
              converted.scene.materials[static_cast<size_t>(id)];
          return (material.preview_surface &&
                  material.preview_surface->normal.is_texture()) ||
                 (material.openpbr && material.openpbr->normal.is_texture());
        };
        bool needed = o.force_tangent || needs_normal(mesh.material_id);
        for (const auto& subset : mesh.material_subsets)
          needed = needed || needs_normal(subset.material_id);
        if (needed && mesh.tangents.empty()) {
          const bool triangulated = mesh.is_triangulated;
          if (!converter.ComputeVertexTangents(&mesh))
            converted.warnings.push_back("Tangents unavailable for mesh " +
                                         mesh.prim_path);
          if (!o.triangulate && !triangulated) {
            if (mesh.tangents_interp == r::Interpolation::FaceVarying &&
                mesh.tangents.size() == mesh.triangulated_indices.size() * 4 &&
                mesh.triangulated_face_vertex_indices.size() ==
                    mesh.triangulated_indices.size()) {
              r::FloatChunked values;
              if (!values.resize_exact(mesh.face_vertex_indices.size() * 4,
                                       0.0f)) {
                std::cerr << "Out of memory retaining polygon tangent frames\n";
                return 1;
              }
              std::vector<bool> written(mesh.face_vertex_indices.size());
              for (size_t i = 0; i < mesh.triangulated_indices.size(); i++) {
                const size_t corner = mesh.triangulated_face_vertex_indices[i];
                if (corner >= written.size() || written[corner]) continue;
                for (size_t c = 0; c < 4; c++)
                  values.mutable_at(corner * 4 + c) = mesh.tangents[i * 4 + c];
                written[corner] = true;
              }
              mesh.tangents = std::move(values);
            }
            mesh.triangulated_indices = r::UInt32Chunked{};
            mesh.triangulated_face_vertex_indices = r::UInt32Chunked{};
            mesh.face_triangle_offsets.clear();
            mesh.is_triangulated = false;
            mesh.tangents_triangulated = false;
          }
        }
      }
  }
  if (!converted.success) {
    std::cerr << converted.error << '\n';
    return 1;
  }
  for (const auto& text : converted.warnings)
    std::cerr << "WARN: " << text << '\n';
  Json display_names = Json::object();
  const auto remember_name = [&](const auto& resource) {
    const auto prim = stage.GetPrimAtPath(resource.prim_path);
    if (prim.IsValid())
      display_names[resource.prim_path] = prim.GetMeta().displayName();
  };
  for (const auto& resource : converted.scene.nodes) remember_name(resource);
  for (const auto& resource : converted.scene.meshes) remember_name(resource);
  for (const auto& resource : converted.scene.materials)
    remember_name(resource);
  for (const auto& resource : converted.scene.animations)
    remember_name(resource);
  for (const auto& resource : converted.scene.cameras) remember_name(resource);
  if (o.indices) {
    size_t resident = converted.scene.memory_usage();
    for (auto& mesh : converted.scene.meshes) {
      const size_t before = mesh.memory_usage();
      const size_t other = resident >= before ? resident - before : 0;
      r::MeshIndexOptions indexing;
      indexing.deduplicate = !o.fast;
      indexing.epsilon = config.mesh.dedup_epsilon;
      indexing.memory_budget_bytes =
          other < config.limits.max_resident_bytes
              ? config.limits.max_resident_bytes - other
              : 1;
      if (!r::BuildVertexIndices(&mesh, indexing, &error)) {
        std::cerr << error << '\n';
        return 1;
      }
      resident = other + mesh.memory_usage();
    }
  }
  if (o.lowmem) stage = lightusd::Stage{};
  auto& scene = converted.scene;
  render_example::PackedAttributes packed;
  if (o.snorm8 && !packed.pack(scene)) {
    std::cerr << "Cannot pack non-finite normals/tangents\n";
    return 1;
  }
  const auto convert_end = std::chrono::steady_clock::now();
  if (o.profile)
    std::cerr << "[timing] USD load: "
              << std::chrono::duration<double, std::milli>(load_end -
                                                           load_start)
                     .count()
              << " ms\n[timing] RenderScene conversion: "
              << std::chrono::duration<double, std::milli>(convert_end -
                                                           load_end)
                     .count()
              << " ms\n";
  Json configuration{
      {"input_file", o.file},
      {"backend", "core"},
      {"triangulate", o.triangulate},
      {"triangulation_method", o.fan ? "TriangleFan" : "Earcut"},
      {"build_vertex_indices", o.indices},
      {"force_fast_index", o.fast},
      {"tangent_method", o.tangent},
      {"compute_tangents", !o.notangent},
      {"force_tangent", o.force_tangent},
      {"load_texture_data", config.material.load_textures},
      {"no_animation", !o.animation},
      {"compose_threads", o.threads},
      {"load_payloads", o.payloads},
      {"mmap_zero_copy", o.mmap},
      {"lowmem", o.lowmem},
      {"normal_storage", o.snorm8 ? "SNorm8x3" : "Float3"},
      {"tangent_storage", o.snorm8 ? "SNorm8x4" : "Float4"},
      {"timecode", std::isnan(o.time) ? Json("default") : Json(o.time)}};
  if (o.memstat) {
    size_t vertices = 0, triangles = 0;
    for (const auto& mesh : scene.meshes) {
      vertices += mesh.point_count();
      triangles += mesh.triangulated_indices.size() / 3;
    }
    std::cout
        << "# Memory Statistics (RenderScene)\n  RenderScene memory usage: "
        << scene.memory_usage() + packed.bytes()
        << " bytes\n  Meshes: " << scene.meshes.size()
        << ", Points: " << scene.points.size()
        << ", Curves: " << scene.curves.size()
        << ", Materials: " << scene.materials.size()
        << ", Lights: " << scene.lights.size()
        << ", Cameras: " << scene.cameras.size()
        << ", Animations: " << scene.animations.size()
        << "\n  Total vertices: " << vertices
        << ", total triangles: " << triangles
        << "\n  RenderScene nodes: " << scene.nodes.size() << '\n';
    for (size_t i = 0; i < scene.meshes.size(); i++)
      std::cout << "  Mesh [" << i << "] " << scene.meshes[i].name << ": "
                << scene.meshes[i].memory_usage() +
                       (o.snorm8 ? packed.normals[i].capacity() +
                                       packed.tangents[i].capacity()
                                 : 0)
                << " bytes\n";
    for (size_t i = 0; i < scene.images.size(); i++)
      std::cout << "  Image [" << i << "] " << scene.images[i].asset_identifier
                << ": " << scene.images[i].data.size() << " bytes\n";
  }
  if (!o.nodump) {
    const auto json = render_example::build_diagnostics(
        scene, o.file, configuration, o.snorm8 ? &packed : nullptr,
        &source_metadata, &display_names);
    if (o.format == "json")
      std::cout << json.dump(2) << '\n';
    else {
      std::cout << "# LightUSD RenderScene YAML Output\n---\n";
      render_example::yaml(std::cout, json);
    }
  }
  if (o.times) render_example::dump_timesamples(scene);
  if (o.snorm8 && (o.obj || o.usd)) packed.restore(scene);
  if (o.obj)
    for (size_t i = 0; i < scene.meshes.size(); i++) {
      std::string obj, mtl;
      if (!r::export_to_obj(scene, static_cast<int>(i), obj, mtl, &warning,
                            &error)) {
        std::cerr << error << '\n';
        return 1;
      }
      const auto base =
          r::OBJMeshBasename(scene.meshes[i], static_cast<int>(i));
      if (!write(base + ".obj", obj) || !write(base + ".mtl", mtl)) return 1;
    }
  if (o.usd) {
    std::string text;
    if (!r::export_to_usda(scene, text, &warning, &error)) {
      std::cerr << error << '\n';
      return 1;
    }
    if (!write(std::filesystem::path(o.file).stem().string() + "export.usda",
               text))
      return 1;
  }
  if (!warning.empty() && (o.obj || o.usd)) std::cerr << warning << '\n';
  return 0;
}
