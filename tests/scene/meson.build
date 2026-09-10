scene_test = executable('remix-scene-test', 'resources.cpp',
  include_directories : test_includes, link_with : scene_library)
configure_file(input : 'fixtures/colors.png', output : 'raster-colors.png', copy : true)
test('Scene resources and image decoding', scene_test, args : [meson.current_build_dir() / 'resources'])
temporal_test = executable('remix-temporal-test', 'temporal.cpp',
  include_directories : test_includes, link_with : scene_library)
test('Temporal submitted-frame ownership and correspondence', temporal_test)
