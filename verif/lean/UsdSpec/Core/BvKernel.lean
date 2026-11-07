import Init.Data.UInt.Lemmas
import Mathlib.Data.BitVec
import Mathlib.Tactic.IntervalCases

/-!
Kernel-checked bitfield identities used by the crate proofs. Extensionality
reduces word equality to its finitely many bit positions; no compiled decision
procedure or SAT-checker axiom is used. Values remain universally quantified.
-/

namespace UsdSpec.BvKernel

/-- Reassemble every byte of a 16-bit unsigned word. -/
theorem joinBytes16 (x : UInt16) :
    x.toUInt8.toUInt16 |||
    ((x >>> 8).toUInt8.toUInt16 <<< 8) = x := by
  apply UInt16.toBitVec_inj.mp
  change (x.toBitVec.setWidth 8).setWidth 16 |||
    (((x.toBitVec >>> 8).setWidth 8).setWidth 16 <<< 8) = x.toBitVec
  apply BitVec.eq_of_getLsbD_eq
  intro i hi
  interval_cases i <;>
    simp only [BitVec.getLsbD_or, BitVec.getLsbD_setWidth,
      BitVec.getLsbD_shiftLeft, BitVec.getLsbD_ushiftRight] <;> simp

/-- Reassemble every byte of a 32-bit unsigned word. -/
theorem joinBytes32 (x : UInt32) :
    x.toUInt8.toUInt32 |||
    ((x >>> 8).toUInt8.toUInt32 <<< 8) |||
    ((x >>> 16).toUInt8.toUInt32 <<< 16) |||
    ((x >>> 24).toUInt8.toUInt32 <<< 24) = x := by
  apply UInt32.toBitVec_inj.mp
  change (x.toBitVec.setWidth 8).setWidth 32 |||
    (((x.toBitVec >>> 8).setWidth 8).setWidth 32 <<< 8) |||
    (((x.toBitVec >>> 16).setWidth 8).setWidth 32 <<< 16) |||
    (((x.toBitVec >>> 24).setWidth 8).setWidth 32 <<< 24) = x.toBitVec
  apply BitVec.eq_of_getLsbD_eq
  intro i hi
  interval_cases i <;>
    simp only [BitVec.getLsbD_or, BitVec.getLsbD_setWidth,
      BitVec.getLsbD_shiftLeft, BitVec.getLsbD_ushiftRight] <;> simp

/-- Reassemble every byte of a 64-bit unsigned word. -/
theorem joinBytes64 (x : UInt64) :
    x.toUInt8.toUInt64 |||
    ((x >>> 8).toUInt8.toUInt64 <<< 8) |||
    ((x >>> 16).toUInt8.toUInt64 <<< 16) |||
    ((x >>> 24).toUInt8.toUInt64 <<< 24) |||
    ((x >>> 32).toUInt8.toUInt64 <<< 32) |||
    ((x >>> 40).toUInt8.toUInt64 <<< 40) |||
    ((x >>> 48).toUInt8.toUInt64 <<< 48) |||
    ((x >>> 56).toUInt8.toUInt64 <<< 56) = x := by
  apply UInt64.toBitVec_inj.mp
  change (x.toBitVec.setWidth 8).setWidth 64 |||
    (((x.toBitVec >>> 8).setWidth 8).setWidth 64 <<< 8) |||
    (((x.toBitVec >>> 16).setWidth 8).setWidth 64 <<< 16) |||
    (((x.toBitVec >>> 24).setWidth 8).setWidth 64 <<< 24) |||
    (((x.toBitVec >>> 32).setWidth 8).setWidth 64 <<< 32) |||
    (((x.toBitVec >>> 40).setWidth 8).setWidth 64 <<< 40) |||
    (((x.toBitVec >>> 48).setWidth 8).setWidth 64 <<< 48) |||
    (((x.toBitVec >>> 56).setWidth 8).setWidth 64 <<< 56) = x.toBitVec
  apply BitVec.eq_of_getLsbD_eq
  intro i hi
  interval_cases i <;>
    simp only [BitVec.getLsbD_or, BitVec.getLsbD_setWidth,
      BitVec.getLsbD_shiftLeft, BitVec.getLsbD_ushiftRight] <;> simp

/-- Reading a single masked flag agrees with bit-index access, including the
word's masked shift-count semantics. -/
theorem flagBit (x k : UInt64) :
    ((x >>> k) &&& 1 == 1) = x.toBitVec.getLsbD (k.toNat % 64) := by
  apply Bool.eq_iff_iff.mpr
  simp only [beq_iff_eq, UInt64.eq_iff_toBitVec_eq, UInt64.toBitVec_and,
    UInt64.toBitVec_shiftRight, UInt64.toBitVec_ofNat]
  rw [BitVec.and_one_eq_setWidth_ofBool_getLsbD]
  change ((BitVec.ofBool (x.toBitVec.getLsbD (k.toNat % 64))).setWidth 64 = 1#64) ↔
    x.toBitVec.getLsbD (k.toNat % 64) = true
  cases h : x.toBitVec.getLsbD (k.toNat % 64) <;> simp [BitVec.ofBool]

end UsdSpec.BvKernel
