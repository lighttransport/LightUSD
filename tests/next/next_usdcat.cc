// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD Next - next_usdcat
//
// Minimal lusdcat-compatible CLI for the src/next module, so the usd-wg
// asset-corpus regression gate (tests/parse-asset-corpus.mjs) can drive next
// the same way it drives the legacy loader:
//   next_usdcat [-l|-f] file.usd[acz]
//     -l  load only (parse the single layer; default)
//     -f  flatten (compose sublayers/references/payloads/... via pcp, print USDA)
// Contract: exit 0 = loaded; diagnostics on stderr prefixed "WARN : " / "ERR : ".

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "logger.hh"  // lightusd::logging (next routes diagnostics through it)
#include "next/pcp/cache.hh"
#include "next/pcp/layer-registry.hh"
#include "next/resolver/asset-resolver.hh"
#include "next/stage/stage.hh"
#include "next/lightusd-next.hh"
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

int main(int argc, char **argv) {
  bool flatten = false;
  // Compose-free parse->write: LoadLayerFromFile (parse only, no composition) ->
  // WriteLayer. Measures RAW parse and RAW write throughput in isolation, and is
  // an idempotent parse-fidelity oracle (rewriting its own output is byte-identical).
  bool rewrite_layer = false;
  bool openusd_compat = false;
  bool aousd_strict = false;
  // Default instance flatten = holder (the historical -f behavior). `native`
  // keeps instancing; `prototypes` = usdcat-style /Flattened_Prototype_N.
  pcp::InstanceFlattenMode inst_mode = pcp::InstanceFlattenMode::Holder;
  pcp::PrototypeNumbering proto_num = pcp::PrototypeNumbering::Deterministic;
  // Parallel composition is OPT-IN via --compose-threads N (default 1 = serial,
  // no threading). -1 means auto = hardware concurrency.
  // It is byte-identical to serial; it helps small compose-bound scenes and
  // currently regresses huge instanced ones, so it stays off by default.
  // (Independent of the writer's LIGHTUSD_NEXT_NUM_THREADS.)
  int compose_threads = 1;
  bool load_payloads = true;
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
      compose_threads = std::atoi(argv[++i]);  // opt-in parallel compose (>1)
      if (compose_threads == 0) {
        compose_threads = 1;
      } else if (compose_threads < -1) {
        std::fprintf(stderr,
                     "Invalid --compose-threads value '%d' (must be -1 or >= 1)\n",
                     compose_threads);
        return 2;
      }
    } else if (std::strcmp(argv[i], "--compose-threads-auto") == 0) {
      compose_threads = -1;
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
    std::fprintf(stderr, "Usage: next_usdcat [-l|-f|--rewrite-layer] [-o out.usda] "
                         "[--instance-mode native|holder|prototypes] "
                         "[--prototype-numbering deterministic|usdcat] "
                         "[--compose-threads N] [--compose-threads-auto] "
                         "[--load-payloads|--defer-payloads] "
                         "[--aousd-strict] "
                         "[--require-prim /Path] "
                         "file.usd[acz]\n");
    return 2;
  }

  if (rewrite_layer && flatten) {
    std::fprintf(stderr, "--rewrite-layer and -f cannot be combined "
                         "(rewrite-layer is a compose-free parse->write).\n");
    return 2;
  }

  // Opt-in wall-clock attribution (load+compose vs write). Printed to stderr with
  // a non-"WARN/ERR : " prefix so the corpus categorizer ignores it; off unless
  // LIGHTUSD_NEXT_TIMING is set. Used to prioritize/measure perf work.
  const bool timing = std::getenv("LIGHTUSD_NEXT_TIMING") != nullptr;
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
    int parse_threads = 0;
    if (const char* nt = std::getenv("LIGHTUSD_NEXT_NUM_THREADS")) {
      parse_threads = std::atoi(nt);
    }
    pcp::LayerLoadOptions load_opts;
    load_opts.parse_num_threads = parse_threads;
    load_opts.usda_parse_options.strict_aousd_conformance = aousd_strict;
    load_opts.strict_aousd_conformance = aousd_strict;
    auto layer = pcp::LoadLayerFromFile(filename, &warn, &err,
                                        load_opts);  // PARSE only
    const auto t_parsed = Clock::now();
    emit_lines(warn, "WARN : ");
    if (!layer) {
      emit_lines(err.empty() ? std::string("load failed") : err, "ERR : ");
      return 1;
    }
    if (!err.empty()) emit_lines(err, "WARN : ");

    USDAWriteOptions wopts;
    wopts.emit_custom = openusd_compat;
    wopts.num_threads = 0;  // auto; LIGHTUSD_NEXT_NUM_THREADS overrides (1 = serial)
    if (const char* nt = std::getenv("LIGHTUSD_NEXT_NUM_THREADS")) {
      wopts.num_threads = std::atoi(nt);
    }
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
                   "[next_usdcat] parse=%.1fms write=%.1fms total=%.1fms\n",
                   pms, wms, ms(t_written - t_start));
      // RAW parse throughput is relative to the input file size.
      long insz = 0;
      if (std::FILE* inf = std::fopen(filename, "rb")) {
        std::fseek(inf, 0, SEEK_END);
        insz = std::ftell(inf);
        std::fclose(inf);
      }
      if (insz > 0 && pms > 0.0) {
        std::fprintf(stderr, "[next_usdcat] parsed %ld bytes = %.0f MB/s\n",
                     insz, double(insz) / 1048576.0 / (pms / 1000.0));
      }
      if (res.bytes_written > 0 && wms > 0.0) {
        std::fprintf(stderr, "[next_usdcat] wrote %zu bytes = %.0f MB/s%s%s\n",
                     res.bytes_written,
                     double(res.bytes_written) / 1048576.0 / (wms / 1000.0),
                     out_path ? " -> " : "", out_path ? out_path : "");
      }
    }
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
    opts.strict_aousd_conformance = aousd_strict;
    if (!variant_fallbacks.empty()) opts.variant_fallbacks = variant_fallbacks;
    opts.usda_parse_options.strict_aousd_conformance = aousd_strict;
    opts.instance_flatten_mode = inst_mode;  // default Holder (self-contained)
    opts.prototype_numbering = proto_num;
    // Parallel compose (pre-warm sources_cache) is OPT-IN via --compose-threads N
    // and byte-identical to serial. Default 1 = serial (no threading),
    // -1 = auto hardware concurrency.
    opts.num_threads = compose_threads;
    opts.load_payloads = load_payloads;
    // Forward the CLI timing flag to the library (which no longer reads the env):
    // gates the [next_compose]/[next_build]/[next_warm] diagnostics.
    opts.enable_timing = timing;
    ok = pcp::ComposeStageFromFile(filename, resolver, &stage, opts, &warn, &err);
  } else {
    LoadUSDOptions load_opts;
    load_opts.input_policy = aousd_strict ? InputPolicy::Untrusted
                                          : InputPolicy::Trusted;
    ok = LoadUSD(filename, &stage, load_opts, &warn, &err);
  }
  const auto t_loaded = Clock::now();

  emit_lines(warn, "WARN : ");
  if (!ok) {
    emit_lines(err.empty() ? std::string("load failed") : err, "ERR : ");
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

  // USDC output: when -o targets a .usdc/.usdz path, write the composed stage as
  // a binary crate (benchmark vehicle for the next crate writer).
  if (flatten && out_path && IsUSDCPath(out_path)) {
    USDCWriteOptions copts;
    copts.crate_options.num_threads = 0;  // auto; LIGHTUSD_NEXT_NUM_THREADS overrides
    if (const char* nt = std::getenv("LIGHTUSD_NEXT_NUM_THREADS")) {
      copts.crate_options.num_threads = std::atoi(nt);
    }
    const auto t_w0 = Clock::now();
    USDCWriteResult res = WriteUSDCToFile(out_path, stage, copts);
    const auto t_written = Clock::now();
    if (!res.success) {
      std::fprintf(stderr, "ERR : %s\n",
                   res.error.empty() ? "usdc write failed" : res.error.c_str());
      return 1;
    }
    if (timing) {
      const double wms = ms(t_written - t_w0);
      std::fprintf(stderr,
                   "[next_usdcat] load+compose=%.1fms write=%.1fms total=%.1fms\n",
                   ms(t_loaded - t_start), wms, ms(t_written - t_start));
      if (res.bytes_written > 0 && wms > 0.0) {
        std::fprintf(stderr,
                     "[next_usdcat] wrote %zu bytes = %.0f MB/s -> %s "
                     "(tokens=%zu paths=%zu specs=%zu)\n",
                     res.bytes_written,
                     double(res.bytes_written) / 1048576.0 / (wms / 1000.0),
                     out_path, res.token_count, res.path_count, res.spec_count);
      }
    }
    return 0;
  }

  if (flatten) {
    USDAWriteOptions wopts;
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
                   "[next_usdcat] load+compose=%.1fms write=%.1fms total=%.1fms\n",
                   ms(t_loaded - t_start), wms, ms(t_written - t_start));
      if (bytes_written > 0 && wms > 0.0) {
        std::fprintf(stderr,
                     "[next_usdcat] wrote %zu bytes = %.0f MB/s%s%s\n",
                     bytes_written,
                     double(bytes_written) / 1048576.0 / (wms / 1000.0),
                     out_path ? " -> " : "", out_path ? out_path : "");
      }
    }
  } else {
    std::printf("loaded: %zu prims\n", stage.GetStats().prim_count);
    if (timing) {
      std::fprintf(stderr, "[next_usdcat] load=%.1fms\n", ms(t_loaded - t_start));
    }
  }
  return 0;
}
