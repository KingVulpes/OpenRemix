postfx_cpu_test = executable('remix-postfx-cpu-test', 'meter.cpp',
  '../../src/native/runtime/settings.cpp', include_directories : native_include)
test('Exposure math, bloom sizing and profile defaults', postfx_cpu_test)
if get_option('gpu_tests')
  postfx_gpu_test = executable('remix-postfx-gpu-test', 'gpu.cpp',
    include_directories : test_includes, dependencies : [vulkan_dep, threads_dep],
    link_with : [postfx_library, pipeline_library],
    cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'))
  test('Deterministic GPU exposure and bloom', postfx_gpu_test, timeout : 120, is_parallel : false)
endif
if get_option('gpu_tests') and system == 'linux'
  subdir('upscale')
  postfx_runtime_test = executable('remix-postfx-runtime-test', 'runtime.cpp',
    include_directories : test_includes, dependencies : [runtime_test_dep, dl_dep])
  test('Post-processing material guides and live lifecycle', postfx_runtime_test,
    args : [meson.current_build_dir() / 'runtime-images'], timeout : 1500, is_parallel : false)
endif
