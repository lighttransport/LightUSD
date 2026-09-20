// SPDX-License-Identifier: Apache-2.0
// lusdrender — CLI option parsing (Options) + the null asset-resolution shim.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>

#include "asset-resolution.hh"
#include "tsd/tinysubdiv.hh"
#include "lusdr_context.hh"

namespace lusdr {

bool ParseIntStrict(const std::string &s, int *out) {
  if (!out || s.empty()) return false;
  errno = 0;
  char *end = nullptr;
  long v = std::strtol(s.c_str(), &end, 10);
  if (errno == ERANGE || end != s.c_str() + s.size()) return false;
  if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
    return false;
  }
  *out = int(v);
  return true;
}

bool ParseFloatStrict(const std::string &s, float *out) {
  if (!out || s.empty()) return false;
  errno = 0;
  char *end = nullptr;
  float v = std::strtof(s.c_str(), &end);
  if (errno == ERANGE || end != s.c_str() + s.size() || !std::isfinite(v)) {
    return false;
  }
  *out = v;
  return true;
}

bool ParseDoubleStrict(const std::string &s, double *out) {
  if (!out || s.empty()) return false;
  errno = 0;
  char *end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (errno == ERANGE || end != s.c_str() + s.size() || !std::isfinite(v)) {
    return false;
  }
  *out = v;
  return true;
}

bool ParseColor(const std::string &s, Vec3 *out) {
  if (!out) return false;
  size_t p0 = s.find(',');
  size_t p1 = (p0 == std::string::npos) ? std::string::npos : s.find(',', p0 + 1);
  if (p0 == std::string::npos || p1 == std::string::npos ||
      s.find(',', p1 + 1) != std::string::npos) {
    return false;
  }
  Vec3 v;
  if (!ParseFloatStrict(s.substr(0, p0), &v.x) ||
      !ParseFloatStrict(s.substr(p0 + 1, p1 - p0 - 1), &v.y) ||
      !ParseFloatStrict(s.substr(p1 + 1), &v.z)) {
    return false;
  }
  *out = v;
  return true;
}

bool ParseLargeSceneProfile(const std::string &s,
                            Options::LargeSceneProfile *out) {
  if (!out) return false;
  std::string v = s;
  for (char &c : v) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  if (v == "off") {
    *out = Options::LargeSceneProfile::Off;
  } else if (v == "auto") {
    *out = Options::LargeSceneProfile::Auto;
  } else if (v == "caldera") {
    *out = Options::LargeSceneProfile::Caldera;
  } else if (v == "island") {
    *out = Options::LargeSceneProfile::Island;
  } else if (v == "alab") {
    *out = Options::LargeSceneProfile::ALab;
  } else {
    return false;
  }
  return true;
}

void PrintUsage(const char *prog) {
  std::cout
      << "lusdrender - CPU preview raytrace renderer for USD\n\n"
      << "Usage:\n"
      << "  " << prog << " input.usd output.png [options]\n\n"
      << "Options:\n"
      << "  -h, -help              Show this help.\n"
      << "  -w, -width <N>         Output width (default 960).\n"
      << "  -height <N>            Output height (default from aspect or 540).\n"
      << "  -camera <path|name>    Camera absolute path or camera name.\n"
      << "  -renderSettings <path> Select a UsdRender RenderSettings prim.\n"
      << "  --aov <color|depth|worldNormal|primId> CPU capture (data AOVs require .pfm)\n"
      << "  --all-products        Render settings products; output is a filename prefix.\n"
      << "  -renderProduct <path>  Select one product from the settings.\n"
      << "  -renderPass <path>     Apply a local RenderPass render source/visibility.\n"
      << "  -fitScale <value>      Auto-fit camera distance multiplier (default 1.8).\n"
      << "  -viewDir <x,y,z>       Auto-fit camera direction from target to eye.\n"
      << "  -purpose <list>        Visible USD purposes: default,render,proxy,guide.\n"
      << "  -showGuide             Include purpose=guide geometry.\n"
      << "  -hideProxy             Hide purpose=proxy geometry.\n"
      << "  -hideRender            Hide purpose=render geometry.\n"
      << "  -hideDefault           Hide default-purpose geometry.\n"
      << "  -timecode <value>      USD timecode to evaluate.\n"
      << "  -samples <N>           Deterministic supersamples per pixel (default 1).\n"
      << "  -bg <r,g,b>            Background color in linear RGB (default 0,0,0).\n"
      << "  -ambient <value>       Ambient diffuse term (default 0.05).\n"
      << "  -noShadows             Disable hard shadow rays.\n"
      << "  -smooth                Interpolate authored normals (smooth shading)\n"
      << "                         instead of per-face geometric normals.\n"
      << "  -ibl <default|envmap>  DomeLight IBL precompute backend: the built-in\n"
      << "                         reference, or the vendored envmap library.\n"
      << "  -noDisplace            Disable UsdPreviewSurface displacement.\n"
      << "  -displaceScale <f>     Global displacement multiplier (default 1.0).\n"
      << "  -rtPreview             Use mmap zero-copy mesh preview path for large USDC.\n"
      << "  -progress              Print long-running load/build progress.\n"
      << "  -quality <fast|default|hq>\n"
      << "                         LightRT BVH build quality (default default).\n"
      << "  -threads <N>           LightRT build threads (0 = backend default).\n"
      << "  -subdiv <N>            Subdivision level for Mesh subdivisionScheme\n"
      << "                         catmullClark/loop/bilinear (default 0).\n"
      << "  -complexity <low|medium|high|veryhigh>\n"
      << "                         usdrecord refinement preset -> subdiv 0/1/2/3.\n"
      << "  -autoframe             usdrecord-style auto camera framing.\n"
      << "  -timecode <t>          Evaluate animation at time code t.\n"
      << "  -defaultTime           Evaluate at the default (non-animated) time.\n"
      << "  -frames <FRAMESPEC>    Render an animation; one image per time code.\n"
      << "                         FRAMESPEC: t | start:end | start:end x stride,\n"
      << "                         comma-separated. Output path uses # for the\n"
      << "                         frame number (e.g. frame.####.png).\n"
      << "  -mask <PATH[,PATH...]> Restrict rendering to these prim subtrees;\n"
      << "                         defer payloads outside the selected subtrees.\n"
      << "  -variant <SET=SEL>     Override variant selection (e.g.\n"
      << "                         --variant districtLod=full). Repeatable.\n"
      << "  -rtLod                 Per-instance view-dependent LOD: distant meshes\n"
      << "                         -> box proxy, sub-pixel placements dropped (CPU\n"
      << "                         -rtPreview two-level TLAS, and -vk/-vkr/-d3d/-hip\n"
      << "                         flatten-side; parity with viewer --rt-lod).\n"
      << "  -rtLodFullPx <px>      Promote to full mesh at/above this projected\n"
      << "                         radius (default 64).\n"
      << "  -rtLodCullPx <px>      Drop placements below this projected radius (def 2).\n"
      << "  -rtLodNoProxy          Full-or-Cull only (no box proxies).\n"
      << "  -rtLodFrustumCull      Also drop off-screen instances (FASTER but changes\n"
      << "                         shadows/reflections/GI; off by default).\n"
      << "  -lodStream             View-dependent district LOD: a proxy pass picks\n"
      << "                         the nearest districts to the camera and promotes\n"
      << "                         them to districtLod=full under a memory budget\n"
      << "                         (the rest stay proxy). Implies -rtPreview/next.\n"
      << "  -maxVram <GiB>         GPU VRAM budget for -lodStream (0 = 0.5*device).\n"
      << "  -lodDistrictMem <GiB>  Est. host RSS charged per promoted district\n"
      << "                         (cost-model calibration; default 10).\n"
      << "  -lodDistrictVram <GiB> Est. VRAM charged per promoted district (def 3).\n"
      << "  -lodMinVerts <N>       Skip container children below N proxy verts\n"
      << "                         (trigger/volume prims; default 1000).\n"
      << "  -lodMaxVerts <N>       Skip children above N proxy verts (sprawling\n"
      << "                         non-district overlays; 0=off, default 2000000).\n"
      << "  -lodContainer <name>   Namespace prim whose children are LOD districts\n"
      << "                         (default mp_wz_island_geo, the Caldera layout).\n"
      << "  -largeSceneProfile off|auto|caldera|island|alab\n"
      << "                         Resolve a Vulkan large-scene preset over existing\n"
      << "                         backend/LOD/memory knobs. Explicit CLI flags win;\n"
      << "                         texture resize/compression flags are unchanged.\n"
      << "  -legacyLoad            Use the legacy eager loader (next is default).\n"
      << "  -materialResolver legacy|tydra-next|compare\n"
      << "                         Next-loader material resolver. tydra-next is\n"
      << "                         the default; legacy is the compatibility path;\n"
      << "                         compare renders legacy and reports differences.\n"
      << "  -materialShading legacy|lightrt-bsdf\n"
      << "                         CPU material shading path. lightrt-bsdf is an\n"
      << "                         experimental opt-in direct-light BSDF path.\n"
#ifdef LIGHTUSD_WITH_QJS
      << "  -js <file.js>          Drive rendering from a JavaScript script.\n"
      << "                         Scene + BVH stay resident across renders\n"
      << "                         (memory-persistent, e.g. camera animation).\n"
      << "                         API: lusdrender.{setCamera,orbit,setResolution,\n"
      << "                         setAmbient,setBackground,setShadows,setSamples,\n"
      << "                         autoframe,bounds,stats,render}.\n"
      << "  -mcp                   Run an MCP stdio control server over the\n"
      << "                         resident scene (tools: eval,set_camera,orbit,\n"
      << "                         set_resolution,render,bounds,stats).\n"
#endif
      << "  -streamHttp <port>     Serve a WebSocket browser viewer that streams\n"
      << "                         rendered frames; orbit/pan/dolly from the browser\n"
      << "                         re-render on the resident scene (no output needed).\n"
      << "  -streamCodec <c>       Idle-refine codec when the view is stable: png\n"
      << "                         (default) or qoi (lossless).\n"
      << "  -streamMotionRes <px>  Long-edge cap for low-quality frames while moving\n"
      << "                         (default 1280; rendered small for speed).\n"
      << "  -streamMotionQuality <n>  Motion JPEG quality 1-100 (default 45).\n"
      << "  -streamIdleMs <ms>     Input-quiet time before the lossless refine\n"
      << "                         frame is rendered (default 320).\n"
      << "  -vk                   Use the Vulkan rasterizer backend.\n"
      << "  -vkr                  Use the Vulkan ray-tracing backend.\n"
      << "  -vkDevice <N>         Select Vulkan physical-device index (default auto).\n"
      << "  -vkInstanced          -vkr + a true two-level GPU TLAS (one BLAS per\n"
      << "                        prototype, shared across instances; saves VRAM on\n"
      << "                        instanced scenes). Falls back to flat if no shares.\n"
      << "  -gpuShade cpu|preview GPU backend shading mode. cpu preserves the current\n"
      << "                        reference CPU shade-after-hit path; preview reserves\n"
      << "                        the fast GPU-preview shader path and currently falls\n"
      << "                        back to cpu until that shader path is enabled.\n"
      << "  -d3d                  Use the Direct3D 11 compute backend (Windows).\n"
      << "  -hip                  Use the HIP/ROCm GPU-compute backend (AMD).\n"
      << "  -cuda                 Use the shared CUDA/NVRTC RT backend (NVIDIA).\n"
      << "  --path-trace          Use the shared multi-bounce GPU integrator.\n"
      << "  --pt-quality interactive|final\n"
      << "  --pt-samples <N>      Path-trace sample target (0 = backend default).\n"
      << "  --pt-max-depth <N>    Maximum path depth (1..64).\n"
      << "  --pt-rr-depth <N>     Russian-roulette start depth.\n"
      << "  --pt-seed <N>         Deterministic path-trace seed.\n"
      << "  --pt-variance <f>     Relative convergence threshold (0 disables).\n"
      << "  -texMaxSize <N>       Downsize loaded textures whose longest edge exceeds N\n"
      << "                         (auto-bounded unless explicitly set).\n"
      << "  -texBudgetMb <N>      Decoded texture budget in MiB (auto-derived unless\n"
      << "                         explicitly set).\n"
      << "  -texCompress off|bc   Request BCn compression; currently falls back to\n"
      << "                        resized 8-bit textures where a backend lacks BCn.\n"
      << "  -udim sparse|atlas    UDIM handling mode (default sparse).\n"
      << "  -noDirectPrims         Tessellate USD shapes/curves/NURBS instead of\n"
      << "                         using lusdrender direct primitive paths.\n"
      << "  -stats                 Print scene/BVH stats.\n"
      << "  -noar                  Disable external asset resolution.\n";
}

bool ParseArgs(int argc, char **argv, Options *opt) {
  if (!opt) return false;
  if (argc <= 1) {
    PrintUsage(argv[0]);
    return false;
  }
  std::vector<std::string> positional;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto need_value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };
    // NOTE: this ~90-branch else-if chain on CLI flag string `a` was
    // converted to standalone ifs -- same MSVC C1061 ("blocks nested too
    // deeply") risk class already fixed for the same reason elsewhere in
    // this codebase (crate-writer-values.cc, stage-converter.cc). OPT_MATCH
    // folds the "did anything match yet" guard into the condition itself
    // (mirroring TRY_PACK_META's style) so the final unknown-option/
    // positional-arg fallback below only fires when no flag matched --
    // exactly the original else-if chain's semantics -- without needing to
    // touch every branch body.
    bool opt_matched = false;
#define OPT_MATCH(cond) if (!opt_matched && (cond) && (opt_matched = true))
    OPT_MATCH(a == "-h" || a == "-help" || a == "--help") {
      PrintUsage(argv[0]);
      std::exit(EXIT_SUCCESS);
    }
    OPT_MATCH(a == "-w" || a == "-width" || a == "--width") {
      const char *v = need_value(a.c_str());
      // Capped: the framebuffer is width*height*4 floats with no other guard, so
      // an absurd size is an OOM, not a render. 32k is far above real use.
      if (!v || !ParseIntStrict(v, &opt->width) || opt->width <= 0 ||
          opt->width > 32768) {
        std::cerr << "Invalid width (1..32768).\n";
        return false;
      }
      opt->width_explicit = true;
    }
    OPT_MATCH(a == "-height" || a == "--height") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseIntStrict(v, &opt->height) || opt->height <= 0 ||
          opt->height > 32768) {
        std::cerr << "Invalid height (1..32768).\n";
        return false;
      }
      opt->height_explicit = true;
    }
    OPT_MATCH(a == "-camera" || a == "--camera") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      opt->camera = v;
      opt->camera_explicit = true;
    }
    OPT_MATCH(a == "--aov") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      const std::string name(v);
      if (name == "color") opt->aov = CaptureAov::Color;
      else if (name == "depth") opt->aov = CaptureAov::Depth;
      else if (name == "worldNormal") opt->aov = CaptureAov::WorldNormal;
      else if (name == "primId") opt->aov = CaptureAov::PrimId;
      else { std::cerr << "Unknown AOV: " << name << "\n"; return false; }
      opt->aov_explicit = true;
      opt->rt_preview = true;
    }
    OPT_MATCH(a == "--all-products") {
      opt->all_render_products = true;
      opt->rt_preview = true;
    }
    OPT_MATCH(a == "-renderSettings" || a == "--renderSettings") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      opt->render_settings = v;
    }
    OPT_MATCH(a == "-renderProduct" || a == "--renderProduct") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      opt->render_product = v;
    }
    OPT_MATCH(a == "-renderPass" || a == "--renderPass") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      opt->render_pass = v;
    }
    OPT_MATCH(a == "-fitScale" || a == "--fitScale") {
      const char *v = need_value(a.c_str());
      // std::stof threw (uncaught -> SIGABRT) on non-numeric/out-of-range input.
      float fs = 0.0f;
      if (!v || !ParseFloatStrict(v, &fs)) {
        std::cerr << "Invalid fitScale.\n";
        return false;
      }
      opt->fit_scale = std::max(0.05f, fs);
    }
    OPT_MATCH(a == "-viewDir" || a == "--viewDir") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseColor(v, &opt->view_dir) || Length(opt->view_dir) < 1.0e-6f) {
        std::cerr << "Invalid viewDir.\n";
        return false;
      }
      opt->has_view_dir = true;
    }
    OPT_MATCH(a == "-purpose" || a == "--purpose") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      uint32_t mask = 0;
      std::string s(v);
      size_t pos = 0;
      while (pos <= s.size()) {
        size_t comma = s.find(',', pos);
        std::string tok = s.substr(pos, comma == std::string::npos
                                            ? std::string::npos
                                            : comma - pos);
        if (tok == "default") {
          mask |= kPurposeDefaultBit;
        } else if (tok == "render") {
          mask |= kPurposeRenderBit;
        } else if (tok == "proxy") {
          mask |= kPurposeProxyBit;
        } else if (tok == "guide") {
          mask |= kPurposeGuideBit;
        } else if (!tok.empty()) {
          std::cerr << "Invalid purpose token: " << tok << "\n";
          return false;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
      opt->purpose_mask = mask;
      opt->purpose_explicit = true;
    }
    OPT_MATCH(a == "-showGuide" || a == "--showGuide") {
      opt->purpose_mask |= kPurposeGuideBit;
      opt->purpose_explicit = true;
    }
    OPT_MATCH(a == "-hideProxy" || a == "--hideProxy") {
      opt->purpose_mask &= ~kPurposeProxyBit;
      opt->purpose_explicit = true;
    }
    OPT_MATCH(a == "-hideRender" || a == "--hideRender") {
      opt->purpose_mask &= ~kPurposeRenderBit;
      opt->purpose_explicit = true;
    }
    OPT_MATCH(a == "-hideDefault" || a == "--hideDefault") {
      opt->purpose_mask &= ~kPurposeDefaultBit;
      opt->purpose_explicit = true;
    }
    OPT_MATCH(a == "-timecode" || a == "--timecode") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseDoubleStrict(v, &opt->timecode)) {
        std::cerr << "Invalid timecode.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-samples" || a == "--samples") {
      const char *v = need_value(a.c_str());
      // Capped: samples multiplies the whole render time with no other guard.
      if (!v || !ParseIntStrict(v, &opt->samples) || opt->samples <= 0 ||
          opt->samples > 65536) {
        std::cerr << "Invalid samples (1..65536).\n";
        return false;
      }
    }
    OPT_MATCH(a == "-bg" || a == "--bg") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseColor(v, &opt->bg)) {
        std::cerr << "Invalid background color. Expected r,g,b.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-ambient" || a == "--ambient") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseFloatStrict(v, &opt->ambient) || opt->ambient < 0.0f) {
        std::cerr << "Invalid ambient value.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-smooth" || a == "--smooth") {
      opt->smooth = true;
    }
    OPT_MATCH(a == "-ibl" || a == "--ibl") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      if (std::string(v) == "default") {
        opt->ibl_envmap = false;
      } else if (std::string(v) == "envmap") {
        opt->ibl_envmap = true;
      } else {
        std::cerr << "-ibl must be default or envmap.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-noDisplace" || a == "--noDisplace") {
      opt->displace = false;
    }
    OPT_MATCH(a == "-texMaxSize" || a == "--texMaxSize") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseIntStrict(v, &opt->texture_max_size) ||
          opt->texture_max_size < 0) {
        std::cerr << "Invalid texture max size.\n";
        return false;
      }
      opt->texture_max_size_explicit = true;
    }
    OPT_MATCH(a == "-texBudgetMb" || a == "--texBudgetMb") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseIntStrict(v, &opt->texture_budget_mb) ||
          opt->texture_budget_mb < 0) {
        std::cerr << "Invalid texture budget.\n";
        return false;
      }
      opt->texture_budget_explicit = true;
    }
    OPT_MATCH(a == "-texCompress" || a == "--texCompress") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      std::string mode(v);
      if (mode == "off") {
        opt->texture_compress = Options::TextureCompress::Off;
      } else if (mode == "bc") {
        opt->texture_compress = Options::TextureCompress::BCn;
      } else {
        std::cerr << "Invalid texture compression mode.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-udim" || a == "--udim") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      std::string mode(v);
      if (mode == "sparse") {
        opt->udim_mode = Options::UdimMode::Sparse;
      } else if (mode == "atlas") {
        opt->udim_mode = Options::UdimMode::Atlas;
      } else {
        std::cerr << "Invalid UDIM mode.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-displaceScale" || a == "--displaceScale") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseFloatStrict(v, &opt->displace_scale)) {
        std::cerr << "Invalid -displaceScale value.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-noShadows" || a == "--noShadows") {
      opt->shadows = false;
    }
    OPT_MATCH(a == "-rtPreview" || a == "--rtPreview" ||
               a == "-mmapRt" || a == "--mmapRt") {
      opt->rt_preview = true;
      opt->direct_prims = false;
      opt->backend_explicit = true;
    }
    OPT_MATCH(a == "-legacyLoad" || a == "--legacyLoad") {
      opt->legacy_load = true;
    }
    OPT_MATCH(a == "-materialResolver" || a == "--materialResolver") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      std::string mode = v;
      if (mode == "legacy") {
        opt->material_resolver = Options::MaterialResolver::Legacy;
      } else if (mode == "tydra-next") {
        opt->material_resolver = Options::MaterialResolver::TydraNext;
      } else if (mode == "compare") {
        opt->material_resolver = Options::MaterialResolver::Compare;
      } else {
        std::cerr << "Invalid material resolver. Expected legacy, tydra-next, or compare.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-materialShading" || a == "--materialShading") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      std::string mode = v;
      if (mode == "legacy") {
        opt->material_shading = Options::MaterialShading::Legacy;
      } else if (mode == "lightrt-bsdf") {
        opt->material_shading = Options::MaterialShading::LightRtBsdf;
      } else {
        std::cerr << "Invalid material shading. Expected legacy or lightrt-bsdf.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-progress" || a == "--progress") {
      opt->progress = true;
    }
    OPT_MATCH(a == "-quality" || a == "--quality") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      std::string q = v;
      if (q == "fast") {
        opt->quality = LRT_TRI_BUILD_FAST;
      } else if (q == "default") {
        opt->quality = LRT_TRI_BUILD_DEFAULT;
      } else if (q == "hq") {
        opt->quality = LRT_TRI_BUILD_HQ;
      } else {
        std::cerr << "Invalid quality. Expected fast, default, or hq.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-threads" || a == "--threads") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseIntStrict(v, &opt->threads) || opt->threads < 0) {
        std::cerr << "Invalid thread count.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-maxMem" || a == "--maxMem") {
      const char *v = need_value(a.c_str());
      char *end = nullptr;
      double g = v ? std::strtod(v, &end) : 0.0;
      if (!v || end == v || g < 0.0) {
        std::cerr << "Invalid -maxMem (expected GiB, e.g. -maxMem 24).\n";
        return false;
      }
      opt->max_mem_gib = g;
      opt->max_mem_explicit = true;
    }
    OPT_MATCH(a == "-lodStream" || a == "--lodStream") {
      opt->lod_stream = true;
      opt->lod_stream_explicit = true;
    }
    OPT_MATCH(a == "-maxVram" || a == "--maxVram") {
      const char *v = need_value(a.c_str());
      char *end = nullptr;
      double g = v ? std::strtod(v, &end) : 0.0;
      if (!v || end == v || g < 0.0) {
        std::cerr << "Invalid -maxVram (expected GiB, e.g. -maxVram 8).\n";
        return false;
      }
      opt->max_vram_gib = g;
      opt->max_vram_explicit = true;
    }
    OPT_MATCH(a == "-rtLod" || a == "--rtLod") {
      opt->rt_lod = true;
      opt->rt_lod_explicit = true;
    }
    OPT_MATCH(a == "-rtLodNoProxy" || a == "--rtLodNoProxy") {
      opt->rt_lod_proxy = false;
    }
    OPT_MATCH(a == "-rtLodFrustumCull" || a == "--rtLodFrustumCull") {
      opt->rt_lod_frustum_cull = true;
    }
    OPT_MATCH(a == "-rtLodFullPx" || a == "--rtLodFullPx") {
      const char *v = need_value(a.c_str());
      char *end = nullptr;
      double g = v ? std::strtod(v, &end) : 0.0;
      if (!v || end == v || g <= 0.0) {
        std::cerr << "Invalid -rtLodFullPx (expected pixels > 0).\n";
        return false;
      }
      opt->rt_lod_full_px = float(g);
      opt->rt_lod_full_px_explicit = true;
    }
    OPT_MATCH(a == "-rtLodCullPx" || a == "--rtLodCullPx") {
      const char *v = need_value(a.c_str());
      char *end = nullptr;
      double g = v ? std::strtod(v, &end) : 0.0;
      if (!v || end == v || g < 0.0) {
        std::cerr << "Invalid -rtLodCullPx (expected pixels >= 0).\n";
        return false;
      }
      opt->rt_lod_cull_px = float(g);
      opt->rt_lod_cull_px_explicit = true;
    }
    OPT_MATCH(a == "-lodDistrictMem" || a == "--lodDistrictMem") {
      const char *v = need_value(a.c_str());
      char *end = nullptr;
      double g = v ? std::strtod(v, &end) : 0.0;
      if (!v || end == v || g <= 0.0) {
        std::cerr << "Invalid -lodDistrictMem (expected GiB > 0).\n";
        return false;
      }
      opt->lod_district_mem_gib = g;
    }
    OPT_MATCH(a == "-lodDistrictVram" || a == "--lodDistrictVram") {
      const char *v = need_value(a.c_str());
      char *end = nullptr;
      double g = v ? std::strtod(v, &end) : 0.0;
      if (!v || end == v || g <= 0.0) {
        std::cerr << "Invalid -lodDistrictVram (expected GiB > 0).\n";
        return false;
      }
      opt->lod_district_vram_gib = g;
    }
    OPT_MATCH(a == "-lodMinVerts" || a == "--lodMinVerts") {
      const char *v = need_value(a.c_str());
      char *end = nullptr;
      double g = v ? std::strtod(v, &end) : 0.0;
      if (!v || end == v || g < 0.0) {
        std::cerr << "Invalid -lodMinVerts (expected a count >= 0).\n";
        return false;
      }
      opt->lod_min_verts = g;
    }
    OPT_MATCH(a == "-lodMaxVerts" || a == "--lodMaxVerts") {
      const char *v = need_value(a.c_str());
      char *end = nullptr;
      double g = v ? std::strtod(v, &end) : 0.0;
      if (!v || end == v || g < 0.0) {
        std::cerr << "Invalid -lodMaxVerts (expected a count >= 0).\n";
        return false;
      }
      opt->lod_max_verts = g;
    }
    OPT_MATCH(a == "-lodContainer" || a == "--lodContainer") {
      const char *v = need_value(a.c_str());
      if (!v) {
        std::cerr << "Invalid -lodContainer (expected a prim name).\n";
        return false;
      }
      opt->lod_container = v;
    }
    OPT_MATCH(a == "-env" || a == "--env") {
      const char *v = need_value(a.c_str());
      if (!v) {
        std::cerr << "Invalid -env (expected an environment-map path).\n";
        return false;
      }
      opt->env_file = v;
    }
    OPT_MATCH(a == "-subdiv" || a == "--subdiv" ||
               a == "-subdivLevel" || a == "--subdivLevel" ||
               a == "-subdivisionLevel" || a == "--subdivisionLevel") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseIntStrict(v, &opt->subdivision_level) ||
          opt->subdivision_level < 0 ||
          opt->subdivision_level > lightusd::tsd::kMaxLevel) {
        std::cerr << "Invalid subdivision level. Expected 0.."
                  << lightusd::tsd::kMaxLevel << ".\n";
        return false;
      }
    }
    OPT_MATCH(a == "-complexity" || a == "--complexity") {
      // OpenUSD usdrecord refinement presets -> subdivision level
      // (low=1.0, medium=1.1, high=1.2, veryhigh=1.3 -> refine 0/1/2/3).
      const char *v = need_value(a.c_str());
      const std::string s = v ? v : "";
      if (s == "low") {
        opt->subdivision_level = 0;
      } else if (s == "medium") {
        opt->subdivision_level = 1;
      } else if (s == "high") {
        opt->subdivision_level = 2;
      } else if (s == "veryhigh") {
        opt->subdivision_level = 3;
      } else {
        std::cerr << "Invalid -complexity. Expected low|medium|high|veryhigh.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-autoframe" || a == "--autoframe") {
      opt->autoframe = true;
    }
    OPT_MATCH(a == "-mask" || a == "--mask") {
      const char *v = need_value(a.c_str());
      if (!v) {
        std::cerr << "-mask requires PRIMPATH[,PRIMPATH...].\n";
        return false;
      }
      // Comma- and/or space-separated absolute prim paths.
      std::string s = v;
      for (char &ch : s) {
        if (ch == ',') ch = ' ';
      }
      std::istringstream iss(s);
      std::string p;
      while (iss >> p) {
        if (!p.empty()) opt->mask.push_back(p);
      }
    }
    OPT_MATCH(a == "-frames" || a == "--frames" || a == "-f") {
      const char *v = need_value(a.c_str());
      if (!v) {
        std::cerr << "-frames requires a FRAMESPEC.\n";
        return false;
      }
      opt->frames = v;
    }
    OPT_MATCH(a == "-defaultTime" || a == "--defaultTime") {
      opt->default_time = true;
    }
    OPT_MATCH(a == "-variant" || a == "--variant") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      std::string s = v;
      size_t eq = s.find('=');
      if (eq == std::string::npos || eq == 0 || eq + 1 >= s.size()) {
        std::cerr << "Invalid --variant. Expected SET=SELECTION (e.g. "
                     "--variant districtLod=full).\n";
        return false;
      }
      opt->variant_overrides[s.substr(0, eq)] = s.substr(eq + 1);
    }
    OPT_MATCH(a == "-largeSceneProfile" || a == "--largeSceneProfile" ||
               a == "-large-scene-profile" || a == "--large-scene-profile") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseLargeSceneProfile(v, &opt->large_scene_profile)) {
        std::cerr << "-largeSceneProfile must be off, auto, caldera, island, or alab.\n";
        return false;
      }
    }
    OPT_MATCH(a.rfind("--largeSceneProfile=", 0) == 0) {
      if (!ParseLargeSceneProfile(a.substr(20), &opt->large_scene_profile)) {
        std::cerr << "-largeSceneProfile must be off, auto, caldera, island, or alab.\n";
        return false;
      }
    }
    OPT_MATCH(a.rfind("--large-scene-profile=", 0) == 0) {
      if (!ParseLargeSceneProfile(a.substr(22), &opt->large_scene_profile)) {
        std::cerr << "-largeSceneProfile must be off, auto, caldera, island, or alab.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-vk" || a == "--vk") {
      opt->vulkan = true;
      opt->backend_explicit = true;
    }
    OPT_MATCH(a == "-vkr" || a == "--vkr") {
      opt->vulkan = true;
      opt->vulkan_rt = true;
      opt->backend_explicit = true;
    }
    OPT_MATCH(a == "-vkDevice" || a == "--vk-device") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseIntStrict(v, &opt->vulkan_device) ||
          opt->vulkan_device < 0) {
        std::cerr << "-vkDevice must be a non-negative physical-device index.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-vkInstanced" || a == "--vkInstanced") {
      opt->vulkan = true;
      opt->vulkan_rt = true;
      opt->vulkan_instanced = true;
      opt->backend_explicit = true;
    }
    OPT_MATCH(a == "-gpuShade" || a == "--gpuShade") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      std::string mode = v;
      if (mode == "cpu") {
        opt->gpu_shade = Options::GpuShadeMode::Cpu;
      } else if (mode == "preview") {
        opt->gpu_shade = Options::GpuShadeMode::Preview;
      } else {
        std::cerr << "-gpuShade must be cpu or preview.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-d3d" || a == "--d3d" || a == "-dx" || a == "--dx") {
      opt->use_d3d = true;
      opt->backend_explicit = true;
    }
    OPT_MATCH(a == "-hip" || a == "--hip") {
      opt->hip = true;
      opt->backend_explicit = true;
    }
    OPT_MATCH(a == "-cuda" || a == "--cuda") {
      opt->cuda = true;
      opt->backend_explicit = true;
    }
    OPT_MATCH(a == "-path-trace" || a == "--path-trace") {
      opt->path_trace = true;
    }
    OPT_MATCH(a == "-pt-quality" || a == "--pt-quality") {
      const char *v = need_value(a.c_str());
      if (!v) return false;
      const std::string quality = v;
      if (quality == "interactive") {
        opt->path_trace_quality = Options::PathTraceQuality::Interactive;
      } else if (quality == "final") {
        opt->path_trace_quality = Options::PathTraceQuality::Final;
      } else {
        std::cerr << "--pt-quality must be interactive or final.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-pt-samples" || a == "--pt-samples" ||
              a == "-pt-max-depth" || a == "--pt-max-depth" ||
              a == "-pt-rr-depth" || a == "--pt-rr-depth" ||
              a == "-pt-seed" || a == "--pt-seed") {
      const std::string option = a;
      const char *v = need_value(a.c_str());
      int parsed = 0;
      if (!v || !ParseIntStrict(v, &parsed) || parsed < 0 ||
          parsed > 1000000) {
        std::cerr << option << " requires an integer in [0, 1000000].\n";
        return false;
      }
      if (option.find("samples") != std::string::npos)
        opt->path_trace_samples = static_cast<uint32_t>(parsed);
      else if (option.find("max-depth") != std::string::npos)
        opt->path_trace_max_depth = static_cast<uint32_t>(parsed);
      else if (option.find("rr-depth") != std::string::npos)
        opt->path_trace_rr_depth = static_cast<uint32_t>(parsed);
      else
        opt->path_trace_seed = static_cast<uint32_t>(parsed);
    }
    OPT_MATCH(a == "-pt-variance" || a == "--pt-variance") {
      const char *v = need_value(a.c_str());
      float parsed = 0.0f;
      if (!v || !ParseFloatStrict(v, &parsed) || parsed < 0.0f) {
        std::cerr << "--pt-variance requires a non-negative number.\n";
        return false;
      }
      opt->path_trace_variance = parsed;
    }
    OPT_MATCH(a == "-js" || a == "--js") {
      const char *v = need_value(a.c_str());
      if (!v) {
        std::cerr << "-js requires a script file path.\n";
        return false;
      }
      opt->js_script = v;
      opt->rt_preview = true;
      opt->direct_prims = false;
    }
    OPT_MATCH(a == "-mcp" || a == "--mcp") {
      opt->mcp = true;
      opt->rt_preview = true;
      opt->direct_prims = false;
    }
    OPT_MATCH(a == "-streamHttp" || a == "--streamHttp") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseIntStrict(v, &opt->stream_http) ||
          opt->stream_http <= 0 || opt->stream_http > 65535) {
        std::cerr << "-streamHttp requires a port (1-65535).\n";
        return false;
      }
      opt->rt_preview = true;
      opt->direct_prims = false;
    }
    OPT_MATCH(a == "-streamCodec" || a == "--streamCodec") {
      const char *v = need_value(a.c_str());
      if (!v) {
        std::cerr << "-streamCodec requires png|qoi.\n";
        return false;
      }
      opt->stream_codec = v;
    }
    OPT_MATCH(a == "-streamMotionRes" || a == "--streamMotionRes") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseIntStrict(v, &opt->stream_motion_res) ||
          opt->stream_motion_res <= 0) {
        std::cerr << "-streamMotionRes requires a positive pixel size.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-streamMotionQuality" || a == "--streamMotionQuality") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseIntStrict(v, &opt->stream_motion_quality) ||
          opt->stream_motion_quality < 1 || opt->stream_motion_quality > 100) {
        std::cerr << "-streamMotionQuality requires 1-100.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-streamIdleMs" || a == "--streamIdleMs") {
      const char *v = need_value(a.c_str());
      if (!v || !ParseIntStrict(v, &opt->stream_idle_ms) ||
          opt->stream_idle_ms < 0) {
        std::cerr << "-streamIdleMs requires a non-negative millisecond value.\n";
        return false;
      }
    }
    OPT_MATCH(a == "-noDirectPrims" || a == "--noDirectPrims") {
      opt->direct_prims = false;
    }
    OPT_MATCH(a == "-stats" || a == "--stats") {
      opt->stats = true;
    }
    OPT_MATCH(a == "-noar" || a == "--noar") {
      opt->no_assetresolver = true;
    }
#undef OPT_MATCH
    if (!opt_matched) {
      if (!a.empty() && a[0] == '-') {
        std::cerr << "Unknown option: " << a << "\n";
        return false;
      } else {
        positional.push_back(a);
      }
    }
  }
  // -js / -mcp drive output paths from the script / MCP calls, so only the
  // input is required there; an output positional is optional.
  const bool output_optional =
      opt->mcp || !opt->js_script.empty() || opt->stream_http > 0;
  if (positional.empty() || positional.size() > 2 ||
      (!output_optional && positional.size() != 2)) {
    PrintUsage(argv[0]);
    return false;
  }
  opt->input = positional[0];
  opt->output = positional.size() > 1 ? positional[1] : std::string();
  if (opt->path_trace && !opt->cuda && !opt->hip && !opt->vulkan_rt) {
    std::cerr << "--path-trace requires -vkr, -cuda, or -hip.\n";
    return false;
  }
  return true;
}

int NullARResolve(const char *, const std::vector<std::string> &, std::string *,
                  std::string *, void *) {
  return -1;
}

int NullARSize(const char *, uint64_t *, std::string *, void *) { return -1; }

int NullARRead(const char *, uint64_t, uint8_t *, uint64_t *, std::string *,
               void *) {
  return -1;
}

void SetupNullAssetResolution(lightusd::AssetResolutionResolver *resolver) {
  if (!resolver) return;
  lightusd::AssetResolutionHandler handler;
  handler.resolve_fun = NullARResolve;
  handler.size_fun = NullARSize;
  handler.read_fun = NullARRead;
  handler.write_fun = nullptr;
  handler.userdata = nullptr;
  resolver->register_wildcard_asset_resolution_handler(handler);
}

}  // namespace lusdr
