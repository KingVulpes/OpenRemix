scene_test = executable('remix-scene-test', 'resources.cpp',
  include_directories : test_includes, link_with : scene_library)
configure_file(input : 'fixtures/colors.png', output : 'raster-colors.png', copy : true)
test('Scene resources and image decoding', scene_test, args : [meson.current_build_dir() / 'resources'])
temporal_test = executable('remix-temporal-test', 'temporal.cpp',
  include_directories : test_includes, link_with : scene_library)
test('Temporal submitted-frame ownership and correspondence', temporal_test)
light_bvh_test = executable('remix-light-bvh-test', 'light_bvh.cpp',
  include_directories : test_includes, link_with : scene_library)
test('Analytic lights and importance hierarchy', light_bvh_test)
emission_test = executable('remix-emission-test', 'emission.cpp',
  include_directories : test_includes, link_with : scene_library)
test('Emission reduction and area light ownership', emission_test)
trace_frame_test = executable('remix-trace-frame-test', 'trace_frame.cpp',
  include_directories : test_includes, link_with : scene_library)
test('CPU tracing preparation and snapshot ownership', trace_frame_test)
pbr_test = executable('remix-pbr-resource-test', 'pbr.cpp',
  include_directories : test_includes, link_with : scene_library)
test('PBR material validation and texture lifetime', pbr_test)
opacity_test = executable('remix-opacity-test', 'opacity.cpp',
  include_directories : test_includes, link_with : scene_library)
test('Opaque traversal atlas coverage and snapshot ownership', opacity_test)
compressed_test = executable('remix-compressed-texture-test', 'compressed.cpp',
  include_directories : test_includes, link_with : scene_library)
test('Compressed BC textures, coverage and aliasing', compressed_test)
usd_lights_test = executable('remix-usd-lights-test', 'usd_lights.cpp', include_directories : test_includes, link_with : scene_library)
test('USD light conversion and BC4 BC6H assets', usd_lights_test)
