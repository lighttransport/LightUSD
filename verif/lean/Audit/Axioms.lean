import UsdSpec
import Lean.Util.CollectAxioms
import Lean.Elab.Command

/-!
Audit the transitive axioms of every compiled theorem owned by a UsdSpec module,
including private helpers. Only Lean's ordinary classical/quotient foundations
are permitted. Explicit project axioms, sorryAx and compiler-trust axioms fail.
This is a trust audit; it does not discharge theorem hypotheses or establish
that a model covers the complete normative specification.
-/

open Lean Elab Command in
elab "audit_usdspec_axioms" : command => do
  let env := (← getEnv).setExporting false
  let allowed : Array Name := #[``propext, ``Classical.choice, ``Quot.sound]
  let mut checked := 0
  let mut privateChecked := 0
  let mut modules : Std.HashSet Name := {}
  let mut used : Std.HashSet Name := {}
  let mut failures : Array String := #[]
  for (name, info) in env.constants.toList do
    let owner := (env.getModuleIdxFor? name).map fun idx => env.header.moduleNames[idx]!
    let owned := owner.any (fun mod => (`UsdSpec).isPrefixOf mod) || (`UsdSpec).isPrefixOf name
    if !owned then continue
    match info with
    | .axiomInfo _ => failures := failures.push s!"explicit project axiom: {name}"
    | .thmInfo _ =>
      checked := checked + 1
      if isPrivateName name then privateChecked := privateChecked + 1
      if let some mod := owner then modules := modules.insert mod
      for ax in ← collectAxioms name do
        used := used.insert ax
        if !allowed.contains ax then
          failures := failures.push s!"{name} depends on unapproved axiom {ax}"
    | _ => pure ()
  if checked == 0 then throwError "No UsdSpec theorems found; axiom audit did not run"
  logInfo m!"UsdSpec axiom audit: {checked} theorems ({privateChecked} private) in {modules.size} modules"
  if !failures.isEmpty then
    throwError "UsdSpec axiom audit failed:\n{String.intercalate "\n" failures.toList}"
  let axioms := used.toArray.toList.map toString |>.mergeSort
  logInfo m!"UsdSpec axiom audit: {checked} theorems in {modules.size} modules; allowed dependencies: {axioms}"

audit_usdspec_axioms
