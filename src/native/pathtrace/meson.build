pathtrace_library = static_library('remix_pathtrace',
  'light_guiding.cpp', 'local_lights.cpp', 'sharc.cpp', 'gbuffer.cpp', 'temporal.cpp', 'blas.cpp', 'geometry.cpp', 'memory.cpp', 'renderer.cpp', 'rendering.cpp', 'tlas.cpp', 'tlas_policy.cpp',
  link_with : [scene_library, raster_library, denoise_library],
  dependencies : [vulkan_dep, nrd_dep], include_directories : [remix_public_include, native_include, image_include],
  cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'),
  gnu_symbol_visibility : 'hidden', pic : true)
