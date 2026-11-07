import UsdSpec.Composition.PrimIndex
import UsdSpec.Composition.Relocates

/-!
# Invariants of the toy prim-index builder

SPEC: core_spec.md L4200-4242 (outline), L4719-4770 (variant selection),
L4876-5009 (relocates), L5234-5374 (implied classes).

Proved here, over `UsdSpec.Composition.PrimIndex`:

* namespace extension (`extendTree`) preserves tree shape and layers and only
  appends the element to every site path;
* implied copies (`impliedCopyTrees`) contain only class arcs and every arc in
  a copy is flagged `implied`, so implied nodes can never masquerade as
  directly authored arcs;
* variant-selection provenance: `selSource` names the site and value that
  `selFrom` returns, so the winning selection always comes from the first
  (strongest) site that authors the set, and it is stable under dropping
  weaker sites;
* a relocate-aware pruning pass removes every non-root node whose prim-name
  path is a relocate source (L4895-4897: opinions at the pre-relocation path
  are not composed), and keeps the root;
* a re-entered crossed site is reported as a cycle and expansion yields no
  node (`expandAvailable_cycle`).

Termination needs no fuel: all recursion is structural on `INode` or on the
cardinality of the finite site set (see `PrimIndex`).
-/

namespace UsdSpec.Composition.PrimIndex

open UsdSpec.Strength

/-! ## Namespace extension -/

mutual
theorem sitesTree_extendTree (e : PathElem) : ∀ n : INode,
    sitesTree (extendTree e n) = (sitesTree n).map fun s => (s.1, s.2 ++ [e])
  | .mk l p a cs t => by
      simp only [extendTree, sitesTree, List.map_cons]
      rw [sitesTrees_extendTrees e cs]
theorem sitesTrees_extendTrees (e : PathElem) : ∀ ns : List INode,
    sitesTrees (extendTrees e ns) = (sitesTrees ns).map fun s => (s.1, s.2 ++ [e])
  | [] => by simp [extendTrees, sitesTrees]
  | c :: cs => by
      simp only [extendTrees, sitesTrees, List.map_append]
      rw [sitesTree_extendTree e c, sitesTrees_extendTrees e cs]
end

/-- Extension never changes which layers contribute nodes. -/
theorem extendTree_layers (e : PathElem) (n : INode) :
    (sitesTree (extendTree e n)).map (·.1) = (sitesTree n).map (·.1) := by
  rw [sitesTree_extendTree]; simp [Function.comp_def]

/-- The root of an extended tree is the extended root. -/
theorem extendTree_root (e : PathElem) (l : String) (p : Path) (a : Option ArcInfo)
    (cs : List INode) (t : List SiteKey) :
    (extendTree e (.mk l p a cs t)).path = p ++ [e] := rfl

/-! ## Implied copies -/

/-- Every arc present in a tree is an implied one. -/
def AllImplied : INode → Prop
  | .mk _ _ a cs _ => (∀ ai, a = some ai → ai.implied = true) ∧ (∀ c ∈ cs, AllImplied c)

mutual
theorem impliedCopyTree_allImplied (l : String) (src dst : Path) : ∀ n : INode,
    AllImplied (impliedCopyTree l src dst n)
  | .mk _ p a cs t => by
      simp only [impliedCopyTree, AllImplied]
      refine ⟨?_, ?_⟩
      · intro ai h
        cases a with
        | none => simp at h
        | some b => simp at h; subst h; rfl
      · exact impliedCopyTrees_allImplied l src dst cs
theorem impliedCopyTrees_allImplied (l : String) (src dst : Path) : ∀ ns : List INode,
    ∀ c ∈ impliedCopyTrees l src dst ns, AllImplied c
  | [] => by simp [impliedCopyTrees]
  | c :: cs => by
      intro x hx
      simp only [impliedCopyTrees] at hx
      split at hx
      · rcases List.mem_cons.1 hx with h | h
        · subst h; exact impliedCopyTree_allImplied l src dst c
        · exact impliedCopyTrees_allImplied l src dst cs x h
      · exact impliedCopyTrees_allImplied l src dst cs x hx
end

/-- Only class (inherit/specialize) arcs are copied at the top level of an
implied copy. -/
theorem impliedCopyTrees_class (l : String) (src dst : Path) :
    ∀ ns : List INode, ∀ c ∈ impliedCopyTrees l src dst ns,
      ∃ n ∈ ns, isClassArc n.arc = true ∧ c = impliedCopyTree l src dst n
  | [] => by simp [impliedCopyTrees]
  | c :: cs => by
      intro x hx
      simp only [impliedCopyTrees] at hx
      split at hx
      · rename_i hc
        rcases List.mem_cons.1 hx with h | h
        · exact ⟨c, by simp, hc, h⟩
        · obtain ⟨n, hn, h1, h2⟩ := impliedCopyTrees_class l src dst cs x h
          exact ⟨n, by simp [hn], h1, h2⟩
      · obtain ⟨n, hn, h1, h2⟩ := impliedCopyTrees_class l src dst cs x hx
        exact ⟨n, by simp [hn], h1, h2⟩

/-- Path translation is the identity outside the referenced subtree. -/
theorem mapPath_outside (src dst p : Path) (h : ¬ src.isPrefixOf p) :
    mapPath src dst p = p := by
  simp [mapPath, h]

/-- Inside the subtree, translation replaces the source prefix by the target. -/
theorem mapPath_inside (src dst p : Path) (h : src.isPrefixOf p) :
    mapPath src dst p = dst ++ p.drop src.length := by
  simp [mapPath, h]

/-! ## Variant-selection provenance -/

/-- The site and value that win the selection of variant set `s` along a
strength-ordered site list. -/
def selSource (ls : Layers) (fuel : Nat) (sites : List (String × Path)) (s : String) :
    Option (String × Path × String) :=
  sites.findSome? fun (l, p) =>
    ((stackSpecs ls fuel l p).flatMap (·.selections)).findSome? fun (k, v) =>
      if k == s then some (l, p, v) else none

theorem findSome_sel_map (l : String) (p : Path) (s : String) :
    ∀ xs : List (String × String),
      (xs.findSome? fun (k, v) => if k == s then some (l, p, v) else none).map (·.2.2) =
        xs.findSome? fun (k, v) => if k == s then some v else none
  | [] => by simp
  | (k, v) :: ys => by
      have ih := findSome_sel_map l p s ys
      by_cases hk : k = s
      · simp [List.findSome?_cons, hk]
      · simpa [List.findSome?_cons, hk] using ih

/-- `selFrom` returns exactly the value of the recorded provenance. -/
theorem selFrom_eq_selSource (ls : Layers) (fuel : Nat) (sites : List (String × Path))
    (s : String) :
    selFrom ls fuel sites s = (selSource ls fuel sites s).map (·.2.2) := by
  unfold selFrom selSource
  induction sites with
  | nil => simp
  | cons x xs ih =>
    obtain ⟨l, p⟩ := x
    simp only [List.flatMap_cons, List.findSome?_append, List.findSome?_cons]
    rw [← findSome_sel_map l p s]
    cases hx : ((stackSpecs ls fuel l p).flatMap (·.selections)).findSome? fun (k, v) =>
        if k == s then some (l, p, v) else none with
    | none => simpa [List.findSome?_append] using ih
    | some r => simp

/-- A weaker site never overrides: if the strongest sites already provide a
selection, appending weaker ones does not change it. -/
theorem selSource_append_of_some (ls : Layers) (fuel : Nat) (strong weak : List (String × Path))
    (s : String) (h : (selSource ls fuel strong s).isSome) :
    selSource ls fuel (strong ++ weak) s = selSource ls fuel strong s := by
  unfold selSource at h ⊢
  rcases Option.isSome_iff_exists.1 h with ⟨r, hr⟩
  rw [List.findSome?_append, hr]
  rfl

/-! ## Relocate-aware pruning -/

/-- The prim-name path of a site; `none` when it passes through a variant
selection (relocates address prim paths only, L4903). -/
def primNames : Path → Option (List String)
  | [] => some []
  | .prim n :: rest => (primNames rest).map (n :: ·)
  | .variant _ _ :: _ => none

def isRelocSource (rs : List UsdSpec.Relocates.Relocate) (p : Path) : Bool :=
  match primNames p with
  | some ns => rs.any fun r => decide (r.src = ns)
  | none => false

mutual
/-- Remove every non-root node sitting at a relocate source path. -/
def pruneTree (rs : List UsdSpec.Relocates.Relocate) : INode → INode
  | .mk l p a cs t => .mk l p a (pruneTrees rs cs) t
def pruneTrees (rs : List UsdSpec.Relocates.Relocate) : List INode → List INode
  | [] => []
  | c :: cs =>
    if isRelocSource rs c.path then pruneTrees rs cs
    else pruneTree rs c :: pruneTrees rs cs
end

mutual
theorem pruneTrees_sites (rs : List UsdSpec.Relocates.Relocate) :
    ∀ ns : List INode, ∀ s ∈ sitesTrees (pruneTrees rs ns), isRelocSource rs s.2 = false
  | [] => by simp [pruneTrees, sitesTrees]
  | c :: cs => by
      intro s hs
      simp only [pruneTrees] at hs
      split at hs
      · exact pruneTrees_sites rs cs s hs
      · rename_i hc
        simp only [sitesTrees, List.mem_append] at hs
        rcases hs with h | h
        · exact pruneTree_sites rs c hc s h
        · exact pruneTrees_sites rs cs s h
theorem pruneTree_sites (rs : List UsdSpec.Relocates.Relocate) (c : INode)
    (hc : ¬ isRelocSource rs c.path = true) :
    ∀ s ∈ sitesTree (pruneTree rs c), isRelocSource rs s.2 = false := by
  intro s hs
  obtain ⟨l, p, a, cs, t⟩ := c
  simp only [pruneTree, sitesTree, List.mem_cons] at hs
  rcases hs with h | h
  · subst h
    have : isRelocSource rs p = false := by simpa [INode.path] using hc
    simpa using this
  · exact pruneTrees_sites rs cs s h
end

/-- The root is never pruned; every descendant site avoids all relocate sources. -/
theorem prune_descendants (rs : List UsdSpec.Relocates.Relocate) (n : INode) :
    (pruneTree rs n).path = n.path ∧
    ∀ s ∈ sitesTrees (pruneTree rs n).children, isRelocSource rs s.2 = false := by
  obtain ⟨l, p, a, cs, t⟩ := n
  exact ⟨rfl, pruneTrees_sites rs cs⟩

/-- Pruning with no relocates is the identity on site lists. -/
example : isRelocSource [] [PathElem.prim "A"] = false := by decide

/-- A source in the model: `/A/B` is dropped, `/A/C` survives. -/
example :
    sitesTrees (pruneTrees [⟨["A", "B"], none⟩]
      [.mk "L" [.prim "A", .prim "B"] none [] [], .mk "L" [.prim "A", .prim "C"] none [] []])
      = [("L", [.prim "A", .prim "C"])] := by decide

/-! ## Cycle reporting -/

/-- Re-entering a crossed site produces no node and exactly one diagnostic. -/
theorem expandAvailable_cycle (ls : Layers) (sel : Sel) (available : Finset SiteKey)
    (trail : List SiteKey) (layer : String) (path : Path) (arc : Option ArcInfo)
    (h : closesArcCycle trail layer path = true) :
    (expandAvailable ls sel available trail layer path arc).node = none ∧
    (expandAvailable ls sel available trail layer path arc).cycles =
      [⟨trail.head?, (layer, path), arc.map (·.kind)⟩] := by
  rw [expandAvailable]
  simp [h]

/-! ## Relocate-aware prim index -/

/-- Some prim-name prefix of `p` (including `p` itself) is a relocate source:
the prim no longer exists at that composed path (L4895-4897, L4911-4917). -/
def relocatedAway (rs : List UsdSpec.Relocates.Relocate) (p : Path) : Bool :=
  match primNames p with
  | none => false
  | some ns => rs.any fun r => decide (r.src <+: ns)

/-- The prim index with relocate sources suppressed. A path at or below a
relocate source has no index; otherwise nodes standing at a source path
inside the tree are pruned. -/
def composePrimIndexRelocated (rs : List UsdSpec.Relocates.Relocate) (ls : Layers)
    (root : String) (p : Path) : Option Node :=
  if relocatedAway rs p then none
  else
    let t₀ := composeTreeWith ls root (fun _ => none) 0 p
    let sel := selFrom ls 0 (orderedSites 0 t₀)
    some (toNodeTree (pruneTree rs (composeTreeWith ls root sel 0 p)))

/-- Prim stack of the relocated index (empty when the prim was relocated away). -/
def composeRelocated (rs : List UsdSpec.Relocates.Relocate) (ls : Layers)
    (root : String) (p : Path) : List Site :=
  match composePrimIndexRelocated rs ls root p with
  | none => []
  | some n =>
    (strengthOrder n).flatMap fun s =>
      (layerStack ls 0 s.layer).filterMap fun l =>
        if (((findLayer ls l).map (·.specs)).getD []).any (fun sp => render sp.path == s.path)
        then some ⟨l, s.path⟩ else none

theorem composeRelocated_away (rs : List UsdSpec.Relocates.Relocate) (ls : Layers)
    (root : String) (p : Path) (h : relocatedAway rs p = true) :
    composeRelocated rs ls root p = [] := by
  simp [composeRelocated, composePrimIndexRelocated, h]

mutual
theorem pruneTree_nil : ∀ n : INode, pruneTree [] n = n
  | .mk l p a cs t => by simp only [pruneTree]; rw [pruneTrees_nil cs]
theorem pruneTrees_nil : ∀ ns : List INode, pruneTrees [] ns = ns
  | [] => rfl
  | c :: cs => by
      have h : isRelocSource [] c.path = false := by
        unfold isRelocSource; cases primNames c.path <;> simp
      simp only [pruneTrees, h]
      rw [pruneTree_nil c, pruneTrees_nil cs]; simp
end

/-- With no relocates, integration changes nothing. -/
theorem composeRelocated_nil (ls : Layers) (root : String) (p : Path) :
    composeRelocated [] ls root p = compose ls root 0 p := by
  have h : relocatedAway [] p = false := by
    unfold relocatedAway; cases primNames p <;> simp
  simp only [composeRelocated, composePrimIndexRelocated, h, pruneTree_nil,
    compose, composePrimIndex, if_false, toNode]
  rfl

def relocL : Layers :=
  [{ name := "root.usda", specs := [{ path := P ["Root"], references := [(some "a.usda", P ["A"])] }] },
   { name := "a.usda", specs := [sp (P ["A"]), sp (P ["A", "Child"])] }]

/-- A relocated-away prim and everything below it has no prim stack. -/
example : composeRelocated [⟨["Root", "Child"], some ["Root", "Moved"]⟩] relocL "root.usda"
    (P ["Root", "Child", "Leaf"]) = [] := by decide +kernel
/-- Unaffected siblings compose as before. -/
example : composeRelocated [⟨["Root", "Child"], some ["Root", "Moved"]⟩] relocL "root.usda"
    (P ["Root"]) = compose relocL "root.usda" 0 (P ["Root"]) := by decide +kernel

/-! ## Implied classes that expand their own authored arcs -/

mutual
/-- Like `propagateTree`, but each implied copy also expands the arcs authored
at its (mapped) site in the referencing stack, e.g. a root-layer class that
itself inherits another class (L5234-5282). -/
def propagateExpandTree (ls : Layers) (sel : Sel) : INode → INode
  | .mk l p a cs trail =>
    let cs' := propagateExpandTrees ls sel cs
    let implied := (cs'.flatMap fun c =>
      if isRefArc c.arc then
        (c.children.filter (isClassArc ·.arc)).map (impliedCopyTree l c.path p)
      else []).map fun n =>
        match addDirectTree ls sel [] n with
        | .mk l' p' a' cs'' t' => INode.mk l' p' a' cs'' t'
    let fresh := implied.foldl (fun acc n => if acc.any (sameSite n) then acc else acc ++ [n]) cs'
    .mk l p a fresh trail
def propagateExpandTrees (ls : Layers) (sel : Sel) : List INode → List INode
  | [] => []
  | c :: cs => propagateExpandTree ls sel c :: propagateExpandTrees ls sel cs
end

def composeTreeImplied (ls : Layers) (root : String) (sel : Sel) (p : Path) : INode :=
  propagateExpandTree ls sel (composeTreeAux ls root sel 0 p.reverse)

def composePrimIndexImplied (ls : Layers) (root : String) (p : Path) : Node :=
  let t₀ := composeTreeImplied ls root (fun _ => none) p
  let sel := selFrom ls 0 (orderedSites 0 t₀)
  toNodeTree (composeTreeImplied ls root sel p)

def composeImplied (ls : Layers) (root : String) (p : Path) : List Site :=
  (strengthOrder (composePrimIndexImplied ls root p)).flatMap fun s =>
    (layerStack ls 0 s.layer).filterMap fun l =>
      if (((findLayer ls l).map (·.specs)).getD []).any (fun sp => render sp.path == s.path)
      then some ⟨l, s.path⟩ else none

def impliedChainL : Layers :=
  [{ name := "root.usda",
     specs := [sp (P ["D"]), { path := P ["C"], inherits := [P ["D"]] },
               { path := P ["Inst"], references := [(some "ref.usda", P ["Ref"])] }] },
   { name := "ref.usda",
     specs := [sp (P ["C"]), { path := P ["Ref"], inherits := [P ["C"]] }] }]

-- The class `C` implied into the root stack inherits `D` there; the stack is
-- checked against native PCP and stock OpenUSD by the generated vector
-- `implied-chain` (the expansion uses well-founded recursion, which `decide`
-- cannot unfold).

/-! ## Relocation destination mapping -/

def pathOfNames (ns : List String) : Path := ns.map .prim

/-- The prim stack of a relocation destination `q`: the destination's own
opinions first, then the opinions that arrive from the relocation source's
composed arcs, with the root-stack opinions at the old source path suppressed
(L4889-4897). Chained relocates and relocates of relocated ancestors are not
modeled (`none` target: the source simply disappears). -/
def composeAtDestination (rs : List UsdSpec.Relocates.Relocate) (ls : Layers)
    (root : String) (q : Path) : List Site :=
  match primNames q with
  | none => compose ls root 0 q
  | some qn =>
    match rs.find? (fun r => decide (r.dst = some qn)) with
    | none => composeRelocated rs ls root q
    | some r =>
      compose ls root 0 q ++
        (compose ls root 0 (pathOfNames r.src)).filter fun s => s.path != render (pathOfNames r.src)

/-- No site at the pre-relocation path survives at the destination stack's
relocated part. -/
theorem composeAtDestination_no_source (rs : List UsdSpec.Relocates.Relocate) (ls : Layers)
    (root : String) (q : Path) (qn : List String) (r : UsdSpec.Relocates.Relocate)
    (hq : primNames q = some qn)
    (hr : rs.find? (fun r => decide (r.dst = some qn)) = some r) :
    ∀ s ∈ composeAtDestination rs ls root q,
      s.path = render (pathOfNames r.src) → s ∈ compose ls root 0 q := by
  intro s hs hp
  simp only [composeAtDestination, hq, hr, List.mem_append, List.mem_filter] at hs
  rcases hs with h | ⟨_, h⟩
  · exact h
  · simp [hp] at h

end UsdSpec.Composition.PrimIndex
