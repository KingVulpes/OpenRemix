if get_option('nrd')
  dispatch_test = executable('remix-nrd-dispatch-test', 'dispatch.cpp',
    include_directories : test_includes, dependencies : nrd_dep)
  test('Pinned NRD diffuse SH dispatch contract', dispatch_test)
endif
