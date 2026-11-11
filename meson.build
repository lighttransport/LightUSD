# SPDX-License-Identifier: Apache-2.0
project('lightusd', ['c', 'cpp'],
  version: '1.0.0',
  default_options: ['cpp_std=c++17', 'c_std=c11'],
  meson_version: '>= 1.1.0',
)

cmake_defines = {
  'CMAKE_POSITION_INDEPENDENT_CODE': true,
  'LIGHTUSD_USE_CCACHE': false,
  'LIGHTUSD_BUILD_TESTS': false,
  'LIGHTUSD_BUILD_EXAMPLES': false,
  'LIGHTUSD_BUILD_TOOLS': false,
  'LIGHTUSD_BUILD_LUSDCAT': false,
  'LIGHTUSD_BUILD_SHARED_C_API': get_option('shared_lib'),
  'LIGHTUSD_BUILD_LEGACY': get_option('legacy'),
  'LIGHTUSD_WITH_LEGACY_TYDRA': get_option('legacy_tydra'),
  'LIGHTUSD_WITH_TYDRA': get_option('tydra'),
  'LIGHTUSD_WITH_USDOBJ': get_option('usdobj'),
  'LIGHTUSD_WITH_EXR': get_option('exr'),
  'LIGHTUSD_WITH_QJS': get_option('qjs'),
  'LIGHTUSD_WITH_MCP_SERVER': get_option('mcp_server'),
  'LIGHTUSD_PRODUCTION_BUILD': get_option('production_build'),
}

if get_option('legacy_tydra') and not get_option('legacy')
  error('legacy_tydra requires explicit -Dlegacy=true')
endif

# These switches describe only the opt-in older implementation. Common core
# settings above always configure the maintained CMake targets.
if get_option('legacy')
  legacy_options = {
    'builtin_image_loader': 'LIGHTUSD_WITH_BUILTIN_IMAGE_LOADER',
    'usdmtlx': 'LIGHTUSD_WITH_USDMTLX',
    'json': 'LIGHTUSD_WITH_JSON',
    'usdvox': 'LIGHTUSD_WITH_USDVOX',
    'audio': 'LIGHTUSD_WITH_AUDIO',
    'colorio': 'LIGHTUSD_WITH_COLORIO',
    'zstd': 'LIGHTUSD_WITH_ZSTD_COMPRESSION',
    'pxr_compat_api': 'LIGHTUSD_WITH_PXR_COMPAT_API',
    'module_usda_reader': 'LIGHTUSD_WITH_MODULE_USDA_READER',
    'module_usda_writer': 'LIGHTUSD_WITH_MODULE_USDA_WRITER',
    'module_usdc_reader': 'LIGHTUSD_WITH_MODULE_USDC_READER',
    'module_usdc_writer': 'LIGHTUSD_WITH_MODULE_USDC_WRITER',
    'usdfbx': 'LIGHTUSD_WITH_USDFBX',
    'meshopt': 'LIGHTUSD_WITH_MESHOPT',
    'alac_audio': 'LIGHTUSD_WITH_ALAC_AUDIO',
    'wamr': 'LIGHTUSD_WITH_WAMR',
    'geogram': 'LIGHTUSD_WITH_GEOGRAM',
    'remotery': 'LIGHTUSD_WITH_REMOTERY',
    'system_zlib': 'LIGHTUSD_USE_SYSTEM_ZLIB',
    'system_zstd': 'LIGHTUSD_USE_SYSTEM_ZSTD',
  }
  foreach option_name, cmake_name : legacy_options
    cmake_defines += {cmake_name: get_option(option_name)}
  endforeach
  cmake_defines += {'LIGHTUSD_BUILD_SHARED_LIBS': get_option('shared_lib')}
endif

# CMake object libraries and generator expressions are built by CMake itself,
# rather than being translated by Meson's limited CMake interpreter.
python = import('python').find_installation('python3')
fs = import('fs')
helper = files('scripts/meson-cmake.py')
fs.read('scripts/meson-cmake.py')
manifest = meson.current_build_dir() / 'cmake-libraries.json'
cmake_defines += {
  'LIGHTUSD_USE_PCH': false,
  'LIGHTUSD_WITH_ALLOCATOR': false,
  'CMAKE_INTERPROCEDURAL_OPTIMIZATION': get_option('b_lto'),
  'CMAKE_BUILD_TYPE': {'debug': 'Debug', 'debugoptimized': 'RelWithDebInfo',
    'release': 'Release', 'minsize': 'MinSizeRel', 'plain': 'Release'}[get_option('buildtype')],
}
cmake_args = get_option('cmake_args')
foreach key, value : cmake_defines
  if key == 'CMAKE_BUILD_TYPE'
    cmake_args += ['-D' + key + '=' + value]
  else
    cmake_args += ['-D' + key + '=' + value.to_string('ON', 'OFF')]
  endif
endforeach
run_command(python, helper, 'configure',
  '--source', meson.project_source_root(),
  '--build', meson.current_build_dir() / 'cmake', '--manifest', manifest,
  '--c-compiler', ';'.join(meson.get_compiler('c').cmd_array()),
  '--cpp-compiler', ';'.join(meson.get_compiler('cpp').cmd_array()),
  '--c-args=' + '\n'.join(get_option('c_args')),
  '--cpp-args=' + '\n'.join(get_option('cpp_args') + (get_option('legacy') and get_option('debug_print') ? ['-DLIGHTUSD_DEBUG_PRINT'] : [])),
  '--sanitize', get_option('b_sanitize'), cmake_args, check: true)
outputs_text = run_command(python, helper, 'query', '--manifest', manifest,
  'artifacts', check: true).stdout().strip()
outputs = outputs_text.split('\n')
cmake_inputs = run_command(python, helper, 'query', '--manifest', manifest,
  'inputs', check: true).stdout().strip().split('\n')
foreach input : cmake_inputs
  fs.read(input)
endforeach
libraries = custom_target('lightusd-cmake-libraries', output: outputs,
  command: [python, helper, 'build', '--manifest', manifest,
    '--output', meson.current_build_dir(), '--jobs', get_option('cmake_jobs').to_string()],
  build_always_stale: true, build_by_default: true)
products = ['core']
if get_option('tydra')
  products += ['tydra']
endif
if get_option('legacy')
  products += ['legacy']
endif
if get_option('shared_lib')
  products += ['shared']
  if get_option('legacy')
    products += ['legacy_shared']
  endif
endif
if get_option('mcp_server')
  products += ['mcp']
endif
product_deps = {}
foreach product : products
  linked = []
  required = run_command(python, helper, 'query', '--manifest', manifest,
    '--product', product, 'outputs', check: true).stdout().strip().split('\n')
  foreach output : required
    foreach i : range(outputs.length())
      if outputs[i] == output
        linked += [libraries[i]]
      endif
    endforeach
  endforeach
  compile_args_text = run_command(python, helper, 'query', '--manifest', manifest,
    '--product', product, 'defines', check: true).stdout().strip()
  include_text = run_command(python, helper, 'query', '--manifest', manifest,
    '--product', product, 'includes', check: true).stdout().strip()
  link_text = run_command(python, helper, 'query', '--manifest', manifest,
    '--product', product, 'link_args', check: true).stdout().strip()
  product_deps += {product: declare_dependency(link_with: linked,
    include_directories: include_directories(include_text.split('\n')),
    compile_args: compile_args_text == '' ? [] : compile_args_text.split('\n'),
    link_args: link_text == '' ? [] : link_text.split('\n'))}
endforeach
lightusd_dep = product_deps['core']
meson.override_dependency('lightusd', lightusd_dep)
if get_option('tydra')
  tydra_dep = product_deps['tydra']
endif
if get_option('legacy')
  lightusd_legacy_dep = product_deps['legacy']
endif
if get_option('mcp_server')
  lightusd_mcp_dep = product_deps['mcp']
endif
if get_option('shared_lib')
  lightusd_shared_dep = product_deps['shared']
  if get_option('legacy')
    lightusd_legacy_shared_dep = product_deps['legacy_shared']
  endif
endif
if get_option('tests')
  core_test = executable('core-api-test', meson.project_source_root() / 'tests/core/test_core.cc',
    dependencies: lightusd_dep)
  test('core-api-test', core_test, workdir: meson.project_source_root())
endif
summary({'core': true, 'tydra': get_option('tydra'),
         'legacy': get_option('legacy'), 'legacy_tydra': get_option('legacy_tydra')})
