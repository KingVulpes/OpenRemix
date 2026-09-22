fsr31_include = include_directories('include', 'src/shared', 'src/backends/shared', is_system : true)
fsr31_sdk = static_library('fsr31_sdk',
  'src/components/fsr3upscaler/ffx_fsr3upscaler.cpp',
  'src/backends/vk/ffx_vk.cpp',
  'src/shared/ffx_assert.cpp', 'src/shared/ffx_message.cpp',
  'src/shared/ffx_object_management.cpp', 'src/shared/ffx_breadcrumbs_list.cpp',
  '../../src/native/upscale/shader_blobs.cpp',
  include_directories : fsr31_include, dependencies : vulkan_dep,
  cpp_args : [cpp.get_id() == 'clang-cl' ? '/FI' : '-include', meson.project_source_root() / 'src/native/upscale/sdk_compat.h'],
  gnu_symbol_visibility : 'hidden', override_options : ['warning_level=0', 'werror=false'], pic : true)
fsr31_dep = declare_dependency(link_with : fsr31_sdk, include_directories : fsr31_include)
install_data('LICENSE.txt', rename : 'FidelityFX.txt', install_dir : get_option('datadir') / 'remix/licenses')
