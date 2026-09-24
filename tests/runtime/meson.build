runtime_test = executable('remix-runtime-test', 'lifecycle.cpp',
  include_directories : [test_includes, include_directories('../../samples')], dependencies : [sdl3_dep, dl_dep, threads_dep])
test('API stubs and failed startup cleanup', runtime_test, args : [remix.full_path()], timeout : 30)
if get_option('gpu_tests')
  test('Vulkan presentation and lifecycle', runtime_test, args : [remix.full_path(), 'gpu'], timeout : 90)
  if system != 'windows'
    test('Live process exit cleanup', runtime_test, args : [remix.full_path(), 'exit'], timeout : 30)
  endif
  if system == 'linux'
    faults = shared_library('remix-vulkan-faults', 'faults.cpp', dependencies : [vulkan_dep, dl_dep])
    foreach fault : ['instance', 'device', 'framebuffer', 'submit', 'device-lost']
      test('Vulkan failure cleanup ' + fault, runtime_test, args : [remix.full_path(), 'fault'],
        env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=' + fault], timeout : 30)
    endforeach
    foreach fault : ['acquire-out-of-date', 'acquire-suboptimal', 'present-suboptimal', 'surface-lost']
      test('Vulkan recovery ' + fault, runtime_test, args : [remix.full_path(), 'recover'],
        env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=' + fault], timeout : 30)
    endforeach
  endif
endif

profile_test = executable('remix-profiles-test', 'profiles.cpp',
  '../../src/native/runtime/settings.cpp', '../../src/native/runtime/profiles.cpp',
  '../../src/native/ui/profile_actions.cpp', include_directories : native_include,
  dependencies : filesystem_dep)
test('Configuration profiles and unsaved actions', profile_test, args : ['@CONFIG_DIR@'])
if get_option('gpu_tests')
  profile_runtime_test = executable('remix-profile-runtime-test', 'profile_runtime.cpp',
    '../../src/native/runtime/settings.cpp', '../../src/native/runtime/profiles.cpp',
    include_directories : [test_includes, include_directories('../../samples')],
    dependencies : [sdl3_dep, filesystem_dep], link_with : remix)
  test('Profile startup and API precedence', profile_runtime_test,
    args : ['@CONFIG_DIR@', meson.current_build_dir() / 'profile-images'], timeout : 600, is_parallel : false)
  if system == 'linux'
    test('Profile preferences survive unsupported backend', profile_runtime_test,
      args : ['@CONFIG_DIR@', meson.current_build_dir() / 'profile-fallback-images'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=pathtrace-capability'], timeout : 60, depends : faults, is_parallel : false)
    test('Profile panel at high DPI', profile_runtime_test,
      args : ['@CONFIG_DIR@', meson.current_build_dir() / 'profile-hidpi-images'],
      env : ['LD_PRELOAD=' + faults.full_path(), 'REMIX_TEST_FAULT=ui-high-dpi', 'REMIX_TEST_PROFILE_UI_ONLY=1'], timeout : 60, depends : faults, is_parallel : false)
  endif
endif
