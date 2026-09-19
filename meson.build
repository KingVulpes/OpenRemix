project('remix-shell', ['c', 'cpp'], version : '0.1003.0', meson_version : '>=1.3',
  default_options : ['cpp_std=c++20', 'c_std=c11', 'warning_level=2', 'b_vscrt=from_buildtype'])
cpp = meson.get_compiler('cpp')
system = host_machine.system()
if system not in ['linux', 'windows', 'darwin', 'android']
  error('Supported platforms: Linux, Windows, macOS and Android.')
endif
if (system in ['linux', 'windows'] and host_machine.cpu_family() != 'x86_64') or (system in ['darwin', 'android'] and host_machine.cpu_family() != 'aarch64')
  error('Supported targets: Linux/Windows x64 and macOS/Android arm64.')
endif
if cpp.get_id() not in ['clang', 'clang-cl']
  error('Use Clang (clang-cl with the Visual Studio toolchain on Windows).')
endif
if system == 'windows'
  add_project_arguments('-DNOMINMAX', language : ['c', 'cpp'])
  if cpp.get_id() == 'clang'
    add_project_arguments('-include', meson.project_source_root() / 'src/native/platform/mingw_compat.h', language : 'cpp')
  endif
endif
add_project_arguments('-DREMIX_HAS_SHARC=' + (get_option('sharc') ? '1' : '0'), language : ['c', 'cpp'])
threads_dep = dependency('threads')
dl_dep = cpp.find_library('dl', required : false)
remix_public_include = include_directories('public/include')
prefix = get_option('deps_root')
if prefix != ''
  sdl3_dep = declare_dependency(
    dependencies : cpp.find_library('SDL3', dirs : [prefix / 'lib']),
    compile_args : ['-I' + prefix / 'include'])
elif get_option('use_system_sdl')
  sdl3_dep = dependency('sdl3', version : '>=3.4.14')
else
  cmake = import('cmake')
  opts = cmake.subproject_options()
  opts.set_override_option('werror', 'false')
  opts.add_cmake_defines({'SDL_SHARED': true, 'SDL_STATIC': false,
    'SDL_TEST_LIBRARY': false, 'SDL_TESTS': false, 'SDL_EXAMPLES': false})
  sdl_project = cmake.subproject('SDL3', options : opts)
  sdl3_dep = sdl_project.dependency('SDL3-shared')
endif
if system == 'android'
  vulkan_dep = cpp.find_library('vulkan')
else
  vulkan_dep = dependency('vulkan', version : '>=1.1')
endif
vulkan_dep = declare_dependency(dependencies : vulkan_dep,
  include_directories : include_directories('include/vulkan/include', is_system : true))
subdir('third_party/imgui')
subdir('third_party/nrd')
subdir('third_party/fsr31')
subdir('src/native')
subdir('samples/remix-native')
if get_option('enable_tests')
  subdir('tests')
endif
if get_option('vulkan_probes')
  subdir('tools/vulkan-probes')
endif
install_subdir('public/include/remix', install_dir : get_option('includedir'))
if get_option('sharc')
  install_data('third_party/sharc/License.md', rename : 'SHARC.md', install_dir : get_option('datadir') / 'remix/licenses')
endif
install_data('LICENSE', 'LICENSE-MIT', 'ThirdPartyLicenses.txt', install_dir : get_option('datadir') / 'remix/licenses')
install_data('third_party/image/LICENSES.txt', rename : 'ImageDecoders.txt',
  install_dir : get_option('datadir') / 'remix/licenses')
