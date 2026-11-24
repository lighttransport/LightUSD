// SPDX-License-Identifier: Apache-2.0
#include "instance-canon.hh"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "layer/arc-reference.hh"
#include "layer/layer.hh"
#include "layer/prim-spec.hh"
#include "writer/prim-printer.hh"

namespace {

using lightusd::Layer;
using lightusd::PrimSpec;

uint64_t Fnv1a64(const std::string& s) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ull;
  }
  return h;
}

std::string Hex16(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

// Internal reference "</A/B>..." -> "/A/B"; empty for external references.
std::string InternalRefTarget(const std::string& arc) {
  const std::string body = lightusd::ArcReferenceBody(arc);
  if (body.empty() || body[0] != '<') return {};
  const size_t end = body.find('>');
  if (end == std::string::npos) return {};
  return body.substr(1, end - 1);
}

// "/A/B" -> "A" (top-level component), or empty.
std::string TopLevelName(const std::string& path) {
  if (path.size() < 2 || path[0] != '/') return {};
  size_t end = 1;
  while (end < path.size() && path[end] != '/' && path[end] != '.' &&
         path[end] != '{')
    ++end;
  return path.substr(1, end - 1);
}

struct Canon {
  Layer& layer;
  std::set<std::string> proto_names;
  std::unordered_map<std::string, uint64_t> hash_cache;  // 0 = in progress

  // Reference list with every prototype target replaced by `subst(name)`.
  template <typename Subst>
  std::vector<std::string> RewriteRefs(const std::vector<std::string>& refs,
                                       Subst subst) const {
    std::vector<std::string> out = refs;
    for (auto& r : out) {
      const std::string target = InternalRefTarget(r);
      if (target.empty()) continue;
      const std::string top = TopLevelName(target);
      if (!proto_names.count(top)) continue;
      const size_t lt = r.find('<');
      // "</top/rest>" -> "</<subst>/rest>"
      r.replace(lt + 2, top.size(), subst(top));
    }
    return out;
  }

  // Iterative subtree serialization: each prim's relative path plus its
  // printed spec (name cleared, prototype references replaced by hashes).
  uint64_t ProtoHash(const std::string& name) {
    auto it = hash_cache.find(name);
    if (it != hash_cache.end()) return it->second;
    hash_cache[name] = 0;  // cycle guard
    const uint32_t root = layer.index_at_path("/" + name);
    if (root == UINT32_MAX) return 0;

    std::vector<std::pair<std::string, uint32_t>> items;  // (rel path, index)
    std::vector<std::pair<std::string, uint32_t>> stack{{"", root}};
    while (!stack.empty()) {
      auto cur = std::move(stack.back());
      stack.pop_back();
      items.push_back(cur);
      for (const PrimSpec* c : layer.children(cur.second)) {
        const uint32_t ci = layer.index_at_path(c->path().str());
        if (ci != UINT32_MAX) stack.push_back({cur.first + "/" + c->name(), ci});
      }
    }
    std::sort(items.begin(), items.end());

    lightusd::PrimPrintOptions opts;
    opts.max_depth = 0;
    std::string s;
    for (const auto& item : items) {
      PrimSpec spec = layer.prim(item.second)->Clone();
      spec.set_name(item.first.empty() ? std::string("_") : spec.name());
      const std::vector<std::string> refs = spec.meta().references.get();
      if (!refs.empty()) {
        spec.meta().references = RewriteRefs(refs, [&](const std::string& n) {
          return "__Proto_" + Hex16(ProtoHash(n));
        });
      }
      s += item.first;
      s += '\n';
      s += lightusd::PrintPrimSpec(spec, opts);
    }
    uint64_t h = Fnv1a64(s);
    if (h == 0) h = 1;
    hash_cache[name] = h;
    return h;
  }
};

}  // namespace

size_t CanonicalizeInstances(Layer& layer) {
  std::set<std::string> root_names;
  for (uint32_t ri : layer.root_indices())
    if (const PrimSpec* p = layer.prim(ri)) root_names.insert(p->name());

  Canon canon{layer, {}, {}};
  for (const PrimSpec& p : layer.prims()) {
    for (const auto& r : p.meta().references) {
      const std::string top = TopLevelName(InternalRefTarget(r));
      if (!top.empty() && root_names.count(top)) canon.proto_names.insert(top);
    }
  }
  if (canon.proto_names.empty()) return 0;

  // Hash before any mutation.
  std::map<std::string, std::string> renamed;
  for (const auto& n : canon.proto_names)
    renamed[n] = "__Proto_" + Hex16(canon.ProtoHash(n));

  // Retarget every internal prototype reference.
  for (size_t i = 0; i < layer.prim_count(); ++i) {
    PrimSpec* p = layer.prim_mutable(static_cast<uint32_t>(i));
    if (!p || p->meta().references.empty()) continue;
    const std::vector<std::string> refs = p->meta().references.get();
    p->meta().references = canon.RewriteRefs(
        refs, [&](const std::string& n) { return renamed[n]; });
  }

  // Rename the roots; identical prototypes collapse onto one canonical root.
  std::set<std::string> taken;
  for (const auto& kv : renamed) {
    const std::string old_path = "/" + kv.first;
    if (taken.count(kv.second) || root_names.count(kv.second)) {
      if (kv.first != kv.second) layer.remove_prim_at_path(old_path);
      continue;
    }
    if (kv.first != kv.second) layer.rename_prim_at_path(old_path, kv.second);
    taken.insert(kv.second);
  }
  return canon.proto_names.size();
}
