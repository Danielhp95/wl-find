project('wl-find-ocr', 'cpp',
  version : '0.1.0',
  default_options : ['cpp_std=c++20', 'warning_level=3', 'buildtype=release', 'b_ndebug=if-release'])

ocr = static_library('ocr', 'ocr.cc')

# The directory with det.onnx and rec.onnx, unless --det and --rec say otherwise.
executable('wl-find-ocr', 'main.cc',
  link_with : ocr,
  dependencies : [dependency('libonnxruntime'), dependency('threads')],
  cpp_args : ['-DWL_FIND_MODELS="' + get_option('models') + '"'],
  install : true)

test('unit', executable('ocr-test', 'test.cc', link_with : ocr))
