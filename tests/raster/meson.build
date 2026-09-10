if get_option('gpu_tests')
  raster_test = executable('remix-raster-test', 'rendering.cpp',
    include_directories : test_includes, dependencies : vulkan_dep, link_with : raster_library,
    cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'))
  test('Raster GPU pixel readback', raster_test, args : [meson.current_build_dir() / 'readback'], timeout : 90)
endif
