// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - lusdcat
//
// The unified lusdcat CLI for the src/next module (formerly `next_usdcat`).
// The usd-wg asset-corpus regression gate (tests/parse-asset-corpus.mjs) drives
// next the same way it drives the legacy loader; `next_usdcat` remains an
// alias of this target for scripts that still use the old name:
//   lusdcat [-l|-f] file.usd[acz]
//     -l  load only (parse the single layer; default)
//     -f  flatten (compose sublayers/references/payloads/... via pcp, print USDA)
// Contract: exit 0 = loaded; diagnostics on stderr prefixed "WARN : " / "ERR : ".

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <memory>
#include <vector>

#include "logger.hh"  // lightusd::logging (next routes diagnostics through it)
#include "next/pcp/cache.hh"
#include "next/pcp/batched-compose.hh"
#include "next/pcp/layer-registry.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/stage/stage.hh"
#include "next/load-usd.hh"
#include "next/writer/usda-writer.hh"
#include "next/writer/usdc-writer.hh"

using namespace lightusd::next;

static void emit_lines(const std::string &msgs, const char *prefix) {
  size_t pos = 0;
  while (pos < msgs.size()) {
    size_t nl = msgs.find('\n', pos);
    if (nl == std::string::npos) nl = msgs.size();
    if (nl > pos) {
      std::fprintf(stderr, "%s%.*s\n", prefix, int(nl - pos), msgs.c_str() + pos);
    }
    pos = nl + 1;
  }
}

static void emit_crate_limit_hint(const std::string &err,
                                  size_t max_crate_elements) {
  if (max_crate_elements == 0 &&
      (err.find("crate table limit") != std::string::npos ||
       err.find("Too many ") != std::string::npos ||
       err.find("exceeds max_array_elements") != std::string::npos)) {
    std::fprintf(stderr,
                 "ERR : hint: raise --max-crate-elements for large trusted "
                 "USDC files\n");
  }
}

// One-shot flattening can spend seconds destroying the composed Stage after
// its output is already closed. Keep this opt-in: _Exit skips all remaining
// destructors and atexit handlers, so it is suitable only after a successful
// write whose C/C++ streams have been flushed explicitly.
[[noreturn]] static void ExitAfterSuccessfulWrite() {
  std::cout.flush();
  std::cerr.flush();
  std::fflush(nullptr);
  std::_Exit(0);
}

int main(int argc, char **argv) {
  bool flatten = false;
  // Compose-free parse->write: LoadLayerFromFile (parse only, no composition) ->
  // WriteLayer. Measures RAW parse and RAW write throughput in isolation, and is
  // an idempotent parse-fidelity oracle (rewriting its own output is byte-identical).
  bool rewrite_layer = false;
  bool compose_only = false;
  enum class OutputFormat { Auto, USDA, USDC };
  OutputFormat output_format = OutputFormat::Auto;
  int parse_threads = -1;  // explicit CLI hint overrides the environment
  size_t max_crate_elements = 0;  // explicit trusted-dataset override only
  size_t max_memory_bytes = 0;    // zero leaves each mode's existing defaults
  // USDA parse fast paths (default on; byte-identical, serial fallback).
  bool async_arrays = true;
  bool parallel_prims = true;
  bool openusd_compat = false;
  bool aousd_strict = false;
  // Default instance flatten = holder (the historical -f behavior). `native`
  // keeps instancing; `prototypes` = usdcat-style /Flattened_Prototype_N.
  pcp::InstanceFlattenMode inst_mode = pcp::InstanceFlattenMode::Holder;
  pcp::PrototypeNumbering proto_num = pcp::PrototypeNumbering::Deterministic;
  // Parallel composition: -1 (or 0) = auto (hardware concurrency, clamped to
  // kMaxExecutionThreads), N = fixed, 1 = serial. Byte-identical to serial.
  // It prefetches arc layers, pre-warms the sources cache (LIVRPS arc
  // resolution, instance prototypes included) and fills opinions
  // concurrently; a net win on every measured scene (reference scene, Scene A, Scene C,
  // House), so it defaults ON like parse/write. Force serial with
  // --compose-threads 1. (Independent of the writer's
  // LIGHTUSD_NEXT_NUM_THREADS.) A build without LIGHTUSD_NEXT_ENABLE_THREAD
  // composes serially regardless.
  int compose_threads = -1;
  // Writer threads: 0 = auto (default), 1 = serial, N = fixed. Overrides
  // LIGHTUSD_NEXT_NUM_THREADS for the writer only (output is byte-identical
  // for any count). -1 = not given (fall back to the env / auto).
  int write_threads = -1;
  bool load_payloads = true;
  // Flatten-to-USDC: release composed property values as the writer encodes
  // them (--no-consume-values keeps the composed stage intact).
  bool consume_values = true;
  // One-shot mode: skip the composed Stage teardown after a successful output
  // write. Disabled by default because it intentionally skips destructors and
  // atexit handlers; see ExitAfterSuccessfulWrite().
  bool fast_exit = false;
  // Debug: compose in namespace batches (pcp::ComposeStageBatched) and merge
  // them; output must match the regular compose byte for byte.
  long batch_roots = -1;
  // --memory-profile low: flatten to USDC in namespace batches, streaming each
  // into the writer (pcp::FlattenToUSDCFileBatched). Same bytes, slower,
  // a fraction of the peak memory.
  bool low_memory = false;
  size_t batch_frontier = 65536;
  size_t batch_prims = 400000;
  // --variant-fallback set=opt1,opt2  (repeatable). Stock pxr registers NO
  // fallbacks; the AOUSD supplemental corpus expectations were generated in
  // an environment with the classic standin->render fallback, so its runner
  // opts in explicitly.
  std::map<std::string, std::vector<std::string>> variant_fallbacks;
  const char *filename = nullptr;
  const char *out_path = nullptr;  // -o/--output: write flatten to a file (FdSink)
  std::vector<std::string> required_prims;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-f") == 0) {
      flatten = true;
    } else if (std::strcmp(argv[i], "--rewrite-layer") == 0) {
      rewrite_layer = true;
    } else if (std::strcmp(argv[i], "--compose-only") == 0) {
      compose_only = true;
      flatten = true;
    } else if (std::strcmp(argv[i], "--output-format") == 0 && i + 1 < argc) {
      const char* format = argv[++i];
      if (std::strcmp(format, "usda") == 0) output_format = OutputFormat::USDA;
      else if (std::strcmp(format, "usdc") == 0) output_format = OutputFormat::USDC;
      else {
        std::fprintf(stderr, "ERR : --output-format must be usda or usdc\n");
        return 2;
      }
    } else if (std::strcmp(argv[i], "--max-memory-mib") == 0 && i + 1 < argc) {
      char* end = nullptr;
      const char* arg = argv[++i];
      const unsigned long long count = std::strtoull(arg, &end, 10);
      if (*arg < '0' || *arg > '9' || *end || count == 0 || count > SIZE_MAX / 1048576) {
        std::fprintf(stderr, "ERR : invalid --max-memory-mib\n");
        return 2;
      }
      max_memory_bytes = static_cast<size_t>(count) * 1048576;
    } else if (std::strcmp(argv[i], "--max-crate-elements") == 0 && i + 1 < argc) {
      char* end = nullptr;
      const char* arg = argv[++i];
      const unsigned long long count = std::strtoull(arg, &end, 10);
      if (*arg < '0' || *arg > '9' || *end || count == 0 || count > UINT32_MAX) {
        std::fprintf(stderr, "ERR : --max-crate-elements must be between 1 and 4294967295\n");
        return 2;
      }
      max_crate_elements = static_cast<size_t>(count);
    } else if (std::strcmp(argv[i], "--parse-threads") == 0 && i + 1 < argc) {
      char* end = nullptr;
      const char* arg = argv[++i];
      const long count = std::strtol(arg, &end, 10);
      if (end == arg || *end || count < 0 || count > kMaxExecutionThreads) {
        std::fprintf(stderr, "ERR : --parse-threads must be between 0 and %d\n",
                     kMaxExecutionThreads);
        return 2;
      }
      parse_threads = static_cast<int>(count);
    } else if (std::strcmp(argv[i], "--no-async-arrays") == 0) {
      async_arrays = false;
    } else if (std::strcmp(argv[i], "--no-parallel-prims") == 0) {
      parallel_prims = false;
    } else if (std::strcmp(argv[i], "-l") == 0) {
      flatten = false;
    } else if ((std::strcmp(argv[i], "-o") == 0 ||
                std::strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
      out_path = argv[++i];
    } else if (std::strcmp(argv[i], "--openusd-compat") == 0) {
      openusd_compat = true;  // re-emit deprecated qualifiers (e.g. `custom`)
    } else if (std::strcmp(argv[i], "--aousd-strict") == 0) {
      aousd_strict = true;
    } else if (std::strcmp(argv[i], "--require-prim") == 0 && i + 1 < argc) {
      required_prims.emplace_back(argv[++i]);
    } else if (std::strcmp(argv[i], "--instance-mode") == 0 && i + 1 < argc) {
      const char *m = argv[++i];
      if (std::strcmp(m, "native") == 0)
        inst_mode = pcp::InstanceFlattenMode::Native;
      else if (std::strcmp(m, "holder") == 0)
        inst_mode = pcp::InstanceFlattenMode::Holder;
      else if (std::strcmp(m, "prototypes") == 0)
        inst_mode = pcp::InstanceFlattenMode::ExtractedPrototypes;
      else {
        std::fprintf(stderr, "Unknown --instance-mode '%s' "
                             "(native|holder|prototypes)\n", m);
        return 2;
      }
    } else if (std::strcmp(argv[i], "--prototype-numbering") == 0 && i + 1 < argc) {
      const char *m = argv[++i];
      if (std::strcmp(m, "deterministic") == 0)
        proto_num = pcp::PrototypeNumbering::Deterministic;
      else if (std::strcmp(m, "usdcat") == 0)
        proto_num = pcp::PrototypeNumbering::UsdcatCompatible;
      else {
        std::fprintf(stderr, "Unknown --prototype-numbering '%s' "
                             "(deterministic|usdcat)\n", m);
        return 2;
      }
    } else if (std::strcmp(argv[i], "--compose-threads") == 0 && i + 1 < argc) {
      compose_threads = std::atoi(argv[++i]);  // -1/0 = auto, 1 = serial
      if (compose_threads == 0) {
        compose_threads = -1;
      } else if (compose_threads < -1) {
        std::fprintf(stderr,
                     "Invalid --compose-threads value '%d' (must be -1, 0 or >= 1)\n",
                     compose_threads);
        return 2;
      }
    } else if (std::strcmp(argv[i], "--write-threads") == 0 && i + 1 < argc) {
      write_threads = std::atoi(argv[++i]);  // 0 = auto, 1 = serial
      if (write_threads < 0) {
        std::fprintf(stderr,
                     "Invalid --write-threads value '%d' (must be 0 or >= 1)\n",
                     write_threads);
        return 2;
      }
    } else if (std::strcmp(argv[i], "--compose-threads-auto") == 0) {
      compose_threads = -1;
    } else if (std::strcmp(argv[i], "--no-consume-values") == 0) {
      consume_values = false;
    } else if (std::strcmp(argv[i], "--memory-profile") == 0 && i + 1 < argc) {
      const char* m = argv[++i];
      if (std::strcmp(m, "low") == 0) low_memory = true;
      else if (std::strcmp(m, "default") == 0) low_memory = false;
      else {
        std::fprintf(stderr, "ERR : --memory-profile must be default or low\n");
        return 2;
      }
    } else if (std::strcmp(argv[i], "--batched-compose") == 0 && i + 1 < argc) {
      batch_roots = std::atol(argv[++i]);
    } else if (std::strcmp(argv[i], "--batch-frontier") == 0 && i + 1 < argc) {
      batch_frontier = static_cast<size_t>(std::atol(argv[++i]));
    } else if (std::strcmp(argv[i], "--batch-prims") == 0 && i + 1 < argc) {
      batch_prims = static_cast<size_t>(std::atol(argv[++i]));
    } else if (std::strcmp(argv[i], "--fast-exit") == 0) {
      fast_exit = true;
    } else if (std::strcmp(argv[i], "--load-payloads") == 0) {
      load_payloads = true;
    } else if (std::strcmp(argv[i], "--defer-payloads") == 0) {
      load_payloads = false;
    } else if (std::strcmp(argv[i], "--variant-fallback") == 0 && i + 1 < argc) {
      std::string spec(argv[++i]);
      const size_t eq = spec.find('=');
      if (eq != std::string::npos && eq > 0) {
        std::vector<std::string> opts_list;
        std::string rest = spec.substr(eq + 1);
        size_t start = 0;
        while (start <= rest.size()) {
          const size_t comma = rest.find(',', start);
          const size_t end = comma == std::string::npos ? rest.size() : comma;
          if (end > start) opts_list.push_back(rest.substr(start, end - start));
          if (comma == std::string::npos) break;
          start = comma + 1;
        }
        if (!opts_list.empty()) {
          variant_fallbacks[spec.substr(0, eq)] = std::move(opts_list);
        }
      }
    } else {
      filename = argv[i];
    }
  }
  if (!filename) {
    std::fprintf(stderr, "Usage: lusdcat [-l|-f|--rewrite-layer] [-o out.usda] "
                         "[--instance-mode native|holder|prototypes] "
                         "[--prototype-numbering deterministic|usdcat] "
                         "[--compose-threads N] [--compose-threads-auto] "
                         "[--parse-threads N] [--write-threads N] "
                         "[--output-format usda|usdc] [--compose-only] "
                         "[--load-payloads|--defer-payloads] "
                         "[--no-consume-values] "
                         "[--memory-profile default|low] [--batch-prims N] "
                         "[--fast-exit] "
                         "[--aousd-strict] "
                         "[--max-crate-elements N] "
                         "[--max-memory-mib N] "
                         "[--no-async-arrays] [--no-parallel-prims] "
                         "[--require-prim /Path] "
                         "file.usd[acz]\n");
    return 2;
  }

  if (rewrite_layer && flatten) {
    std::fprintf(stderr, "--rewrite-layer and -f cannot be combined "
                         "(rewrite-layer is a compose-free parse->write).\n");
    return 2;
  }
  if (compose_only && (!flatten || out_path ||
                       output_format != OutputFormat::Auto)) {
    std::fprintf(stderr, "ERR : --compose-only cannot be combined with -l or output options\n");
    return 2;
  }
  const bool output_usdc = output_format == OutputFormat::USDC ||
      (output_format == OutputFormat::Auto && out_path && IsUSDCPath(out_path));
  if (output_usdc && (!out_path || (!flatten && !rewrite_layer))) {
    std::fprintf(stderr, "ERR : USDC output requires -o and -f or --rewrite-layer\n");
    return 2;
  }
  if (low_memory && !(flatten && output_usdc)) {
    std::fprintf(stderr, "WARN : --memory-profile low applies to -f with USDC "
                         "output; using the default profile\n");
  }
  if (parse_threads < 0) {
    const char* nt = std::getenv("LIGHTUSD_NEXT_NUM_THREADS");
    parse_threads = nt ? std::atoi(nt) : 0;
  }

  // Opt-in wall-clock attribution (load+compose vs write). Printed to stderr with
  // a non-"WARN/ERR : " prefix so the corpus categorizer ignores it; off unless
  // LIGHTUSD_NEXT_TIMING is set. Used to prioritize/measure perf work.
  const bool timing = std::getenv("LIGHTUSD_NEXT_TIMING") != nullptr;
  const auto set_crate_limits = [&](CrateLimits& limits, size_t& array_limit) {
    if (!max_crate_elements) return;
    limits.max_tokens = max_crate_elements;
    limits.max_strings = max_crate_elements;
    limits.max_fields = max_crate_elements;
    limits.max_fieldset_indices = max_crate_elements;
    limits.max_specs = max_crate_elements;
    limits.max_paths = max_crate_elements;
    array_limit = max_crate_elements;
  };
  if (timing) {
    // The next library emits its [next_build]/[next_warm]/[next_compose] timing
    // through lightusd::logging now (not a hardcoded stderr fprintf). Point the
    // logger at stderr and enable Info so those lines appear like before.
    lightusd::logging::Logger::getInstance().setStream(&std::cerr);
    lightusd::logging::Logger::getInstance().setLogLevel(
        lightusd::logging::LogLevel::Info);
  }
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
  };
  const auto t_start = Clock::now();

  // Compose-free RAW parse->write path (measure parser/writer in isolation).
  if (rewrite_layer) {
    std::string warn, err;
    // Parse-thread hint (large-array parallel parse): explicit arg to the library
    // (which no longer reads the environment itself). 0 = auto, 1 = serial.
    pcp::LayerLoadOptions load_opts;
    set_crate_limits(load_opts.usdc_limits, load_opts.max_array_elements);
    load_opts.parse_num_threads = parse_threads;
    load_opts.enable_timing = timing;
    load_opts.usda_parse_options.enable_timing = timing;
    load_opts.usda_parse_options.strict_aousd_conformance = aousd_strict;
    load_opts.usda_parse_options.async_arrays = async_arrays;
    load_opts.usda_parse_options.parallel_prims = parallel_prims;
    load_opts.strict_aousd_conformance = aousd_strict;
    // Benchmark path: flattened scenes exceed the default input cap. 0 keeps
    // only the unconditional 16 GiB readable-file ceiling.
    load_opts.max_memory = max_memory_bytes;
    auto layer = pcp::LoadLayerFromFile(filename, &warn, &err,
                                        load_opts);  // PARSE only
    const auto t_parsed = Clock::now();
    emit_lines(warn, "WARN : ");
    if (!layer) {
      emit_lines(err.empty() ? std::string("load failed") : err, "ERR : ");
      emit_crate_limit_hint(err, max_crate_elements);
      return 1;
    }
    if (!err.empty()) emit_lines(err, "WARN : ");

    if (output_usdc) {
      USDCWriteOptions options;
      const char* nt = std::getenv("LIGHTUSD_NEXT_NUM_THREADS");
      options.crate_options.num_threads = write_threads >= 0 ? write_threads
                                                           : (nt ? std::atoi(nt) : 0);
      options.crate_options.enable_timing = timing;
      auto res = WriteLayerToUSDCFile(out_path, *layer, options);
      const auto t_written = Clock::now();
      if (!res.success) {
        std::fprintf(stderr, "ERR : %s\n", res.error.c_str());
        return 1;
      }
      if (timing) {
        std::fprintf(stderr, "[lusdcat] parse=%.1fms write=%.1fms total=%.1fms\n",
                     ms(t_parsed - t_start), ms(t_written - t_parsed),
                     ms(t_written - t_start));
        std::fprintf(stderr, "[lusdcat] wrote %zu bytes (tokens=%zu paths=%zu specs=%zu)\n",
                     res.bytes_written, res.token_count, res.path_count, res.spec_count);
      }
      if (fast_exit) ExitAfterSuccessfulWrite();
      layer.reset();
      if (timing) std::fprintf(stderr, "[lusdcat] teardown=%.1fms\n",
                               ms(Clock::now() - t_written));
      return 0;
    }

    USDAWriteOptions wopts;
    wopts.enable_timing = timing;
    wopts.emit_custom = openusd_compat;
    wopts.num_threads = 0;  // auto; LIGHTUSD_NEXT_NUM_THREADS overrides (1 = serial)
    if (const char* nt = std::getenv("LIGHTUSD_NEXT_NUM_THREADS")) {
      wopts.num_threads = std::atoi(nt);
    }
    if (write_threads >= 0) wopts.num_threads = write_threads;
    std::FILE* fp = stdout;
    if (out_path) {
      fp = std::fopen(out_path, "wb");
      if (!fp) {
        std::fprintf(stderr, "ERR : cannot open output file: %s\n", out_path);
        return 1;
      }
    }
    USDAWriteResult res;
    {
      StreamWriter w(StdioSink(fp));
      res = WriteLayer(w, *layer, wopts);  // flushes before returning
    }
    const bool flush_ok = (std::fflush(fp) == 0);
    const bool close_ok = out_path ? (std::fclose(fp) == 0) : true;
    if (!res.success || !flush_ok || !close_ok) {
      std::fprintf(stderr, "ERR : %s\n",
                   res.error.empty() ? "write/flush failed (disk full?)"
                                     : res.error.c_str());
      return 1;
    }
    const auto t_written = Clock::now();
    if (timing) {
      const double pms = ms(t_parsed - t_start);
      const double wms = ms(t_written - t_parsed);
      std::fprintf(stderr,
                   "[lusdcat] parse=%.1fms write=%.1fms total=%.1fms\n",
                   pms, wms, ms(t_written - t_start));
      // RAW parse throughput is relative to the input file size.
      long insz = 0;
      if (std::FILE* inf = std::fopen(filename, "rb")) {
        std::fseek(inf, 0, SEEK_END);
        insz = std::ftell(inf);
        std::fclose(inf);
      }
      if (insz > 0 && pms > 0.0) {
        std::fprintf(stderr, "[lusdcat] parsed %ld bytes = %.0f MB/s\n",
                     insz, double(insz) / 1048576.0 / (pms / 1000.0));
      }
      if (res.bytes_written > 0 && wms > 0.0) {
        std::fprintf(stderr, "[lusdcat] wrote %zu bytes = %.0f MB/s%s%s\n",
                     res.bytes_written,
                     double(res.bytes_written) / 1048576.0 / (wms / 1000.0),
                     out_path ? " -> " : "", out_path ? out_path : "");
      }
    }
    if (fast_exit) ExitAfterSuccessfulWrite();
    layer.reset();
    if (timing) std::fprintf(stderr, "[lusdcat] teardown=%.1fms\n",
                             ms(Clock::now() - t_written));
    return 0;
  }

  std::string warn, err;
  Stage stage;
  bool ok = false;
  if (flatten) {
    AssetResolver resolver;
    // Anchor relative asset paths (references/payloads/sublayers) to the input
    // file's directory, like usdcat/lusdcat.
    {
      std::string fn(filename);
      auto slash = fn.find_last_of("/\\");
      if (slash != std::string::npos) {
        resolver.SetWorkingDirectory(fn.substr(0, slash));
      }
    }
    pcp::CompositionOptions opts;
    set_crate_limits(opts.usdc_limits, opts.max_array_elements);
    if (max_memory_bytes) opts.max_layer_memory = max_memory_bytes;
    opts.strict_aousd_conformance = aousd_strict;
    if (!variant_fallbacks.empty()) opts.variant_fallbacks = variant_fallbacks;
    opts.usda_parse_options.strict_aousd_conformance = aousd_strict;
    opts.usda_parse_options.async_arrays = async_arrays;
    opts.usda_parse_options.parallel_prims = parallel_prims;
    // USDA parse-thread hint for every layer composition loads (0 = auto,
    // 1 = serial parse); same env knob as the --rewrite-layer path.
    opts.usda_parse_options.num_threads = parse_threads;
    opts.instance_flatten_mode = inst_mode;  // default Holder (self-contained)
#if defined(LIGHTUSD_NEXT_DEFERRED_FILL)
    // Experimental: the stage is written once to USDC, so compose each prim's
    // opinions on demand in the writer instead of up front.
    opts.defer_opinion_fill = flatten && output_usdc;
#endif
    opts.prototype_numbering = proto_num;
    // Parallel compose is byte-identical to serial. Default -1 = auto
    // (hardware concurrency); --compose-threads 1 = serial.
    opts.num_threads = compose_threads;
    opts.load_payloads = load_payloads;
    // Forward the CLI timing flag to the library (which no longer reads the env):
    // gates the [next_compose]/[next_build]/[next_warm] diagnostics.
    opts.enable_timing = timing;
    // The stage is written right after composing: free the composition cache
    // while the writer starts.
    opts.background_reclaim = true;
    // ... and its source layers as soon as the opinion fill is past them.
    opts.release_layers_during_fill = true;
    if (low_memory && output_usdc) {
      pcp::BatchedComposeOptions bopts;
      bopts.max_frontier = batch_frontier;
      bopts.target_batch_prims = batch_prims;
      if (batch_roots > 0) bopts.roots_per_batch = static_cast<size_t>(batch_roots);
      CrateWriteOptions wopts;
      wopts.num_threads = 0;
      if (const char* nt = std::getenv("LIGHTUSD_NEXT_NUM_THREADS"))
        wopts.num_threads = std::atoi(nt);
      if (write_threads >= 0) wopts.num_threads = write_threads;
      wopts.enable_timing = timing;
      wopts.consume_values = consume_values;
      wopts.consume_prims = consume_values;
      CrateWriteResult res;
      const bool flat_ok = pcp::FlattenToUSDCFileBatched(
          filename, resolver, out_path, opts, bopts, wopts, &res, &warn, &err);
      emit_lines(warn, "WARN : ");
      emit_crate_limit_hint(warn, max_crate_elements);
      if (!flat_ok) {
        if (!res.error.empty()) emit_lines(res.error, "ERR : ");
        emit_lines(err.empty() ? std::string("low-memory flatten failed") : err,
                   "ERR : ");
        return 1;
      }
      if (!err.empty()) emit_lines(err, "WARN : ");
      if (timing) {
        std::fprintf(stderr,
                     "[lusdcat] flatten(low-memory)=%.1fms wrote %zu bytes "
                     "(tokens=%zu paths=%zu specs=%zu)\n",
                     ms(Clock::now() - t_start), res.bytes_written,
                     res.token_count, res.path_count, res.spec_count);
      }
      if (fast_exit) ExitAfterSuccessfulWrite();
      return 0;
    }
    if (batch_roots >= 0) {
      pcp::BatchedComposeOptions bopts;
      bopts.max_frontier = batch_frontier;
      bopts.target_batch_prims = batch_prims;
      bopts.roots_per_batch = static_cast<size_t>(batch_roots);
      ok = pcp::ComposeStageBatched(filename, resolver, &stage, opts, bopts,
                                    &warn, &err);
    } else {
      ok = pcp::ComposeStageFromFile(filename, resolver, &stage, opts, &warn, &err);
    }
  } else {
    LoadUSDOptions load_opts;
    set_crate_limits(load_opts.usdc_options.crate_options,
                     load_opts.usdc_options.crate_options.max_array_elements);
    if (max_crate_elements) load_opts.limits.max_array_elements = max_crate_elements;
    if (max_memory_bytes) {
      load_opts.limits.max_input_bytes = max_memory_bytes;
      load_opts.limits.max_asset_bytes = max_memory_bytes;
      load_opts.limits.max_resident_bytes = max_memory_bytes;
      load_opts.usdc_options.crate_options.max_memory = max_memory_bytes;
      load_opts.usda_options.parse_options.max_file_size = max_memory_bytes;
    }
    load_opts.input_policy = aousd_strict ? InputPolicy::Untrusted
                                          : InputPolicy::Trusted;
    load_opts.usda_options.parse_options.num_threads = parse_threads;
    load_opts.usda_options.parse_options.enable_timing = timing;
    load_opts.usda_options.parse_options.async_arrays = async_arrays;
    load_opts.usda_options.parse_options.parallel_prims = parallel_prims;
    load_opts.usdc_options.crate_options.num_threads = parse_threads;
    load_opts.usdc_options.crate_options.enable_timing = timing;
    ok = LoadUSD(filename, &stage, load_opts, &warn, &err);
  }
  const auto t_loaded = Clock::now();

  emit_lines(warn, "WARN : ");
  // Composition reports a layer the limits refused as a warning and goes on.
  emit_crate_limit_hint(warn, max_crate_elements);
  if (!ok) {
    emit_lines(err.empty() ? std::string("load failed") : err, "ERR : ");
    emit_crate_limit_hint(err, max_crate_elements);
    return 1;
  }
  for (const std::string& path : required_prims) {
    if (!stage.GetPrimAtPath(path).IsValid()) {
      std::fprintf(stderr, "ERR : required composed prim is missing: %s\n",
                   path.c_str());
      return 1;
    }
  }
  // Composition errors are accumulated non-fatally; surface them as warnings
  // in flatten mode (the file still loaded).
  if (!err.empty()) emit_lines(err, "WARN : ");

  if (compose_only) {
    std::printf("composed: %zu prims\n", stage.GetStats().prim_count);
    if (timing) std::fprintf(stderr, "[lusdcat] load+compose=%.1fms\n",
                             ms(t_loaded - t_start));
    pcp::WaitForBackgroundReclaim();
    stage = Stage();
    if (timing) std::fprintf(stderr, "[lusdcat] teardown=%.1fms\n",
                             ms(Clock::now() - t_loaded));
    return 0;
  }

  // USDC output: when -o targets a .usdc/.usdz path, write the composed stage as
  // a binary crate (benchmark vehicle for the next crate writer).
  if (flatten && output_usdc) {
    USDCWriteOptions copts;
    copts.crate_options.num_threads = 0;  // auto; LIGHTUSD_NEXT_NUM_THREADS overrides
    copts.crate_options.enable_timing = timing;  // [next_crate_write] phases
    // The composed stage is discarded right after this write: let the writer
    // release each prim's values once they are encoded (byte-identical).
    copts.crate_options.consume_values = consume_values;
    copts.crate_options.consume_prims = consume_values;
    if (const char* nt = std::getenv("LIGHTUSD_NEXT_NUM_THREADS")) {
      copts.crate_options.num_threads = std::atoi(nt);
    }
    if (write_threads >= 0) copts.crate_options.num_threads = write_threads;
    const auto t_w0 = Clock::now();
    // The writer's tables (~6 GB on a full-LOD stage) outlive the write
    // here: --fast-exit leaves them to process exit, otherwise they are freed
    // with the stage teardown below.
    std::unique_ptr<CrateWriter> writer(new CrateWriter(copts.crate_options));
    const CrateWriteResult res = writer->WriteToFile(out_path, stage);
    const auto t_written = Clock::now();
    if (!res.success) {
      std::fprintf(stderr, "ERR : %s\n",
                   res.error.empty() ? "usdc write failed" : res.error.c_str());
      return 1;
    }
    if (timing) {
      const double wms = ms(t_written - t_w0);
      std::fprintf(stderr,
                   "[lusdcat] load+compose=%.1fms write=%.1fms total=%.1fms\n",
                   ms(t_loaded - t_start), wms, ms(t_written - t_start));
      if (res.bytes_written > 0 && wms > 0.0) {
        std::fprintf(stderr,
                     "[lusdcat] wrote %zu bytes = %.0f MB/s -> %s "
                     "(tokens=%zu paths=%zu specs=%zu)\n",
                     res.bytes_written,
                     double(res.bytes_written) / 1048576.0 / (wms / 1000.0),
                     out_path, res.token_count, res.path_count, res.spec_count);
      }
    }
    if (fast_exit) ExitAfterSuccessfulWrite();
    writer.reset();
    pcp::WaitForBackgroundReclaim();
    stage = Stage();
    if (timing) std::fprintf(stderr, "[lusdcat] teardown=%.1fms\n",
                             ms(Clock::now() - t_written));
    return 0;
  }

  if (flatten) {
    USDAWriteOptions wopts;
    wopts.enable_timing = timing;
    // A composed stage carries `custom` on user attributes exactly like pxr's
    // `usdcat --flatten` (which always emits it). `--openusd-compat` additionally
    // forces it on the non-flatten/layer path.
    wopts.emit_custom = true;
    // The stage came from pcp composition: emit composed-stage semantics
    // (consumed subLayers/relocates dropped, apiSchemas composed to explicit).
    wopts.composed_stage_output = true;
    // Parallel subtree serialization (only effective in a LIGHTUSD_ENABLE_THREAD
    // build). Default auto (= hardware_concurrency); LIGHTUSD_NEXT_NUM_THREADS
    // overrides (1 = serial). Output is byte-identical regardless of count.
    wopts.num_threads = 0;  // auto
    if (const char* nt = std::getenv("LIGHTUSD_NEXT_NUM_THREADS")) {
      wopts.num_threads = std::atoi(nt);
    }
    if (write_threads >= 0) wopts.num_threads = write_threads;
    // Write through the next StreamWriter with the native C-stdio backend
    // (buffered + blocked writes). `-o <file>` targets a FILE*; otherwise stdout.
    // This is the default native sink; a WASM/WASI host would supply its own
    // StreamWriter::BlockSink instead.
    std::FILE* fp = stdout;
    if (out_path) {
      fp = std::fopen(out_path, "wb");
      if (!fp) {
        std::fprintf(stderr, "ERR : cannot open output file: %s\n", out_path);
        return 1;
      }
    }
    USDAWriteResult res;
    {
      StreamWriter w(StdioSink(fp));
      res = WriteUSDA(w, stage, wopts);  // flushes the buffer before returning
    }
    const bool flush_ok = (std::fflush(fp) == 0);
    const bool close_ok = out_path ? (std::fclose(fp) == 0) : true;
    if (!res.success || !flush_ok || !close_ok) {
      std::fprintf(stderr, "ERR : %s\n",
                   res.error.empty() ? "write/flush failed (disk full?)"
                                     : res.error.c_str());
      return 1;
    }
    size_t bytes_written = res.bytes_written;
    const auto t_written = Clock::now();
    if (timing) {
      const double wms = ms(t_written - t_loaded);
      std::fprintf(stderr,
                   "[lusdcat] load+compose=%.1fms write=%.1fms total=%.1fms\n",
                   ms(t_loaded - t_start), wms, ms(t_written - t_start));
      if (bytes_written > 0 && wms > 0.0) {
        std::fprintf(stderr,
                     "[lusdcat] wrote %zu bytes = %.0f MB/s%s%s\n",
                     bytes_written,
                     double(bytes_written) / 1048576.0 / (wms / 1000.0),
                     out_path ? " -> " : "", out_path ? out_path : "");
      }
    }
  } else {
    std::printf("loaded: %zu prims\n", stage.GetStats().prim_count);
    if (timing) {
      std::fprintf(stderr, "[lusdcat] load=%.1fms\n", ms(t_loaded - t_start));
    }
  }
  if (flatten && fast_exit) ExitAfterSuccessfulWrite();
  const auto t_teardown = Clock::now();
  pcp::WaitForBackgroundReclaim();
  stage = Stage();
  if (timing) std::fprintf(stderr, "[lusdcat] teardown=%.1fms\n",
                           ms(Clock::now() - t_teardown));
  return 0;
}
