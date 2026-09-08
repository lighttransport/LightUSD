# Clarify signed-zero preservation in USDA, Crate compression, and exact-value conformance (Core 1.0.1)

## Specification and affected text

This is a clarification request for [AOUSD Core Specification 1.0.1 (2025-12-12)](https://github.com/aousd/specifications-public/blob/main/core/1.0.1/core_spec.md), specifically:

- **Compliance Rubric → Composed Stage Population Compliance / Value Resolution Compliance:** exactness requirements for authored floating-point values at time samples and spline knots.
- **§16.2.5.1, Double-Precision Floating Point Representation when Writing USDA content:** shortest-string formatting that parses back to the original value.
- **Crate → Compressed Floating Point Arrays:** eligibility for integer encoding (`i`) and value deduplication in lookup-table encoding (`t`).

## Ambiguity

Does preserving an unchanged authored floating-point value require preserving the IEEE-754 sign of zero, or may a conforming implementation replace `-0.0` with `+0.0` (and vice versa)?

Both zeros compare equal under ordinary floating-point equality, but have different representations and are distinguishable by sign-bit inspection and operations such as `copysign`. Consequently, numerical equality alone does not settle what “exact” or “original value” means here.

The distinction affects three concrete cases:

1. **USDA:** Must a writer emit `-0` for negative zero, and must a reader retain the sign when parsing `-0`, `-0.0`, or `-0e0` into a floating-point attribute? Is the same preservation policy intended for `half` and `float`, as well as `double` and components of aggregate values?
2. **Crate integer compression:** Does negative zero qualify as representable by an integer? Encoding it as integer zero loses its sign.
3. **Crate lookup-table compression:** May positive and negative zero share a table entry? Numerical deduplication can change one zero's sign to that of the other.

Without an explicit rule, readers, writers, and conformance tests can make different choices while each appears consistent with numerical equality.

## Reference implementation context

Source review of OpenUSD revision [`2095fafafd033fa23386d7ec6d58c7cc33974518`](https://github.com/PixarAnimationStudios/OpenUSD/tree/2095fafafd033fa23386d7ec6d58c7cc33974518) illustrates why clarification is useful:

- The [USDA parser](https://github.com/PixarAnimationStudios/OpenUSD/blob/2095fafafd033fa23386d7ec6d58c7cc33974518/pxr/usd/sdf/textParserHelpers.cpp) explicitly handles `-0` as a negative floating-point zero.
- The [string formatter](https://github.com/PixarAnimationStudios/OpenUSD/blob/2095fafafd033fa23386d7ec6d58c7cc33974518/pxr/base/tf/stringUtils.cpp) uses double-conversion without its `UNIQUE_ZERO` option, preserving the negative sign.
- The scalar floating-point array overload of [`_WritePossiblyCompressedArray`](https://github.com/PixarAnimationStudios/OpenUSD/blob/2095fafafd033fa23386d7ec6d58c7cc33974518/pxr/usd/sdf/crateFile.cpp) tests integer representability using numerical equality and builds its lookup table using `std::find`. Negative zero can therefore become integer zero, or share a lookup-table entry with positive zero. This implementation considers these compression paths for arrays of at least 16 elements; that is an implementation threshold, distinct from the specification's small-array guidance.

These are source-review observations, not a claim that OpenUSD violates the current specification or a report of a newly executed runtime test.

## Suggested reproduction / conformance cases

Author the following layer, convert USDA → USDC → USDA without evaluating transforms, and inspect zero signs after each read:

```usda
#usda 1.0
def "SignedZero"
{
    custom double scalar = -0
    custom double[] integerCandidate = [-0, 0, -0, 0, -0, 0, -0, 0, -0, 0, -0, 0, -0, 0, -0, 0]
    custom double[] lookupCandidate = [0, -0, 0.5, 0.5, 0, -0, 0.5, 0.5, 0, -0, 0.5, 0.5, 0, -0, 0.5, 0.5]
}
```

The second array prevents an all-integer encoding while offering repeated values for a lookup table; actual codec selection is implementation-dependent. Also test the lookup candidate with the first two elements swapped, and repeat for `half[]` and `float[]`.

Use `signbit`, IEEE representation inspection, or Python `math.copysign(1.0, value)` to distinguish zeros. An equality-only round-trip test cannot detect this change. Include direct defaults, authored time samples, spline knots where applicable, and floating-point components of vectors, quaternions, and matrices in the clarified test scope.

## Requested clarification

Please specify whether zero-sign preservation is required for unchanged authored values during serialization and exact-value resolution, and make the USDA and Crate rules consistent with that choice.

If preservation is required, please clarify that negative zero must survive USDA parsing/writing, cannot use a sign-losing integer encoding, and must remain distinct from positive zero in a compression lookup table. A negative-zero formatting example would help.

If canonicalization is permitted, please state where it is permitted, whether either sign may result, and how conformance tests should compare zeros. This would also document the compatibility implications of existing compression behavior.

This request concerns the sign of zero only. It does not propose bitwise equality for ordinary USD comparisons or hashing, requirements for NaN payload preservation, or preservation of source zero signs through arithmetic such as interpolation, matrix multiplication, inversion, or decomposition.
