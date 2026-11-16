project('codec2-gbinder', 'cpp', version: '0.1.0',
  default_options: ['cpp_std=c++17', 'warning_level=3'])

gbinder = dependency('libgbinder', version: '>=1.1.43')
glib = dependency('glib-2.0')

codec2 = static_library('codec2-gbinder',
  'src/hidl.cpp', 'src/store.cpp', 'src/metadata.cpp',
  'src/params.cpp', 'src/component.cpp',
  'src/work.cpp', 'src/dmabuf.cpp',
  'src/frame.cpp', 'src/av1_packet.cpp',
  dependencies: [gbinder, glib])
c2_dep = declare_dependency(link_with: codec2,
  include_directories: include_directories('src'),
  dependencies: [gbinder, glib])

executable('c2-list', 'src/list.cpp', dependencies: c2_dep, install: true)
c2_create = executable('c2-create', 'src/create.cpp', dependencies: c2_dep, install: true)
av = [dependency('libavformat'), dependency('libavcodec'), dependency('libavutil')]
executable('c2-decode', 'src/decode.cpp', 'src/input.cpp',
  dependencies: [c2_dep, av], install: true)

amf_sdk = get_option('amf_sdk')
if amf_sdk != ''
  amf_includes = ['-isystem', amf_sdk]
  meson.get_compiler('cpp').has_header('AMF/components/VideoDecoderUVD.h',
    args: amf_includes, required: true)
  shared_library('amfrt64', 'src/amf/runtime.cpp', 'src/amf/host.cpp', 'src/amf/decoder.cpp',
    dependencies: c2_dep, cpp_args: amf_includes, soversion: '1',
    gnu_symbol_visibility: 'hidden',
    link_args: ['-Wl,-z,nodelete', '-Wl,--exclude-libs,ALL'])
endif

va = dependency('libva', version: '>=1.16', required: false)
drm = dependency('libdrm', required: false)
if va.found() and drm.found()
  shared_module('codec2_drv_video', 'src/va_driver.cpp', 'src/va_h264.cpp', 'src/va_hevc.cpp',
    'src/va_encode.cpp', 'src/va_encode_pixels.cpp', 'src/v4l2_encoder.cpp',
    dependencies: [c2_dep, va, drm], name_prefix: '', gnu_symbol_visibility: 'hidden',
    link_args: ['-Wl,-z,nodelete', '-Wl,--exclude-libs,ALL'])
  executable('c2-prime-test', 'tests/mtk-prime.cpp', dependencies: [c2_dep, va, drm, av])
endif

core_tests = executable('c2-tests', 'tests/core.cpp', dependencies: c2_dep)
test('codec2 core', core_tests, args: [files('tests/media_codecs.xml')], protocol: 'tap')
test('c2-create arguments', find_program('sh'), args: [files('tests/create-args.sh'), c2_create])
