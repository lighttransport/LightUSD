// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
// CPU-only, opt-in profiling probe; not registered as a regression test.
// Usage: bench_scene_readback scene.usdc [threads=1] [metadata|retained]
#include "next/crate/crate-reader.hh"
#include "next/layer/layer.hh"
#include "tydra/next/render-converter.hh"
#include "logger.hh"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
struct Stamp {
  Clock::time_point wall = Clock::now();
  std::clock_t cpu = std::clock();
};

void Report(const char* gate, const Stamp& start, size_t bytes = 0) {
  const double wall = std::chrono::duration<double, std::milli>(
      Clock::now() - start.wall).count();
  const double cpu = 1000.0 * static_cast<double>(std::clock() - start.cpu) /
                     CLOCKS_PER_SEC;
  double peak_mib = 0;
#if defined(__unix__) || defined(__APPLE__)
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
    peak_mib = static_cast<double>(usage.ru_maxrss) / (1024 * 1024);
#else
    peak_mib = static_cast<double>(usage.ru_maxrss) / 1024;
#endif
  }
#endif
  std::printf("[readback_profile] gate=%s wall_ms=%.3f cpu_ms=%.3f "
              "peak_mib=%.3f bytes=%zu\n", gate, wall, cpu, peak_mib, bytes);
  std::fflush(stdout);
}

template <class Element>
size_t CopyArrays(const std::vector<
    const lightusd::tydra::next::ChunkedArray<Element>*>& arrays, bool scalar) {
  // Retain all buffers of this element type together, as an owned mesh copy
  // does. The caller retains floats and indices separately; this is a native
  // copy lower bound, not JS marshaling/heap-growth or a complete mesh object.
  std::vector<std::vector<Element>> owned;
  owned.reserve(arrays.size());
  size_t bytes = 0;
  for (const auto* array : arrays) {
    owned.emplace_back(array->size());
    if (scalar) {
      for (size_t i = 0; i < array->size(); ++i) owned.back()[i] = (*array)[i];
    } else if (!array->copy_bytes_to(owned.back().data(),
                                    owned.back().size() * sizeof(Element))) {
      return 0;
    }
    bytes += owned.back().size() * sizeof(Element);
  }
  return bytes;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 4) {
    std::fprintf(stderr, "Usage: bench_scene_readback scene.usdc [threads=1] "
                         "[metadata|retained]\n");
    return 2;
  }
  int threads = 1;
  if (argc >= 3) {
    const std::string text(argv[2]);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), threads);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        threads < 1 || threads > 64) return 2;
  }
  const std::string mode = argc == 4 ? argv[3] : "retained";
  if (mode != "retained" && mode != "metadata") return 2;

  lightusd::logging::Logger::getInstance().setLogLevel(
      lightusd::logging::LogLevel::Info);

  lightusd::next::CrateReadOptions options;
  options.max_memory = size_t(512) << 20;
  options.num_threads = threads;
  options.enable_timing = true;
  options.lazy_arrays = true;
  options.use_mmap = true;
  Stamp start;
  lightusd::next::CrateReader reader(options);
  auto loaded = reader.ReadFile(argv[1]);
  Report("read_complete", start);
  if (!loaded.success) {
    std::fprintf(stderr, "Read failed\n");
    return 1;
  }
  std::printf("[readback_profile] prims=%zu threads=%d mode=%s\n",
              loaded.stage.GetRootLayer()->prim_count(), threads, mode.c_str());

  lightusd::tydra::next::ConverterConfig config;
  config.limits.max_resident_bytes = size_t(512) << 20;
  config.execution.max_threads = threads;
  config.material.load_textures = false;
  config.animation.enabled = false;
  config.mesh.retain_geometry = mode == "retained";
  config.mesh.retain_triangulation = mode == "metadata";
  std::string previous_gate;
  Stamp previous;
  config.progress_callback = [&](float, const std::string& message) {
    // Strip mesh names: only phase gates belong in shareable profile logs.
    const std::string gate = message.substr(0, message.find(':'));
    if (gate == previous_gate) return;
    Report(gate.c_str(), previous);
    previous_gate = gate;
    previous = Stamp{};
  };
  lightusd::tydra::next::RenderSceneConverter converter(config);
  start = Stamp{};
  auto result = converter.Convert(loaded.stage);
  Report("convert_complete", start);
  if (!result.success) {
    std::fprintf(stderr, "Conversion failed\n");
    return 1;
  }
  for (bool scalar : {false, true}) {
    size_t copied = 0;
    start = Stamp{};
    for (const auto& mesh : result.scene.meshes) {
      copied += CopyArrays<float>(
          {&mesh.points, &mesh.normals, &mesh.tangents, &mesh.texcoords_0,
           &mesh.texcoords_1, &mesh.colors, &mesh.opacities}, scalar);
      copied += CopyArrays<uint32_t>({&mesh.triangulated_indices}, scalar);
    }
    Report(scalar ? "native_scalar_chunk_copy" : "native_owned_chunk_copy", start, copied);
  }
  std::printf("[readback_profile] meshes=%zu warnings=%zu\n",
              result.scene.meshes.size(), result.warnings.size());
  return 0;
}
