test_array = executable('test-array', 'test-array.c',
                        dependencies : [pipewire_dep],
                        install : false)
test('test-array', test_array)

if have_cpp
test_cpp = executable('test-cpp', 'test-cpp.cpp',
                        dependencies : [pipewire_dep],
                        install : false)
test('test-cpp', test_cpp)
endif
