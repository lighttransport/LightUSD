import UsdSpec.Crate.Lz4
import Gen.Inc

/-!
LZ4 / crate-framing vectors: `LEAN_LZ4_FRAME(id, n_uncompressed, "uncompressed hex",
"framed compressed hex")`. The framed bytes are `frame k comp xs` (OpenUSD
`TfFastCompression` layout, spec L11576-11598 as corrected by
issues/aousd/01) with the reference compressors of `UsdSpec.Crate.Lz4`, whose
roundtrip is proven (`decompress_compressGreedy`, `unframe_frame`).

* `lz4-vectors.inc`: `k = LZ4_MAX_INPUT_SIZE` (0x7E000000), so every row is a
  single block (`n_chunks = 0`), exactly what OpenUSD's writer emits for these
  sizes. Even ids: literals-only compressor; odd ids: greedy matcher (window 65535).
* `lz4-multichunk-vectors.inc`: the same macro, but framed with tiny chunk sizes
  (8 / 64 / 100 bytes) so the multi-chunk path (`n_chunks > 0`, int32 LE
  compressed size per chunk) is exercised. OpenUSD's *writer* never splits
  below 0x7E000000 bytes, but its reader (and LightUSD's) accept any split; use
  these as decoder-only vectors. A writer-side multi-chunk vector would need
  > 2 GiB of input and is not practical.
-/
namespace Gen.Lz4Vec
open UsdSpec.Crate.Lz4

def hex (bs : Bytes) : String :=
  String.join (bs.map fun b =>
    let s := String.ofList (Nat.toDigits 16 b.toNat)
    if s.length < 2 then "0" ++ s else s)

def ofStr (s : String) : Bytes := s.toUTF8.toList

def lcgBytes (seed n modulus : Nat) : Bytes := Id.run do
  let mut g : Lcg := ⟨UInt64.ofNat seed⟩
  let mut out : Bytes := []
  for _ in [0:n] do
    let (g1, r) := g.next
    g := g1
    out := out ++ [UInt8.ofNat (r % modulus)]
  return out

def samples : List Bytes :=
  [ [], [0x41], ofStr "hello", (List.range 12).map UInt8.ofNat, (List.range 13).map UInt8.ofNat,
    List.replicate 13 0, List.replicate 15 0x5a, List.replicate 16 0x5a, List.replicate 19 0x5a,
    List.replicate 64 0, List.replicate 300 0xff, List.replicate 600 0x01,
    (List.range 270).map (fun i => UInt8.ofNat (i * 7 + 3)),
    (List.range 40).map (fun i => UInt8.ofNat (97 + i % 3)),
    (List.range 100).map (fun i => UInt8.ofNat (i % 10)),
    ofStr "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789--end--",
    ofStr "#usda 1.0\ndef Xform \"World\"\n{\n    def Mesh \"m\"\n    {\n    }\n    def Mesh \"n\"\n    {\n    }\n}\n",
    -- distance > 255: a 300-byte random block repeated, then a tail
    (let b := lcgBytes 7 300 256; b ++ b ++ ofStr "tail-literals"),
    -- distance 65535-ish is impractical here; distance 1000
    (let b := lcgBytes 9 1000 256; b ++ b.take 200 ++ ofStr "0123456789AB"),
    lcgBytes 1 200 4, lcgBytes 2 333 2, lcgBytes 3 512 256, lcgBytes 4 777 16 ]

def k0 : Nat := lz4MaxInputSize

def rowsMain : String := Id.run do
  let mut out := ""
  let mut id := 0
  for xs in samples do
    for comp in [compressLiteralsOnly, compressGreedy 65535] do
      let f := frame k0 comp xs
      out := out ++ row "LEAN_LZ4_FRAME" [toString id, toString xs.length, cstr (hex xs), cstr (hex f)]
      id := id + 1
  return out

def rowsMulti : String := Id.run do
  let mut out := ""
  let mut id := 1000
  for xs in samples do
    if xs.length > 0 then
      for k in [8, 64, 100] do
        if xs.length ≤ 255 * k && xs.length > k then
          let f := frame k (compressGreedy 65535) xs
          out := out ++ row "LEAN_LZ4_FRAME" [toString id, toString xs.length, cstr (hex xs), cstr (hex f)]
          id := id + 1
  return out

def files : List (String × String) :=
  [ ("lz4-vectors.inc", header "Gen.Vectors.Lz4" ++
      "// LEAN_LZ4_FRAME(id, n_uncompressed, uncompressed_hex, framed_hex): framed = 1-byte n_chunks (0)\n" ++
      "// + one LZ4 block (spec L11576-11598 corrected per issues/aousd/01). Even id: literals-only,\n" ++
      "// odd id: greedy matcher. Decode framed_hex and compare with uncompressed_hex.\n" ++ rowsMain),
    ("lz4-multichunk-vectors.inc", header "Gen.Vectors.Lz4" ++
      "// LEAN_LZ4_FRAME(id, n_uncompressed, uncompressed_hex, framed_hex): DECODER-ONLY multi-chunk frames\n" ++
      "// (n_chunks > 0, each chunk = int32 LE compressed size + LZ4 block; chunk sizes 8/64/100 bytes,\n" ++
      "// which OpenUSD's writer never emits but its reader accepts).\n" ++ rowsMulti) ]

end Gen.Lz4Vec
