render_thread_test = executable('remix-render-thread-test', 'render_thread.cpp',
  include_directories : test_includes, dependencies : threads_dep)
test('Render thread handoff, draining and errors', render_thread_test, timeout : 30)
if get_option('gpu_tests')
  input_events_test = executable('remix-input-events-test', 'input_events.cpp',
    link_with : shell_platform, include_directories : test_includes,
    dependencies : [sdl3_dep, vulkan_dep, threads_dep])
  test('SDL raw events during UI capture', input_events_test, timeout : 30)
endif
