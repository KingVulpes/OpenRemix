if get_option('gpu_tests')
  input_events_test = executable('remix-input-events-test', 'input_events.cpp',
    link_with : shell_platform, include_directories : test_includes,
    dependencies : [sdl3_dep, vulkan_dep, threads_dep])
  test('SDL raw events during UI capture', input_events_test, timeout : 30)
endif
