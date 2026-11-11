// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
//
// LightUSD - Lazy array memory benchmark
//
// Demonstrates the composition memory win of lazy ValueRefs: cloning a prim K
// times (as composition does per reference/payload) deep-copies every array in
// the eager path but only bumps a shared_ptr in the lazy path.
//
// Usage:
//   bench_lazy_mem gen   <file.usdc> [numVerts]
//   bench_lazy_mem eager <file.usdc> [K]
//   bench_lazy_mem lazy  <file.usdc> [K]
//
// Each measured run reads from file + clones K times, then prints peak RSS.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include "crate/crate-reader.hh"
#include "crate/crate-writer.hh"
#include "layer/layer.hh"
#include "pipeline/flatten.hh"
#include "stage/stage.hh"
#include "types/value.hh"

using namespace lightusd;

static long peak_rss_kb() {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;  // kilobytes on Linux
}

// Current resident set size (KB) from /proc/self/statm.
static long cur_rss_kb() {
  std::ifstream f("/proc/self/statm");
  long total = 0, resident = 0;
  if (f >> total >> resident) return resident * (sysconf(_SC_PAGESIZE) / 1024);
  return 0;
}

static bool read_file(const char* path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return false;
  out.resize(static_cast<size_t>(f.tellg()));
  f.seekg(0);
  return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()),
                                  static_cast<std::streamsize>(out.size())));
}

static int do_gen(const char* path, size_t num_verts) {
  std::vector<float> points(num_verts * 3);
  for (size_t i = 0; i < points.size(); i++) points[i] = static_cast<float>(i) * 0.25f;
  std::vector<int32_t> indices(num_verts);
  for (size_t i = 0; i < num_verts; i++) indices[i] = static_cast<int32_t>(i);

  StageBuilder sb;
  sb.SetDefaultPrim("BigMesh");
  LayerBuilder& lb = sb.GetLayerBuilder();
  lb.begin_prim("BigMesh", "Mesh");
  lb.add_property("points", Value::MakeFloat3Array(std::move(points)));
  lb.add_property("faceVertexIndices", Value::MakeIntArray(std::move(indices)));
  lb.end_prim();
  lb.finalize();
  Stage stage = sb.Build();

  CrateWriter w;
  std::vector<uint8_t> buf;
  CrateWriteResult r = w.WriteLayerToMemory(buf, *stage.GetRootLayer());
  if (!r.success) {
    std::cerr << "gen: write failed\n";
    return 1;
  }
  std::ofstream of(path, std::ios::binary);
  of.write(reinterpret_cast<const char*>(buf.data()),
           static_cast<std::streamsize>(buf.size()));
  std::cout << "gen: wrote " << buf.size() << " bytes (" << num_verts
            << " verts) to " << path << "\n";
  return 0;
}

// Build a layer of N prims in a deep chain (mimics deeply-nested real scenes),
// write it via CrateWriter, then re-read to check prim count survives + measure
// write time/RSS. Isolates the writer's path-encoding scalability/correctness.
static int do_genmany(int n, int chain_depth) {
  StageBuilder sb;
  LayerBuilder& lb = sb.GetLayerBuilder();
  int depth = 0;
  for (int i = 0; i < n; i++) {
    lb.begin_prim("p" + std::to_string(i), "Xform");
    lb.add_property("visibility", Value::MakeToken("inherited"));
    depth++;
    if (depth >= chain_depth) {  // close the chain, start a new sibling subtree
      while (depth > 0) { lb.end_prim(); depth--; }
    }
  }
  while (depth > 0) { lb.end_prim(); depth--; }
  lb.finalize();
  Stage stage = sb.Build();
  size_t built = stage.GetRootLayer()->prim_count();
  long rss_after_build = cur_rss_kb();

  CrateWriter w;
  std::vector<uint8_t> buf;
  CrateWriteResult r = w.WriteLayerToMemory(buf, *stage.GetRootLayer());
  long rss_after_write = cur_rss_kb();
  if (!r.success) { std::cerr << "genmany write failed\n"; return 1; }
  std::cout << "  rss after build=" << rss_after_build
            << " KB, after write=" << rss_after_write << " KB\n";

  // Re-read and count.
  size_t reread = 0;
  CrateReadOptions o; CrateReader rd(o);
  CrateReadResult rr = rd.Read(buf.data(), buf.size());
  if (rr.success) reread = rr.stage.GetRootLayer()->prim_count();

  std::cout << "genmany n=" << n << " chain=" << chain_depth
            << " built_prims=" << built << " out=" << buf.size()
            << " reread_prims=" << reread
            << " peak_rss=" << peak_rss_kb() << " KB"
            << (reread == built ? "  OK" : "  *** PRIM LOSS ***") << "\n";
  return 0;
}

static int do_measure(const char* path, bool lazy, int K) {
  std::vector<uint8_t> bytes;
  if (!read_file(path, bytes)) {
    std::cerr << "measure: cannot read " << path << "\n";
    return 1;
  }

  CrateReadOptions opts;
  opts.lazy_arrays = lazy;
  CrateReader reader(opts);
  CrateReadResult res = reader.Read(bytes.data(), bytes.size());
  if (!res.success) {
    std::cerr << "measure: parse failed\n";
    return 1;
  }
  const Layer* layer = res.stage.GetRootLayer();
  const PrimSpec* ps = layer->prim_at_path("/BigMesh");
  if (!ps) {
    std::cerr << "measure: /BigMesh not found\n";
    return 1;
  }

  // Clone K times — this is what composition does per reference/payload arc.
  std::vector<PrimSpec> clones;
  clones.reserve(static_cast<size_t>(K));
  size_t sink = 0;
  for (int i = 0; i < K; i++) {
    clones.push_back(ps->Clone());
    sink += clones.back().child_count();  // touch to defeat DCE
  }

  long rss = peak_rss_kb();
  std::cout << (lazy ? "lazy " : "eager")
            << "  K=" << K << "  peak_rss=" << rss << " KB"
            << "  (input=" << bytes.size() / 1024 << " KB, clones=" << clones.size()
            << ", sink=" << sink << ")\n";
  return 0;
}

// End-to-end facade: read -> flatten -> write, measuring full-pipeline peak RSS.
// `owned` uses the move-in single-copy input path.
static int do_flatten(const char* path, bool lazy, bool owned) {
  pipeline::FlattenOptions opts;
  opts.read.lazy_arrays = lazy;
  opts.flatten = (getenv("NOFLAT") == nullptr);  // NOFLAT=1 => read+write only
  pipeline::FlattenStats stats;
  std::vector<uint8_t> out;
  std::string err;
  bool ok;
  if (owned) {
    // Read straight into a std::string and hand ownership to the reader.
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { std::cerr << "flatten: cannot read " << path << "\n"; return 1; }
    std::string s(static_cast<size_t>(f.tellg()), '\0');
    f.seekg(0);
    f.read(&s[0], static_cast<std::streamsize>(s.size()));
    ok = pipeline::FlattenUSDCToUSDCOwned(std::move(s), out, opts, &stats, &err);
  } else {
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes)) {
      std::cerr << "flatten: cannot read " << path << "\n";
      return 1;
    }
    ok = pipeline::FlattenUSDCToUSDC(bytes.data(), bytes.size(), out, opts, &stats, &err);
  }
  if (!ok) {
    std::cerr << "flatten failed: " << err << "\n";
    return 1;
  }
  long rss = peak_rss_kb();
  if (const char* of = getenv("OUTFILE")) {
    std::ofstream o(of, std::ios::binary);
    o.write(reinterpret_cast<const char*>(out.data()),
            static_cast<std::streamsize>(out.size()));
  }
  // Re-read the flattened output to confirm it is valid + count prims/props.
  size_t out_prims = 0, out_props = 0, out_rels = 0, out_conns = 0;
  {
    CrateReadOptions o2; o2.lazy_arrays = true;
    CrateReader r2(o2);
    CrateReadResult rr2 = r2.Read(out.data(), out.size());
    if (rr2.success) {
      const Layer* l2 = rr2.stage.GetRootLayer();
      out_prims = l2->prim_count();
      for (size_t i = 0; i < l2->prim_count(); i++) {
        const PrimSpec* pp = l2->prim(static_cast<uint32_t>(i));
        out_props += pp->properties().slots().size();
        for (const auto& s : pp->properties().slots())
          if (s.flags & PropSlot::kFlagConnection) out_conns++;
        out_rels += pp->relationship_names().size();
      }
    }
  }
  std::cout << (lazy ? (owned ? "flat-lazy-owned" : "flat-lazy      ")
                     : "flat-eager     ")
            << "  peak_rss=" << rss << " KB  in=" << stats.input_bytes / 1024
            << " KB out=" << stats.output_bytes / 1024
            << " KB passthrough=" << stats.arrays_passed_through
            << " reencoded=" << stats.arrays_reencoded
            << "  [reread prims=" << out_prims << " props=" << out_props
            << " conns=" << out_conns << " rels=" << out_rels << "]\n";
  return 0;
}

static int do_diag(const char* path) {
  std::vector<uint8_t> bytes;
  if (!read_file(path, bytes)) { std::cerr << "diag: cannot read\n"; return 1; }
  CrateReadOptions opts; opts.lazy_arrays = true;
  CrateReader reader(opts);
  CrateReadResult res = reader.Read(bytes.data(), bytes.size());
  if (!res.success) { std::cerr << "diag: parse failed\n"; return 1; }
  const Layer* layer = res.stage.GetRootLayer();
  PropNameTable& nt = GetPropNameTable();
  std::cout << "loaded layer prim_count=" << layer->prim_count() << "\n";
  // Summary: how many prims actually carry properties / are meshes.
  size_t with_props = 0, meshes = 0, shown = 0;
  size_t total_props = 0, total_rels = 0, total_conns = 0, total_rel_targets = 0;
  for (size_t i = 0; i < layer->prim_count(); i++) {
    const PrimSpec* p = layer->prim(static_cast<uint32_t>(i));
    total_props += p->properties().slots().size();
    for (const auto& rn : p->relationship_names()) {
      total_rels++;
      if (const auto* t = p->relationship(rn)) total_rel_targets += t->size();
    }
    for (const auto& s : p->properties().slots()) {
      if (s.flags & PropSlot::kFlagConnection) total_conns++;
    }
    if (p->properties().slots().size() > 0) {
      with_props++;
      if (shown < 6) {
        std::cout << "  with-props [" << i << "] path='" << p->path().str()
                  << "' type='" << p->type_name() << "' props="
                  << p->properties().slots().size() << " [";
        for (const auto& s : p->properties().slots()) std::cout << nt.get(s.name_id) << " ";
        std::cout << "]\n";
        shown++;
      }
    }
    if (p->type_name() == "Mesh") meshes++;
  }
  std::cout << "prims with props=" << with_props << "  Mesh prims=" << meshes << "\n";
  std::cout << "IN-MEMORY model: total_props(slots)=" << total_props
            << " of which connections=" << total_conns
            << " | relationships=" << total_rels
            << " (targets=" << total_rel_targets << ")\n";
  // Spec-type breakdown (raw crate specs) + sample attribute paths.
  const auto& specs = reader.specs();
  const auto& paths = reader.paths();
  const auto& fields = reader.fields();
  const auto& tokens = reader.tokens();
  // Full spec-type histogram (12 SpecType enum values).
  size_t spec_hist[12] = {0};
  // For Attribute/Relationship specs, count which key fields appear.
  size_t attr_with_default = 0, attr_with_conn = 0, attr_only_conn = 0;
  size_t rel_with_targets = 0, rel_no_targets = 0;
  size_t n_prim = 0, n_attr = 0, n_other = 0, attr_shown = 0, prim_shown = 0,
         rel_shown = 0, conn_shown = 0;
  auto fieldset_has = [&](uint32_t fs_idx, const char* fname) -> bool {
    const auto& fsi = reader.fieldset_indices();
    for (size_t k = fs_idx; k < fsi.size(); ++k) {
      uint32_t fi = fsi[k];
      if (fi == 0xFFFFFFFF) break;
      if (fi < fields.size()) {
        uint32_t ti = fields[fi].token_index.value;
        if (ti < tokens.size() && tokens[ti] == fname) return true;
      }
    }
    return false;
  };
  auto fieldset_dump = [&](uint32_t fs_idx) -> std::string {
    const auto& fsi = reader.fieldset_indices();
    std::string s;
    for (size_t k = fs_idx; k < fsi.size(); ++k) {
      uint32_t fi = fsi[k];
      if (fi == 0xFFFFFFFF) break;
      if (fi < fields.size()) {
        uint32_t ti = fields[fi].token_index.value;
        if (ti < tokens.size()) { s += tokens[ti]; s += " "; }
      }
    }
    return s;
  };
  for (const auto& sp : specs) {
    uint32_t st = static_cast<uint32_t>(sp.spec_type);
    if (st < 12) spec_hist[st]++;
    if (sp.spec_type == SpecType::Prim) {
      n_prim++;
      if (prim_shown < 4 && sp.path_index.value < paths.size()) {
        std::cout << "  prim-spec raw path='" << paths[sp.path_index.value] << "'\n";
        prim_shown++;
      }
    } else if (sp.spec_type == SpecType::Attribute) {
      n_attr++;
      bool has_def = fieldset_has(sp.fieldset_index.value, "default");
      bool has_conn = fieldset_has(sp.fieldset_index.value, "connectionPaths") ||
                      fieldset_has(sp.fieldset_index.value, "connectionChildren");
      if (has_def) attr_with_default++;
      if (has_conn) attr_with_conn++;
      if (has_conn && !has_def) attr_only_conn++;
      if (has_conn && attr_shown < 6 && sp.path_index.value < paths.size()) {
        std::cout << "  conn-attr path='" << paths[sp.path_index.value]
                  << "' fields=[" << fieldset_dump(sp.fieldset_index.value) << "]\n";
        attr_shown++;
      }
    } else if (sp.spec_type == SpecType::Relationship) {
      bool has_tgt = fieldset_has(sp.fieldset_index.value, "targetPaths") ||
                     fieldset_has(sp.fieldset_index.value, "targetChildren");
      if (has_tgt) rel_with_targets++; else rel_no_targets++;
      if (rel_shown < 6 && sp.path_index.value < paths.size()) {
        std::cout << "  rel-spec path='" << paths[sp.path_index.value]
                  << "' fields=[" << fieldset_dump(sp.fieldset_index.value) << "]\n";
        rel_shown++;
      }
      n_other++;
    } else {
      if (sp.spec_type == SpecType::Connection && conn_shown < 4 &&
          sp.path_index.value < paths.size()) {
        std::cout << "  conn-spec path='" << paths[sp.path_index.value]
                  << "' fields=[" << fieldset_dump(sp.fieldset_index.value) << "]\n";
        conn_shown++;
      }
      n_other++;
    }
  }
  const char* st_names[12] = {"Unknown","Attribute","Connection","Expression",
    "Mapper","MapperArg","Prim","PseudoRoot","Relationship","RelationshipTarget",
    "Variant","VariantSet"};
  std::cout << "spec-type histogram:\n";
  for (int k = 0; k < 12; ++k)
    if (spec_hist[k]) std::cout << "  " << st_names[k] << "=" << spec_hist[k] << "\n";
  std::cout << "Attribute specs: with_default=" << attr_with_default
            << " with_conn=" << attr_with_conn
            << " conn_only(no default)=" << attr_only_conn << "\n";
  std::cout << "Relationship specs: with_targets=" << rel_with_targets
            << " no_targets=" << rel_no_targets << "\n";
  std::cout << "specs: Prim=" << n_prim << " Attribute=" << n_attr
            << " other=" << n_other << "\n";
  // Clone() directly
  {
    const PrimSpec* p0 = layer->prim(0);
    PrimSpec cl = p0->Clone();
    std::cout << "Clone() of [0] props=" << cl.properties().slots().size() << " [";
    for (const auto& s : cl.properties().slots()) std::cout << nt.get(s.name_id) << " ";
    std::cout << "]\n";
  }
  Compositor comp;
  std::unique_ptr<Layer> composed = comp.Compose(*layer);
  std::cout << "composed layer prim_count=" << composed->prim_count() << "\n";
  for (size_t i = 0; i < composed->prim_count(); i++) {
    const PrimSpec* p = composed->prim(static_cast<uint32_t>(i));
    std::cout << "  [" << i << "] path='" << p->path().str() << "' name='" << p->name()
              << "' props=" << p->properties().slots().size() << "\n";
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: bench_lazy_mem gen|eager|lazy|flat-eager|flat-lazy|diag <file.usdc> [N]\n";
    return 2;
  }
  if (std::string(argv[1]) == "diag") return do_diag(argv[2]);
  if (std::string(argv[1]) == "genmany") {
    int n = argc > 2 ? std::atoi(argv[2]) : 25000;
    int chain = argc > 3 ? std::atoi(argv[3]) : 60;
    return do_genmany(n, chain);
  }
  const std::string mode = argv[1];
  const char* path = argv[2];
  if (mode == "gen") {
    size_t n = (argc > 3) ? static_cast<size_t>(std::atoll(argv[3])) : 1000000;
    return do_gen(path, n);
  }
  if (mode == "flat-eager") return do_flatten(path, false, false);
  if (mode == "flat-lazy") return do_flatten(path, true, false);
  if (mode == "flat-lazy-owned") return do_flatten(path, true, true);
  int K = (argc > 3) ? std::atoi(argv[3]) : 20;
  if (mode == "eager") return do_measure(path, false, K);
  if (mode == "lazy") return do_measure(path, true, K);
  std::cerr << "unknown mode: " << mode << "\n";
  return 2;
}
