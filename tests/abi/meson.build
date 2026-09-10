abi_test = executable('remix-abi-test', 'layout.c', include_directories : remix_public_include)
test('C ABI layouts', abi_test)
test('SDK and export baseline', python, args : [files('audit.py'), meson.project_source_root(), remix.full_path()])
