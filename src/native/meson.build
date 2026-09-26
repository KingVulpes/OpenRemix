native_include = include_directories('.')
image_include = include_directories('../../third_party', is_system : true)
shell_deps = [sdl3_dep, vulkan_dep, threads_dep, imgui_dep, nrd_dep, fsr31_dep, dlss_dep]
shell_args = ['-DREMIX_LIBRARY_EXPORTS=1'] + cpp.get_supported_arguments('-Wno-missing-field-initializers', '-Wno-unused-parameter')
subdir('platform')
subdir('api')
subdir('runtime')
subdir('ui')
# GCC 8's standard library ships filesystem in a separate static archive.
filesystem_dep = cpp.find_library('stdc++fs', required : false)
subdir('scene')
usd_libraries = []
if get_option('usd')
  subdir('usd')
endif
subdir('raster')
subdir('denoise')
subdir('upscale')
subdir('pathtrace')
shell_link_args = []
if system in ['linux', 'android']
  shell_link_args += ['-Wl,--version-script=' + meson.current_source_dir() / 'api/remix.map']
endif
shell_rpath = ''
shell_versions = {}
if system not in ['windows', 'android']
  shell_versions = {'version' : meson.project_version(), 'soversion' : '0'}
endif
if system == 'darwin'
  shell_rpath = '@loader_path'
elif system == 'linux'
  shell_rpath = '$ORIGIN'
endif
remix = shared_library('remix',
  api_sources + runtime_sources + ui_sources,
  name_prefix : system == 'windows' ? '' : 'lib',
  link_with : usd_libraries + [shell_platform, imgui_vulkan, scene_library, raster_library, pathtrace_library, denoise_library, upscale_library], dependencies : shell_deps,
  include_directories : [remix_public_include, native_include, image_include], cpp_args : shell_args,
  link_args : shell_link_args, link_depends : 'api/remix.map',
  gnu_symbol_visibility : 'hidden',
  kwargs : shell_versions,
  # The SDK's ABI minor 1003 exceeds Mach-O's 8-bit version component.
  darwin_versions : ['0.0.0', '0.0.0'],
  install_rpath : shell_rpath,
  install : true)
