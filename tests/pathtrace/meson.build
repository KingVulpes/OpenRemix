policy_test = executable('remix-tlas-policy-test', 'tlas_policy.cpp',
  include_directories : test_includes, link_with : pathtrace_library)
test('Tracing scene and TLAS policy', policy_test)
if get_option('gpu_tests')
  pathtrace_test = executable('remix-pathtrace-test', 'rendering.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('Path tracing capabilities and GPU scenes', pathtrace_test,
    args : [meson.current_build_dir() / 'pathtrace-scenes'], timeout : 180)
  test('Diffuse SH convergence against raw reference', pathtrace_test,
    args : [meson.current_build_dir() / 'denoise-quality'], env : ['REMIX_TEST_DENOISE_QUALITY=1'], timeout : 240)
  test('TAA stationary edge and disabled jitter', pathtrace_test,
    args : [meson.current_build_dir() / 'taa-stability'], env : ['REMIX_TEST_TAA_STABILITY=1'], timeout : 180)
  test('TAA stationary edge with shared atlas animation', pathtrace_test,
    args : [meson.current_build_dir() / 'taa-atlas-stability'],
    env : ['REMIX_TEST_TAA_STABILITY=1', 'REMIX_TEST_TAA_ATLAS=1'], timeout : 180)
  test('Tracing presentation and lifecycle', runtime_test, args : [remix.full_path(), 'gpu'],
    env : ['REMIX_TEST_BACKEND=pathtrace'], timeout : 180)
  if system != 'windows'
    test('Tracing live process exit cleanup', runtime_test, args : [remix.full_path(), 'exit'],
      env : ['REMIX_TEST_BACKEND=pathtrace'], timeout : 30)
  endif
  if system == 'linux'
    foreach fault : ['submit', 'device-lost']
      test('Tracing failure cleanup ' + fault, runtime_test, args : [remix.full_path(), 'fault'],
        env : ['REMIX_TEST_BACKEND=pathtrace', 'LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=' + fault], timeout : 30)
    endforeach
    test('Temporal descriptor limits retain raw tracing', pathtrace_test,
      args : [meson.current_build_dir() / 'descriptor-limit-scenes'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=temporal-descriptor-limit'], timeout : 45)
    test('Temporal allocation failure retains raw tracing', pathtrace_test,
      args : [meson.current_build_dir() / 'pathtrace-scenes', 'denoise-failure'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=denoise-allocation'], timeout : 45)
    test('Tracing initialization failure fallback', pathtrace_test,
      args : [meson.current_build_dir() / 'pathtrace-scenes', 'init-failure'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=compute'], timeout : 30)
  endif
endif
