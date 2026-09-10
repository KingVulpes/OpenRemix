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
