// SPDX-License-Identifier: Apache-2.0
// Copyright 2024-Present Light Transport Entertainment Inc.
#include "cache-internal.hh"
namespace lightusd {
namespace next {
namespace pcp {

NamespaceMapping Cache::Impl::WithStackRelocates(uint32_t stack, NamespaceMapping m) const {
    const StackRelocates &rel = layer_stacks[stack].relocates;
    if (rel.empty()) return m;
    std::vector<NamespaceMapping::Pair> renames;
    // A relationship/connection target that points at a relocated prim maps to
    // the relocate destination. The pointing target may be expressed in EITHER
    // namespace, so emit a rename from both source spellings:
    //   * the RAW (pre-ancestral-relocation) source (`src_to_dst`) -- a target
    //     delivered by a reference that lands at the pre-relocation path
    //     (BasicRelocateToAnimInterface: pathRig.usd referenced at PathRig,
    //     before Path is relocated out);
    //   * the COMPOSED (as-authored) source (`arrivals.src_site`) -- a target
    //     delivered by a reference that lands at the POST-relocation path (an
    //     `over` on an ancestral arrival: TrickyMultipleRelocations2 rigRel from
    //     rig.usd referenced at the composed `over Model_2/Rig`).
    // Longest-prefix matching in Apply picks the right one; when raw==composed
    // (non-chained) the two pairs coincide harmlessly.
    const auto add = [&](const std::string &src, const std::string &dst) {
      // A DELETION edit (empty destination) contributes no rename pair: pxr
      // keeps targets to deleted paths as authored (RelocateToNone). A relocate
      // destination OUTSIDE the arc's namespace maps to "" (a DROP-marker) so
      // targets under the source are dropped like pxr, not leaked.
      if (dst.empty()) return;
      renames.emplace_back(src,
                           m.crosses_arc ? m.ApplyTarget(dst) : m.Apply(dst));
    };
    renames.reserve(rel.src_to_dst.size());
    for (const auto &r : rel.src_to_dst) add(r.first, r.second);
    for (const auto &kv : rel.arrivals) {
      for (const StackRelocates::Arrival &a : kv.second) {
        add(a.src_site, a.dst_site);
      }
    }
    for (auto &rename : renames) {
      m.AddRename(rename.first, rename.second);
    }
    return m;
  }

std::string Cache::Impl::SourcesKey(uint32_t stack, const std::string &path) {
    return stack == 0 ? path : ("@" + UIntToStr(stack) + ":" + path);
  }

Cache::Impl::ChildRelocations Cache::Impl::RelocationsAt(const std::vector<Src> &srcs) const {
    ChildRelocations out;
    std::set<std::string> seen;
    for (const Src &s : srcs) {
      if (s.variant) continue;
      const StackRelocates &rel = layer_stacks[s.stack_idx].relocates;
      if (rel.empty()) continue;
      if (const std::set<std::string> *d = rel.DepartedAt(s.site)) {
        out.departed.insert(d->begin(), d->end());
      }
      if (const std::vector<StackRelocates::Arrival> *a =
              rel.ArrivalsAt(s.site)) {
        for (const StackRelocates::Arrival &arrival : *a) {
          if (seen.insert(arrival.name).second) out.arrived.push_back(arrival.name);
        }
      }
    }
    return out;
  }

void Cache::Impl::SaltInternalRefContent(std::vector<Src> &content, const Src &ps,
                              const StackRelocates::Arrival &arrival,
                              std::string *warn, std::string *err) {
    for (Src &c : content) {
      if (c.suppress_site_specs) continue;
      if (FlowsThroughRelocateSource(c, arrival) ||
          ContentSiteDeparted(c, ps, arrival, warn, err)) {
        c.suppress_site_specs = true;
      }
    }
  }

bool Cache::Impl::ContentSiteDeparted(const Src &c, const Src &ps,
                           const StackRelocates::Arrival &arrival,
                           std::string *warn, std::string *err) {
    if (c.stack_idx != ps.stack_idx || c.arc_kind != ArcType::Reference)
      return false;
    if (IsPathAtOrUnder(c.site, arrival.src_site)) return false;
    const Path cp(c.site);
    const Path parent = cp.parent();
    if (parent.is_root() || parent.empty() || parent.str() == "/") return false;
    // Guard the re-entrant composition of the parent.
    const std::string gk = "$dep:" + UIntToStr(c.stack_idx) + ":" + parent.str();
    if (!reloc_content_in_progress.insert(gk).second) return false;
    const std::vector<Src> psrcs = SourcesForSite(0, parent, warn, err);
    reloc_content_in_progress.erase(gk);
    const ChildRelocations edits = RelocationsAt(psrcs);
    return edits.departed.count(cp.name()) != 0;
  }

bool Cache::Impl::FlowsThroughRelocateSource(const Src &c,
                                  const StackRelocates::Arrival &arrival) const {
    // Only REFERENCE-delivered content: the leaf-name-under-target suffix is
    // valid only across NAME-PRESERVING reference arcs. An INHERIT renames its
    // subtree (SymRig -> LRig), so the intermediate path would be mis-computed
    // (TrickyRelocationOfPrimFromPayload false-positive on inherited /Rig/SymRig).
    if (c.arc_kind != ArcType::Reference) return false;
    if (!c.arc_sites || c.arc_sites->size() < 2) return false;
    const auto &trail = *c.arc_sites;
    const std::string &leaf_tgt = trail.back().second;
    if (!NamespaceMapping::AtOrUnder(c.site, leaf_tgt)) return false;
    const std::string suffix = c.site.substr(leaf_tgt.size());  // e.g. "/Child"
    if (suffix.empty()) return false;
    for (size_t i = 0; i + 1 < trail.size(); ++i) {
      const std::string inter = trail[i].second + suffix;
      // The current arrival's OWN relocate source is not a departure here.
      if (inter == arrival.src_site ||
          NamespaceMapping::AtOrUnder(inter, arrival.src_site) ||
          NamespaceMapping::AtOrUnder(arrival.src_site, inter))
        continue;
      if (PathInRelocateSource(trail[i].first, inter)) return true;
    }
    return false;
  }

std::vector<Src> Cache::Impl::SourcesForRelocatedContent(
      const Src &ps, const StackRelocates::Arrival &arrival,
      const std::string &child_composed, std::string *warn, std::string *err) {
    const std::string guard_key =
        "@" + UIntToStr(ps.stack_idx) + ":" + child_composed;
    // Cross-stack relocate validity: a relocate whose SOURCE is itself a
    // relocation source (departed) in a CONTRIBUTING REFERENCED stack -- not the
    // relocating stack itself -- is invalid; pxr composes nothing (an empty
    // `over`, ErrorInvalidReferenceToRelocationSource: the root relocates
    // /ReferencedChar/PreRelo, but char.usd already relocated Char/PreRelo away).
    // Detect by composing the source's parent and checking whether a referenced
    // stack departed this name. (Only for a root-authored relocate, where the
    // source path is already in the composed namespace.)
    if (ps.stack_idx == 0 && isolated_reloc_depth_ == 0 &&
        reloc_content_in_progress.empty()) {
      const Path src_p(arrival.src_site);
      const Path parent = src_p.parent();
      if (!(parent.is_root() || parent.empty() || parent.str() == "/")) {
        reloc_content_in_progress.insert(guard_key);
        const std::vector<Src> psrcs = SourcesForSite(0, parent, warn, err);
        reloc_content_in_progress.erase(guard_key);
        const std::string name = src_p.name();
        for (const Src &s : psrcs) {
          if (s.variant || s.stack_idx == ps.stack_idx) continue;
          const StackRelocates &rel = layer_stacks[s.stack_idx].relocates;
          if (!rel.empty() && rel.IsDeparted(s.site, name)) {
            return {};  // invalid relocate -> empty over
          }
        }
      }
    }
    if (ComposedRelocApplicable(ps, arrival)) {
      reloc_content_in_progress.insert(guard_key);
      std::vector<Src> comp =
          ComposedRelocatedContent(ps, arrival, child_composed, warn, err);
      reloc_content_in_progress.erase(guard_key);
      SaltInternalRefContent(comp, ps, arrival, warn, err);
      return comp;
    }
    std::vector<Src> iso =
        IsolatedRelocatedContent(ps, arrival, child_composed, warn, err);
    SaltInternalRefContent(iso, ps, arrival, warn, err);
    // Chained relocate: the isolated walk resolves in the relocating stack's own
    // namespace, so its implied-class propagation chain never reaches ROOT and it
    // MISSES a root over on an INHERITED class that the relocate moved (e.g.
    // JointBlend on /CharRig/Rig/LegsRig/SymLegRig/.../Seg2 inherited into the
    // relocated Knee -- TrickyMultipleRelocationsAndClasses2). The composed
    // walk-back derivation (FullyUnrelocate + SourcesForSite(0,..)) DOES compose
    // it. SUPPLEMENT the isolated content with the composed walk's ROOT-stack
    // sources it lacks -- without REPLACING the isolated content, which is
    // correct (incl. post-relocation `over` opinions the walk-back would drop,
    // TrickyMultipleRelocations2).
    if (ComposedBaseApplicable(ps, arrival) && IsChainedArrival(ps, arrival)) {
      reloc_content_in_progress.insert(guard_key);
      std::vector<Src> comp =
          ComposedRelocatedContent(ps, arrival, child_composed, warn, err);
      reloc_content_in_progress.erase(guard_key);
      for (Src &c : comp) {
        if (c.stack_idx != 0 || c.suppress_site_specs) continue;
        if (Specs(c.stack_idx, c.site).empty()) continue;
        bool present = false;
        for (const Src &i : iso)
          if (i.stack_idx == c.stack_idx && i.site == c.site) { present = true; break; }
        if (!present) iso.push_back(std::move(c));
      }
    }
    return iso;
  }

bool Cache::Impl::IsChainedArrival(const Src &ps,
                        const StackRelocates::Arrival &arrival) const {
    const StackRelocates &rel = layer_stacks[ps.stack_idx].relocates;
    for (Path a = Path(arrival.src_site).parent();
         !(a.is_root() || a.empty() || a.str() == "/"); a = a.parent()) {
      const Path ap = a.parent();
      const std::string aparent =
          (ap.is_root() || ap.empty()) ? "/" : ap.str();
      if (rel.ArrivalOf(aparent, a.name())) return true;
    }
    return false;
  }

bool Cache::Impl::ComposedBaseApplicable(const Src &ps,
                              const StackRelocates::Arrival &arrival) {
    if (isolated_reloc_depth_ != 0) return false;
    if (!reloc_content_in_progress.empty()) return false;
    if (ps.stack_idx != 0) {
      NamespaceMapping arc_only = ArcOnlyMapping(ps);
      if (arc_only.MatchSource(arrival.src_site) < 0) return false;
    }
    if (!Specs(ps.stack_idx, arrival.src_site).empty()) return false;
    return true;
  }

bool Cache::Impl::ComposedRelocApplicable(const Src &ps,
                               const StackRelocates::Arrival &arrival) {
    return ComposedBaseApplicable(ps, arrival) && !IsChainedArrival(ps, arrival);
  }

NamespaceMapping Cache::Impl::ArcOnlyMapping(const Src &ps) const {
    NamespaceMapping arc_only = Mapping(ps.map_idx);
    const StackRelocates &rel = layer_stacks[ps.stack_idx].relocates;
    std::vector<NamespaceMapping::Pair> kept;
    for (const auto &pr : arc_only.pairs) {
      if (rel.src_to_dst.count(pr.first)) continue;  // relocate rename
      kept.push_back(pr);
    }
    arc_only.pairs = std::move(kept);
    return arc_only;
  }

std::string Cache::Impl::FullyUnrelocate(uint32_t stack, std::string path) const {
    const StackRelocates &rel = layer_stacks[stack].relocates;
    if (rel.empty()) return path;
    for (int iter = 0; iter < 64; ++iter) {
      bool changed = false;
      for (const auto &r : rel.src_to_dst) {
        if (r.second.empty()) continue;
        if (NamespaceMapping::AtOrUnder(path, r.second)) {
          path = r.first + path.substr(r.second.size());
          changed = true;
          break;
        }
      }
      if (!changed) break;
    }
    return path;
  }

std::vector<Src> Cache::Impl::IsolatedRelocatedContent(
      const Src &ps, const StackRelocates::Arrival &arrival,
      const std::string &child_composed, std::string *warn, std::string *err) {
    std::vector<Src> out;
    NamespaceMapping outer = Mapping(ps.map_idx);
    outer.AddRename(arrival.src_site, child_composed);
    const std::vector<Src> &content =
        SourcesForRelocateSource(ps.stack_idx, arrival.src_site, warn, err);
    out.reserve(content.size());
    for (const Src &s : content) {
      Src c = s;
      if (c.stack_idx == ps.stack_idx && c.arc_kind == ArcType::Root &&
          c.site == arrival.src_site) {
        c.suppress_site_specs = true;
      }
      c.map_idx =
          InternMapping(NamespaceMapping::Compose(outer, Mapping(s.map_idx)));
      c.offset = ps.offset.Compose(s.offset);
      if (!c.expression_variables) c.expression_variables = ps.expression_variables;
      if (c.arc_kind == ArcType::Root) c.arc_kind = ArcType::Relocate;
      out.push_back(std::move(c));
    }
    return out;
  }

std::vector<Src> Cache::Impl::ComposedRelocatedContent(
      const Src &ps, const StackRelocates::Arrival &arrival,
      const std::string &child_composed, std::string *warn, std::string *err) {
    std::vector<Src> out;
    // Map the source path from the relocating stack's namespace into root using
    // ps's mapping WITHOUT this stack's relocate renames -- ps.map_idx bakes in
    // every relocate of the stack (WithStackRelocates), so a plain Apply would
    // send src_site to its own relocate DESTINATION (circular). Drop the pairs
    // that rename a relocate source of this stack, keeping only the arc pairs,
    // so src_site maps to its PRE-relocate composed location.
    const NamespaceMapping arc_only = ArcOnlyMapping(ps);
    // Walk back through the relocating stack's CHAINED relocates so the source
    // lands at its fully-un-relocated composed class-inheriting path (a chained
    // relocate leaves arrival.src_site under an ANCESTOR relocate's destination;
    // one arc_only.Apply only un-does the arc, not those ancestor relocates).
    // Composing THERE via SourcesForSite(0,..) pulls the inherited class + its
    // root implied-class over (TrickyMultipleRelocationsAndClasses2 JointBlend).
    const std::string src_unreloc =
        FullyUnrelocate(ps.stack_idx, arrival.src_site);
    const std::string src_root = arc_only.Apply(src_unreloc);
    const Path src_root_path(src_root);
    const Path src_root_parent = src_root_path.parent();
    const bool parent_is_root = src_root_parent.is_root() ||
                                src_root_parent.empty() ||
                                src_root_parent.str() == "/";
    if (parent_is_root)  // relocate to a new root prim: keep the isolated walk.
      return IsolatedRelocatedContent(ps, arrival, child_composed, warn, err);

    const std::vector<Src> &parent_srcs =
        SourcesForSite(0, src_root_parent, warn, err);
    // Suppress the relocating stack's own departure of this child so the source
    // stays resolvable. raw match is (stack, site) in the parent-source's ns;
    // for a root-authored relocate the relocating stack's site == src_root_parent.
    std::vector<Src> derived = DeriveChildSources(
        parent_srcs, src_root_path.name(), src_root, warn, err, ps.stack_idx,
        src_root_parent.str());
    // Salt BEFORE expansion: opinions authored in the RELOCATING stack AT the
    // relocation source are ignored by pxr ("opinions at relocation source"),
    // and so are the ARCS they author (ExpandList skips a salted spec's arcs).
    // Marking after ExpandList would be too late -- the source's own
    // (prohibited) reference/inherit would already have been expanded
    // (ErrorOpinionAtRelocationSource). The Src stays so arc-delivered content
    // and chained derivation still descend through it.
    for (Src &s : derived) {
      if (s.stack_idx == ps.stack_idx && s.arc_kind == ArcType::Root &&
          s.site == src_root) {
        s.suppress_site_specs = true;
      }
    }
    // Expand the source prim's own (non-salted) arcs -- the isolated walk does
    // this via ExpandList at each ancestor level. Arc-delivered content (a
    // reference on an ANCESTOR that carries the source, e.g. `/Group references
    // @g.usd@`) is what should follow the move (TrickyInheritsAndRelocates).
    derived = ExpandList(derived, child_composed, warn, err);

    // Redirect the source subtree (root ns) onto the arrival destination.
    NamespaceMapping redirect;
    redirect.AddRename(src_root, child_composed);
    out.reserve(derived.size());
    for (const Src &s : derived) {
      Src c = s;
      c.map_idx =
          InternMapping(NamespaceMapping::Compose(redirect, Mapping(s.map_idx)));
      if (!c.expression_variables) c.expression_variables = ps.expression_variables;
      if (c.arc_kind == ArcType::Root) c.arc_kind = ArcType::Relocate;
      out.push_back(std::move(c));
    }
    return out;
  }

void Cache::Impl::AddRelocatedSources(const Src &ps,
                           const StackRelocates::Arrival &arrival,
                           const std::string &child_composed,
                           std::vector<Src> *base, std::string *warn,
                           std::string *err,
                           std::vector<Src> *content_out) {
    if (!content_out) content_out = base;
    // The parent's namespace mapping, plus the rename this relocate performs
    // (expressed in the parent stack's namespace -> composed namespace). Both
    // pairs are kept: paths outside the relocated subtree still map through the
    // arc (a connection to a non-relocated sibling), paths inside it land at the
    // new address (TrickyConnectionToRelocatedAttribute).
    const auto renamed_map = [&](const std::string &site) {
      NamespaceMapping m = Mapping(ps.map_idx);
      m.AddRename(site, child_composed);
      return m;
    };

    Src dst;
    dst.stack_idx = ps.stack_idx;
    dst.site = arrival.dst_site;
    dst.map_idx = InternMapping(renamed_map(arrival.dst_site));
    dst.offset = ps.offset;
    dst.arc_kind = ps.arc_kind;
    dst.implied_anchor = ps.implied_anchor;
    dst.suppress_site_specs = false;  // the TARGET address is a legal site
    dst.expression_variables = ps.expression_variables;
    dst.arc_chain = ps.arc_chain;
    dst.arc_sites = ps.arc_sites;
    dst.ancestral = true;  // child derivation, like DeriveChildSources
    base->push_back(std::move(dst));

    // Relocated content, already in final composed form (see
    // SourcesForRelocatedContent). Deferred below arrival-local dst opinions
    // when content_out != base.
    std::vector<Src> content =
        SourcesForRelocatedContent(ps, arrival, child_composed, warn, err);
    for (Src &c : content) content_out->push_back(std::move(c));
  }

std::vector<Src> Cache::Impl::DeriveChildSources(const std::vector<Src> &psrcs,
                                      const std::string &cn,
                                      const std::string &child_composed,
                                      std::string *warn, std::string *err,
                                      uint32_t raw_stack,
                                      const std::string &raw_site) {
    std::vector<Src> base;
    base.reserve(psrcs.size());
    // A relocate DESTINATION owns the arrival name. pxr composes, for that name:
    // (a) destination-site opinions authored AT-OR-STRONGER-THAN the relocating
    // stack (the relocating stack's own `over`/variant graft, and stronger
    // stacks' overs), then (b) the relocated content, WEAKEST. A coincidental
    // same-name child in a stack WEAKER than the relocating stack is a COLLISION
    // pxr shadows ("should not show up"). `arrival_stacks` accumulates the
    // relocating stacks as we walk psrcs strong->weak, so a NORMAL child from a
    // weaker DIFFERENT stack is shadowed while stronger pre-arrival opinions and
    // same-stack grafts are kept. `deferred_content` holds (b), appended last.
    // (TrickyVariantOverrideOfRelocatedPrim / TrickyMultipleRelocations /
    // TrickySpookyVariantSelection, verified vs pcp.txt prim stacks.)
    std::vector<Src> deferred_content;
    std::set<uint32_t> arrival_stacks;
    for (const Src &ps : psrcs) {
      // Variant sources are inline opinions on the prim itself; they do not
      // (yet) carry child prims, so they don't propagate to children.
      if (ps.variant) continue;
      const StackRelocates &rel = layer_stacks[ps.stack_idx].relocates;
      const bool raw = ps.stack_idx == raw_stack && ps.site == raw_site;
      if (!rel.empty() && !raw) {
        if (rel.IsDeparted(ps.site, cn)) continue;
        if (const StackRelocates::Arrival *arrival =
                rel.ArrivalOf(ps.site, cn)) {
          // dst-site opinions inline (strong); relocated content deferred.
          AddRelocatedSources(ps, *arrival, child_composed, &base, warn, err,
                              &deferred_content);
          arrival_stacks.insert(ps.stack_idx);
          continue;
        }
      }
      // Same-name collision with a relocate destination owned by another
      // (stronger) stack: shadowed. Opinions stronger than the arrival (seen
      // before it here) and same-stack grafts pass through.
      if (!arrival_stacks.empty() && !arrival_stacks.count(ps.stack_idx))
        continue;
      Src c;
      c.stack_idx = ps.stack_idx;
      c.site = ps.site + "/" + cn;
      c.map_idx = ps.map_idx;  // share the parent's mapping (no string copy)
      c.expression_variables = ps.expression_variables;
      c.offset = ps.offset;
      c.arc_kind = ps.arc_kind;
      c.implied_anchor = ps.implied_anchor;
      // Prohibited relocation-source namespace extends to DESCENDANTS: their
      // raw-site specs (a_src-style overrides at pre-relocation paths) are
      // ignored, while the Srcs themselves stay traversable for chained
      // relocations (arrivals registered under another relocate's source).
      c.suppress_site_specs = ps.suppress_site_specs;
      c.arc_chain = ps.arc_chain;  // implied class arcs need the whole chain
      c.arc_sites = ps.arc_sites;  // ancestral cycle trail (see Src::arc_sites)
      c.ancestral = true;          // arc was introduced at the parent
      base.push_back(std::move(c));
    }
    // Relocated content composes weakest, after every arrival-local dst opinion.
    for (Src &c : deferred_content) base.push_back(std::move(c));
    return base;
  }

const std::vector<Src> &Cache::Impl::SourcesForRelocateSource(uint32_t stack,
                                                   const std::string &site,
                                                   std::string *warn,
                                                   std::string *err) {
    const auto raw_key = [&](const std::string &path) {
      return "!" + UIntToStr(stack) + ":" + path;
    };
    const std::string key = raw_key(site);
    if (const std::vector<Src> *hit = FindCachedSources(key)) return *hit;
    if (!sources_in_progress.insert(key).second) return empty_sources_;
    ++isolated_reloc_depth_;

    // The WHOLE ancestor chain is addressed raw in this stack, not just the leaf:
    // relocates chain, so an ancestor of this source may itself have been moved
    // away (`/Legs/RightRig/Leg_bone -> /Legs/LeftRig/Leg_bone/Leg_bone`, then
    // `.../Leg_bone/Leg_bone/Knee_bone -> /Knee_bone`). The composed namespace no
    // longer holds those ancestors, but the pre-relocation namespace this source
    // is expressed in does.
    std::vector<Path> pending;  // leaf -> topmost
    {
      Path cur(site);
      for (;;) {
        pending.push_back(cur);
        Path parent = cur.parent();
        if (parent.is_root() || parent.empty() || parent.str() == "/") break;
        if (HasCachedSources(raw_key(parent.str()))) break;
        cur = parent;
      }
    }

    const std::vector<Src> *result = nullptr;
    for (size_t i = pending.size(); i-- > 0;) {
      const Path &p = pending[i];
      const Path parent = p.parent();
      // Relocate paths are keyed in the COMPOSED namespace (see BuildStack-
      // Relocates), so the source's ANCESTORS exist there -- either normal prims
      // or ARRIVALS of ancestral relocates carrying an `over` plus the relocated
      // content. Compose them normally (arrival handling ON). Only the LEAF (the
      // relocate source itself) departed to the relocate target: suppress its
      // departure so it stays resolvable, and its own spec opinions (salted
      // earth) below.
      const bool is_leaf = (p.str() == site);
      std::vector<Src> base;
      if (parent.is_root() || parent.empty() || parent.str() == "/") {
        // A non-leaf root ancestor may itself be a relocate ARRIVAL (chained
        // relocate whose source sits under a root-level arrival): compose it.
        const StackRelocates &rel = layer_stacks[stack].relocates;
        const StackRelocates::Arrival *arrival =
            is_leaf ? nullptr : rel.ArrivalOf("/", p.name());
        Src s;
        s.stack_idx = stack;
        s.site = p.str();
        s.expression_variables = std::make_shared<const Value>(
            layer_stacks[stack].expression_variables);
        if (arrival) {
          AddRelocatedSources(s, *arrival, p.str(), &base, warn, err);
        } else {
          base.push_back(std::move(s));
        }
      } else {
        const std::vector<Src> &psrc =
            GetOrCreateCachedSources(raw_key(parent.str()));
        // raw_stack/raw_site suppresses BOTH departure and arrival for the
        // matching source; use it ONLY for the leaf (whose departure we cancel)
        // -- intermediate ancestors keep arrival handling so their `over` and
        // relocated content compose (TrickyMultipleRelocations2 over Model_2).
        if (is_leaf) {
          base = DeriveChildSources(psrc, p.name(), p.str(), warn, err, stack,
                                    parent.str());
        } else {
          base = DeriveChildSources(psrc, p.name(), p.str(), warn, err);
        }
      }
      // The relocation SOURCE itself: this stack's own opinions there are
      // prohibited (pxr "opinions at relocation source") — including the
      // spec's ARCS and variant selections, so mark before expansion.
      if (is_leaf) {
        for (Src &b : base) {
          if (b.stack_idx == stack && b.arc_kind == ArcType::Root &&
              b.site == site) {
            b.suppress_site_specs = true;
          }
        }
      }
      std::vector<Src> &slot = GetOrCreateCachedSources(raw_key(p.str()));
      slot = ExpandList(base, p.str(), warn, err);
      result = &slot;
    }
    --isolated_reloc_depth_;
    sources_in_progress.erase(key);
    return *result;
  }

const std::vector<Src> &Cache::Impl::SourcesForPath(const Path &path, std::string *warn,
                                         std::string *err) {
    return SourcesForSite(0, path, warn, err);
  }

const std::vector<Src> &Cache::Impl::SourcesForSite(uint32_t stack, const Path &path,
                                         std::string *warn, std::string *err) {
    const std::string key = SourcesKey(stack, path.str());
    if (const std::vector<Src> *hit = FindCachedSources(key)) return *hit;
    if (!sources_in_progress.insert(key).second) return empty_sources_;

    // Collect the uncached ancestor chain (leaf -> topmost uncached), then
    // expand top-down, so a pathologically deep query path walks a loop instead
    // of the C++ stack.
    std::vector<Path> pending;
    {
      Path cur = path;
      for (;;) {
        pending.push_back(cur);
        Path parent = cur.parent();
        if (parent.is_root() || parent.empty() || parent.str() == "/") break;
        if (HasCachedSources(SourcesKey(stack, parent.str()))) break;
        cur = parent;
      }
    }

    const std::vector<Src> *result = nullptr;
    for (size_t i = pending.size(); i-- > 0;) {
      const Path &p = pending[i];
      const std::string key_i = SourcesKey(stack, p.str());

      std::vector<Src> base;
      Path parent = p.parent();
      const bool parent_is_root =
          parent.is_root() || parent.empty() || parent.str() == "/";
      if (parent_is_root) {
        // A relocate may move a prim to a NEW ROOT PRIM: its content comes from
        // the source site, while opinions authored at the new root name (an
        // `over` in the root layer) still compose on top.
        const StackRelocates &rel = layer_stacks[stack].relocates;
        const StackRelocates::Arrival *arrival = rel.ArrivalOf("/", p.name());
        Src s;
        s.stack_idx = stack;
        s.site = p.str();
        s.map_idx = InternMapping(WithStackRelocates(stack, NamespaceMapping{}));
        s.expression_variables = std::make_shared<const Value>(
            layer_stacks[stack].expression_variables);
        if (arrival) {
          AddRelocatedSources(s, *arrival, p.str(), &base, warn, err);
        } else {
          base.push_back(std::move(s));
        }
      } else {
        const std::vector<Src> &psrc =
            GetOrCreateCachedSources(SourcesKey(stack, parent.str()));
        base = DeriveChildSources(psrc, p.name(), p.str(), warn, err);
      }

      // SrcCache element refs are stable across later inserts (deque-backed),
      // and pending[0] (the queried leaf) is filled last, so this captures it.
      std::vector<Src> &slot = GetOrCreateCachedSources(key_i);
      slot = ExpandList(base, p.str(), warn, err);
      result = &slot;
    }
    sources_in_progress.erase(key);
    return *result;
  }

}  // namespace pcp
}  // namespace next
}  // namespace lightusd
