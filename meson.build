project('codec2-gbinder', 'cpp', version: '0.1.0',
  default_options: ['cpp_std=c++17', 'warning_level=3'])

gbinder = dependency('libgbinder', version: '>=1.1.43')
glib = dependency('glib-2.0')

codec2 = static_library('codec2-gbinder',
  'src/hidl.cpp', 'src/store.cpp', 'src/metadata.cpp',
  'src/params.cpp', 'src/component.cpp',
  dependencies: [gbinder, glib])
c2_dep = declare_dependency(link_with: codec2,
  include_directories: include_directories('src'),
  dependencies: [gbinder, glib])

executable('c2-list', 'src/list.cpp', dependencies: c2_dep, install: true)
c2_create = executable('c2-create', 'src/create.cpp', dependencies: c2_dep, install: true)

core_tests = executable('c2-tests', 'tests/core.cpp', dependencies: c2_dep)
test('codec2 core', core_tests, args: [files('tests/media_codecs.xml')], protocol: 'tap')
test('c2-create arguments', find_program('sh'), args: [files('tests/create-args.sh'), c2_create])
