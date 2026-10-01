// SPDX-License-Identifier: Apache-2.0
// lusdview - a USD viewer example for lightusd.
//
// Usage:
//   lusdview [options] [file.usd|usda|usdc|usdz]
//   --backend gl|vk   Select rendering backend (default: vk when available)
//
// Loads a USD file via lightusd, converts it with the Tydra RenderScene API and
// renders it with OpenGL (and Vulkan if built with HAVE_VULKAN). The GUI uses an
// ImGui docking layout with a prim hierarchy browser, a property inspector and a
// 3D viewport with Maya-style navigation (Alt+LMB orbit / Alt+MMB pan /
// Alt+RMB+wheel dolly, 'F' to frame the scene).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "security-policy.hh"

#include "app.hh"
#include "config.hh"
#include "log.hh"
#include "renderer.hh"
#include "lightusd-render-cpp.hh"

namespace {

enum class LargeSceneProfile { Off, Auto, Balanced, InstanceHeavy, ProceduralHeavy };

const char* ProfileName(LargeSceneProfile p) {
  switch (p) {
    case LargeSceneProfile::Off: return "off";
    case LargeSceneProfile::Auto: return "auto";
    case LargeSceneProfile::Balanced: return "balanced";
    case LargeSceneProfile::InstanceHeavy: return "instance-heavy";
    case LargeSceneProfile::ProceduralHeavy: return "procedural-heavy";
  }
  return "off";
}

std::string LowerCopy(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

bool ParseProfile(const char* s, LargeSceneProfile* out) {
  if (!s || !out) return false;
  const std::string v = LowerCopy(s);
  if (v == "off") *out = LargeSceneProfile::Off;
  else if (v == "auto") *out = LargeSceneProfile::Auto;
  else if (v == "balanced") *out = LargeSceneProfile::Balanced;
  else if (v == "instance-heavy") *out = LargeSceneProfile::InstanceHeavy;
  else if (v == "procedural-heavy") *out = LargeSceneProfile::ProceduralHeavy;
  else return false;
  return true;
}

static const std::pair<const char*, lusdview::RenderMode> kModeTable[] = {
    {"shaded", lusdview::RenderMode::Shaded},
    {"wireframe", lusdview::RenderMode::Wireframe},
    {"normals", lusdview::RenderMode::Normals},
    {"material-id", lusdview::RenderMode::MaterialId},
    {"geom-normal", lusdview::RenderMode::GeomNormal},
    {"uv", lusdview::RenderMode::Uv},
    {"depth", lusdview::RenderMode::Depth},
    {"albedo", lusdview::RenderMode::Albedo},
    {"facing", lusdview::RenderMode::Facing},
    {"roughness", lusdview::RenderMode::Roughness},
    {"metallic", lusdview::RenderMode::Metallic},
    {"emissive", lusdview::RenderMode::Emissive},
    {"opacity", lusdview::RenderMode::Opacity},
    {"position", lusdview::RenderMode::Position},
    {"barycentric", lusdview::RenderMode::Barycentric},
    {"prim-id", lusdview::RenderMode::PrimId},
    {"mesh-id", lusdview::RenderMode::MeshId},
    {"picking", lusdview::RenderMode::MeshId},
    {"pick-id", lusdview::RenderMode::MeshId},
    {"purpose", lusdview::RenderMode::Purpose},
    {"missing-normals", lusdview::RenderMode::MissingNormals},
    {"double-sided", lusdview::RenderMode::DoubleSided},
    {"skin-weights", lusdview::RenderMode::SkinWeights},
    {"tangent", lusdview::RenderMode::Tangent},
    {"uv-checker", lusdview::RenderMode::UvChecker},
    {"ao", lusdview::RenderMode::AmbientOcclusion},
    {"curvature", lusdview::RenderMode::Curvature},
    {"instance-id", lusdview::RenderMode::InstanceId},
    {"bvh-heatmap", lusdview::RenderMode::BvhHeatmap},
    {"soft-shadow", lusdview::RenderMode::SoftShadow},
    {"kind", lusdview::RenderMode::Kind},
    {"udim", lusdview::RenderMode::UdimTile},
    {"uv1", lusdview::RenderMode::Uv1},
    {"blend-influence", lusdview::RenderMode::BlendInfluence},
    {"texel-density", lusdview::RenderMode::TexelDensity},
    {"source-face-id", lusdview::RenderMode::SourceFaceId},
    {"coat-normal", lusdview::RenderMode::CoatNormal},
    {"coat-weight", lusdview::RenderMode::CoatWeight},
    {"coat-color", lusdview::RenderMode::CoatColor},
    {"coat-roughness", lusdview::RenderMode::CoatRoughness},
    {"specular-f0", lusdview::RenderMode::SpecularF0},
    {"ior-f0", lusdview::RenderMode::IorF0},
};

// Name -> RenderMode, shared by --mode and --mode-sweep.
bool ParseRenderModeName(const char* name, lusdview::RenderMode* out) {
  const auto* it = std::find_if(
      std::begin(kModeTable), std::end(kModeTable),
      [name](const std::pair<const char*, lusdview::RenderMode>& e) {
        return std::strcmp(name, e.first) == 0;
      });
  if (it == std::end(kModeTable)) return false;
  if (out) *out = it->second;
  return true;
}

std::uint64_t ParseByteCount(const std::string& text) {
  std::string v = text;
  std::uint64_t mul = 1;
  if (!v.empty()) {
    char s = v.back();
    if (s == 'k' || s == 'K') { mul = 1024ull; v.pop_back(); }
    else if (s == 'm' || s == 'M') { mul = 1024ull * 1024ull; v.pop_back(); }
    else if (s == 'g' || s == 'G') {
      mul = 1024ull * 1024ull * 1024ull;
      v.pop_back();
    }
  }
  return static_cast<std::uint64_t>(std::strtoull(v.c_str(), nullptr, 10)) * mul;
}

bool ParsePrimLevel(const std::string& text, std::string* prim, int* level) {
  const size_t eq = text.rfind('=');
  const size_t colon = text.rfind(':');
  size_t sep = std::string::npos;
  if (eq != std::string::npos && colon != std::string::npos) {
    sep = std::max(eq, colon);
  } else {
    sep = (eq != std::string::npos) ? eq : colon;
  }
  if (sep == std::string::npos || sep == 0 || sep + 1 >= text.size()) return false;
  std::string value = text.substr(sep + 1);
  char* end = nullptr;
  long v = std::strtol(value.c_str(), &end, 10);
  if (!end || *end != '\0' || v < 0 || v > 16) return false;
  *prim = text.substr(0, sep);
  *level = static_cast<int>(v);
  return !prim->empty();
}

bool ParseWindowSize(const char* text, int* width, int* height) {
  if (!text || !width || !height) return false;
  char* widthEnd = nullptr;
  const long parsedWidth = std::strtol(text, &widthEnd, 10);
  if (!widthEnd || widthEnd == text || *widthEnd != 'x') return false;
  char* heightEnd = nullptr;
  const long parsedHeight = std::strtol(widthEnd + 1, &heightEnd, 10);
  if (!heightEnd || heightEnd == widthEnd + 1 || *heightEnd != '\0') return false;
  constexpr long kMaxWindowDimension = 32768;
  if (parsedWidth <= 0 || parsedWidth > kMaxWindowDimension ||
      parsedHeight <= 0 || parsedHeight > kMaxWindowDimension) {
    return false;
  }
  *width = static_cast<int>(parsedWidth);
  *height = static_cast<int>(parsedHeight);
  return true;
}

bool ParseUnsigned(const char* text, unsigned* value) {
  if (!text || !value || text[0] == '\0' || text[0] == '-') return false;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(text, &end, 10);
  if (!end || end == text || *end != '\0' ||
      parsed > static_cast<unsigned long>(UINT32_MAX)) {
    return false;
  }
  *value = static_cast<unsigned>(parsed);
  return true;
}

bool ParseFinitePositiveFloat(const char* text, float* value) {
  if (!text || !value || text[0] == '\0') return false;
  char* end = nullptr;
  const float parsed = std::strtof(text, &end);
  if (!end || end == text || *end != '\0' || !std::isfinite(parsed) ||
      parsed <= 0.0f) {
    return false;
  }
  *value = parsed;
  return true;
}

bool ParseFiniteNonNegativeFloat(const char* text, float* value) {
  if (!text || !value || text[0] == '\0') return false;
  char* end = nullptr;
  const float parsed = std::strtof(text, &end);
  if (!end || end == text || *end != '\0' || !std::isfinite(parsed) ||
      parsed < 0.0f) {
    return false;
  }
  *value = parsed;
  return true;
}

// Host memory the budget tree may plan against: MemAvailable, capped at the
// 32 GiB the policy targets (planning against a 256 GiB workstation's full RAM
// would size the stage/geometry limits far past anything sensible). Falls back
// to 32 GiB where /proc/meminfo does not exist (macOS, Windows).
uint64_t HostMemoryCapacityBytes() {
  constexpr uint64_t kTarget = (uint64_t{32} << 30);
  std::ifstream f("/proc/meminfo");
  std::string tok;
  while (f >> tok) {
    if (tok == "MemAvailable:") {
      uint64_t kb = 0;
      f >> kb;
      const uint64_t bytes = kb * 1024ull;
      return bytes ? std::min(bytes, kTarget) : kTarget;
    }
    std::getline(f, tok);
  }
  return kTarget;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string executableName =
      (argc > 0 && argv[0]) ? std::filesystem::path(argv[0]).filename().string()
                            : std::string();
  const bool virtualHumanProfile = executableName == "vchar" ||
                                   executableName == "vchar.exe";
#if defined(HAVE_VULKAN)
  lusdview::Backend backend = lusdview::Backend::Vulkan;
#else
  lusdview::Backend backend = lusdview::Backend::GL;
#endif
  bool backendExplicit = false;
  std::string autoriggerExecutable;
  std::optional<std::string> configPath;
  std::string file;
  std::string screenshot;
  std::string linearOutput;
  std::string renderReport;
  int checkpointEvery = 0;
  std::string checkpointPattern;
  std::string windowShot;
  int maxFrames = -1;
  int windowWidth = 0;
  int windowHeight = 0;
  bool windowSizeExplicit = false;
  long long maxTris = 0;      // 0 = default budget
  double maxGpuMemGiB = 0.0;  // --max-gpu-mem: raster full-mesh VRAM cap (GiB)
  // --vram-budget: the ONE number the whole budget tree descends from. Left at 0
  // it uses a conservative fallback. Set LUSDVIEW_VULKAN_VRAM_PROBE=1 to
  // perform the legacy pre-renderer Vulkan probe, or pass --vram-budget.
  double vramBudgetGiB = 0.0;
  bool vramBudgetExplicit = false;
  long long maxDrawMeshes = 0;  // --max-draw-meshes: raster full-mesh count cap
  bool robustFrame = true;      // trim outlier meshes from fit-all auto-frame
  bool rtLod = false;           // --rt-lod: view-dependent RT instance LOD
  float rtLodFullPx = 0.0f;     // 0 => keep App default
  float rtLodCullPx = -1.0f;    // <0 => keep App default
  float rtLodBand = -1.0f;      // <0 => keep App default (stochastic band width)
  bool rasterLod = false;       // --raster-lod: view-dependent raster instance LOD
  bool dynamicLod = false;      // generated prototype LOD + FPS controller
  float rasterLodFullPx = 0.0f; // 0 => keep App default
  float rasterLodCullPx = -1.0f;// <0 => keep App default
  LargeSceneProfile largeSceneProfile = LargeSceneProfile::Off;
  bool largeSceneProfileExplicit = false;
  lusdview::PreviewCacheMode previewCacheMode =
      lusdview::PreviewCacheMode::Auto;
  bool previewCacheExplicit = false;
  std::string previewCacheDir;
  double previewCacheMaxGiB = 8.0;
  bool maxTrisExplicit = false;
  bool maxGpuMemExplicit = false;
  bool maxDrawMeshesExplicit = false;
  bool rtLodExplicit = false;
  bool rtLodFullExplicit = false;
  bool rtLodCullExplicit = false;
  bool rtLodBandExplicit = false;
  bool rasterLodExplicit = false;
  bool dynamicLodExplicit = false;
  bool rasterLodFullExplicit = false;
  bool rasterLodCullExplicit = false;
  bool useNextExplicit = false;
  bool lodMaxMemExplicit = false;
  bool lodMaxVramExplicit = false;
  bool maxAssetBytesExplicit = false;
  std::uint64_t maxAssetReadBytes = 0;
  double timeBudget = 0.0;    // 0 = unlimited
  unsigned compositionThreads = 0;
  unsigned conversionThreads = 0;
  size_t compositionOpinionBatch = 0;
  size_t instanceChunkSamples = 0;
  size_t meshConversionChunkPrims = 0;
  size_t meshConversionChunkMB = 0;
  size_t curveParallelMinPrims = 0;
  double uploadBudgetMs = 8.0;
  size_t streamBufferMB = 64;
  size_t previewMaxBoxes = 0;
  bool timing = false;
  bool quitAfterFullUpload = false;
  bool quitAfterConvert = false;
  bool fullFidelity = false;
  std::optional<float> uiScale;  // Explicit CLI override for font/widget/window scale.
  bool wantRt = false;        // request Vulkan ray tracing (if supported)
  lusdview::RendererDevicePreference devicePreference;
  lusdview::TransparencyMode transparencyMode =
      lusdview::TransparencyMode::Auto;
  bool transparencyExplicit = false;
  bool vkDeviceExplicit = false;
  bool wantCuda = false;      // --cuda: CUDA BVH ray-traced screenshot (cuew runtime)
  lusdview::CudaRtBackend cudaRtBackend = lusdview::CudaRtBackend::Auto;
  bool cudaRtBackendExplicit = false;
  std::string cudaCacheDir;   // --cuda-cache-dir: override compiled PTX cache
  bool wantHip = false;       // --hip: HIP/ROCm BVH ray-traced screenshot (hipew runtime)
  bool wantCpuRt = false;     // --cpu-rt: CPU (lightrt_c) ray tracer
  int rtSamples = 1;          // --rt-samples: AA supersamples for RT paths
  bool wantPathTrace = false;
  lusdview::PathTraceSettings pathTraceSettings;
  bool ptFinal = false;
  std::optional<unsigned> ptSamples;
  std::optional<unsigned> ptMaxDepth;
  std::optional<unsigned> ptRrDepth;
  std::optional<unsigned> ptMotionSegments;
  std::optional<unsigned> ptSeed;
  std::optional<unsigned> ptMaxSssEvents;
  std::optional<unsigned> ptMaxVolumeEvents;
  std::optional<float> ptVariance;
  std::optional<float> ptFireflyClamp;
  lusdview::PathTraceDenoise ptDenoise = lusdview::PathTraceDenoise::Auto;
  bool ptDenoiseExplicit = false;
  float fStopOverride = 0.0f;
  float focusDistanceOverride = 0.0f;
  long long rtMaxInstances = 16000000;  // --max-instances: CUDA/HIP instance cap (0=off)
  bool lodStream = false;     // --lod-stream: view-dependent district LOD (needs --next)
  double lodMaxMem = 0.0;     // --max-mem GiB: host budget for --lod-stream (0=auto)
  double lodMaxVram = 0.0;    // --max-vram GiB: GPU budget for --lod-stream (0=auto)
  std::optional<size_t> curvePreviewPrims;
  std::optional<size_t> curvePreviewStrands;
  bool wantWireframe = false;  // --wireframe: start in wireframe render mode
  bool wantMaterialId = false;
  std::vector<std::pair<std::string, lusdview::RenderMode>> modeSweep;  // --mode-sweep
  std::optional<lusdview::RenderMode> wantMode;  // --mode <name>: any render mode
  std::vector<std::pair<std::string, float>> wantBlend;  // --blend NAME=WEIGHT
  std::string wantSelect;  // --select <prim path>
  bool mcpStdio = false;      // MCP server: stdio transport
  int mcpHttpPort = 0;        // MCP server: HTTP transport port (0 = off)
  bool liveShaderWatch = false;  // poll active GPU shader/kernel source
  size_t materialXVulkanShaderMaxKiB = 129;
  bool materialXVulkanShaderMaxExplicit = false;
  int materialXVulkanCompileTimeoutSec = 30;
  bool materialXVulkanCompileTimeoutExplicit = false;
  int streamHttpPort = 0;     // WebSocket stream server port (0 = off)
  std::string streamCodec = "png";   // idle-refinement codec: png|qoi
  int streamMotionRes = 1280;        // motion-frame long-edge cap (px)
  int streamMotionQuality = 45;      // motion-frame JPEG quality (1-100)
  int streamIdleMs = 350;            // ms of input quiet before the lossless refine
  bool headless = false;      // windowless offscreen rendering (Vulkan only)
  bool threaded = false;
  bool threadedExplicit = false;
  bool adaptiveQuality = true;
  bool adaptiveQualityExplicit = false;
  float targetRenderFps = 30.0f;
  bool targetRenderFpsExplicit = false;
  float minRenderScale = 0.5f;
  bool minRenderScaleExplicit = false;
  bool useNextLoader = true;              // next-core is the default scene path
  bool noCull = false;                     // --no-cull: disable frustum culling
  bool showGrid = true;                    // --no-grid: deterministic clean capture
  bool showSkeleton = true;                // --no-skeleton: hide skeleton helpers
  float camDolly = 1.0f;                    // --cam-dolly: fitted-distance scale
  std::string cameraName;                   // --camera: USD camera to frame (--next)
  bool stereoRequested = false;             // --stereo: authored left/right pair
  bool rasterMotionEnabled = true;           // native raster shutter accumulation
  lusdview::CameraConform cameraConform{lusdview::CameraConform::Fit};
  bool cameraConformExplicit = false;
  bool viewDirExplicit = false;              // --view-dir: deterministic auto-fit view
  float viewDir[3] = {0.0f, 0.0f, -1.0f};   // normalized eye-to-target direction
  bool noComposition = false;             // --no-composition: root layer only
  std::optional<bool> deferPayloads;      // --defer-payloads / --load-payloads
  bool deferReferences = false;           // --defer-references (explicit opt-in)
  bool allowParentPaths = true;           // USD layer-relative paths may use '..'.
  // Bound the interactive viewer's default texture residency. Each of these
  // defaults remains explicitly disableable by the command-line switches.
  lusdview::TextureRuntimeOptions textureOptions;
  textureOptions.maxTextureSize = 4096;
  textureOptions.compression = lusdview::TextureCompressionMode::Auto;
  textureOptions.keepCompressed = true;
  textureOptions.generateMips = true;
  bool domeIblExplicit = false;
  bool rasterQualityHigh = false;
  std::string materialPurpose = "preview";
  bool materialPurposeExplicit = false;
  std::string envmapPath;
  float envmapIntensity = 1.0f;
  float envmapRotation = 0.0f;
  bool maxTextureSizeExplicit = false;
  lightusd::api::TextureFit textureFit{0, LIGHTUSD_TEXTURE_FIT_DEFAULT, 0};  // Default (2/3 of VRAM)
  bool textureFitExplicit = false;
  bool textureBudgetExplicit = false;
  bool textureCompressionExplicit = false;
  bool mipsExplicit = false;
  std::optional<size_t> ptexInitialFaces;
  std::optional<size_t> ptexCacheMB;
  std::optional<int> subdivisionLevel;
  bool subdivisionAuto = false;
  bool asyncTextureDecode = false;
  bool subdivisionAutoExplicit = false;
  int subdivisionAutoMaxLevel = 3;
  bool subdivisionAutoMaxExplicit = false;
  std::map<std::string, int> subdivisionPrimLevels;
  std::optional<double> timeCode;         // --time T: evaluate the scene at this
                                          // time code (animated screenshots)
  lusdview::SkinningMode skinningMode = lusdview::SkinningMode::Auto;
  bool playAnim = false;                  // --play: start timeline playback on load

  for (int i = 1; i < argc; ++i) {
    const int argumentIndex = i;
    if (std::strcmp(argv[i], "--config") == 0) {
      if ((i + 1) >= argc) {
        LOGE("--config requires a path");
        return 1;
      }
      configPath = argv[++i];
    } if (std::strncmp(argv[i], "--config=", 9) == 0) {
      configPath = argv[i] + 9;
    } if (std::strcmp(argv[i], "--backend") == 0 && (i + 1) < argc) {
      ++i;
      if (std::strcmp(argv[i], "vk") == 0 || std::strcmp(argv[i], "vulkan") == 0) {
        backend = lusdview::Backend::Vulkan;
      } else if (std::strcmp(argv[i], "gl") == 0 ||
                 std::strcmp(argv[i], "opengl") == 0) {
        backend = lusdview::Backend::GL;
      } else {
        LOGE("--backend must be gl, opengl, vk, or vulkan");
        return 1;
      }
      backendExplicit = true;
    } if (std::strcmp(argv[i], "--vk-device") == 0 && (i + 1) < argc) {
      devicePreference.vulkanDevice = argv[++i];
      vkDeviceExplicit = true;
      backend = lusdview::Backend::Vulkan;
      backendExplicit = true;
    } if (std::strncmp(argv[i], "--vk-device=", 12) == 0) {
      devicePreference.vulkanDevice = argv[i] + 12;
      vkDeviceExplicit = true;
      backend = lusdview::Backend::Vulkan;
      backendExplicit = true;
    } if (std::strcmp(argv[i], "--transparency") == 0 && (i + 1) < argc) {
      const char* mode = argv[++i];
      if (std::strcmp(mode, "auto") == 0) {
        transparencyMode = lusdview::TransparencyMode::Auto;
      } else if (std::strcmp(mode, "weighted") == 0) {
        transparencyMode = lusdview::TransparencyMode::Weighted;
      } else if (std::strcmp(mode, "sorted") == 0) {
        transparencyMode = lusdview::TransparencyMode::Sorted;
      } else {
        LOGE("--transparency must be auto, weighted, or sorted");
        return 1;
      }
      transparencyExplicit = true;
    } if (std::strncmp(argv[i], "--transparency=", 15) == 0) {
      const char* mode = argv[i] + 15;
      if (std::strcmp(mode, "auto") == 0) {
        transparencyMode = lusdview::TransparencyMode::Auto;
      } else if (std::strcmp(mode, "weighted") == 0) {
        transparencyMode = lusdview::TransparencyMode::Weighted;
      } else if (std::strcmp(mode, "sorted") == 0) {
        transparencyMode = lusdview::TransparencyMode::Sorted;
      } else {
        LOGE("--transparency must be auto, weighted, or sorted");
        return 1;
      }
      transparencyExplicit = true;
    } if (std::strcmp(argv[i], "--frames") == 0 && (i + 1) < argc) {
      maxFrames = std::atoi(argv[++i]);
    } if (std::strcmp(argv[i], "--size") == 0) {
      if ((i + 1) >= argc ||
          !ParseWindowSize(argv[++i], &windowWidth, &windowHeight)) {
        LOGE("--size must be WxH with dimensions in the range 1..32768");
        return 1;
      }
      windowSizeExplicit = true;
    } if (std::strcmp(argv[i], "--screenshot") == 0 && (i + 1) < argc) {
      screenshot = argv[++i];
    } if (std::strcmp(argv[i], "--render-report") == 0 &&
               (i + 1) < argc) {
      renderReport = argv[++i];
    } if (std::strcmp(argv[i], "--checkpoint-every") == 0 &&
               (i + 1) < argc) {
      checkpointEvery = std::atoi(argv[++i]);
      if (checkpointEvery < 1) {
        LOGE("--checkpoint-every must be at least 1");
        return 1;
      }
    } if (std::strcmp(argv[i], "--checkpoint-pattern") == 0 &&
               (i + 1) < argc) {
      checkpointPattern = argv[++i];
    } if (std::strcmp(argv[i], "--max-tris") == 0 && (i + 1) < argc) {
      maxTris = std::atoll(argv[++i]);
      maxTrisExplicit = true;
    } if (std::strcmp(argv[i], "--max-asset-bytes") == 0 && (i + 1) < argc) {
      // Override per-asset composition/resolver read cap (default 512MB).
      // Accepts a byte count with optional K/M/G suffix, e.g. 2G.
      maxAssetReadBytes = ParseByteCount(argv[++i]);
      maxAssetBytesExplicit = true;
    } if (std::strcmp(argv[i], "--max-gpu-mem") == 0 && (i + 1) < argc) {
      maxGpuMemGiB = std::atof(argv[++i]);
      maxGpuMemExplicit = true;
    } if (std::strcmp(argv[i], "--vram-budget") == 0 && (i + 1) < argc) {
      vramBudgetGiB = std::atof(argv[++i]);
      vramBudgetExplicit = true;
    } if (std::strcmp(argv[i], "--max-draw-meshes") == 0 && (i + 1) < argc) {
      maxDrawMeshes = std::atoll(argv[++i]);
      maxDrawMeshesExplicit = true;
    } if (std::strcmp(argv[i], "--no-robust-frame") == 0) {
      robustFrame = false;
    } if (std::strcmp(argv[i], "--rt-lod") == 0) {
      rtLod = true;
      rtLodExplicit = true;
    } if (std::strcmp(argv[i], "--no-rt-lod") == 0) {
      rtLod = false;
      rtLodExplicit = true;
    } if (std::strcmp(argv[i], "--rt-lod-full-px") == 0 && (i + 1) < argc) {
      rtLodFullPx = static_cast<float>(std::atof(argv[++i]));
      rtLodFullExplicit = true;
    } if (std::strcmp(argv[i], "--rt-lod-cull-px") == 0 && (i + 1) < argc) {
      rtLodCullPx = static_cast<float>(std::atof(argv[++i]));
      rtLodCullExplicit = true;
    } if (std::strcmp(argv[i], "--rt-lod-band") == 0 && (i + 1) < argc) {
      rtLodBand = static_cast<float>(std::atof(argv[++i]));
      rtLodBandExplicit = true;
    } if (std::strcmp(argv[i], "--raster-lod") == 0) {
      rasterLod = true;
      rasterLodExplicit = true;
    } if (std::strcmp(argv[i], "--dynamic-lod") == 0) {
      dynamicLod = true;
      dynamicLodExplicit = true;
    } if (std::strcmp(argv[i], "--no-dynamic-lod") == 0) {
      dynamicLod = false;
      dynamicLodExplicit = true;
    } if (std::strcmp(argv[i], "--raster-lod-full-px") == 0 && (i + 1) < argc) {
      rasterLodFullPx = static_cast<float>(std::atof(argv[++i]));
      rasterLodFullExplicit = true;
    } if (std::strcmp(argv[i], "--raster-lod-cull-px") == 0 && (i + 1) < argc) {
      rasterLodCullPx = static_cast<float>(std::atof(argv[++i]));
      rasterLodCullExplicit = true;
    } if (std::strcmp(argv[i], "--large-scene-profile") == 0 && (i + 1) < argc) {
      if (!ParseProfile(argv[++i], &largeSceneProfile)) {
        LOGE("--large-scene-profile must be off, auto, balanced, "
             "instance-heavy, or procedural-heavy");
        return 1;
      }
      largeSceneProfileExplicit = true;
    } if (std::strncmp(argv[i], "--large-scene-profile=", 22) == 0) {
      if (!ParseProfile(argv[i] + 22, &largeSceneProfile)) {
        LOGE("--large-scene-profile must be off, auto, balanced, "
             "instance-heavy, or procedural-heavy");
        return 1;
      }
      largeSceneProfileExplicit = true;
    } if (std::strcmp(argv[i], "--preview-cache") == 0) {
      if ((i + 1) >= argc) {
        LOGE("--preview-cache requires auto, off, or refresh");
        return 1;
      }
      const std::string mode = argv[++i];
      if (mode == "auto") previewCacheMode = lusdview::PreviewCacheMode::Auto;
      else if (mode == "off") previewCacheMode = lusdview::PreviewCacheMode::Off;
      else if (mode == "refresh") previewCacheMode = lusdview::PreviewCacheMode::Refresh;
      else { LOGE("--preview-cache must be auto, off, or refresh"); return 1; }
      previewCacheExplicit = true;
    } if (std::strncmp(argv[i], "--preview-cache=", 16) == 0) {
      const std::string mode = argv[i] + 16;
      if (mode == "auto") previewCacheMode = lusdview::PreviewCacheMode::Auto;
      else if (mode == "off") previewCacheMode = lusdview::PreviewCacheMode::Off;
      else if (mode == "refresh") previewCacheMode = lusdview::PreviewCacheMode::Refresh;
      else { LOGE("--preview-cache must be auto, off, or refresh"); return 1; }
      previewCacheExplicit = true;
    } if (std::strcmp(argv[i], "--preview-cache-dir") == 0) {
      if ((i + 1) >= argc || argv[i + 1][0] == '\0') {
        LOGE("--preview-cache-dir requires a non-empty path");
        return 1;
      }
      previewCacheDir = argv[++i];
    } if (std::strcmp(argv[i], "--preview-cache-max-gb") == 0) {
      if ((i + 1) >= argc) {
        LOGE("--preview-cache-max-gb requires a number");
        return 1;
      }
      previewCacheMaxGiB = std::max(0.0, std::atof(argv[++i]));
    } if (std::strcmp(argv[i], "--time-budget") == 0 && (i + 1) < argc) {
      timeBudget = std::atof(argv[++i]);
    } if (std::strcmp(argv[i], "--compose-threads") == 0 && (i + 1) < argc) {
      compositionThreads = static_cast<unsigned>(std::max(1, std::atoi(argv[++i])));
    } if (std::strcmp(argv[i], "--convert-threads") == 0 && (i + 1) < argc) {
      conversionThreads = static_cast<unsigned>(std::max(1, std::atoi(argv[++i])));
    } if (std::strcmp(argv[i], "--compose-opinion-batch") == 0 &&
               (i + 1) < argc) {
      compositionOpinionBatch = static_cast<size_t>(
          std::strtoull(argv[++i], nullptr, 10));
    } if (std::strcmp(argv[i], "--instance-chunk-samples") == 0 &&
               (i + 1) < argc) {
      instanceChunkSamples = static_cast<size_t>(
          std::strtoull(argv[++i], nullptr, 10));
    } if (std::strcmp(argv[i], "--mesh-convert-chunk-prims") == 0 &&
               (i + 1) < argc) {
      meshConversionChunkPrims = static_cast<size_t>(
          std::strtoull(argv[++i], nullptr, 10));
    } if (std::strcmp(argv[i], "--mesh-convert-chunk-mb") == 0 &&
               (i + 1) < argc) {
      meshConversionChunkMB = static_cast<size_t>(
          std::strtoull(argv[++i], nullptr, 10));
    } if (std::strcmp(argv[i], "--curve-parallel-min-prims") == 0 &&
               (i + 1) < argc) {
      curveParallelMinPrims = static_cast<size_t>(
          std::strtoull(argv[++i], nullptr, 10));
    } if (std::strcmp(argv[i], "--upload-budget-ms") == 0 && (i + 1) < argc) {
      uploadBudgetMs = std::clamp(std::atof(argv[++i]), 1.0, 33.0);
    } if (std::strcmp(argv[i], "--stream-buffer-mb") == 0 &&
               (i + 1) < argc) {
      streamBufferMB = static_cast<size_t>(
          std::clamp(std::atoi(argv[++i]), 4, 1024));
    } if (std::strcmp(argv[i], "--preview-boxes") == 0 &&
               (i + 1) < argc) {
      previewMaxBoxes = static_cast<size_t>(
          std::strtoull(argv[++i], nullptr, 10));
    } if (std::strcmp(argv[i], "--quit-after-full-upload") == 0) {
      quitAfterFullUpload = true;
    } if (std::strcmp(argv[i], "--quit-after-convert") == 0) {
      quitAfterConvert = true;
    } if (std::strcmp(argv[i], "--full-fidelity") == 0) {
      fullFidelity = true;
    } if (std::strcmp(argv[i], "--timing") == 0) {
      timing = true;
    } if (std::strcmp(argv[i], "--ui-scale") == 0 && (i + 1) < argc) {
      uiScale = static_cast<float>(std::atof(argv[++i]));
    } if (std::strcmp(argv[i], "--window-shot") == 0 && (i + 1) < argc) {
      windowShot = argv[++i];
    } if (std::strcmp(argv[i], "--headless") == 0) {
      headless = true;
    } if (std::strcmp(argv[i], "--threaded") == 0) {
      threaded = true;
      threadedExplicit = true;
    } if (std::strcmp(argv[i], "--no-threaded") == 0) {
      threaded = false;
      threadedExplicit = true;
    } if (std::strcmp(argv[i], "--adaptive-quality") == 0) {
      adaptiveQuality = true;
      adaptiveQualityExplicit = true;
    } if (std::strcmp(argv[i], "--no-adaptive-quality") == 0) {
      adaptiveQuality = false;
      adaptiveQualityExplicit = true;
    } if (std::strcmp(argv[i], "--target-render-fps") == 0 &&
               (i + 1) < argc) {
      targetRenderFps = static_cast<float>(std::atof(argv[++i]));
      targetRenderFpsExplicit = true;
      if (!(targetRenderFps >= 1.0f && targetRenderFps <= 240.0f)) {
        LOGE("--target-render-fps must be in [1, 240]");
        return 1;
      }
    } if (std::strcmp(argv[i], "--min-render-scale") == 0 &&
               (i + 1) < argc) {
      minRenderScale = static_cast<float>(std::atof(argv[++i]));
      minRenderScaleExplicit = true;
      if (!(minRenderScale >= 0.25f && minRenderScale <= 1.0f)) {
        LOGE("--min-render-scale must be in [0.25, 1]");
        return 1;
      }
    } if (std::strcmp(argv[i], "--next") == 0) {
      useNextLoader = true;
      useNextExplicit = true;
    } if (std::strcmp(argv[i], "--legacy-load") == 0 ||
               std::strcmp(argv[i], "--legacyLoad") == 0) {
      useNextLoader = false;
      useNextExplicit = true;
    } if (std::strcmp(argv[i], "--no-cull") == 0) {
      noCull = true;
    } if (std::strcmp(argv[i], "--no-grid") == 0) {
      showGrid = false;
    } if (std::strcmp(argv[i], "--no-skeleton") == 0) {
      showSkeleton = false;
    } if (std::strcmp(argv[i], "--cam-dolly") == 0 && (i + 1) < argc) {
      camDolly = static_cast<float>(std::atof(argv[++i]));
    } if (std::strcmp(argv[i], "--camera") == 0 && (i + 1) < argc) {
      cameraName = argv[++i];
    } if (std::strcmp(argv[i], "--stereo") == 0) {
      stereoRequested = true;
    } if (std::strcmp(argv[i], "--no-raster-motion") == 0) {
      rasterMotionEnabled = false;
    } if (std::strcmp(argv[i], "--camera-conform") == 0 &&
               (i + 1) < argc) {
      cameraConformExplicit = true;
      const std::string value = argv[++i];
      if (value == "fit") cameraConform = lusdview::CameraConform::Fit;
      else if (value == "crop") cameraConform = lusdview::CameraConform::Crop;
      else if (value == "horizontal")
        cameraConform = lusdview::CameraConform::Horizontal;
      else if (value == "vertical")
        cameraConform = lusdview::CameraConform::Vertical;
      else if (value == "none") cameraConform = lusdview::CameraConform::None;
      else {
        LOGE("--camera-conform must be fit, crop, horizontal, vertical, or none");
        return 1;
      }
    } if (std::strcmp(argv[i], "--view-dir") == 0 && (i + 1) < argc) {
      char trailing = '\0';
      const char* value = argv[++i];
      if (std::sscanf(value, "%f,%f,%f%c", &viewDir[0], &viewDir[1],
                      &viewDir[2], &trailing) != 3 ||
          !std::isfinite(viewDir[0]) || !std::isfinite(viewDir[1]) ||
          !std::isfinite(viewDir[2])) {
        LOGE("--view-dir requires three finite comma-separated values: X,Y,Z");
        return 1;
      }
      const float len = std::sqrt(viewDir[0] * viewDir[0] +
                                  viewDir[1] * viewDir[1] +
                                  viewDir[2] * viewDir[2]);
      if (!(len > 1e-6f)) {
        LOGE("--view-dir must be non-zero");
        return 1;
      }
      viewDir[0] /= len;
      viewDir[1] /= len;
      viewDir[2] /= len;
      viewDirExplicit = true;
    } if (std::strcmp(argv[i], "--no-composition") == 0) {
      noComposition = true;
    } if (std::strcmp(argv[i], "--defer-payloads") == 0) {
      deferPayloads = true;
    } if (std::strcmp(argv[i], "--load-payloads") == 0) {
      deferPayloads = false;
    } if (std::strcmp(argv[i], "--defer-references") == 0) {
      deferReferences = true;
    } if (std::strcmp(argv[i], "--allow-parent-paths") == 0) {
      allowParentPaths = true;
    } if (std::strcmp(argv[i], "--texture-max-size") == 0 && (i + 1) < argc) {
      textureOptions.maxTextureSize = std::atoi(argv[++i]);
      if (textureOptions.maxTextureSize < 0) {
        textureOptions.maxTextureSize = 0;
      }
      maxTextureSizeExplicit = true;
    } if (std::strcmp(argv[i], "--texture-budget-mb") == 0 && (i + 1) < argc) {
      textureOptions.textureBudgetMB = std::atoi(argv[++i]);
      if (textureOptions.textureBudgetMB < 0) {
        textureOptions.textureBudgetMB = 0;
      }
      textureBudgetExplicit = true;
    } if (std::strcmp(argv[i], "--async-texture-decode") == 0) {
      asyncTextureDecode = true;
    } if (std::strcmp(argv[i], "--ptex-initial-faces") == 0 &&
               (i + 1) < argc) {
      ptexInitialFaces = static_cast<size_t>(
          std::max(0, std::atoi(argv[++i])));
    } if (std::strcmp(argv[i], "--ptex-cache-mb") == 0 &&
               (i + 1) < argc) {
      ptexCacheMB = static_cast<size_t>(
          std::clamp(std::atoi(argv[++i]), 1, 4096));
    } if (std::strcmp(argv[i], "--subdivision-level") == 0 && (i + 1) < argc) {
      subdivisionLevel = std::max(0, std::atoi(argv[++i]));
    } if (std::strncmp(argv[i], "--subdivision-level=", 20) == 0) {
      subdivisionLevel = std::max(0, std::atoi(argv[i] + 20));
    } if (std::strcmp(argv[i], "--subdivision-auto") == 0) {
      subdivisionAuto = true;
      subdivisionAutoExplicit = true;
    } if (std::strcmp(argv[i], "--no-subdivision-auto") == 0) {
      subdivisionAuto = false;
      subdivisionAutoExplicit = true;
    } if (std::strcmp(argv[i], "--subdivision-auto-max-level") == 0 && (i + 1) < argc) {
      subdivisionAutoMaxLevel = std::max(0, std::atoi(argv[++i]));
      subdivisionAutoMaxExplicit = true;
    } if (std::strncmp(argv[i], "--subdivision-auto-max-level=", 29) == 0) {
      subdivisionAutoMaxLevel = std::max(0, std::atoi(argv[i] + 29));
      subdivisionAutoMaxExplicit = true;
    } if (std::strcmp(argv[i], "--subdivision-prim") == 0 && (i + 1) < argc) {
      std::string prim;
      int level = 0;
      if (!ParsePrimLevel(argv[++i], &prim, &level)) {
        LOGE("--subdivision-prim expects /Prim/Path=N");
        return 1;
      }
      subdivisionPrimLevels[prim] = level;
    } if (std::strncmp(argv[i], "--subdivision-prim=", 19) == 0) {
      std::string prim;
      int level = 0;
      if (!ParsePrimLevel(argv[i] + 19, &prim, &level)) {
        LOGE("--subdivision-prim expects /Prim/Path=N");
        return 1;
      }
      subdivisionPrimLevels[prim] = level;
    } if (std::strcmp(argv[i], "--texture-fit") == 0 && (i + 1) < argc) {
      if (!lightusd::api::ParseTextureFit(argv[++i], &textureFit)) {
        LOGE("--texture-fit must be modest|default|aggressive|never|always or a "
             "byte threshold like 4G");
        return 1;
      }
      textureFitExplicit = true;
    } if (std::strncmp(argv[i], "--texture-fit=", 14) == 0) {
      if (!lightusd::api::ParseTextureFit(argv[i] + 14, &textureFit)) {
        LOGE("--texture-fit must be modest|default|aggressive|never|always or a "
             "byte threshold like 4G");
        return 1;
      }
      textureFitExplicit = true;
    } if (std::strcmp(argv[i], "--texture-compress") == 0 && (i + 1) < argc) {
      textureCompressionExplicit = true;
      const char* mode = argv[++i];
      if (std::strcmp(mode, "off") == 0) {
        textureOptions.compression = lusdview::TextureCompressionMode::Off;
      } else if (std::strcmp(mode, "bc") == 0) {
        textureOptions.compression = lusdview::TextureCompressionMode::BCn;
      } else if (std::strcmp(mode, "bc7") == 0) {
        textureOptions.compression = lusdview::TextureCompressionMode::BC7;
      } else if (std::strcmp(mode, "astc") == 0) {
        textureOptions.compression = lusdview::TextureCompressionMode::Astc;
      } else if (std::strcmp(mode, "etc2") == 0) {
        textureOptions.compression = lusdview::TextureCompressionMode::Etc2;
      } else if (std::strcmp(mode, "auto") == 0) {
        textureOptions.compression = lusdview::TextureCompressionMode::Auto;
      } else {
        LOGE("--texture-compress must be off, bc, bc7, astc, etc2 or auto");
        return 1;
      }
    } if (std::strcmp(argv[i], "--texture-gpu") == 0 && (i + 1) < argc) {
      const char* backend = argv[++i];
      if (std::strcmp(backend, "off") == 0) {
        textureOptions.gpuBackend = lusdview::TextureGpuBackend::Off;
      } else if (std::strcmp(backend, "vulkan") == 0) {
        textureOptions.gpuBackend = lusdview::TextureGpuBackend::Vulkan;
      } else if (std::strcmp(backend, "cuda") == 0) {
        textureOptions.gpuBackend = lusdview::TextureGpuBackend::CUDA;
      } else if (std::strcmp(backend, "hip") == 0) {
        textureOptions.gpuBackend = lusdview::TextureGpuBackend::HIP;
      } else {
        LOGE("--texture-gpu must be off, vulkan, cuda or hip");
        return 1;
      }
    } if (std::strncmp(argv[i], "--texture-gpu-device=", 21) == 0) {
      textureOptions.gpuDevice = argv[i] + 21;
    } if (std::strcmp(argv[i], "--texture-keep-compressed") == 0 &&
               (i + 1) < argc) {
      const char* mode = argv[++i];
      if (std::strcmp(mode, "on") == 0) {
        textureOptions.keepCompressed = true;
      } else if (std::strcmp(mode, "off") == 0) {
        textureOptions.keepCompressed = false;
      } else {
        LOGE("--texture-keep-compressed must be on or off");
        return 1;
      }
    } if (std::strcmp(argv[i], "--texture-mips") == 0 && (i + 1) < argc) {
      mipsExplicit = true;
      const char* mode = argv[++i];
      if (std::strcmp(mode, "off") == 0) {
        textureOptions.generateMips = false;
      } else if (std::strcmp(mode, "on") == 0) {
        textureOptions.generateMips = true;
      } else {
        LOGE("--texture-mips must be on or off");
        return 1;
      }
    } if (std::strcmp(argv[i], "--dome-ibl") == 0 && (i + 1) < argc) {
      domeIblExplicit = true;
      const char* mode = argv[++i];
      if (std::strcmp(mode, "off") == 0) {
        textureOptions.domeIbl = 0;
      } else if (std::strcmp(mode, "low") == 0) {
        textureOptions.domeIbl = 1;
      } else if (std::strcmp(mode, "high") == 0) {
        textureOptions.domeIbl = 2;
      } else {
        LOGE("--dome-ibl must be off, low or high");
        return 1;
      }
    } if (std::strcmp(argv[i], "--raster-quality") == 0 && (i + 1) < argc) {
      const char* quality = argv[++i];
      if (std::strcmp(quality, "current") == 0) {
        rasterQualityHigh = false;
      } else if (std::strcmp(quality, "high") == 0) {
        rasterQualityHigh = true;
      } else {
        LOGE("--raster-quality must be current or high");
        return 1;
      }
    } if (std::strcmp(argv[i], "--material-purpose") == 0 &&
             (i + 1) < argc) {
      materialPurpose = argv[++i];
      materialPurposeExplicit = true;
      if (materialPurpose != "preview" && materialPurpose != "full") {
        LOGE("--material-purpose must be preview or full");
        return 1;
      }
    } if (std::strcmp(argv[i], "--envmap") == 0 && (i + 1) < argc) {
      envmapPath = argv[++i];
    } if (std::strcmp(argv[i], "--envmap-intensity") == 0 &&
             (i + 1) < argc) {
      if (!ParseFiniteNonNegativeFloat(argv[++i], &envmapIntensity)) {
        LOGE("--envmap-intensity must be a finite non-negative number");
        return 1;
      }
    } if (std::strcmp(argv[i], "--envmap-rotation") == 0 &&
             (i + 1) < argc) {
      char* end = nullptr;
      envmapRotation = std::strtof(argv[++i], &end);
      if (!end || *end != '\0' || !std::isfinite(envmapRotation)) {
        LOGE("--envmap-rotation must be a finite number of degrees");
        return 1;
      }
    } if (std::strcmp(argv[i], "--udim") == 0 && (i + 1) < argc) {
      const char* mode = argv[++i];
      if (std::strcmp(mode, "sparse") == 0) {
        textureOptions.udimMode = lusdview::UdimMode::Sparse;
      } else if (std::strcmp(mode, "atlas") == 0) {
        textureOptions.udimMode = lusdview::UdimMode::Atlas;
      } else {
        LOGE("--udim must be sparse or atlas");
        return 1;
      }
    } if ((std::strcmp(argv[i], "--time") == 0 ||
                std::strcmp(argv[i], "--frame") == 0) &&
               (i + 1) < argc) {
      timeCode = std::atof(argv[++i]);
    } if (std::strcmp(argv[i], "--skinning") == 0 && (i + 1) < argc) {
      const char* mode = argv[++i];
      if (std::strcmp(mode, "cpu") == 0) {
        skinningMode = lusdview::SkinningMode::CPU;
      } else if (std::strcmp(mode, "gpu") == 0) {
        skinningMode = lusdview::SkinningMode::GPU;
      } else if (std::strcmp(mode, "auto") == 0) {
        skinningMode = lusdview::SkinningMode::Auto;
      } else {
        LOGE("--skinning must be auto, cpu, or gpu");
        return 1;
      }
    } if (std::strcmp(argv[i], "--play") == 0) {
      playAnim = true;
    } if (std::strcmp(argv[i], "--rt") == 0) {
      wantRt = true;
    } if (std::strcmp(argv[i], "--path-trace") == 0) {
      wantPathTrace = true;
    } if (std::strcmp(argv[i], "--pt-quality") == 0 && i + 1 < argc) {
      const char* q = argv[++i];
      if (std::strcmp(q, "final") == 0) ptFinal = true;
      else if (std::strcmp(q, "interactive") == 0) ptFinal = false;
      else { LOGE("--pt-quality must be interactive or final"); return 1; }
    } if (std::strcmp(argv[i], "--pt-samples") == 0) {
      unsigned v = 0;
      if (i + 1 >= argc || !ParseUnsigned(argv[++i], &v)) {
        LOGE("--pt-samples must be an unsigned integer"); return 1;
      }
      ptSamples = v;
    } if (std::strcmp(argv[i], "--pt-max-depth") == 0) {
      unsigned v = 0;
      if (i + 1 >= argc || !ParseUnsigned(argv[++i], &v) || v == 0) {
        LOGE("--pt-max-depth must be a positive integer"); return 1;
      }
      ptMaxDepth = v;
    } if (std::strcmp(argv[i], "--pt-rr-depth") == 0) {
      unsigned v = 0;
      if (i + 1 >= argc || !ParseUnsigned(argv[++i], &v)) {
        LOGE("--pt-rr-depth must be an unsigned integer"); return 1;
      }
      ptRrDepth = v;
    } if (std::strcmp(argv[i], "--pt-motion-segments") == 0) {
      unsigned v = 0;
      if (i + 1 >= argc || !ParseUnsigned(argv[++i], &v) || v == 0) {
        LOGE("--pt-motion-segments must be a positive integer"); return 1;
      }
      ptMotionSegments = v;
    } if (std::strcmp(argv[i], "--pt-seed") == 0) {
      unsigned v = 0;
      if (i + 1 >= argc || !ParseUnsigned(argv[++i], &v)) {
        LOGE("--pt-seed must be an unsigned integer"); return 1;
      }
      ptSeed = v;
    } if (std::strcmp(argv[i], "--pt-max-sss-events") == 0) {
      unsigned v = 0;
      if (i + 1 >= argc || !ParseUnsigned(argv[++i], &v) || v == 0) {
        LOGE("--pt-max-sss-events must be a positive integer"); return 1;
      }
      ptMaxSssEvents = v;
    } if (std::strcmp(argv[i], "--pt-max-volume-events") == 0) {
      unsigned v = 0;
      if (i + 1 >= argc || !ParseUnsigned(argv[++i], &v) || v == 0) {
        LOGE("--pt-max-volume-events must be a positive integer"); return 1;
      }
      ptMaxVolumeEvents = v;
    } if (std::strcmp(argv[i], "--pt-variance") == 0) {
      float v = 0.0f;
      if (i + 1 >= argc || !ParseFiniteNonNegativeFloat(argv[++i], &v)) {
        LOGE("--pt-variance must be a finite non-negative number"); return 1;
      }
      ptVariance = v;
    } if (std::strcmp(argv[i], "--pt-firefly-clamp") == 0) {
      float v = 0.0f;
      if (i + 1 >= argc || !ParseFiniteNonNegativeFloat(argv[++i], &v)) {
        LOGE("--pt-firefly-clamp must be a finite non-negative number");
        return 1;
      }
      ptFireflyClamp = v;
    } if (std::strcmp(argv[i], "--pt-denoise") == 0 && i + 1 < argc) {
      const char* d = argv[++i];
      ptDenoiseExplicit = true;
      if (std::strcmp(d, "off") == 0) ptDenoise = lusdview::PathTraceDenoise::Off;
      else if (std::strcmp(d, "auto") == 0) ptDenoise = lusdview::PathTraceDenoise::Auto;
      else if (std::strcmp(d, "on") == 0) ptDenoise = lusdview::PathTraceDenoise::On;
      else { LOGE("--pt-denoise must be off, auto, or on"); return 1; }
    } if (std::strcmp(argv[i], "--linear-output") == 0) {
      if (i + 1 >= argc || argv[i + 1][0] == '\0') {
        LOGE("--linear-output requires an EXR path"); return 1;
      }
      linearOutput = argv[++i];
    } if (std::strcmp(argv[i], "--f-stop") == 0) {
      float v = 0.0f;
      if (i + 1 >= argc || !ParseFinitePositiveFloat(argv[++i], &v)) {
        LOGE("--f-stop must be a finite positive number"); return 1;
      }
      fStopOverride = v;
    } if (std::strcmp(argv[i], "--focus-distance") == 0) {
      float v = 0.0f;
      if (i + 1 >= argc || !ParseFinitePositiveFloat(argv[++i], &v)) {
        LOGE("--focus-distance must be a finite positive number"); return 1;
      }
      focusDistanceOverride = v;
    } if (std::strcmp(argv[i], "--cuda") == 0) {
      wantCuda = true;
    } if (std::strcmp(argv[i], "--optix") == 0) {
      wantCuda = true;
      cudaRtBackend = lusdview::CudaRtBackend::Optix;
      cudaRtBackendExplicit = true;
    } if (std::strcmp(argv[i], "--no-optix") == 0) {
      wantCuda = true;
      cudaRtBackend = lusdview::CudaRtBackend::SoftwareBvh;
      cudaRtBackendExplicit = true;
    } if (std::strcmp(argv[i], "--cuda-rt-backend") == 0) {
      if (i + 1 >= argc) {
        LOGE("--cuda-rt-backend requires auto, optix, or software");
        return 1;
      }
      const char* value = argv[++i];
      cudaRtBackendExplicit = true;
      if (std::strcmp(value, "auto") == 0)
        cudaRtBackend = lusdview::CudaRtBackend::Auto;
      else if (std::strcmp(value, "optix") == 0) {
        cudaRtBackend = lusdview::CudaRtBackend::Optix;
        wantCuda = true;
      } else if (std::strcmp(value, "software") == 0)
        cudaRtBackend = lusdview::CudaRtBackend::SoftwareBvh;
      else {
        LOGE("--cuda-rt-backend requires auto, optix, or software");
        return 1;
      }
    } if (std::strncmp(argv[i], "--cuda-rt-backend=", 18) == 0) {
      const char* value = argv[i] + 18;
      cudaRtBackendExplicit = true;
      if (std::strcmp(value, "auto") == 0)
        cudaRtBackend = lusdview::CudaRtBackend::Auto;
      else if (std::strcmp(value, "optix") == 0) {
        cudaRtBackend = lusdview::CudaRtBackend::Optix;
        wantCuda = true;
      } else if (std::strcmp(value, "software") == 0)
        cudaRtBackend = lusdview::CudaRtBackend::SoftwareBvh;
      else {
        LOGE("--cuda-rt-backend requires auto, optix, or software");
        return 1;
      }
    } if (std::strcmp(argv[i], "--cuda-cache-dir") == 0) {
      if (i + 1 >= argc) {
        LOGE("--cuda-cache-dir requires a non-empty path");
        return 1;
      }
      cudaCacheDir = argv[++i];
      if (cudaCacheDir.empty()) {
        LOGE("--cuda-cache-dir requires a non-empty path");
        return 1;
      }
    } if (std::strncmp(argv[i], "--cuda-cache-dir=", 17) == 0) {
      cudaCacheDir = argv[i] + 17;
      if (cudaCacheDir.empty()) {
        LOGE("--cuda-cache-dir requires a non-empty path");
        return 1;
      }
    } if (std::strcmp(argv[i], "--hip") == 0) {
      wantHip = true;
    } if (std::strcmp(argv[i], "--cpu-rt") == 0) {
      wantCpuRt = true;
    } if (std::strcmp(argv[i], "--rt-samples") == 0 && i + 1 < argc) {
      rtSamples = std::atoi(argv[++i]);
      if (rtSamples < 1) rtSamples = 1;
    } if (std::strcmp(argv[i], "--lod-stream") == 0) {
      lodStream = true;
    } if (std::strcmp(argv[i], "--curve-preview-prims") == 0 &&
               i + 1 < argc) {
      curvePreviewPrims = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
    } if (std::strcmp(argv[i], "--curve-preview-strands") == 0 &&
               i + 1 < argc) {
      curvePreviewStrands =
          static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
    } if (std::strcmp(argv[i], "--max-mem") == 0 && i + 1 < argc) {
      lodMaxMem = std::atof(argv[++i]);
      lodMaxMemExplicit = true;
    } if (std::strcmp(argv[i], "--max-vram") == 0 && i + 1 < argc) {
      lodMaxVram = std::atof(argv[++i]);
      lodMaxVramExplicit = true;
    } if (std::strcmp(argv[i], "--max-instances") == 0 && i + 1 < argc) {
      rtMaxInstances = std::atoll(argv[++i]);
      if (rtMaxInstances < 0) rtMaxInstances = 0;
    } if (std::strcmp(argv[i], "--wireframe") == 0) {
      wantWireframe = true;
    } if (std::strcmp(argv[i], "--material-id") == 0) {
      wantMaterialId = true;
    } if (std::strcmp(argv[i], "--mode") == 0 && (i + 1) < argc) {
      const char* m = argv[++i];
      lusdview::RenderMode parsed{};
      if (!ParseRenderModeName(m, &parsed)) {
        LOGE("--mode: unknown '%s'", m);
        return 1;
      }
      wantMode = parsed;
    } if (std::strcmp(argv[i], "--mode-sweep") == 0 && (i + 1) < argc) {
      // Comma-separated mode list rendered from ONE load. Each mode gets
      // --frames frames and its own screenshot, so a caller that wants N AOVs
      // of the same scene pays one process start and one Vulkan device
      // creation instead of N. --screenshot must contain {mode}.
      std::string list = argv[++i];
      size_t pos = 0;
      while (pos <= list.size()) {
        const size_t comma = list.find(',', pos);
        const std::string name =
            list.substr(pos, comma == std::string::npos ? std::string::npos
                                                        : comma - pos);
        if (!name.empty()) {
          lusdview::RenderMode parsed{};
          if (!ParseRenderModeName(name.c_str(), &parsed)) {
            LOGE("--mode-sweep: unknown mode '%s'", name.c_str());
            return 1;
          }
          modeSweep.push_back({name, parsed});
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
      if (modeSweep.empty()) {
        LOGE("--mode-sweep: empty mode list");
        return 1;
      }
    } if (std::strcmp(argv[i], "--select") == 0 && (i + 1) < argc) {
      // Select a prim by absolute path once loaded (highlights it; a GeomSubset
      // highlights just its faces). Also handy for headless screenshots.
      wantSelect = argv[++i];
    } if (std::strcmp(argv[i], "--blend") == 0 && (i + 1) < argc) {
      // --blend NAME=WEIGHT (repeatable): manually drive a blendshape weight,
      // overriding the SkelAnimation. Honors in-between shapes. For headless
      // posing + the Maya-like blend editor.
      const char* spec = argv[++i];
      const char* eq = std::strchr(spec, '=');
      if (!eq) { LOGE("--blend expects NAME=WEIGHT, got '%s'", spec); return 1; }
      wantBlend.emplace_back(std::string(spec, eq), std::atof(eq + 1));
    } if (std::strcmp(argv[i], "--mcp-stdio") == 0) {
      mcpStdio = true;
    } if (std::strcmp(argv[i], "--autorigger") == 0 && (i + 1) < argc) {
      autoriggerExecutable = argv[++i];
    } if (std::strncmp(argv[i], "--mcp-http", 10) == 0) {
      const char* eq = std::strchr(argv[i], '=');
      mcpHttpPort = eq ? std::atoi(eq + 1) : 8080;
      if (mcpHttpPort <= 0) mcpHttpPort = 8080;
    } if (std::strcmp(argv[i], "--mcp") == 0) {
      mcpStdio = true;
      if (mcpHttpPort == 0) mcpHttpPort = 8080;
    } if (std::strcmp(argv[i], "--live-shader-reload") == 0) {
      liveShaderWatch = true;
    } if (std::strcmp(argv[i], "--materialx-vk-shader-max-kib") == 0 &&
             (i + 1) < argc) {
      unsigned value = 0;
      if (!ParseUnsigned(argv[++i], &value) || value == 0) {
        LOGE("--materialx-vk-shader-max-kib must be a positive integer");
        return 1;
      }
      materialXVulkanShaderMaxKiB = value;
      materialXVulkanShaderMaxExplicit = true;
    } if (std::strcmp(argv[i], "--materialx-vk-compile-timeout") == 0 &&
             (i + 1) < argc) {
      unsigned value = 0;
      if (!ParseUnsigned(argv[++i], &value) || value == 0 || value > 3600) {
        LOGE("--materialx-vk-compile-timeout must be in [1, 3600] seconds");
        return 1;
      }
      materialXVulkanCompileTimeoutSec = static_cast<int>(value);
      materialXVulkanCompileTimeoutExplicit = true;
    } if (std::strncmp(argv[i], "--stream-http", 13) == 0) {
      // Optional value: `--stream-http=PORT`, `--stream-http PORT`, or bare
      // `--stream-http` (defaults to 8090). The space form consumes the next
      // argument only when it starts with a digit (else it's a positional/flag).
      const char* eq = std::strchr(argv[i], '=');
      if (eq) {
        streamHttpPort = std::atoi(eq + 1);
      } else if ((i + 1) < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
        streamHttpPort = std::atoi(argv[++i]);
      } else {
        streamHttpPort = 8090;
      }
      if (streamHttpPort <= 0) streamHttpPort = 8090;
    } if (std::strcmp(argv[i], "--stream-codec") == 0 && (i + 1) < argc) {
      streamCodec = argv[++i];
    } if (std::strcmp(argv[i], "--stream-motion-res") == 0 && (i + 1) < argc) {
      streamMotionRes = std::atoi(argv[++i]);
    } if (std::strcmp(argv[i], "--stream-idle-ms") == 0 && (i + 1) < argc) {
      streamIdleMs = std::atoi(argv[++i]);
    } if (std::strcmp(argv[i], "--stream-motion-quality") == 0 &&
               (i + 1) < argc) {
      streamMotionQuality = std::atoi(argv[++i]);
    } if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
      std::printf(
          "Usage: lusdview [--config PATH] [--backend gl|vk] [--rt] [--frames N] "
          "[--size WxH] [--screenshot out.png|out.jpg|out.ppm]\n"
          "                [--max-tris N] [--time-budget SECONDS] [--ui-scale S]\n"
          "                [--no-composition] [--defer-payloads | --load-payloads] "
          "[--defer-references] [--time CODE] [--skinning auto|cpu|gpu]\n"
          "                [--headless] [--mcp-stdio] [--mcp-http[=PORT]] [--mcp] "
          "[file.usd|usda|usdc|usdz]\n"
          "  --config PATH Load JSON startup config (otherwise uses the platform "
          "default config path).\n"
          "  --backend gl|vk Select renderer backend (default: Vulkan when built "
          "and available, otherwise OpenGL).\n"
          "  --vk-device INDEX|NAME  Select a Vulkan physical device by index or "
          "case-insensitive device/driver substring (e.g. 0, nvidia, rtx, llvmpipe). "
          "Also available in config as vulkan_device.\n"
          "  --rt          Use Vulkan ray tracing (ray query) when supported "
          "(implies --backend vk).\n"
          "  --transparency auto|weighted|sorted  Select Vulkan raster "
          "transparency (default: auto/sorted; weighted warms OIT).\n"
          "  --path-trace  Use the production multi-bounce integrator. Select "
          "Vulkan by default, or combine with --cuda/--hip.\n"
          "  --pt-quality interactive|final  Select 6-bounce interactive or "
          "12-bounce final defaults.\n"
          "  --pt-samples N  Target samples (0 = continuous interactive).\n"
          "  --pt-max-depth N / --pt-rr-depth N  Path and roulette depths.\n"
          "  --pt-variance X  Adaptive convergence threshold (0 disables).\n"
          "  --pt-firefly-clamp X  Robust local firefly threshold (default 8, "
          "lower is stronger, 0 disables; EXR is unaffected).\n"
          "  --pt-denoise off|auto|on  Built-in edge-aware denoising policy.\n"
          "  --pt-motion-segments N  Stratified shutter snapshots.\n"
          "  --no-raster-motion  Render only the selected timeline pose in "
          "raster mode (for external sampling and debugging).\n"
          "  --pt-seed N  Deterministic sampling seed.\n"
          "  --linear-output out.exr  Write unfiltered scene-linear half EXR.\n"
          "  --f-stop F / --focus-distance D  Override USD camera lens values.\n"
          "  --stereo      Resolve authored stereoRole left/right cameras; "
          "headless raster screenshots are side-by-side (use --camera when "
          "multiple pairs exist).\n"
          "  --cuda        Ray-trace the screenshot on CUDA (driver API + NVRTC "
          "loaded at runtime via cuew; falls back if no CUDA device).\n"
          "  --cuda-rt-backend MODE  CUDA traversal: auto, optix, or software.\n"
          "  --optix       Alias for --cuda --cuda-rt-backend optix.\n"
          "  --no-optix    Alias for --cuda --cuda-rt-backend software.\n"
          "  --cuda-cache-dir PATH  Store compiled CUDA PTX in PATH (default: the "
          "platform cache directory under lusdview/cuda).\n"
          "  --hip         Ray-trace the screenshot on HIP/ROCm (loaded at runtime "
          "via hipew + hiprtc; falls back if no AMD/ROCm device).\n"
          "  --rt-samples N  Supersampled AA for RT screenshots/CPU viewport "
          "(Halton sub-pixel jitter; default 1 = off).\n"
          "  --max-instances N  Cap the --cuda/--hip 2-level-BVH instance count "
          "(default 16M; 0 = unlimited). Bounds the host BVH build for massively "
          "very large instanced scenes.\n"
          "  --max-tris N  Cap triangles in the CUDA/HIP software RT scene.\n"
          "  --time-budget SECONDS  Stop the headless run after this wall-time budget.\n"
          "  --size WxH    Set the render/window size. Overrides the startup config.\n"
          "  --ui-scale S  Override the interface scale factor.\n"
          "  --lod-stream  View-dependent district LOD: promote "
          "the camera-nearest districts to full under memory budgets.\n"
          "  --max-mem G / --max-vram G  Host / GPU GiB budgets for --lod-stream "
          "(0 = auto, 50%%).\n"
          "  --camera NAME Frame a named USD Camera instead of "
          "auto-fitting the whole scene (useful for vast scenes).\n"
          "  --camera-conform MODE  Filmback policy: fit, crop, horizontal, "
          "vertical, or none (default: fit).\n"
          "  --view-dir X,Y,Z  Set the normalized world-space eye-to-target "
          "direction after auto-fit; cannot be combined with --camera.\n"
          "  --select /Prim/Path  Select a prim after loading.\n"
          "  --cam-dolly D  Apply a startup dolly offset to the framed camera.\n"
          "  --no-cull / --no-robust-frame  Disable frustum culling or robust "
          "outlier-resistant auto framing.\n"
          "  --no-grid     Hide the ground grid (useful for deterministic captures).\n"
          "  --no-skeleton Hide skeleton helper overlays (useful for AOV comparisons).\n"
          "  --dome-ibl off|low|high  Control DomeLight IBL precomputation.\n"
          "  --envmap PATH  Override authored DomeLights with an HDR/EXR/image.\n"
          "  --envmap-intensity N / --envmap-rotation DEG  Environment controls.\n"
          "  --raster-quality current|high  Opt into filmic PBR raster shading.\n"
          "  --material-purpose preview|full  Select UsdShade binding purpose.\n"
          "  --large-scene-profile off|auto|balanced|instance-heavy|"
          "procedural-heavy  Select a generic Vulkan workload preset. Profiles set existing "
          "large-scene knobs only; explicit CLI flags win. No texture resize or "
          "compression behavior is changed.\n"
          "  --preview-cache auto|off|refresh  Reuse or rebuild a validated "
          "bounds/camera preview cache (auto for large-scene profiles).\n"
          "  --preview-cache-dir PATH  Override the platform preview cache directory.\n"
          "  --preview-cache-max-gb N  Preview-cache size cap (default 8 GiB).\n"
          "  --compose-threads N  Composition worker count (default: hardware, capped).\n"
          "  --convert-threads N  Geometry conversion worker count.\n"
          "  --compose-opinion-batch N  Prim opinions claimed per composition "
          "worker job (0=auto).\n"
          "  --instance-chunk-samples N  Point-instancer placements per "
          "streaming chunk (0=auto).\n"
          "  --mesh-convert-chunk-prims N  Source meshes per conversion wave "
          "(0=auto).\n"
          "  --mesh-convert-chunk-mb N  Estimated geometry per conversion wave "
          "(0=auto).\n"
          "  --curve-parallel-min-prims N  Minimum curves prims for parallel "
          "conversion (0=auto).\n"
          "  --upload-budget-ms N  Interactive upload slice, 1..33 ms.\n"
          "  --stream-buffer-mb N  Bounded CPU geometry queue for interactive "
          "OpenGL streaming (default 64 MiB).\n"
          "  --preview-boxes N  Composition preview marker boxes (0=auto).\n"
          "  --quit-after-full-upload  Exit after progressive conversion, upload, "
          "and one complete present.\n"
          "  --quit-after-convert  Exit when complete CPU renderer data has been "
          "produced; excludes final upload/presentation latency.\n"
          "  --full-fidelity  Disable profile geometry/curve admission caps while "
          "retaining security and payload-policy controls.\n"
          "  --timing             Print detailed load/conversion timing.\n"
          "  --vram-budget G  GPU memory the large-scene budgets may plan "
          "against (GiB). Default: probed from the device. Everything else "
          "(--max-gpu-mem, texture edge/byte caps, upload staging) is derived "
          "from it, so lowering this one number rehearses a smaller card.\n"
          "  --max-asset-bytes N  Override the per-asset composition read cap "
          "(default 512M; accepts K/M/G suffix, e.g. 2G) for scenes with large "
          "single crates (e.g. Moore Lane's 896MB subLayer).\n"
          "  --wireframe   Start in wireframe render mode (raster + both RT "
          "backends draw triangle edges only).\n"
          "  --material-id Start in material-id visualization (a distinct flat "
          "color per material; all backends).\n"
          "  --mode NAME   Start in a render mode: shaded, wireframe, normals, "
          "material-id, geom-normal, uv, depth, albedo, facing, roughness, "
          "metallic, emissive, opacity, position, barycentric, prim-id, mesh-id, "
          "picking, "
          "purpose, missing-normals, double-sided, skin-weights, tangent, "
          "uv-checker, ao, curvature, instance-id, bvh-heatmap, soft-shadow, "
          "kind, udim, uv1, blend-influence, texel-density, source-face-id, coat-normal, coat-weight, coat-color, coat-roughness, specular-f0, ior-f0.\n"
          "  --threaded / --no-threaded  Enable or disable the dedicated render "
          "thread (default on for interactive Vulkan when built with "
          "LUSDVIEW_ENABLE_GL_THREAD).\n"
          "  --adaptive-quality / --no-adaptive-quality  Dynamically lower the "
          "Vulkan viewport scale while navigating (default on interactively).\n"
          "  --target-render-fps N  Adaptive rendering floor (default 30).\n"
          "  --min-render-scale S  Lowest adaptive 3D viewport scale, 0.25..1 "
          "(default 0.5; UI remains full resolution).\n"
          "  --blend NAME=W  Manually set a blendshape weight (repeatable), "
          "overriding the SkelAnimation; honors in-between shapes. Also editable "
          "live in the Inspector's Blend Shapes panel.\n"
          "  --headless    Windowless offscreen rendering, no display needed "
          "(Vulkan only; needs --frames + --screenshot/--window-shot).\n"
          "  --next        Use the default next-core + tydra-next loader "
          "(compatibility flag).\n"
          "  --legacy-load Use the legacy eager loader.\n"
          "  --no-composition  Load the root layer only (skip USD composition arcs).\n"
          "  --defer-payloads  Lazy payloads: skip payload arcs on load and load "
          "them on demand from the GUI.\n"
          "  --load-payloads   Compose payload arcs eagerly (default).\n"
          "  --defer-references  Also defer `references` arcs (loaded on demand "
          "like payloads). Non-standard: USD assumes references always resolve, "
          "so most scene content stays unloaded until requested.\n"
          "  --allow-parent-paths  Compatibility flag: parent-directory ('..') "
          "segments are permitted by default because USD anchors them to the "
          "authoring layer.\n"
          "  --texture-max-size N  Downsize decoded textures whose longest edge "
          "exceeds N texels (default 4096; 0 = keep source size).\n"
          "  --texture-budget-mb N  Best-effort decoded texture memory budget "
          "for viewer uploads (0 = unlimited).\n"
          "  --texture-fit modest|default|aggressive|never|always|<N>G  How "
          "eagerly to shrink/block-compress textures so the scene fits GPU "
          "memory. The scene is left untouched when its estimated resident size "
          "(geometry + textures) stays under 1/3 (modest), 2/3 (default), or "
          "90%% (aggressive) of VRAM capacity, or under an explicit threshold "
          "such as 4G. 'never' never shrinks or compresses (and re-enables the "
          "KTX2 zero-copy passthrough); 'always' always does. Mip generation is "
          "governed separately. --vram-budget rehearses a different card.\n"
          "  --async-texture-decode  Decode ordinary filesystem textures from "
          "camera-prioritized background workers.\n"
          "  --ptex-initial-faces N  Decode only the first N Ptex faces for the "
          "startup atlas (0 = all), then refine visible meshes.\n"
          "  --ptex-cache-mb N  Mutable physical page cache per Ptex texture "
          "(1..4096 MiB).\n"
          "  --curve-preview-prims N  Convert at most N Curves prims (0 = all; "
          "preset limit when enabled).\n"
          "  --curve-preview-strands N  Retain at most N complete curve strands "
          "(0 = all; presets may provide a limit).\n"
          "  --subdivision-level N  Scene-wide conversion-time subdivision "
          "surface refinement level (default 2; 0 = off). Applies only to meshes whose USD "
          "subdivisionScheme is not none.\n"
          "  --subdivision-prim /Prim/Path=N  Override subdivision level for one "
          "mesh prim. Repeatable; ':' is also accepted as the separator.\n"
          "  --subdivision-auto  Estimate per-prim subdivision levels from the "
          "auto-fit camera screen coverage and re-convert once. Explicit "
          "--subdivision-prim overrides win.\n"
          "  --subdivision-auto-max-level N  Clamp --subdivision-auto levels "
          "(default 3, hard cap 10).\n"
          "  --texture-compress off|bc|bc7|astc|etc2|auto  Request GPU texture "
          "compression (default "
          "auto; selects a supported BC/ASTC/ETC2 format). Backends without "
          "block upload support fall back to resized RGBA8.\n"
          "  --texture-gpu off|vulkan|cuda|hip  Use GPU texture preprocessing "
          "with CPU fallback.\n"
          "  --texture-gpu-device=NAME|INDEX  Select the Vulkan GPU used for "
          "texture preprocessing.\n"
          "  --texture-mips on|off  Generate content-aware texture mip chains "
          "(default on; interactive --next default off).\n"
          "  --texture-keep-compressed  Preserve supported KTX2 block payloads "
          "instead of decoding/re-encoding them.\n"
          "  --udim sparse|atlas  UDIM handling mode (default sparse; atlas rebakes "
          "UVs and can consume much more memory on large tile sets).\n"
          "  --time CODE   Evaluate the scene at this USD time code (animated "
          "transforms/points/skinning). Useful with --frames for a screenshot at "
          "a specific frame. Interactive runs play from the Timeline panel.\n"
          "  --skinning MODE  Skinning path: auto (default), cpu, or gpu.\n"
          "  --play        Start timeline playback on load. With --frames the "
          "playback clock steps a fixed 1/60 s per frame (deterministic pose at "
          "every frame, so --screenshot captures are pixel-comparable).\n"
          "  --frame CODE  Alias for --time CODE.\n"
          "  --screenshot PATH  Save the viewport image after --frames.\n"
          "  --render-report PATH  Write a schema-versioned JSON render report.\n"
          "  --checkpoint-every N  Save the viewport every N fixed frames "
          "(raster or Vulkan RT).\n"
          "  --checkpoint-pattern PATH  Checkpoint filename containing {frame}; "
          "default derives from --screenshot.\n"
          "  --window-shot PATH  Save the complete window, including UI.\n"
          "  --raster-lod / --rt-lod  Enable view-dependent raster or Vulkan-RT "
          "LOD (--no-rt-lod disables RT LOD for deterministic full-scene capture); "
          "final path-trace quality disables implicit RT LOD; tune explicit LOD "
          "with --*-lod-full-px, --*-lod-cull-px, and --rt-lod-band.\n"
          "  --dynamic-lod / --no-dynamic-lod  Enable or disable asynchronous "
          "multi-level prototype LOD and its FPS controller.\n"
          "  --max-draw-meshes N / --max-gpu-mem G  Bound raster mesh count or "
          "geometry memory (GiB).\n"
          "  --mcp-stdio   Run the MCP server over stdio (JSON-RPC on stdin/stdout).\n"
          "  --autorigger PATH  External JSON-RPC auto-rigger executable (vchar).\n"
          "  --mcp-http    Run the MCP server over HTTP (default port 8080).\n"
          "  --mcp         Both transports.\n"
          "  --live-shader-reload  Watch the active Vulkan/CUDA/HIP RT source and\n"
          "                        transactionally recompile it after file saves.\n"
          "  --materialx-vk-shader-max-kib N  Reject larger Vulkan shader sources\n"
          "                        before compilation (default 129 KiB).\n"
          "  --materialx-vk-compile-timeout N  Kill glslc after N seconds\n"
          "                        (default 30).\n"
          "  --stream-http[=PORT]  WebSocket browser viewer streaming the window "
          "(incl. ImGui); default port 8090. Navigate/click from the browser.\n"
          "  --stream-codec png|qoi  Idle-refinement codec sent when the view is "
          "stable (default png; lossless).\n"
          "  --stream-motion-res PX  Long-edge cap for the low-quality JPEG sent "
          "while moving (default 1280).\n"
          "  --stream-motion-quality N  JPEG quality (1-100) for motion frames "
          "(default 45).\n"
          "  --stream-idle-ms MS  Input-quiet time before the lossless refine "
          "frame is sent (default 350).\n");
      return 0;
    } else if (i == argumentIndex && argv[i][0] != '-') {
      file = argv[i];
    }
  }

  if (viewDirExplicit && !cameraName.empty()) {
    LOGE("--view-dir cannot be combined with --camera");
    return 1;
  }

  if ((quitAfterFullUpload || quitAfterConvert) && maxFrames >= 0) {
    LOGE("--quit-after-full-upload/--quit-after-convert cannot be combined with --frames");
    return 1;
  }

  LargeSceneProfile effectiveProfile = largeSceneProfile;
  // Large production assets should receive the interactive streaming budget
  // even when opened directly from the command line. Keep `--large-scene-
  // profile off` as an explicit escape hatch, and retain the old opt-in
  // behavior for headless/fixed-frame captures where deterministic full-load
  // behavior is more important than first-frame latency.
  const bool automaticInteractiveProfile =
      !largeSceneProfileExplicit && !headless && maxFrames < 0;
  if (automaticInteractiveProfile) {
    effectiveProfile = LargeSceneProfile::Auto;
  }
  if (!dynamicLodExplicit)
    dynamicLod = automaticInteractiveProfile && !fullFidelity;
  if (effectiveProfile == LargeSceneProfile::Auto) {
    // Auto is deliberately content-name agnostic. Use the conservative generic
    // preset; users can select a workload preset before opening or via CLI.
    effectiveProfile = LargeSceneProfile::Balanced;
  }
  // Every large-scene budget -- --max-gpu-mem, the texture edge/byte caps, the
  // upload-staging cap, the proxy threshold -- descends from these two numbers.
  // They used to be the literals GiB(32) and GiB(16): the budget tree was
  // computed for an imaginary 32 GiB / 16 GiB machine no matter what you ran on,
  // so an 8 GiB card was handed a 8 GiB VRAM limit and a 24 GiB card left half
  // its memory unused. Probe the real values; --vram-budget overrides the GPU
  // side (useful to rehearse a smaller card, or when the probe is unavailable).
  const uint64_t hostCapacity = HostMemoryCapacityBytes();
  uint64_t vramCapacity = 0;
  if (vramBudgetExplicit && vramBudgetGiB > 0.0) {
    vramCapacity = static_cast<uint64_t>(
        vramBudgetGiB * double((uint64_t{1} << 30)));
  } else {
#if defined(HAVE_VULKAN)
    // Enumerating physical devices through a throw-away instance can runtime-
    // resume every installed GPU. On hybrid systems that includes displayless
    // secondary adapters and has been observed to stall in amdgpu resume before
    // the real renderer even starts. Avoid that redundant probe by default.
    // The explicit environment switch remains useful for budget diagnostics.
    if (const char* probe = std::getenv("LUSDVIEW_VULKAN_VRAM_PROBE")) {
      if (std::atoi(probe) != 0)
        vramCapacity = lusdview::QueryDeviceLocalVramBytes();
    }
#endif
    // No Vulkan, or the probe failed: keep the historical 16 GiB assumption
    // rather than collapsing every budget to zero.
    if (vramCapacity == 0) vramCapacity = (uint64_t{16} << 30);
  }
  const lightusd::api::ResourceBudget targetBudget =
      lightusd::api::ComputeResourceBudget(hostCapacity, vramCapacity);
  const double targetVramGiB =
      double(targetBudget.vram_limit) / double((uint64_t{1} << 30));
  const double targetHostGiB =
      double(targetBudget.host_limit) / double((uint64_t{1} << 30));

  // Bound texture residency for ordinary --next loads as well as named
  // large-scene profiles. The decoder applies these limits while reading, so
  // this also prevents a large source image set from peaking before the
  // draw-side residency pass runs. Explicit command-line values win,
  // including an explicit zero.
  // "Full fidelity" has always meant "do not degrade the asset", and texture
  // shrink/compress is a degradation -- so it implies --texture-fit never
  // unless the user asked for something specific. Must precede the derivation
  // below, which consumes textureFit.
  if (fullFidelity && !textureFitExplicit) {
    textureFit.policy = LIGHTUSD_TEXTURE_FIT_NEVER;
  }
  // The "comfort" budget (25% of resident VRAM) is what the mip decision keeps
  // using. It must NOT follow --texture-fit: widening the mip skip makes every
  // later frame sample minified textures at full resolution, which thrashes the
  // texture cache (it once took the texture-semantic AOV suite from 52 s to
  // over 300 s). Only compression and resize follow the policy.
  const uint64_t textureComfortBytes = targetBudget.gpu_texture_limit;
  uint64_t textureFitThreshold = 0;
  if (lightusd::api::TextureFitThresholdBytes(textureFit, vramCapacity,
                                             &textureFitThreshold) != LIGHTUSD_OK) {
    LOGE("invalid texture-fit policy");
    return 1;
  }
  // The threshold is a fraction of VRAM, but the decoded set is resident in
  // HOST memory while loading. On a big card with a small host (24 GiB GPU,
  // 16 GiB RAM) an aggressive policy would otherwise authorise ~21 GiB of host
  // allocation. Clamp, and say so.
  {
    const uint64_t hostClamp =
        lightusd::api::BudgetPercent(targetBudget.host_limit, 60);
    // Only the fraction-of-VRAM policies are clamped. `never` and `always` are
    // explicit user intent -- "never" must mean never, or the escape hatch is
    // not one -- and `absolute` is already a number the user chose.
    const bool clampable =
        textureFit.policy == LIGHTUSD_TEXTURE_FIT_MODEST ||
        textureFit.policy == LIGHTUSD_TEXTURE_FIT_DEFAULT ||
        textureFit.policy == LIGHTUSD_TEXTURE_FIT_AGGRESSIVE;
    if (clampable && hostClamp > 0 && textureFitThreshold > hostClamp) {
      LOGI("texture-fit: threshold %.1f GiB clamped to %.1f GiB by host memory",
           double(textureFitThreshold) / double((uint64_t{1} << 30)),
           double(hostClamp) / double((uint64_t{1} << 30)));
      textureFitThreshold = hostClamp;
    }
  }
  {
    if (textureFit.policy == LIGHTUSD_TEXTURE_FIT_ALWAYS) {
      // Pre-policy behaviour: hard 2048 edge cap + 25% byte budget.
      if (!maxTextureSizeExplicit && targetBudget.texture_max_edge > 0) {
        textureOptions.maxTextureSize =
            static_cast<int>(targetBudget.texture_max_edge);
      }
      if (!textureBudgetExplicit && targetBudget.gpu_texture_limit > 0) {
        textureOptions.textureBudgetMB = static_cast<int>(
            targetBudget.gpu_texture_limit / (1024ull * 1024ull));
      }
    } else {
      // Threshold policies (and `never`) leave the edge uncapped: the decoder's
      // live byte budget shrinks individual images only if the running total
      // actually crosses the threshold, so a scene that fits is never touched.
      // Zeroing BOTH (never) is also the precondition that re-enables the KTX2
      // zero-copy passthrough in TryKeepCompressedTexture.
      if (!maxTextureSizeExplicit) textureOptions.maxTextureSize = 0;
      if (!textureBudgetExplicit) {
        textureOptions.textureBudgetMB =
            (textureFit.policy == LIGHTUSD_TEXTURE_FIT_NEVER)
                ? 0
                : static_cast<int>(textureFitThreshold / (1024ull * 1024ull));
      }
    }
  }
  if (effectiveProfile != LargeSceneProfile::Off) {
    if (!useNextExplicit) useNextLoader = true;
    if (!backendExplicit) {
      backend = lusdview::Backend::Vulkan;
      backendExplicit = true;
    }
    // An ordinary interactive open starts at full authored geometry. Raster
    // LOD substitutes small meshes with generic AABBs, which is useful for an
    // explicitly selected large-scene profile but can visibly corrupt thin,
    // overlapping architectural parts (wall boards, trim, roof panels) in
    // scenes that already fit comfortably in VRAM. Adaptive render scaling is
    // the first response to a missed frame target; the GPU-budget pass remains
    // the safety net when the full scene does not fit. Keep raster LOD opt-in
    // for the automatic interactive profile.
    if (!rasterLodExplicit && !automaticInteractiveProfile) rasterLod = true;
    if (!rtLodExplicit) rtLod = true;
    if (!rtLodFullExplicit) rtLodFullPx = 64.0f;
    if (!rtLodCullExplicit) rtLodCullPx = 2.0f;
    if (!rtLodBandExplicit) rtLodBand = 0.25f;
    if (!maxAssetBytesExplicit) maxAssetReadBytes = 2ull * 1024ull * 1024ull * 1024ull;
    if (!domeIblExplicit) textureOptions.domeIbl = 0;
    if (effectiveProfile == LargeSceneProfile::Balanced) {
      if (!maxTrisExplicit) maxTris = 40000000;
      if (!maxGpuMemExplicit) maxGpuMemGiB = targetVramGiB;
      if (!maxDrawMeshesExplicit) maxDrawMeshes = 80000;
      if (!rasterLodFullExplicit) rasterLodFullPx = 48.0f;
      if (!rasterLodCullExplicit) rasterLodCullPx = 1.0f;
    } else if (effectiveProfile == LargeSceneProfile::InstanceHeavy) {
      if (!maxGpuMemExplicit) maxGpuMemGiB = targetVramGiB;
      if (!maxDrawMeshesExplicit) maxDrawMeshes = 20000;
      if (!rasterLodFullExplicit) rasterLodFullPx = 48.0f;
      if (!rasterLodCullExplicit) rasterLodCullPx = 1.0f;
    } else if (effectiveProfile == LargeSceneProfile::ProceduralHeavy) {
      if (!maxGpuMemExplicit) maxGpuMemGiB = targetVramGiB;
      if (!maxDrawMeshesExplicit) maxDrawMeshes = 50000;
      if (!rasterLodFullExplicit) rasterLodFullPx = 36.0f;
      if (!rasterLodCullExplicit) rasterLodCullPx = 1.0f;
    } else {
      if (!maxGpuMemExplicit) maxGpuMemGiB = targetVramGiB;
      if (!maxDrawMeshesExplicit) maxDrawMeshes = 40000;
      if (!rasterLodFullExplicit) rasterLodFullPx = 48.0f;
      if (!rasterLodCullExplicit) rasterLodCullPx = 1.5f;
    }
    // The district wrapper prepass composes a full proxy stage before the
    // viewer load and can consume the entire first-frame latency budget.
    // Keep it as an explicit refinement tool (`--lod-stream`); automatic
    // profiles start from deferred payload markers instead.
    if (lodStream) {
      if (!lodMaxMemExplicit) lodMaxMem = targetHostGiB;
      if (!lodMaxVramExplicit) lodMaxVram = targetVramGiB;
    }
  }
  if (fullFidelity) {
    // Fidelity mode admits every selected-payload render primitive. View-based
    // LOD/culling needs a retained CPU copy of every instance transform; leave
    // it off unless explicitly requested so the renderer can release that copy
    // immediately after upload while still drawing every authored instance.
    maxDrawMeshes = 0;
    maxTris = 0;
    maxGpuMemGiB = 0.0;
    if (!rasterLodExplicit) rasterLod = false;
    if (!rtLodExplicit) rtLod = false;
  }
  if (wantPathTrace && ptFinal && !rtLodExplicit) {
    // Final-quality transport must never replace authored geometry with the
    // generic AABB proxy. Besides changing silhouettes, the proxy has no
    // authored material and therefore appears as a camera-dependent white box.
    // An explicit --rt-lod remains available for memory-constrained renders.
    rtLod = false;
  }
  if (maxAssetReadBytes > 0) {
    lightusd::security_policy::SetMaxAssetReadBytes(
        static_cast<size_t>(maxAssetReadBytes));
  }

  if (wantPathTrace) {
    if (wantCpuRt) {
      LOGE("--path-trace supports Vulkan, CUDA, and HIP; --cpu-rt remains a preview renderer");
      return 1;
    }
    pathTraceSettings = ptFinal ? lusdview::PathTraceSettings::Final()
                                : lusdview::PathTraceSettings::Interactive();
    pathTraceSettings.enabled = true;
    if (ptSamples) pathTraceSettings.targetSamples = *ptSamples;
    if (ptMaxDepth) pathTraceSettings.maxDepth = *ptMaxDepth;
    if (ptRrDepth) pathTraceSettings.russianRouletteDepth = *ptRrDepth;
    if (ptMotionSegments) pathTraceSettings.motionSegments = *ptMotionSegments;
    if (ptSeed) pathTraceSettings.seed = *ptSeed;
    if (ptMaxSssEvents)
      pathTraceSettings.maxSubsurfaceEvents = *ptMaxSssEvents;
    if (ptMaxVolumeEvents)
      pathTraceSettings.maxVolumeEvents = *ptMaxVolumeEvents;
    if (ptVariance) pathTraceSettings.varianceThreshold = *ptVariance;
    if (ptFireflyClamp) pathTraceSettings.fireflyClamp = *ptFireflyClamp;
    if (ptDenoiseExplicit) pathTraceSettings.denoise = ptDenoise;
    pathTraceSettings.sanitize();
    if (!wantCuda && !wantHip) wantRt = true;
    if ((wantCuda || wantHip) && pathTraceSettings.targetSamples > 0)
      rtSamples = static_cast<int>(std::min(pathTraceSettings.targetSamples,
                                           1048576u));
    if (headless && !wantCuda && !wantHip && maxFrames < 0 &&
        pathTraceSettings.targetSamples > 0)
      maxFrames = static_cast<int>(pathTraceSettings.targetSamples);
  } else if (!linearOutput.empty()) {
    LOGE("--linear-output requires --path-trace");
    return 1;
  }
  if (!linearOutput.empty() &&
      LowerCopy(std::filesystem::path(linearOutput).extension().string()) !=
          ".exr") {
    LOGE("--linear-output must use the .exr extension");
    return 1;
  }

  // Ray tracing is a Vulkan technique, so --rt implies the Vulkan backend.
  if (wantRt) {
    backend = lusdview::Backend::Vulkan;
    backendExplicit = true;
  }
  // Windowless rendering is a Vulkan-only path (GL needs a window/context).
  if (viewDirExplicit && !cameraName.empty()) {
    LOGE("--view-dir cannot be combined with the selected --camera/profile camera");
    return 1;
  }

  if (headless) {
    backend = lusdview::Backend::Vulkan;
    backendExplicit = true;
  }
  // The `next` loader is the OpenGL large-scene mesh preview; default it to the
  // GL backend unless the user picked one explicitly (it feeds either backend).
  if (useNextLoader && !backendExplicit) {
    backend = lusdview::Backend::GL;
  }

#if !defined(HAVE_VULKAN)
  if (backend == lusdview::Backend::Vulkan) {
    LOGW("Vulkan backend not compiled in; using OpenGL.");
    backend = lusdview::Backend::GL;
    wantRt = false;
  }
#endif

  if ((quitAfterFullUpload || quitAfterConvert) &&
      ((backend != lusdview::Backend::GL && backend != lusdview::Backend::Vulkan) ||
       !useNextLoader || threaded || headless ||
       wantRt || wantCuda || wantHip || wantCpuRt)) {
    LOGE("--quit-after-full-upload/--quit-after-convert requires interactive "
         "non-threaded OpenGL/Vulkan with --next");
    return 1;
  }

  if (effectiveProfile != LargeSceneProfile::Off) {
    LOGI("resource budget: vram capacity=%.1f GiB (%s) -> limit=%.1f GiB, "
         "host capacity=%.1f GiB -> limit=%.1f GiB",
         double(vramCapacity) / double((uint64_t{1} << 30)),
         vramBudgetExplicit ? "--vram-budget" : "probed",
         targetVramGiB,
         double(hostCapacity) / double((uint64_t{1} << 30)),
         targetHostGiB);
    LOGI("large-scene-profile %s resolved: backend=%s --next=%s "
         "--raster-lod=%s full=%.1f cull=%.1f --rt-lod=%s full=%.1f cull=%.1f "
         "--max-gpu-mem=%.1f --max-draw-meshes=%lld --max-tris=%lld",
         ProfileName(effectiveProfile),
         backend == lusdview::Backend::Vulkan ? "vk" : "gl",
         useNextLoader ? "on" : "off",
         rasterLod ? "on" : "off", rasterLodFullPx, rasterLodCullPx,
         rtLod ? "on" : "off", rtLodFullPx, rtLodCullPx,
         maxGpuMemGiB, maxDrawMeshes, maxTris);
    if (!fullFidelity && effectiveProfile == LargeSceneProfile::ProceduralHeavy && !headless &&
        maxFrames < 0) {
      LOGI("large-scene-profile procedural-heavy: interactive preview textures<=%d px, "
           "compression=%s, mips=%s, curves<=64 prims/100000 strands",
           maxTextureSizeExplicit ? textureOptions.maxTextureSize : 512,
           textureCompressionExplicit ? "explicit" : "auto",
           mipsExplicit ? (textureOptions.generateMips ? "on" : "off") : "off");
    }
    if (maxAssetReadBytes > 0) {
      LOGI("large-scene-profile %s: max asset read bytes=%llu",
           ProfileName(effectiveProfile),
           static_cast<unsigned long long>(maxAssetReadBytes));
    }
  }

  const lusdview::ConfigLoadResult config = lusdview::LoadStartupConfig(configPath);
  if (config.status == lusdview::ConfigLoadStatus::Error) {
    if (!config.path.empty()) {
      LOGE("config %s: %s", config.path.string().c_str(), config.error.c_str());
    } else {
      LOGE("config: %s", config.error.c_str());
    }
    if (config.explicitPath) return 1;
    LOGW("ignoring invalid default config and continuing with built-in defaults.");
  } else if (config.status == lusdview::ConfigLoadStatus::Loaded) {
    LOGI("loaded config %s", config.path.string().c_str());
    for (const std::string& warning : config.warnings) {
      LOGW("config %s: %s", config.path.string().c_str(), warning.c_str());
    }
  }

  if (!cameraConformExplicit &&
      config.status == lusdview::ConfigLoadStatus::Loaded &&
      config.config.cameraConform) {
    const std::string& value = *config.config.cameraConform;
    if (value == "crop") cameraConform = lusdview::CameraConform::Crop;
    else if (value == "horizontal") cameraConform = lusdview::CameraConform::Horizontal;
    else if (value == "vertical") cameraConform = lusdview::CameraConform::Vertical;
    else if (value == "none") cameraConform = lusdview::CameraConform::None;
    else cameraConform = lusdview::CameraConform::Fit;
  }

  if (!envmapPath.empty() && !std::filesystem::is_regular_file(envmapPath)) {
    LOGE("--envmap file does not exist: %s", envmapPath.c_str());
    return 1;
  }
  lusdview::App app(backend);
  app.setVirtualHumanProfile(virtualHumanProfile);
  app.setAutoriggerExecutable(autoriggerExecutable);
  if (argc > 0 && argv[0] && argv[0][0]) {
    std::error_code executablePathError;
    const std::filesystem::path executablePath =
        std::filesystem::absolute(argv[0], executablePathError);
    app.setExecutablePath(executablePathError ? std::filesystem::path(argv[0])
                                              : executablePath);
  }
  if (config.status == lusdview::ConfigLoadStatus::Loaded) {
    if (config.config.adaptiveQuality && !adaptiveQualityExplicit)
      adaptiveQuality = *config.config.adaptiveQuality;
    if (config.config.targetRenderFps && !targetRenderFpsExplicit)
      targetRenderFps = *config.config.targetRenderFps;
    if (config.config.minRenderScale && !minRenderScaleExplicit)
      minRenderScale = *config.config.minRenderScale;
    if (config.config.transparency && !transparencyExplicit) {
      if (*config.config.transparency == "weighted") {
        transparencyMode = lusdview::TransparencyMode::Weighted;
      } else if (*config.config.transparency == "sorted") {
        transparencyMode = lusdview::TransparencyMode::Sorted;
      } else {
        transparencyMode = lusdview::TransparencyMode::Auto;
      }
    }
    if (config.config.cudaRtBackend && !cudaRtBackendExplicit) {
      if (*config.config.cudaRtBackend == "optix")
        cudaRtBackend = lusdview::CudaRtBackend::Optix;
      else if (*config.config.cudaRtBackend == "software")
        cudaRtBackend = lusdview::CudaRtBackend::SoftwareBvh;
      else
        cudaRtBackend = lusdview::CudaRtBackend::Auto;
    }
    if (config.config.materialXVulkanShaderMaxKiB &&
        !materialXVulkanShaderMaxExplicit) {
      materialXVulkanShaderMaxKiB = static_cast<size_t>(
          *config.config.materialXVulkanShaderMaxKiB);
    }
    if (config.config.materialXVulkanCompileTimeoutSec &&
        !materialXVulkanCompileTimeoutExplicit) {
      materialXVulkanCompileTimeoutSec =
          *config.config.materialXVulkanCompileTimeoutSec;
    }
    if (config.config.fontSizePx) app.setFontSize(*config.config.fontSizePx);
    if (config.config.windowScale) app.setWindowScale(*config.config.windowScale);
    if (!windowSizeExplicit && config.config.windowWidth &&
        config.config.windowHeight) {
      // Config dimensions are a first-run fallback. Persisted native geometry
      // in imgui.ini wins unless the user explicitly passes --size.
      app.setDefaultWindowSize(*config.config.windowWidth,
                               *config.config.windowHeight);
    }
    if (config.config.orbitSensitivity) {
      app.setOrbitSensitivity(*config.config.orbitSensitivity);
    }
    if (config.config.panSensitivity) {
      app.setPanSensitivity(*config.config.panSensitivity);
    }
    if (config.config.dollySensitivity) {
      app.setDollySensitivity(*config.config.dollySensitivity);
    }
    if (config.config.invertYaw) {
      app.setInvertYaw(*config.config.invertYaw);
    }
    if (config.config.invertDolly) {
      app.setInvertDolly(*config.config.invertDolly);
    }
    app.setRecentScenes(config.config.recentScenes);
    if (config.config.vulkanDevice && !vkDeviceExplicit) {
      devicePreference.vulkanDevice = *config.config.vulkanDevice;
    }
    if (config.config.subdivisionLevel && !subdivisionLevel.has_value()) {
      subdivisionLevel = *config.config.subdivisionLevel;
    }
    if (config.config.subdivisionAuto && !subdivisionAutoExplicit) {
      subdivisionAuto = *config.config.subdivisionAuto;
    }
    if (config.config.subdivisionAutoMaxLevel && !subdivisionAutoMaxExplicit) {
      subdivisionAutoMaxLevel = *config.config.subdivisionAutoMaxLevel;
    }
    for (const auto& kv : config.config.subdivisionPrimLevels) {
      subdivisionPrimLevels.emplace(kv.first, kv.second);
    }
  }
  // Test/headless wrappers can select the hardware Vulkan device without
  // rewriting every manifest or shell command. Keep CLI precedence, then the
  // environment, then the startup config above. "auto" deliberately leaves
  // the normal device selection untouched.
  if (!vkDeviceExplicit) {
    const char* envVkDevice = std::getenv("LUSDVIEW_VK_DEVICE");
    if (envVkDevice && *envVkDevice &&
        std::strcmp(envVkDevice, "auto") != 0) {
      devicePreference.vulkanDevice = envVkDevice;
    }
  }
  // Persist the recent-scenes list back to the resolved config path (the default
  // platform path when none was given), so File > Open Recent survives restarts.
  if (!config.path.empty()) app.setConfigPath(config.path);
  // USD composition options: CLI > config > defaults. Payloads default to
  // lazy (deferred) interactively; headless/--frames runs load them eagerly so
  // screenshots are complete.
  {
    lusdview::LoadOptions lo;
    if (rasterQualityHigh && !materialPurposeExplicit) materialPurpose = "full";
    if (rasterQualityHigh && !domeIblExplicit) textureOptions.domeIbl = 2;
    lo.materialPurpose = materialPurpose;
    if (rasterQualityHigh) lo.curveTessellationSegments = 16;
    lo.composition = !noComposition;
    lo.compositionThreads = compositionThreads;
    lo.conversionThreads = conversionThreads;
    lo.compositionOpinionBatch = compositionOpinionBatch;
    lo.instanceChunkSamples = instanceChunkSamples;
    lo.meshConversionChunkPrims = meshConversionChunkPrims;
    lo.meshConversionChunkBytes = meshConversionChunkMB * 1024ull * 1024ull;
    lo.curveParallelMinPrims = curveParallelMinPrims;
    // Full-fidelity runs measure/produce authoritative render data directly;
    // the proxy handoff would add an extra renderer scene transition before the
    // result is usable. Ordinary interactive presets retain the early preview.
    lo.progressivePreview = effectiveProfile != LargeSceneProfile::Off &&
                            !headless && maxFrames < 0 && !fullFidelity;
    lo.previewCache.mode = previewCacheExplicit
                               ? previewCacheMode
                               : (effectiveProfile != LargeSceneProfile::Off
                                      ? lusdview::PreviewCacheMode::Auto
                                      : lusdview::PreviewCacheMode::Off);
    lo.previewCache.directory = previewCacheDir;
    lo.previewCache.maxBytes = static_cast<size_t>(
        previewCacheMaxGiB * 1024.0 * 1024.0 * 1024.0);
    lo.previewCache.timing = timing;
    // Instance-heavy startup can be dominated by eager Ptex fallback construction. Keep a
    // bounded representative set and stream the remaining faces on demand.
    if (effectiveProfile == LargeSceneProfile::InstanceHeavy) {
      // Keep startup bounded; admitted meshes enqueue their remaining source
      // faces for physical-cache streaming after the first usable frame.
      lo.ptexInitialFaces = 16;
      lo.ptexPhysicalCacheBytes = 8ull * 1024ull * 1024ull;
      lo.curveTessellationSegments = 2;
      if (maxDrawMeshes > 0 && !automaticInteractiveProfile) {
        lo.maxMeshConversions = static_cast<size_t>(maxDrawMeshes);
      }
    } else if (effectiveProfile == LargeSceneProfile::ProceduralHeavy && !headless &&
               maxFrames < 0) {
      // Baked procedurals and large texture sets can exceed ordinary workstation
      // VRAM. Start with a shaded, topology-valid preview; explicit/headless
      // production runs keep the requested quality settings.
      lo.curveTessellationSegments = 1;
      lo.maxCurvePrims = 64;
      lo.maxCurveStrands = 100000;
      lo.asyncTextureDecode = true;
    }
    if (ptexInitialFaces) lo.ptexInitialFaces = *ptexInitialFaces;
    if (ptexCacheMB)
      lo.ptexPhysicalCacheBytes = *ptexCacheMB * 1024ull * 1024ull;
    if (asyncTextureDecode) lo.asyncTextureDecode = true;
    if (curvePreviewPrims) lo.maxCurvePrims = *curvePreviewPrims;
    if (curvePreviewStrands) lo.maxCurveStrands = *curvePreviewStrands;
    lo.timing = timing;
    lo.streamBufferBytes = streamBufferMB * 1024ull * 1024ull;
    lo.previewMaxBoxes = previewMaxBoxes;
    // Keep the next loader bounded even without a named large-scene profile.
    // Profiles may tighten these values below, but ordinary preview/headless
    // loads must not fall back to unlimited composition or GPU staging.
    lo.maxMemoryBytes = static_cast<size_t>(targetBudget.host_limit);
    lo.gpuGeometryBudgetBytes = fullFidelity
        ? 0
        : static_cast<size_t>(
              maxGpuMemExplicit && maxGpuMemGiB > 0.0
                  ? maxGpuMemGiB * double((uint64_t{1} << 30))
                  : double(targetBudget.gpu_geometry_limit));
    lo.uploadStagingBytes =
        static_cast<size_t>(targetBudget.upload_staging_limit);
    if (config.status == lusdview::ConfigLoadStatus::Loaded) {
      if (config.config.composition && !noComposition) {
        lo.composition = *config.config.composition;
      }
      if (config.config.payloadPolicy && !deferPayloads.has_value()) {
        deferPayloads = (*config.config.payloadPolicy == "defer");
      }
    }
    // Match ordinary USD viewer semantics: the initial scene is complete unless
    // lazy payload loading was explicitly requested. Geometry/VRAM budgets and
    // dynamic LOD remain the safety net for genuinely large composed scenes.
    const bool defer = deferPayloads.value_or(false);
    lo.payloadPolicy = defer ? lusdview::PayloadPolicy::DeferAll
                             : lusdview::PayloadPolicy::LoadAll;
    // Explicit opt-in only (no headless default flip): deferring references is
    // non-standard, so honor the flag verbatim even for --frames runs.
    lo.deferReferences = deferReferences;
    lo.allowParentRelativePaths = allowParentPaths;
    lo.textureOptions = textureOptions;
    if (fullFidelity) {
      lo.maxMeshConversions = 0;
      lo.maxCurvePrims = 0;
      lo.maxCurveStrands = 0;
    }
    // Default: skip the expensive CPU mip-generation + BCn-compression pipeline
    // whenever the decoded (uncompressed) textures fit comfortably in half the
    // device VRAM. The loader treats half the texture budget as its "comfortable
    // resident" ceiling, so a budget of the full VRAM means: convert only when
    // textures exceed ~VRAM/2. An ordinary 1 GB RGBA8 texture set is ~1/16 of a
    // 16 GB card and skips conversion entirely; a 16 GB card keeps uncompressed
    // textures resident with room for geometry and framebuffers. Explicit
    // --texture-compress / --texture-mips / --texture-budget-mb override this.
    lo.optimizeTextureUpload = useNextLoader;
    lo.textureGpuBudgetBytes = vramCapacity;
    lo.textureFit = textureFit;
    lo.textureFitThresholdBytes = static_cast<size_t>(textureFitThreshold);
    lo.textureComfortBytes = static_cast<size_t>(textureComfortBytes);
    lo.textureBudgetExplicit = textureBudgetExplicit;
    lo.textureCompressionExplicit = textureCompressionExplicit;
    lo.textureMipsExplicit = mipsExplicit;
    if (effectiveProfile == LargeSceneProfile::ProceduralHeavy && !headless &&
        maxFrames < 0) {
      // An explicit --texture-fit outranks a profile's implicit texture cap
      // (but never an explicit --texture-max-size).
      if (!maxTextureSizeExplicit && !textureFitExplicit)
        lo.textureOptions.maxTextureSize = 512;
      if (!textureCompressionExplicit) {
        lo.textureOptions.compression =
            lusdview::TextureCompressionMode::Auto;
      }
      if (!mipsExplicit) lo.textureOptions.generateMips = false;
    }
    // Interactive --next loads should reach a usable frame promptly even for
    // texture sets larger than VRAM/2 (where the comfortable-skip above does not
    // apply): defer CPU mip generation so the uploaded base level can render
    // immediately. Headless/benchmark runs rely on the comfortable-skip default
    // above and honor an explicit --texture-mips on.
    if (useNextLoader && !headless && maxFrames < 0 && !mipsExplicit) {
      lo.textureOptions.generateMips = false;
      LOGI("interactive --next: deferring CPU texture mip generation "
           "(use --texture-mips on to enable)");
    }
    lo.subdivisionLevel = std::max(0, subdivisionLevel.value_or(2));
    lo.subdivisionAuto = subdivisionAuto;
    lo.subdivisionAutoMaxLevel =
        std::max(0, std::min(10, subdivisionAutoMaxLevel));
    for (const auto& kv : subdivisionPrimLevels) {
      if (!kv.first.empty()) {
        lo.subdivisionPrimLevels[kv.first] = std::max(0, kv.second);
      }
    }
    if (timeCode.has_value()) lo.timecode = *timeCode;
    app.setLoadOptions(lo);
  }
  app.setUploadBudgetMs(uploadBudgetMs);
  app.setQuitAfterFullUpload(quitAfterFullUpload);
  app.setQuitAfterConvert(quitAfterConvert);
  app.setLoadBudget(static_cast<std::size_t>(maxTris < 0 ? 0 : maxTris), timeBudget);
  if (maxTris > 0)
    app.setRtMaxTris(static_cast<std::size_t>(maxTris));
  app.setGpuBudget(
      maxGpuMemGiB > 0.0 ? static_cast<std::size_t>(maxGpuMemGiB * 1024.0 *
                                                    1024.0 * 1024.0)
                         : 0,
      static_cast<std::size_t>(maxDrawMeshes < 0 ? 0 : maxDrawMeshes));
  app.setRobustFrame(robustFrame);
  app.setRtLod(rtLod, rtLodFullPx, rtLodCullPx, rtLodBand);
  app.setRasterLod(rasterLod, rasterLodFullPx, rasterLodCullPx);
  app.setDynamicPrototypeLod(dynamicLod);
  if (uiScale) {
    if (*uiScale > 0.25f) {
      app.clearWindowSizeOverride();
      app.setUiScale(*uiScale);
    } else {
      LOGW("ignoring invalid --ui-scale %.3f (must be > 0.25)", *uiScale);
    }
  }
  if (windowSizeExplicit) app.setWindowSize(windowWidth, windowHeight);
  app.setUseNextLoader(useNextLoader);
  app.setCullEnabled(!noCull);
  app.setShowGrid(showGrid);
  app.setShowSkeleton(showSkeleton || virtualHumanProfile);
  app.setCamDolly(camDolly);
  app.setWindowShot(windowShot);
  app.setRequestRayTracing(wantRt);
  app.setDevicePreference(devicePreference);
  app.setTransparencyMode(transparencyMode);
  app.setRasterQualityHigh(rasterQualityHigh);
  app.setStartupEnvmap(envmapPath, envmapIntensity, envmapRotation);
  app.setAllowBackendFallback(!backendExplicit && backend == lusdview::Backend::Vulkan);
  app.setSkinningMode(skinningMode);
  app.setPlayAnimation(playAnim);
  app.setMcpStdio(mcpStdio);
  app.setMcpHttp(mcpHttpPort);
  app.setLiveShaderWatch(liveShaderWatch);
  app.setMaterialXVulkanShaderLimits(
      materialXVulkanShaderMaxKiB * 1024u,
      materialXVulkanCompileTimeoutSec);
  app.setStreamHttp(streamHttpPort);
  app.setStreamCodec(streamCodec);
  app.setStreamMotionRes(streamMotionRes);
  app.setStreamMotionQuality(streamMotionQuality);
  app.setStreamIdleMs(streamIdleMs);
  app.setHeadless(headless);
  const bool useThreaded = threadedExplicit
      ? threaded
      : backend == lusdview::Backend::Vulkan && !headless && maxFrames < 0;
  app.setThreaded(useThreaded);
  app.setAdaptiveQuality(adaptiveQuality && !headless && maxFrames < 0,
                         targetRenderFps, minRenderScale);
  if (virtualHumanProfile && (wantCuda || wantHip)) {
    LOGW("vchar CUDA/HIP RT slots are reserved but deferred; using GL/Vulkan");
    wantCuda = false;
    wantHip = false;
  }
  app.setCudaRt(wantCuda);
  app.setCudaRtBackend(cudaRtBackend);
  app.setCudaCacheDir(cudaCacheDir);
  app.setHipRt(wantHip);
  app.setCpuRt(wantCpuRt);
  app.setRtSamples(rtSamples);
  app.setPathTraceSettings(pathTraceSettings);
  app.setLinearOutput(linearOutput);
  app.setLensOverrides(fStopOverride, focusDistanceOverride);
  app.setRenderReport(renderReport);
  app.setCheckpointOutput(checkpointEvery, checkpointPattern);
  app.setLargeSceneProfile(ProfileName(effectiveProfile));
  app.setRtMaxInstances(static_cast<size_t>(rtMaxInstances));
  app.setLodStream(lodStream);
  app.setLodMaxMemGiB(lodMaxMem);
  app.setLodMaxVramGiB(lodMaxVram);
  app.setCameraName(cameraName);
  app.setStereoRequested(stereoRequested);
  app.setRasterMotionEnabled(rasterMotionEnabled);
  app.setCameraConform(cameraConform);
  if (viewDirExplicit) app.setViewDirection(viewDir[0], viewDir[1], viewDir[2]);
  if (wantWireframe) app.setRenderMode(lusdview::RenderMode::Wireframe);
  if (wantMaterialId) app.setRenderMode(lusdview::RenderMode::MaterialId);
  if (wantMode) app.setRenderMode(*wantMode);
  if (!modeSweep.empty()) {
    if (screenshot.empty() || screenshot.find("{mode}") == std::string::npos) {
      LOGE("--mode-sweep needs --screenshot with a {mode} placeholder, e.g. "
           "--screenshot out-{mode}.ppm");
      return 1;
    }
    // CUDA and HIP trace AFTER the frame loop and write the screenshot
    // themselves; the sweep captures the in-loop viewport. Accepting the
    // combination silently produced N raster images labelled as ray-traced
    // AOVs, plus one file named with the literal "{mode}" from the tracer's
    // own write. Refuse instead of emitting wrong output.
    if (wantCuda || wantHip) {
      LOGE("--mode-sweep is not supported with --cuda/--hip: those backends "
           "trace after the frame loop and write the screenshot themselves. "
           "Run one mode per invocation for them.");
      return 1;
    }
    app.setModeSweep(modeSweep, screenshot);
  }
  for (const auto& bw : wantBlend) app.setBlendWeight(bw.first, bw.second);
  if (!wantSelect.empty()) app.setInitialSelection(wantSelect);
  return app.run(file, maxFrames, screenshot);
}
