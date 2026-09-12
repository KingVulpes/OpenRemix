policy_test = executable('remix-tlas-policy-test', 'tlas_policy.cpp',
  include_directories : test_includes, link_with : pathtrace_library)
test('Tracing scene and TLAS policy', policy_test)
if get_option('gpu_tests')
  guide_cells_test = executable('remix-guide-cells-test', 'guide_cells.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('Colored guide cells and linear lighting energy', guide_cells_test,
    args : [meson.current_build_dir() / 'guide-cell-scenes'], timeout : 300)
  debug_views_test = executable('remix-debug-views-test', 'debug_views.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('Live diagnostics preserve lighting histories', debug_views_test,
    args : [meson.current_build_dir() / 'debug-view-scenes'], timeout : 300)
  adaptive_sampling_test = executable('remix-adaptive-sampling-test', 'adaptive_sampling.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('Adaptive light budget continuity and configured floors', adaptive_sampling_test,
    args : [meson.current_build_dir() / 'adaptive-sampling-scenes'],
    env : ['REMIX_EMISSIVE_HIT_DISTANCE=budget'], timeout : 180)
  texture_history_test = executable('remix-texture-history-test', 'texture_history.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('FSR texture history at translated world coordinates', texture_history_test,
    args : [meson.current_build_dir() / 'texture-history-scenes'], timeout : 240)
  upscale_test = executable('remix-upscale-test', 'upscale.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('FSR 3.1 presets and temporal composition', upscale_test,
    args : [meson.current_build_dir() / 'fsr-scenes'], timeout : 300)
  if system == 'linux'
    foreach fault : ['fsr-allocation', 'fsr-pipeline']
      test('FSR 3.1 fallback ' + fault, upscale_test,
        args : [meson.current_build_dir() / fault],
        env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=' + fault], timeout : 90)
    endforeach
  endif
  guide_boundary_test = executable('remix-guide-boundary-test', 'guide_boundary.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('Torch guide translation and sparse stationary feedback', guide_boundary_test,
    args : [meson.current_build_dir() / 'guide-boundary-scenes'], timeout : 180)
  torch_stability_test = executable('remix-torch-stability-test', 'torch_stability.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('Four torches and motion stability', torch_stability_test,
    args : [meson.current_build_dir() / 'torch-stability-scenes'], timeout : 300)
  test('Mixed emissive and torch sampling stability', torch_stability_test,
    args : [meson.current_build_dir() / 'mixed-light-scenes'],
    env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_ADAPTIVE_LOCAL=1'], timeout : 300)
  test('Near emissive solid-angle sampling and motion', torch_stability_test,
    args : [meson.current_build_dir() / 'angular-emission-scenes'],
    env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_SOLID_ANGLE=1'], timeout : 360)
  foreach angular : ['0', '1']
    test('Broad near emitter reference mode ' + angular, torch_stability_test,
      args : [meson.current_build_dir() / ('near-emitter-scenes-' + angular)],
      env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_NEAR_EMITTERS=1', 'REMIX_TEST_SOLID_ANGLE=' + angular], timeout : 360)
  endforeach
  test('Many emissive clusters and sampling stability', torch_stability_test,
    args : [meson.current_build_dir() / 'emissive-cluster-scenes'],
    env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_EMISSIVE_CLUSTERS=1', 'REMIX_TEST_ADAPTIVE_LOCAL=1'], timeout : 300)
  foreach mode : ['1', '2']
    test('Separate direct histories mode ' + mode, torch_stability_test,
      args : [meson.current_build_dir() / ('separate-light-scenes-' + mode)],
      env : ['REMIX_TEST_MIXED_LIGHTS=1', 'REMIX_TEST_ADAPTIVE_LOCAL=1', 'REMIX_TEST_SPLIT_LOCAL=' + mode], timeout : 360)
  endforeach
  water_indirect_test = executable('remix-water-indirect-test', 'water_indirect.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('Water indirect reuse and reference energy', water_indirect_test,
    args : [meson.current_build_dir() / 'water-indirect-scenes'], timeout : 240)
  local_light_test = executable('remix-local-light-test', 'local_lights.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('Local sphere lights and many-light hierarchy', local_light_test,
    args : [meson.current_build_dir() / 'local-light-scenes'], timeout : 300)
  shadow_test = executable('remix-shadow-test', 'shadows.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('SIGMA sun penumbra and temporal separation', shadow_test,
    args : [meson.current_build_dir() / 'shadow-scenes'], timeout : 240)
  if system == 'linux'
    test('Local visibility guiding resource fallback', local_light_test,
      args : [meson.current_build_dir() / 'guiding-fallback'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=light-guiding-allocation'], timeout : 90)
    test('Local visibility guiding shader fallback', local_light_test,
      args : [meson.current_build_dir() / 'guiding-shader-fallback'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=light-guiding-compute'], timeout : 90)
    test('Local light resource fallback', local_light_test,
      args : [meson.current_build_dir() / 'local-fallback'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=local-descriptors'], timeout : 90)
    foreach fault : ['sigma-allocation', 'sigma-descriptor-limit', 'sigma-compute']
      test('SIGMA fallback ' + fault, shadow_test,
        args : [meson.current_build_dir() / fault],
        env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=' + fault], timeout : 60)
    endforeach
  endif
  sharc_test = executable('remix-sharc-test', 'sharc.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('SHARC diffuse cache and temporal changes', sharc_test,
    args : [meson.current_build_dir() / 'sharc-scenes'], timeout : 240)
  if system == 'linux'
    foreach fault : ['sharc-compute', 'sharc-capability', 'sharc-allocation']
      test('SHARC fallback ' + fault, sharc_test,
        args : [meson.current_build_dir() / fault],
        env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=' + fault], timeout : 45)
    endforeach
  endif
  environment_test = executable('remix-environment-test', 'environment.cpp',
    include_directories : test_includes, dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('Sky illumination and temporal changes', environment_test,
    args : [meson.current_build_dir() / 'sky-scenes'], timeout : 240)
  test('Emissive maps and sampled area lights', environment_test,
    args : [meson.current_build_dir() / 'emission-scenes'], env : ['REMIX_TEST_EMISSION=1'], timeout : 240)
  test('Stable emissive casting preserves maps and controls', environment_test,
    args : [meson.current_build_dir() / 'stable-emission-scenes'],
    env : ['REMIX_TEST_EMISSION=1', 'REMIX_TEST_EMISSION_STABILITY=1'], timeout : 240)
  test('Prism and gamma display transfer', environment_test,
    args : [meson.current_build_dir() / 'prism-scenes'], env : ['REMIX_TEST_DISPLAY=1'], timeout : 240)
  if system == 'linux'
    test('Prism and gamma UNORM display transfer', environment_test,
      args : [meson.current_build_dir() / 'prism-unorm-scenes'],
      env : ['REMIX_TEST_DISPLAY=1', 'LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=unorm-swapchain'], timeout : 240)
  endif
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
  test('FSR 3.1 stationary edge alignment', pathtrace_test,
    args : [meson.current_build_dir() / 'fsr-edge'], env : ['REMIX_TEST_FSR_STABILITY=1'], timeout : 180)
  test('FSR 3.1 stationary edge with shared atlas animation', pathtrace_test,
    args : [meson.current_build_dir() / 'fsr-atlas-edge'],
    env : ['REMIX_TEST_FSR_STABILITY=1', 'REMIX_TEST_TAA_ATLAS=1'], timeout : 180)
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
    test('Tracing transparent pipeline failure fallback', pathtrace_test,
      args : [meson.current_build_dir() / 'pathtrace-scenes', 'init-failure'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=transparency-compute'], timeout : 30)
  endif
endif
