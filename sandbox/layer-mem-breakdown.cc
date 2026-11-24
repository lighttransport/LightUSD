// Categorical memory breakdown of a flattened Layer — picks "diet PrimSpec"
// targets without a heap profiler (heaptrack's per-allocation log thrashes on
// the ~20M-allocation flattens). Walks the composed layer and attributes
// estimated bytes per structural category; COW-shared array payloads are
// deduplicated by data-pointer identity.
//
//   layer-mem-breakdown <scene-root.usd>
//
// Build (ad hoc):
//   clang++ -O2 -std=c++17 -Isrc -Isrc/external sandbox/layer-mem-breakdown.cc \
//     build/liblightusd_static.a -o /tmp/layer-mem-breakdown -fno-exceptions -lpthread
#include <cstdio>
#include <map>
#include <set>
#include <string>

#include "asset-resolution.hh"
#include "composition.hh"
#include "core/prim-spec.hh"
#include "io-util.hh"
#include "layer.hh"
#include "lightusd.hh"

using namespace lightusd;

using namespace lightusd::legacy;  // NOLINT

struct Tally {
  size_t prims = 0;
  size_t props = 0;
  size_t attrs = 0;
  size_t rels = 0;
  size_t prim_fixed = 0;        // sizeof(PrimSpec) * prims
  size_t prim_names = 0;        // name + typeName string capacity
  size_t stamped_state = 0;     // cwp string + asset_search_paths vector/strings
  size_t prim_metas = 0;        // allocated PrimMeta blocks + their string content (approx)
  size_t props_map_overhead = 0;// map node + key strings
  size_t prop_fixed = 0;        // sizeof(Property) * props
  size_t attr_strings = 0;      // attribute name + type_name capacities
  size_t value_payload = 0;     // unique array payload bytes (COW dedup'd)
  size_t value_payload_dup = 0; // payload bytes that are COW-shared (saved)
  size_t timesamples = 0;       // timesample payloads
  size_t children_vec = 0;      // children vector capacity overhead
  std::set<const void *> seen_payloads;
};

static size_t StringCap(const std::string &s) {
  // SSO strings own no heap.
  return (s.capacity() > 15) ? s.capacity() + 1 : 0;
}

static void TallyValue(const value::Value &v, Tally &t) {
  const size_t asize = v.array_size();
  size_t bytes = 0;
  if (asize > 0) {
    // Flat 12 B/element heuristic (UE arrays are mostly float3/float2/int);
    // we only need relative category weights.
    bytes = asize * 12 + 40;  // + vector block + holder
  } else {
    bytes = 16;  // scalar-ish heap or inline; negligible either way
  }
  // COW dedup by payload identity: use the type-erased data pointer.
  const void *key = v.get_raw().template cast<char>();
  if (key && t.seen_payloads.count(key)) {
    t.value_payload_dup += bytes;
  } else {
    if (key) t.seen_payloads.insert(key);
    t.value_payload += bytes;
  }
}

static void TallyPrim(const PrimSpec &ps, Tally &t) {
  t.prims++;
  t.prim_fixed += sizeof(PrimSpec);
  t.prim_names += StringCap(ps.name()) + StringCap(ps.typeName());

  t.stamped_state += StringCap(ps.get_current_working_path());
  t.stamped_state += ps.get_asset_search_paths().capacity() * sizeof(std::string);
  for (const auto &sp : ps.get_asset_search_paths()) {
    t.stamped_state += StringCap(sp);
  }

  // PrimMeta block (lazily allocated). Approximate content: assetInfo /
  // customData strings are the usual UE payload; count the block itself plus
  // a rough per-meta share via estimate if available. Keep it simple: block.
  if (ps.metas().authored()) {
    t.prim_metas += 704;  // sizeof(PrimMeta)
  }

  for (const auto &pp : ps.props()) {
    t.props++;
    t.props_map_overhead += 48 /* rb-node */ + sizeof(std::pair<const std::string, Property>);
    t.props_map_overhead += StringCap(pp.first);
    t.prop_fixed += sizeof(Property);
    const Property &prop = pp.second;
    if (prop.is_attribute()) {
      t.attrs++;
      const Attribute &a = prop.get_attribute();
      t.attr_strings += StringCap(a.name()) + StringCap(a.type_name());
      TallyValue(a.get_var().value_raw(), t);
      if (a.get_var().has_timesamples()) {
        t.timesamples += a.get_var().ts_raw().size() * 64;  // rough
      }
    } else if (prop.is_relationship()) {
      t.rels++;
    }
  }

  t.children_vec += ps.children().capacity() * sizeof(PrimSpec);
  for (const auto &c : ps.children()) {
    TallyPrim(c, t);
  }
  for (const auto &vs : ps.variantSets()) {
    for (const auto &v : vs.second.variantSet) {
      TallyPrim(v.second, t);
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <scene-root.usd>\n", argv[0]);
    return 1;
  }
  const std::string filepath = argv[1];
  std::string warn, err;

  Layer root_layer;
  if (!LoadLayerFromFile(filepath, &root_layer, &warn, &err)) {
    fprintf(stderr, "load failed: %s\n", err.c_str());
    return 1;
  }

  std::string base_dir = io::GetBaseDir(filepath);
  AssetResolutionResolver resolver;
  resolver.set_current_working_path(base_dir);
  resolver.set_search_paths({base_dir});

  SublayersCompositionOptions slopts;
  slopts.allow_parent_relative_paths = true;
  ReferencesCompositionOptions ropts;
  ropts.allow_parent_relative_paths = true;
  PayloadCompositionOptions popts;
  popts.allow_parent_relative_paths = true;
  std::map<std::string, Layer> layer_cache;
  ropts.layer_cache = &layer_cache;
  popts.layer_cache = &layer_cache;

  Layer src = root_layer;
  {
    Layer tmp;
    if (!CompositeSublayers(resolver, src, &tmp, &warn, &err, slopts)) {
      fprintf(stderr, "sublayers failed: %s\n", err.c_str());
      return 1;
    }
    src = std::move(tmp);
  }
  for (int i = 0; i < 32; i++) {
    bool unresolved = false;
    if (src.check_unresolved_references()) {
      Layer tmp;
      if (!CompositeReferencesInPlace(resolver, std::make_unique<Layer>(std::move(src)), &tmp, &warn, &err, ropts)) return 1;
      src = std::move(tmp); unresolved = true;
    }
    if (src.check_unresolved_payload()) {
      Layer tmp;
      if (!CompositePayloadInPlace(resolver, std::make_unique<Layer>(std::move(src)), &tmp, &warn, &err, popts)) return 1;
      src = std::move(tmp); unresolved = true;
    }
    if (src.check_unresolved_inherits()) {
      Layer tmp;
      if (!CompositeInherits(src, &tmp, &warn, &err)) return 1;
      src = std::move(tmp); unresolved = true;
    }
    if (src.check_unresolved_variant()) {
      unresolved = true;
      // AOUSD Core Spec 10.3.2.5: defer variants until refs/payloads resolve.
      if (!ShouldDeferVariantComposition(src)) {
        Layer tmp;
        if (!CompositeVariant(src, &tmp, &warn, &err)) return 1;
        src = std::move(tmp);
      }
    }
    if (!unresolved) break;
  }

  Tally t;
  for (const auto &item : src.primspecs()) {
    TallyPrim(item.second, t);
  }

  auto mb = [](size_t b) { return double(b) / 1e6; };
  printf("prims=%zu props=%zu attrs=%zu rels=%zu\n", t.prims, t.props, t.attrs, t.rels);
  printf("%-28s %10.1f MB\n", "PrimSpec fixed", mb(t.prim_fixed));
  printf("%-28s %10.1f MB\n", "prim name/type strings", mb(t.prim_names));
  printf("%-28s %10.1f MB\n", "stamped resolver state", mb(t.stamped_state));
  printf("%-28s %10.1f MB\n", "PrimMeta blocks", mb(t.prim_metas));
  printf("%-28s %10.1f MB\n", "props map overhead+keys", mb(t.props_map_overhead));
  printf("%-28s %10.1f MB\n", "Property fixed", mb(t.prop_fixed));
  printf("%-28s %10.1f MB\n", "attr name/type strings", mb(t.attr_strings));
  printf("%-28s %10.1f MB\n", "value payload (unique)", mb(t.value_payload));
  printf("%-28s %10.1f MB\n", "value payload COW-shared", mb(t.value_payload_dup));
  printf("%-28s %10.1f MB\n", "timesamples", mb(t.timesamples));
  printf("%-28s %10.1f MB\n", "children vec capacity", mb(t.children_vec));
  size_t total = t.prim_fixed + t.prim_names + t.stamped_state + t.prim_metas +
                 t.props_map_overhead + t.prop_fixed + t.attr_strings +
                 t.value_payload + t.timesamples + t.children_vec;
  printf("%-28s %10.1f MB (excl. COW-shared %.1f MB)\n", "TOTAL (steady-state est.)",
         mb(total), mb(t.value_payload_dup));
  return 0;
}
