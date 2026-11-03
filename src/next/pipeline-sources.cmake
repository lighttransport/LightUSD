# SPDX-License-Identifier: Apache-2.0
# Common next pipeline sources shared by the standalone library and the
# compatibility combined WASM module. Relative to src/next. Shared legacy
# support (notably LZ4) is supplied by each parent, not duplicated here.
set(LIGHTUSD_NEXT_PIPELINE_SOURCES
    types/type-info.cc
    types/value.cc
    types/value-constructors.cc
    types/value-readers.cc
    types/value-dict.cc
    c/string-index.c
    c/usda-string.c
    writer/escape-string.cc
    core/string-index.cc
    types/spline.cc
    types/interpolation.cc
    writer/dtoa.cc
    ../external/zmij/zmij.cc  # zmij usdcat fast path (used by writer/dtoa.cc)
    ../unicode-xid-c.c         # XID_Start/XID_Continue tables (identifier.hh)
    c/strfmt.c                 # lu_uint_to/lu_int_to (strfmt.hh)
    c/half.c                   # lu_half_to_float/lu_float_to_half (crate-format.hh)
    c/type-names.c             # built-in type metadata (type-info.cc)
    c/descriptor.c
    c/chunks.c
    c/storage.c
    c/value.c                 # C11 lu_value semi-any core (value.cc wrapper)
    c/identifier.c             # identifier/path grammar (identifier.hh)
    c/crate-int.c              # crate integer decode (crate-format.cc)
    c/crate-blob.c             # LZ4 + crate blob framing (crate-format.cc)
    c/crate-typeid.c           # crate value-type name/size (crate-format.cc)
    c/crate-delta-encode.c     # crate delta encoders (crate-format.cc)
    c/decimal.c                # decimal integer parsing (value-parser-numeric.hh)
    c/mat4.c                   # 4x4 double transform math (xform-eval.cc)
    c/path.c                   # USD path string algorithms (path.cc)
    c/interpolation.c          # lerp/slerp interpolation kernels (interpolation.cc)
    c/value-storage.c          # value/array storage type queries (value.cc, value-view.cc)
    c/spline.c                 # cubic spline segment math (spline-eval.hh)
    ../core/logger.cc               # logger/trace method bodies (logger.hh)
    writer/value-printer.cc
    prim/path.cc
    layer/array-edit.cc
    layer/property-index.cc
    layer/prim-spec.cc
    layer/prim-spec-storage.cc
    layer/layer.cc
    stage/stage.cc
    crate/crate-format.cc
    crate/crate-data-source.cc
    crate/crate-reader-arrays.cc
    crate/crate-reader-decode.cc
    crate/crate-reader-api.cc
    crate/crate-reader.cc
    crate/crate-reader-sections.cc
    crate/crate-reader-stage.cc
    crate/crate-reader-stage-meta.cc
    crate/crate-reader-properties.cc
    crate/crate-reader-prim.cc
    crate/crate-reader-unpack.cc
    crate/crate-reader-vectors.cc
    crate/crate-writer-types.cc
    crate/crate-writer.cc
    crate/variant-holders.cc
    parser/lexer.cc
    parser/value-parser.cc
    parser/value-parser-types.cc
    parser/ascii-parser.cc
    parser/ascii-parser-metadata.cc
    parser/ascii-parser-prim.cc
    parser/ascii-parser-timesamples.cc
    parser/ascii-parser-utils.cc
    parser/ascii-parser-variants.cc
    parser/simd-scan.cc
    c/simd-scan.c              # SIMD byte scanning (parser/simd-scan.cc)
    c/array-parser.c          # bounded array grammar (value-parser-arrays.inc)
    c/lexer.c                  # lexical byte classification + number scan (parser/lexer.cc)
    composition/composition.cc
    composition/composition-variants.cc
    composition/composition-merge.cc
    composition/composition-merge-helpers.cc
    resolver/asset-resolver.cc
    pipeline/flatten.cc
    pcp/layer-registry.cc
    pcp/layer-registry-load.cc
    reader/usda-reader.cc
    reader/usdc-reader.cc
    reader/usdz-reader.cc
    schema/geom-mesh.cc
    schema/geom-xform.cc
    schema/usd-shade.cc
    eval/xform-eval.cc
    eval/attribute-eval.cc
    eval/value-clip.cc
    schema/schema-registry.cc
)
