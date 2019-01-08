test_buffer = executable('test-buffer', 'test-buffer.c',
                        include_directories : [spa_inc ],
                        dependencies : [],
                        install : false)
test_utils = executable('test-utils', 'test-utils.c',
                        include_directories : [spa_inc ],
                        dependencies : [],
                        install : false)
test_cpp = executable('test-cpp', 'test-cpp.cpp',
                        include_directories : [spa_inc ],
                        dependencies : [],
                        install : false)

test('test-buffer', test_buffer)
test('test-utils', test_utils)
test('test-cpp', test_cpp)
