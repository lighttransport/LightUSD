project('fuzz_lightusd_legacy', 'c', 'cpp', default_options : ['cpp_std=c++17'])

# The maintained core fuzzers are configured with CMake's
# LIGHTUSD_BUILD_FUZZERS option. This older harness is explicitly opt-in.
if not get_option('legacy')
  error('Legacy harness disabled; use CMake -DLIGHTUSD_BUILD_FUZZERS=ON for core fuzzers, or explicitly set -Dlegacy=true')
endif

lightusd_sources=[
  '../../src/legacy/asset-resolution.cc',
  '../../src/lz4/lz4.c',
  '../../src/legacy/lightusd.cc',
  '../../src/legacy/lz4-compression.cc',
  '../../src/legacy/integerCoding.cpp',
  '../../src/io-util.cc',
  '../../src/legacy/str-util.cc',
  '../../src/legacy/pprinter.cc',
  '../../src/legacy/performance.cc',
  '../../src/legacy/prim-types.cc',
  '../../src/legacy/prim-reconstruct.cc',
  '../../src/legacy/usdGeom.cc',
  '../../src/legacy/usdShade.cc',
  '../../src/legacy/usdSkel.cc',
  '../../src/legacy/usdLux.cc',
  '../../src/legacy/usdObj.cc',
  '../../src/legacy/primvar.cc',
  '../../src/legacy/path-util.cc',
  '../../src/legacy/ascii-parser.cc',
  '../../src/legacy/ascii-parser-basetype.cc',
  '../../src/legacy/ascii-parser-timesamples.cc',
  '../../src/legacy/ascii-parser-timesamples-array.cc',
  '../../src/legacy/usda-reader.cc',
  '../../src/legacy/usda-writer.cc',
  '../../src/legacy/usdc-reader.cc',
  '../../src/legacy/usdc-writer.cc',
  '../../src/legacy/crate-reader.cc',
  '../../src/legacy/crate-format.cc',
  '../../src/legacy/crate-pprint.cc',
  '../../src/legacy/value-types.cc',
  '../../src/legacy/value-pprint.cc',
  '../../src/image-loader.cc',
  '../../src/image-writer.cc',
  '../../src/legacy/image-util.cc',
  '../../src/legacy/linear-algebra.cc',
  '../../src/legacy/xform.cc',
  '../../src/legacy/stage.cc',
  '../../src/legacy/tiny-format.cc',
  '../../src/legacy/tiny-string.cc',
  '../../src/legacy/timesamples.cc',
  '../../src/legacy/timesamples-pprint.cc',
  '../../src/legacy/typed-array.cc',
  '../../src/legacy/crate-writer.cc',
  '../../src/legacy/crate-reader-timesamples.cc',
  '../../src/legacy/crate-dump.cc',
  '../../src/legacy/composition.cc',
  '../../src/legacy/prim-composition.cc',
  '../../src/legacy/layer.cc',
  '../../src/legacy/enum-handlers.cc',
  '../../src/legacy/color-space.cc',
  '../../src/legacy/parser-timing.cc',
  '../../src/legacy/prim-pprint-parallel.cc',
  '../../src/legacy/stage-converter.cc',
  '../../src/sha256.cc',
  '../../src/legacy/pxr-compat.cc',
  '../../src/legacy/uuid-gen.cc',
  '../../src/legacy/usdMtlx.cc',
  '../../src/core/mtlx-dom.cc',
  '../../src/legacy/mtlx-simple-parser.cc',
  '../../src/mtlx-xml-parser.cc',
  '../../src/mtlx-xml-tokenizer.cc',
  '../../src/legacy/usd-dump.cc',
  '../../src/legacy/crate-path-utils/path_sort.cc',
  '../../src/legacy/crate-path-utils/tree_encode.cc',
  '../../src/legacy/tydra/render-data.cc',
  '../../src/legacy/tydra/render-data-pprint.cc',
  '../../src/legacy/tydra/prim-apply.cc',
  '../../src/legacy/tydra/shader-network.cc',
  '../../src/legacy/tydra/scene-access.cc',
  '../../src/legacy/tydra/attribute-eval.cc',
  '../../src/legacy/tydra/attribute-eval-typed-all.cc',
  '../../src/legacy/tydra/attribute-eval-typed-inst-scalar.cc',
  '../../src/legacy/tydra/attribute-eval-typed-inst-array.cc',
  '../../src/tydra/texture-util.cc',
  '../../src/legacy/tydra/materialx-to-json.cc',
  '../../src/legacy/tydra/bone-util.cc',
  '../../src/legacy/tydra/render-scene-dump.cc',
  # deps
  '../../src/external/mikktspace/mikktspace.c',
  '../../src/external/fpng.cpp',
  '../../src/external/pystring.cpp',
  #'../../src/external/string_id/string_id.cpp',
  #'../../src/external/string_id/error.cpp',
  #'../../src/external/string_id/database.cpp',
  ]

incdirs = include_directories(['../../src',
  ])

# LIGHTUSD_FUZZER_BUILD = suppress some debug message
executable('fuzz_lightusd',
  'lightusd_fuzzmain.cc',
  lightusd_sources,
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_pathutil',
  'pathutil_fuzzmain.cc',
  lightusd_sources,
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_intcoding_decompress',
  '../../src/lz4/lz4.c',
  'intCoding_decompress_fuzzmain.cc',
  '../../src/legacy/lz4-compression.cc',
  '../../src/legacy/integerCoding.cpp',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_lz4_decompress',
  '../../src/lz4/lz4.c',
  'lz4_decompress_fuzzmain.cc',
  '../../src/legacy/lz4-compression.cc',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )


executable('fuzz_usdaparser',
  'usdaparser_fuzzmain.cc',
  lightusd_sources,
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_usdcparser',
  'usdcparser_fuzzmain.cc',
  lightusd_sources,
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_dr_mp3_decoding',
  'dr_mp3_decoding_fuzzmain.cc',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_dr_wav_decoding',
  'dr_wav_decoding_fuzzmain.cc',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_wuffs_jpeg_decoding',
  'wuffs_jpeg_decoding_fuzzmain.cc',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_ascii_value_parser',
  'ascii_value_parser_fuzzmain.cc',
  '../../src/legacy/tiny-string.cc',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer')

executable('fuzz_alac_decoding',
  'alac_decoding_fuzzmain.cc',
  '../../src/external/alac/codec/EndianPortable.c',
  '../../src/external/alac/codec/ALACBitUtilities.c',
  '../../src/external/alac/codec/ALACDecoder.cpp',
  '../../src/external/alac/codec/ALACEncoder.cpp',
  '../../src/external/alac/codec/ag_dec.c',
  '../../src/external/alac/codec/ag_enc.c',
  '../../src/external/alac/codec/dp_dec.c',
  '../../src/external/alac/codec/dp_enc.c',
  '../../src/external/alac/codec/matrix_dec.c',
  '../../src/external/alac/codec/matrix_enc.c',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_mtlx_xml_parser',
  'mtlx_xml_parser_fuzzmain.cc',
  '../../src/mtlx-xml-parser.cc',
  '../../src/mtlx-xml-tokenizer.cc',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_usd_writer',
  'usd_writer_fuzzmain.cc',
  lightusd_sources,
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_stb_image',
  'stb_image_fuzzmain.cc',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_tsd',
  'fuzz_tsd.cc',
  '../../src/tsd/tsd-validate.cc',
  '../../src/tsd/tsd-topology.cc',
  '../../src/tsd/tsd-bilinear.cc',
  '../../src/tsd/tsd-catmark.cc',
  '../../src/tsd/tsd-loop.cc',
  '../../src/tsd/tsd-fvar.cc',
  '../../src/tsd/tsd-refine.cc',
  '../../src/tsd/tsd-stream.cc',
  '../../src/tsd/tsd-limit.cc',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_lz4_null_deref',
  'lz4_null_deref_fuzzmain.cc',
  '../../src/lz4/lz4.c',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_str_parse_null',
  'str_parse_null_fuzzmain.cc',
  '../../src/legacy/str-util.cc',
  '../../src/legacy/value-types.cc',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_tiny_hashmap_collision',
  'tiny_hashmap_collision_fuzzmain.cc',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_string_pool_stability',
  'string_pool_stability_fuzzmain.cc',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_orm_texture_bounds',
  'orm_texture_bounds_fuzzmain.cc',
  '../../src/tydra/texture-util.cc',
  '../../src/image-writer.cc',
  '../../src/legacy/image-util.cc',
  '../../src/io-util.cc',
  '../../src/legacy/tiny-format.cc',
  '../../src/legacy/value-types.cc',
  '../../src/external/fpng.cpp',
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )

executable('fuzz_layer_find_primspec',
  'layer_find_primspec_fuzzmain.cc',
  lightusd_sources,
  install: true,
  include_directories : incdirs,
  cpp_args : ['-fsanitize=address,fuzzer', '-DLIGHTUSD_FUZZER_BUILD=1'],
  link_args : '-fsanitize=address,fuzzer' )
