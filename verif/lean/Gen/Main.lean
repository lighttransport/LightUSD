import Gen.Vectors.ListOp
import Gen.Vectors.IntegerCoding
import Gen.Vectors.LayerOffset
import Gen.Vectors.PathSchema
import Gen.Vectors.Crate
import Gen.Vectors.TimeSamples
import Gen.Vectors.Spline
import Gen.Vectors.Clips
import Gen.Vectors.Lz4
import Gen.Vectors.Usda

/-!
`usdspec-gen --out DIR [--check]`

Writes every generated vector file into DIR, or with `--check` compares the
in-memory content with the files on disk and exits 1 on any drift (missing or
different file).
-/
open Gen

def allFiles : List (String × String) :=
  ListOpVec.files ++ IntCodeVec.files ++ OffsetVec.files ++ PathSchemaVec.files ++ CrateVec.files ++ VrVec.files ++ SplineVec.files ++ ClipVec.files ++ Lz4Vec.files ++ UsdaVec.files

def main (args : List String) : IO UInt32 := do
  let mut out : Option String := none
  let mut check := false
  let mut rest := args
  while !rest.isEmpty do
    match rest with
    | "--out" :: dir :: tl => out := some dir; rest := tl
    | "--check" :: tl => check := true; rest := tl
    | other :: _ => IO.eprintln s!"usdspec-gen: unknown argument {other}"; return 2
    | [] => pure ()
  let some dir := out | IO.eprintln "usdspec-gen: --out DIR is required"; return 2
  let mut drift := 0
  for (name, content) in allFiles do
    let path := System.FilePath.mk dir / name
    if check then
      let ok ← (do
        if ← path.pathExists then
          let cur ← IO.FS.readFile path
          pure (cur == content)
        else pure false)
      if ok then IO.println s!"ok      {path}"
      else IO.println s!"DRIFT   {path}"; drift := drift + 1
    else
      IO.FS.createDirAll (System.FilePath.mk dir)
      IO.FS.writeFile path content
      IO.println s!"wrote   {path} ({content.length} bytes)"
  if drift > 0 then
    IO.eprintln s!"usdspec-gen: {drift} file(s) differ; regenerate with --out"
    return 1
  return 0
