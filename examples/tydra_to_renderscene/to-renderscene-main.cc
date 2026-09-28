// SPDX-License-Identifier: Apache 2.0
// Copyright 2022-Present Light Transport Entertainment Inc.

//
// Command-line tool to convert USD Stage to RenderScene(glTF-like data
// structure)
//
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

#include "io-util.hh"
#include "pprinter.hh"
#include "prim-pprint.hh"
#include "str-util.hh"
#include "lightusd.hh"
#include "mmap-array-ref.hh"
#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
#include "lightusd-render-cpp.hh"
#endif
#include "tydra/obj-export.hh"
#include "tydra/render-data.hh"
#include "tydra/scene-access.hh"
#include "tydra/shader-network.hh"
#include "tydra/usd-export.hh"
#include "usdShade.hh"
#include "value-pprint.hh"
#include "value-types.hh"

static std::string format_memory_size(size_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  int unit_index = 0;
  double size = static_cast<double>(bytes);
  while (size >= 1024.0 && unit_index < 4) {
    size /= 1024.0;
    unit_index++;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "%.2f %s", size, units[unit_index]);
  return std::string(buf);
}

static int NullARResolve(const char *asset_name,
                       const std::vector<std::string> &search_paths,
                       std::string *resolved_asset_name, std::string *err,
                       void *userdata) {
  (void)err;
  (void)userdata;
  (void)search_paths;
  (void)asset_name;
  (void)resolved_asset_name;
  return -1;
}

static int NullARSize(const char *asset_name, uint64_t *nbytes, std::string *err,
                    void *userdata) {
  (void)userdata;
  (void)asset_name;
  (void)nbytes;
  (void)err;

  return -1;
}

static int NullARRead(const char *asset_name, uint64_t req_nbytes, uint8_t *out_buf,
             uint64_t *nbytes, std::string *err, void *userdata) {

  (void)asset_name;
  (void)req_nbytes;
  (void)out_buf;
  (void)nbytes;
  (void)err;
  (void)userdata;

  return -1;
}

static bool SetupNullAssetResolution(
  lightusd::AssetResolutionResolver &resolver)
{
  lightusd::AssetResolutionHandler handler;
  handler.resolve_fun = NullARResolve;
  handler.size_fun = NullARSize;
  handler.read_fun = NullARRead;
  handler.write_fun = nullptr;
  handler.userdata = nullptr;

  resolver.register_wildcard_asset_resolution_handler(handler);

  return true;
}

static std::string AnimationPathToString(lightusd::tydra::AnimationPath path) {
  switch (path) {
    case lightusd::tydra::AnimationPath::Translation:
      return "Translation";
    case lightusd::tydra::AnimationPath::Rotation:
      return "Rotation";
    case lightusd::tydra::AnimationPath::Scale:
      return "Scale";
    case lightusd::tydra::AnimationPath::Weights:
      return "Weights";
    case lightusd::tydra::AnimationPath::CustomProperty:
      return "CustomProperty";
  }
  return "Unknown";
}

static std::string InterpolationToString(lightusd::tydra::AnimationInterpolation interp) {
  switch (interp) {
    case lightusd::tydra::AnimationInterpolation::Linear:
      return "Linear";
    case lightusd::tydra::AnimationInterpolation::Step:
      return "Step";
    case lightusd::tydra::AnimationInterpolation::CubicSpline:
      return "CubicSpline";
  }
  return "Unknown";
}

static void DumpAnimationTimesamples(const lightusd::tydra::RenderScene& scene) {
  std::cout << "\n========================================\n";
  std::cout << "Animation Timesamples Dump\n";
  std::cout << "========================================\n";
  std::cout << "Total animations: " << scene.animations.size() << "\n\n";

  for (size_t anim_idx = 0; anim_idx < scene.animations.size(); anim_idx++) {
    const auto& anim = scene.animations[anim_idx];

    std::cout << "Animation [" << anim_idx << "]: " << anim.name << "\n";
    std::cout << "  Prim name: " << anim.prim_name << "\n";
    std::cout << "  Abs path: " << anim.abs_path << "\n";
    std::cout << "  Duration: " << anim.duration << " seconds\n";
    std::cout << "  Channels: " << anim.channels.size() << "\n";
    std::cout << "  Samplers: " << anim.samplers.size() << "\n";

    // Dump each channel
    for (size_t ch_idx = 0; ch_idx < anim.channels.size(); ch_idx++) {
      const auto& channel = anim.channels[ch_idx];

      std::cout << "\n  Channel [" << ch_idx << "]:\n";
      std::cout << "    Path: " << AnimationPathToString(channel.path) << "\n";

      if (channel.target_type == lightusd::tydra::ChannelTargetType::SceneNode) {
        std::cout << "    Target type: SceneNode\n";
        std::cout << "    Target node: " << channel.target_node << "\n";
      } else {
        std::cout << "    Target type: SkeletonJoint\n";
        std::cout << "    Skeleton ID: " << channel.skeleton_id << "\n";
        std::cout << "    Joint ID: " << channel.joint_id << "\n";
      }
      std::cout << "    Sampler index: " << channel.sampler << "\n";

      // Dump sampler data
      if (channel.sampler >= 0 &&
          static_cast<size_t>(channel.sampler) < anim.samplers.size()) {
        const auto& sampler = anim.samplers[channel.sampler];

        std::cout << "    Interpolation: " << InterpolationToString(sampler.interpolation) << "\n";
        std::cout << "    Keyframes: " << sampler.num_keyframes() << "\n";

        // Determine components per value based on path
        size_t components = 1;
        switch (channel.path) {
          case lightusd::tydra::AnimationPath::Translation:
          case lightusd::tydra::AnimationPath::Scale:
            components = 3;  // vec3
            break;
          case lightusd::tydra::AnimationPath::Rotation:
            components = 4;  // quat (x, y, z, w)
            break;
          case lightusd::tydra::AnimationPath::Weights:
          case lightusd::tydra::AnimationPath::CustomProperty:
            // Variable, depends on number of morph targets
            if (!sampler.times.empty() && !sampler.values.empty()) {
              components = sampler.values.size() / sampler.times.size();
            }
            break;
        }

        // Dump timesamples
        std::cout << "    Timesamples:\n";
        for (size_t i = 0; i < sampler.times.size(); i++) {
          std::cout << "      [" << i << "] t=" << sampler.times[i] << " : (";

          size_t value_start = i * components;
          for (size_t c = 0; c < components && (value_start + c) < sampler.values.size(); c++) {
            if (c > 0) std::cout << ", ";
            std::cout << sampler.values[value_start + c];
          }
          std::cout << ")\n";
        }
      } else {
        std::cout << "    [Invalid sampler index]\n";
      }
    }
    std::cout << "\n";
  }

  std::cout << "========================================\n\n";
}

static void print_help(const char* prog_name) {
  std::cout << "Usage: " << prog_name << " input.usd [OPTIONS]\n";
  std::cout << "\nConvert USD Stage to RenderScene (glTF-like data structure)\n";
  std::cout << "\nOptions:\n";
  std::cout << "  --help                Show this help message\n";
  std::cout << "  --timecode VALUE      Specify timecode value (e.g. 3.14)\n";
  std::cout << "  --noidxbuild          Do not rebuild vertex indices\n";
  std::cout << "  --notri               Do not triangulate mesh\n";
  std::cout << "  --trifan              Use triangle fan triangulation (instead of earcut)\n";
  std::cout << "  --texload             Load textures\n";
  std::cout << "  --noar                Do not use (default) AssetResolver\n";
  std::cout << "  --usdprint            Print parsed USD\n";
  std::cout << "  --dumpobj             Dump mesh as wavefront .obj (for visual debugging)\n";
  std::cout << "  --dumpusd             Dump scene as USD (USDA Ascii)\n";
  std::cout << "  --dump-timesamples    Dump animation channel timesamples values\n";
  std::cout << "  --nodump              Do not dump RenderScene output\n";
  std::cout << "  --memstat             Print memory usage statistics\n";
  std::cout << "  --notangent           Do not compute tangents/binormals\n";
  std::cout << "  --calctangent         Force tangent computation even without normal map\n";
  std::cout << "  --tangent-method M    Tangent method: lengyel (default), mikktspace, fast-mikktspace\n";
  std::cout << "  --yaml                Output RenderScene as YAML (human-readable)\n";
  std::cout << "  --json                Output RenderScene as JSON (machine-readable)\n";
  std::cout << "  --mmap-lowmem         Enable mmap zero-copy for uncompressed USDC arrays\n";
  std::cout << "  --lowmem              Free GeomMesh data after conversion (reduces peak memory)\n";
  std::cout << "  --snorm8              Use SNorm8x3 normals (3 bytes) and SNorm8x4 tangents (4 bytes)\n";
  std::cout << "  --fast-index-build    Force BuildVertexIndicesFastImpl (reproduces WASM code path)\n";
#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
  std::cout << "  --next                Use next-core pipeline (compose/payload-aware)\n";
  std::cout << "  --compose-threads N   next composition worker hint\n";
  std::cout << "  --compose-threads-auto Use hw-concurrency for next composition\n";
  std::cout << "  --load-payloads       Load payloads during next compose (default)\n";
  std::cout << "  --defer-payloads      Defer all payloads during next compose\n";
  std::cout << "  --no-animation        Skip animation extraction in next pipeline\n";
#endif
  std::cout << "  --profile             Print [timing] for load and RenderScene conversion phases\n";
}

int main(int argc, char **argv) {
  if (argc < 2) {
    print_help(argv[0]);
    return EXIT_FAILURE;
  }

  // When Xform, Mesh, Material, etc. have time-varying values,
  // values are evaluated at `timecode` time(except for animation values in
  // SkelAnimation)
  double timecode = lightusd::value::TimeCode::Default();

  bool build_indices = true;
  bool triangulate = true;
  bool use_triangle_fan = false;
  bool export_obj = false;
  bool export_usd = false;
  bool usdprint = false;
  bool texload = false;
  bool no_assetresolver = false;
  bool dump_timesamples = false;
  bool no_dump = false;
  bool memstat = false;
  bool no_tangent = false;
  bool force_tangent = false;
  bool mmap_lowmem = false;
  bool lowmem = false;
  bool snorm8 = false;
  bool force_fast_index = false;
  bool profile = false;
#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
  bool use_next = false;
  int compose_threads = 1;
  bool next_load_payloads = true;
  bool no_animation = false;
#endif
  auto tangent_method = lightusd::tydra::MeshConverterConfig::TangentComputationMethod::Lengyel;
  std::string output_format = "yaml";  // "yaml" (human-readable), "json" (machine-readable)

  std::string filepath;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      print_help(argv[0]);
      return EXIT_SUCCESS;
    } else if (strcmp(argv[i], "--notri") == 0) {
      triangulate = false;
    } else if (strcmp(argv[i], "--trifan") == 0) {
      use_triangle_fan = true;
    } else if (strcmp(argv[i], "--noidxbuild") == 0) {
      build_indices = false;
    } else if (strcmp(argv[i], "--usdprint") == 0) {
      usdprint = true;
    } else if (strcmp(argv[i], "--texload") == 0) {
      texload = true;
    } else if (strcmp(argv[i], "--noar") == 0) {
      no_assetresolver = true;
    } else if (strcmp(argv[i], "--dumpobj") == 0) {
      export_obj = true;
    } else if (strcmp(argv[i], "--dumpusd") == 0) {
      export_usd = true;
    } else if (strcmp(argv[i], "--dump-timesamples") == 0) {
      dump_timesamples = true;
    } else if (strcmp(argv[i], "--nodump") == 0) {
      no_dump = true;
    } else if (strcmp(argv[i], "--memstat") == 0) {
      memstat = true;
    } else if (strcmp(argv[i], "--notangent") == 0) {
      no_tangent = true;
    } else if (strcmp(argv[i], "--calctangent") == 0) {
      force_tangent = true;
    } else if (strcmp(argv[i], "--tangent-method") == 0) {
      if ((i + 1) >= argc) {
        std::cerr << "arg is missing for --tangent-method flag.\n";
        return -1;
      }
      std::string tm = argv[i + 1];
      if (tm == "lengyel") {
        tangent_method = lightusd::tydra::MeshConverterConfig::TangentComputationMethod::Lengyel;
      } else if (tm == "mikktspace") {
        tangent_method = lightusd::tydra::MeshConverterConfig::TangentComputationMethod::MikkTSpace;
      } else if (tm == "fast-mikktspace") {
        tangent_method = lightusd::tydra::MeshConverterConfig::TangentComputationMethod::FastMikkTSpace;
      } else {
        std::cerr << "Unknown tangent method: " << tm << ". Use lengyel, mikktspace, or fast-mikktspace.\n";
        return -1;
      }
      i++;
    } else if (strcmp(argv[i], "--timecode") == 0) {
      if ((i + 1) >= argc) {
        std::cerr << "arg is missing for --timecode flag.\n";
        return -1;
      }
      timecode = std::stod(argv[i + 1]);
      std::cout << "Use timecode: " << timecode << "\n";
      i++;
    } else if (strcmp(argv[i], "--yaml") == 0) {
      output_format = "yaml";
    } else if (strcmp(argv[i], "--json") == 0) {
      output_format = "json";
    } else if (strcmp(argv[i], "--mmap-lowmem") == 0) {
      mmap_lowmem = true;
    } else if (strcmp(argv[i], "--lowmem") == 0) {
      lowmem = true;
    } else if (strcmp(argv[i], "--snorm8") == 0) {
      snorm8 = true;
    } else if (strcmp(argv[i], "--fast-index-build") == 0) {
      force_fast_index = true;
#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
    } else if (strcmp(argv[i], "--next") == 0) {
      use_next = true;
    } else if (strcmp(argv[i], "--compose-threads") == 0) {
      if ((i + 1) >= argc) {
        std::cerr << "arg is missing for --compose-threads flag.\n";
        return -1;
      }
      compose_threads = std::atoi(argv[i + 1]);
      if (compose_threads == 0) {
        compose_threads = 1;
      }
      if (compose_threads < 1) {
        std::cerr << "invalid compose thread count: " << compose_threads << "\n";
        return -1;
      }
      i++;
    } else if (strcmp(argv[i], "--compose-threads-auto") == 0) {
      compose_threads = -1;
    } else if (strcmp(argv[i], "--load-payloads") == 0) {
      next_load_payloads = true;
    } else if (strcmp(argv[i], "--defer-payloads") == 0) {
      next_load_payloads = false;
    } else if (strcmp(argv[i], "--no-animation") == 0) {
      no_animation = true;
#endif
    } else if (strcmp(argv[i], "--profile") == 0) {
      profile = true;
    } else {
      filepath = argv[i];
    }
  }

  std::string warn;
  std::string err;

  bool use_next_pipeline = false;
#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
  use_next_pipeline = use_next;
  lightusd::api::Stage next_stage;
#endif
  lightusd::Stage stage;

  if (!lightusd::IsUSD(filepath)) {
    std::cerr << "File not found or not a USD format: " << filepath << "\n";
  }

  bool is_usdz = lightusd::IsUSDZ(filepath);

  // Add base directory early since next load path also needs it.
  std::string usd_basedir = lightusd::io::GetBaseDir(filepath);

  // Collect config info for formatted output
  std::vector<std::pair<std::string, std::string>> config_info;

  // Use mmap if available to save memory (avoids copying entire file)
  lightusd::io::MMapFileHandle mmap_handle;
  bool using_mmap = false;
  bool ret = false;

  auto _t_load_begin = std::chrono::steady_clock::now();
#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
  if (use_next_pipeline) {
    config_info.push_back({"load_pipeline", "next"});
    lightusd_load_options load_options;
    lightusd::api::InitLoadOptions(&load_options);
    load_options.preserve_native_instances = 1;
    if (mmap_lowmem) {
      config_info.push_back({"mmap_zero_copy", "true"});
    }
    config_info.push_back({"asset_resolver", no_assetresolver ? "null" : "default"});

    load_options.max_threads = compose_threads < 0 ? 0 : compose_threads;
    load_options.load_payloads = next_load_payloads;
    ret = next_stage.load(filepath.c_str(), &load_options) == LIGHTUSD_OK;
    if (!ret) err = lightusd::api::LastError();
    else {
      lightusd::api::String warnings;
      if (next_stage.take_warnings(&warnings) == LIGHTUSD_OK) {
        const auto text = lightusd::api::StringView(warnings);
        warn.assign(text.data ? text.data : "", text.len);
      }
    }
    if (compose_threads == -1) {
      config_info.push_back({"compose_threads", "auto"});
    } else {
      config_info.push_back({"compose_threads", std::to_string(compose_threads)});
    }
    config_info.push_back({"load_payloads", next_load_payloads ? "true" : "false"});
  } else {
#endif
  lightusd::USDLoadOptions load_options;
  if (mmap_lowmem) {
    load_options.mmap_zero_copy = true;
    config_info.push_back({"mmap_zero_copy", "true"});
  }

  if (lightusd::io::IsMMapSupported()) {
    config_info.push_back({"loading_method", "mmap"});
    if (!lightusd::io::MMapFile(filepath, &mmap_handle, /* writable */false, &err)) {
      std::cerr << "Failed to mmap USD file: " << err << "\n";
      return EXIT_FAILURE;
    }
    using_mmap = true;

    // Load USD from mmap'd memory
    ret = lightusd::LoadUSDFromMemory(mmap_handle.addr, mmap_handle.size,
                                       filepath, &stage, &warn, &err,
                                       load_options);
  } else {
    // Fallback to file-based loading
    config_info.push_back({"loading_method", "file"});
    if (mmap_lowmem) {
      std::cerr << "WARN: --mmap-lowmem requested but mmap is not supported on this platform.\n";
    }
    ret = lightusd::LoadUSDFromFile(filepath, &stage, &warn, &err);
  }
#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
  }
#endif
  auto _t_load_end = std::chrono::steady_clock::now();
  if (profile) {
    double _load_ms = std::chrono::duration<double, std::milli>(_t_load_end - _t_load_begin).count();
    std::cerr << "[timing] USD load: " << _load_ms << " ms\n";
  }

  if (!warn.empty()) {
    std::cerr << "WARN : " << warn << "\n";
  }

  if (!err.empty()) {
    std::cerr << "ERR : " << err << "\n";
  }

  if (!ret) {
    std::cerr << "Failed to load USD file: " << filepath << "\n";
    if (using_mmap) {
      lightusd::io::UnmapFile(mmap_handle, &err);
    }
    return EXIT_FAILURE;
  }

  if (memstat) {
    size_t stage_mem = 0;
#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
    if (use_next_pipeline) {
      lightusd_stage_stats stats{};
      if (next_stage.stats(&stats) == LIGHTUSD_OK) stage_mem = stats.memory_bytes;
    } else
#endif
    {
      stage_mem = stage.estimate_memory_usage();
    }
    std::cout << "# Memory Statistics (Stage)\n";
    std::cout << "  Stage memory usage: " << format_memory_size(stage_mem)
              << " (" << stage_mem << " bytes)\n";
    if (!use_next_pipeline && stage.has_mmap_zero_copy()) {
      uint64_t deferred = stage.mmap_table()->total_deferred_bytes();
      std::cout << "  mmap zero-copy: " << stage.mmap_table()->size()
                << " deferred arrays, " << format_memory_size(deferred)
                << " deferred to mmap (not in Stage heap)\n";
    }
    std::cout << "\n";
  }

  if (usdprint && !use_next_pipeline) {
    std::string s = stage.ExportToString();
    std::cout << s << "\n";
    std::cout << "--------------------------------------"
              << "\n";
  }

  // RenderScene: Scene graph object which is suited for GL/Vulkan renderer
  lightusd::tydra::RenderScene render_scene;
#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
  lightusd::api::RenderScene next_render_scene;
  bool next_render_scene_ready = false;
#endif

  config_info.push_back({"input_file", filepath});
  config_info.push_back({"is_usdz", is_usdz ? "true" : "false"});
  config_info.push_back({"output_format", output_format});
  config_info.push_back({"triangulate", triangulate ? "true" : "false"});
  config_info.push_back({"triangulation_method", use_triangle_fan ? "TriangleFan" : "Earcut"});
  auto _t_conv_begin = std::chrono::steady_clock::now();
  if (use_next_pipeline) {
#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
    if (snorm8) {
      std::cerr << "WARN: --snorm8 is not yet supported in next converter; ignored.\n";
      config_info.push_back({"normal_storage", "unsupported_in_next"});
      config_info.push_back({"tangent_storage", "unsupported_in_next"});
    }

    lightusd_render_config conv_cfg;
    lightusd::api::InitRenderConfig(&conv_cfg);
    conv_cfg.time_code = timecode;
    conv_cfg.triangulate = triangulate;
    conv_cfg.triangulation_method = use_triangle_fan ? 1 : 0;
    conv_cfg.build_vertex_indices = build_indices;
    conv_cfg.compute_normals = !no_tangent;
    conv_cfg.compute_tangents = !no_tangent;
    conv_cfg.discard_geometry = lowmem;
    conv_cfg.discard_instance_source_arrays = 1;
    conv_cfg.load_textures = texload && !no_assetresolver;
    conv_cfg.disable_animation = no_animation;
    conv_cfg.target_color_space = 0;  // Preserve the converter's sRGB default.
    conv_cfg.use_default_asset_resolver = !no_assetresolver;

    if (!lightusd::value::TimeCode(timecode).is_default()) {
      config_info.push_back({"timecode", std::to_string(timecode)});
    } else {
      config_info.push_back({"timecode", "default"});
    }
    config_info.push_back({"compute_tangents", (!no_tangent ? "true" : "false")});
    config_info.push_back({"force_tangent", (force_tangent ? "true" : "false")});
    config_info.push_back({"build_vertex_indices", build_indices ? "true" : "false"});
    config_info.push_back({"load_texture_data", texload ? "true" : "false"});
    config_info.push_back({"no_animation", no_animation ? "true" : "false"});

    switch (tangent_method) {
      case lightusd::tydra::MeshConverterConfig::TangentComputationMethod::Lengyel:
        conv_cfg.tangent_method = 1;
        break;
      case lightusd::tydra::MeshConverterConfig::TangentComputationMethod::MikkTSpace:
        conv_cfg.tangent_method = 2;
        break;
      case lightusd::tydra::MeshConverterConfig::TangentComputationMethod::FastMikkTSpace:
      case lightusd::tydra::MeshConverterConfig::TangentComputationMethod::Hybrid:
        conv_cfg.tangent_method = 3;
        break;
    }

    const char* method_names[] = {"lengyel", "mikktspace", "fast-mikktspace", "hybrid"};
    config_info.push_back({"tangent_method", method_names[int(tangent_method)]});

    ret = lightusd::api::Convert(next_stage, &next_render_scene, &conv_cfg) == LIGHTUSD_OK;
    next_render_scene_ready = ret;
    if (ret) {
      lightusd::api::StringList warnings;
      if (lightusd::api::RenderSceneWarnings(next_render_scene, &warnings) == LIGHTUSD_OK) {
        for (size_t i = 0; i < lightusd::api::StringListSize(warnings); ++i) {
          const auto text = lightusd::api::StringListGet(warnings, i);
          config_info.push_back({"converter_warning", std::string(text.data ? text.data : "", text.len)});
        }
      }
    } else {
      std::cerr << "Failed to convert next Stage to RenderScene: "
                << lightusd::api::LastError() << "\n";
    }
#else
    (void)snorm8;
    ret = false;
#endif
  } else {
    lightusd::tydra::RenderSceneConverter converter;
    lightusd::tydra::RenderSceneConverterEnv env(stage);

    env.mesh_config.triangulate = triangulate;
    env.mesh_config.triangulation_method =
        use_triangle_fan
            ? lightusd::tydra::MeshConverterConfig::TriangulationMethod::TriangleFan
            : lightusd::tydra::MeshConverterConfig::TriangulationMethod::Earcut;
    env.mesh_config.build_vertex_indices = build_indices;
    if (no_tangent) {
      env.mesh_config.compute_tangents_and_binormals = false;
      config_info.push_back({"compute_tangents", "false"});
    } else {
      config_info.push_back({"compute_tangents", "true"});
    }

    if (force_tangent) {
      // Force tangent computation even without a normal map texture.
      env.mesh_config.compute_tangents_only_with_normal_map = false;
      config_info.push_back({"force_tangent", "true"});
    } else {
      config_info.push_back({"force_tangent", "false"});
    }

    if (lowmem) {
      env.mesh_config.lowmem = true;
      config_info.push_back({"lowmem", "true"});
    }

    if (force_fast_index) {
      env.mesh_config.force_fast_index_build = true;
      config_info.push_back({"force_fast_index_build", "true"});
    }

    if (snorm8) {
      env.mesh_config.normal_storage =
          lightusd::tydra::MeshConverterConfig::NormalStorageFormat::PackedSNorm8;
      env.mesh_config.tangent_storage =
          lightusd::tydra::MeshConverterConfig::TangentStorageFormat::PackedSNorm8;
      config_info.push_back({"normal_storage", "snorm8"});
      config_info.push_back({"tangent_storage", "snorm8"});
    }

    env.mesh_config.tangent_method = tangent_method;
    {
      const char *legacy_method_names[] = {"lengyel", "mikktspace", "fast-mikktspace"};
      config_info.push_back({"tangent_method", legacy_method_names[int(tangent_method)]});
    }

    config_info.push_back({"load_texture_data", texload ? "true" : "false"});
    env.scene_config.load_texture_assets = texload;

    config_info.push_back({"search_path", usd_basedir});

    lightusd::USDZAsset usdz_asset;

    if (is_usdz) {
      // Setup AssetResolutionResolver to read assets from USDZ container.
      // Reuse the mmap handle if already mmap'd, otherwise fall back to file-based.
      if (using_mmap) {
        // Use ReadUSDZAssetInfoFromMemory with asset_on_memory=true
        // This avoids copying the USDZ data, just references the mmap'd address
        if (!lightusd::ReadUSDZAssetInfoFromMemory(
              mmap_handle.addr, mmap_handle.size,
              /* asset_on_memory */ true,
              &usdz_asset, &warn, &err)) {
          std::cerr << "Failed to read USDZ assetInfo from memory: " << err << "\n";
          lightusd::io::UnmapFile(mmap_handle, &err);
          return EXIT_FAILURE;
        }
      } else {
        // Fallback to file-based loading (copies entire file into memory)
        if (!lightusd::ReadUSDZAssetInfoFromFile(filepath, &usdz_asset, &warn,
                                                 &err)) {
          std::cerr << "Failed to read USDZ assetInfo from file: " << err << "\n";
          return EXIT_FAILURE;
        }
      }
      if (warn.size()) {
        std::cout << warn << "\n";
      }

      lightusd::AssetResolutionResolver arr;
      if (no_assetresolver) {
        SetupNullAssetResolution(arr);
        config_info.push_back({"asset_resolver", "null"});
      } else {
        // NOTE: Pointer address of usdz_asset must be valid until the call of
        // RenderSceneConverter::ConvertToRenderScene.
        if (!lightusd::SetupUSDZAssetResolution(arr, &usdz_asset)) {
          std::cerr << "Failed to setup AssetResolution for USDZ asset\n";
          exit(-1);
        };
        config_info.push_back({"asset_resolver", "usdz"});
      }
      env.asset_resolver = arr;
    } else {
      env.set_search_paths({usd_basedir});
      if (no_assetresolver) {
        SetupNullAssetResolution(env.asset_resolver);
        config_info.push_back({"asset_resolver", "null"});
      } else {
        config_info.push_back({"asset_resolver", "default"});
      }
    }

    if (!lightusd::value::TimeCode(timecode).is_default()) {
      config_info.push_back({"timecode", std::to_string(timecode)});
    } else {
      config_info.push_back({"timecode", "default"});
    }
    env.timecode = timecode;
    ret = converter.ConvertToRenderScene(env, &render_scene);
    std::string converter_warn = converter.GetWarning();
    if (!converter_warn.empty()) {
      config_info.push_back({"converter_warning", converter_warn});
    }
    if (!ret) {
      std::cerr << "Failed to convert USD Stage to RenderScene: \n"
                << converter.GetError() << "\n";
    }
  }

  auto _t_conv_end = std::chrono::steady_clock::now();
  if (profile) {
    double _conv_ms = std::chrono::duration<double, std::milli>(_t_conv_end - _t_conv_begin).count();
    std::cerr << "[timing] RenderScene conversion: " << _conv_ms << " ms\n";
#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
    if (use_next_pipeline) {
      lightusd_render_stats next_stats{};
      lightusd::api::RenderSceneStats(next_render_scene, &next_stats);
      std::cerr << "[timing] Next RenderScene nodes: " << next_stats.node_count
                << ", meshes: " << next_stats.mesh_count
                << ", triangles: " << next_stats.total_triangles << "\n";
    }
#endif
  }

  if (!ret) {
    return EXIT_FAILURE;
  }

  if (memstat) {
#if defined(LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE)
    if (use_next_pipeline && next_render_scene_ready) {
      lightusd_render_stats stats{};
      lightusd::api::RenderSceneStats(next_render_scene, &stats);
      size_t render_mem = lightusd::api::RenderSceneMemoryBytes(next_render_scene);
      std::cout << "# Memory Statistics (RenderScene)\n";
      std::cout << "  RenderScene memory usage: " << format_memory_size(render_mem)
                << " (" << render_mem << " bytes)\n";
      std::cout << "  Meshes: " << stats.mesh_count
                << ", Points: " << stats.points_count
                << ", Curves: " << stats.curves_count
                << ", PointInstancers: " << stats.point_instancer_count
                << ", Materials: " << stats.material_count
                << ", Lights: " << stats.light_count
                << ", Cameras: " << stats.camera_count
                << ", Animations: " << stats.animation_count << "\n";
      std::cout << "  Total vertices: " << stats.total_vertices
                << ", total triangles: " << stats.total_triangles << "\n";
      std::cout << "  RenderScene nodes: " << stats.node_count << "\n";
    } else {
      size_t render_mem = render_scene.estimate_memory_usage();
      std::cout << "# Memory Statistics (RenderScene)\n";
      std::cout << "  RenderScene memory usage: " << format_memory_size(render_mem)
                << " (" << render_mem << " bytes)\n";

      // Per-mesh breakdown
      if (!render_scene.meshes.empty()) {
        std::cout << "\n  ## Meshes (" << render_scene.meshes.size() << ")\n";
        for (size_t i = 0; i < render_scene.meshes.size(); i++) {
          const auto &mesh = render_scene.meshes[i];
          size_t mesh_mem = mesh.estimate_memory_usage();
          std::cout << "    [" << i << "] " << mesh.prim_name
                    << ": " << format_memory_size(mesh_mem)
                    << " (" << mesh.points.size() << " verts, "
                    << mesh.triangulatedFaceVertexIndices.size() << " tri-indices)\n";
        }
      }

      // Per-buffer (texture) breakdown
      if (!render_scene.buffers.empty()) {
        size_t total_buf = 0;
        std::cout << "\n  ## Buffers/Textures (" << render_scene.buffers.size() << ")\n";
        for (size_t i = 0; i < render_scene.buffers.size(); i++) {
          const auto &buf = render_scene.buffers[i];
          size_t buf_bytes = buf.data.capacity();
          total_buf += buf_bytes;
          // Show image name from matching TextureImage if available
          std::string label;
          for (const auto &img : render_scene.images) {
            if (img.buffer_id == static_cast<int64_t>(i)) {
              label = img.asset_identifier;
              break;
            }
          }
          if (label.empty()) label = "(buffer " + std::to_string(i) + ")";
          std::cout << "    [" << i << "] " << label
                    << ": " << format_memory_size(buf_bytes) << "\n";
        }
        std::cout << "    Total buffer memory: " << format_memory_size(total_buf) << "\n";
      }
    }
#endif  // LIGHTUSD_USE_NEXT_PCP_LARGE_SCENE
    std::cout << "\n";
  }

  // Next conversion currently does not have legacy RenderScene dump exporters.
  if (use_next_pipeline) {
    if (dump_timesamples) {
      std::cerr << "WARN: --dump-timesamples is currently unsupported for --next.\n";
    }
    if (export_obj) {
      std::cerr << "WARN: --export-obj is currently unsupported for --next.\n";
      return EXIT_FAILURE;
    }
    if (export_usd) {
      std::cerr << "WARN: --export-usd is currently unsupported for --next.\n";
      return EXIT_FAILURE;
    }
  }

  // Dump animation timesamples if requested
  if (!use_next_pipeline && dump_timesamples) {
    DumpAnimationTimesamples(render_scene);
  }

  if (!use_next_pipeline) {
    // Per-mesh breakdown
    if (memstat) {
      // Keep existing detailed breakdown below.
    }

    // Output handling follows legacy RenderScene path.
    auto escape_for_comment = [](const std::string &s) -> std::string {
      std::string result;
      result.reserve(s.size());
      for (char c : s) {
        if (c == '\n') {
          result += "\\n";
        } else if (c == '\r') {
          result += "\\r";
        } else if (c == '\t') {
          result += "\\t";
        } else {
          result += c;
        }
      }
      return result;
    };

    if (!no_dump) {
      // Output config info in appropriate format
      if (output_format == "yaml") {
        // YAML: Output as comments
        std::cout << "# LightUSD tydra_to_renderscene Configuration\n";
        std::cout << "# ==========================================\n";
        for (const auto &kv : config_info) {
          std::cout << "# " << kv.first << ": " << escape_for_comment(kv.second) << "\n";
        }
        std::cout << "#\n";
      } else if (output_format == "json") {
        // JSON: Output config as a separate JSON object before main output
        std::cout << "// LightUSD tydra_to_renderscene Configuration\n";
        std::cout << "// config: {\n";
        for (size_t i = 0; i < config_info.size(); i++) {
          std::cout << "//   \"" << config_info[i].first << "\": \"" << escape_for_comment(config_info[i].second) << "\"";
          if (i < config_info.size() - 1) std::cout << ",";
          std::cout << "\n";
        }
        std::cout << "// }\n";
      } else {
        // KDL or other: output as comments
        for (const auto &kv : config_info) {
          std::cout << "// " << kv.first << ": " << escape_for_comment(kv.second) << "\n";
        }
      }

      std::cout << DumpRenderScene(render_scene, output_format) << "\n";
    }

    if (export_obj) {
      std::cout << "Dump RenderMesh as wavefront .obj\n";
      for (size_t i = 0; i < render_scene.meshes.size(); i++) {
        std::string obj_str;
        std::string mtl_str;
        if (!lightusd::tydra::export_to_obj(render_scene, i, obj_str, mtl_str,
                                            &warn, &err)) {
          std::cerr << "obj export error: " << err << "\n";
          exit(-1);
        }

        std::string obj_filename =
            std::to_string(i) + render_scene.meshes[i].prim_name + ".obj";
        std::string mtl_filename =
            std::to_string(i) + render_scene.meshes[i].prim_name + ".mtl";
        {
          std::ofstream obj_ofs(obj_filename);
          obj_ofs << obj_str;
        }

        {
          std::ofstream mtl_ofs(mtl_filename);
          mtl_ofs << mtl_str;
        }
        std::cout << "  Wrote " << obj_filename << "\n";
      }
    }

    if (export_usd) {
      std::string ext = lightusd::io::GetFileExtension(filepath);
      std::string usd_basename = lightusd::io::GetBaseFilename(filepath);
      std::string usd_filename =
          lightusd::removeSuffix(usd_basename, ext) + "export.usda";

      std::string usda_str;
      if (!lightusd::tydra::export_to_usda(render_scene, usda_str, &warn, &err)) {
        std::cerr << "Failed to export RenderScene to USDA: " << err << "\n";
      }
      if (warn.size()) {
        std::cout << "WARN: " << warn << "\n";
      }

      {
        std::ofstream ofs(usd_filename);
        ofs << usda_str;
      }
      std::cout << "Exported RenderScene as USDA: " << usd_filename << "\n";
    }

  }

  // Cleanup mmap if used
  if (using_mmap) {
    std::string unmap_err;
    if (!lightusd::io::UnmapFile(mmap_handle, &unmap_err)) {
      std::cerr << "WARN: Failed to unmap file: " << unmap_err << "\n";
    }
  }

  return EXIT_SUCCESS;
}
