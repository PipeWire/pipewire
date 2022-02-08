pwtest_sources = [
  'pwtest.h',
  'pwtest-implementation.h',
  'pwtest.c',
  'pwtest-compat.c',
]

pwtest_deps = [
  pipewire_dep,
  mathlib,
  dl_lib,
  cap_lib,
  epoll_shim_dep
]

pwtest_c_args = [
  '-DBUILD_ROOT="@0@"'.format(meson.project_build_root()),
  '-DSOURCE_ROOT="@0@"'.format(meson.project_source_root()),
]

pwtest_inc = [
  pipewire_inc,
  configinc,
  includes_inc,
]

pwtest_lib = static_library(
  'pwtest',
  pwtest_sources,
  c_args: pwtest_c_args,
  dependencies: pwtest_deps,
  include_directories: pwtest_inc,
)

test('test-pwtest',
    executable('test-pwtest',
               'test-pwtest.c',
               include_directories: pwtest_inc,
               dependencies: [ spa_dep ],
               link_with: pwtest_lib)
)

# Compilation only, this is the example file for how pwtest works and most
# of its tests will fail.
executable('test-example',
  'test-example.c',
  include_directories: pwtest_inc,
  dependencies: [ spa_dep ],
  link_with: pwtest_lib)

test('test-pw-utils',
    executable('test-pw-utils',
               'test-properties.c',
               'test-array.c',
               'test-map.c',
               'test-utils.c',
               include_directories: pwtest_inc,
               dependencies: [ spa_dep ],
               link_with: pwtest_lib)
)

test('test-lib',
    executable('test-lib',
               'test-lib.c',
               include_directories: pwtest_inc,
               dependencies: [ spa_dep ],
               link_with: pwtest_lib)
)

test('test-client',
    executable('test-client',
               'test-client.c',
               include_directories: pwtest_inc,
               dependencies: [ spa_dep ],
               link_with: pwtest_lib)
)

test('test-loop',
    executable('test-loop',
               'test-loop.c',
               include_directories: pwtest_inc,
               dependencies: [ spa_dep ],
               link_with: pwtest_lib)
)

test('test-context',
    executable('test-context',
               'test-context.c',
               'test-config.c',
               include_directories: pwtest_inc,
               dependencies: [spa_dep, spa_support_dep, spa_dbus_dep],
               link_with: [pwtest_lib,
                            pipewire_module_protocol_native,
                            pipewire_module_client_node,
                            pipewire_module_client_device,
                            pipewire_module_adapter,
                            pipewire_module_metadata,
                            pipewire_module_session_manager])
)

test('test-support',
    executable('test-support',
               'test-support.c',
               'test-logger.c',
               include_directories: pwtest_inc,
               dependencies: [spa_dep, systemd_dep, spa_support_dep, spa_journal_dep],
               link_with: [pwtest_lib])
)
test('test-spa',
    executable('test-spa',
               'test-spa-buffer.c',
               'test-spa-json.c',
               'test-spa-utils.c',
               'test-spa-log.c',
               'test-spa-node.c',
               'test-spa-pod.c',
               include_directories: pwtest_inc,
               dependencies: [ spa_dep ],
               link_with: pwtest_lib)
)

valgrind = find_program('valgrind', required: false)
summary({'valgrind (test setup)': valgrind.found()}, bool_yn: true, section: 'Optional programs')
if valgrind.found()
  valgrind_env = environment({'PIPEWIRE_DEBUG': 'D'})
  add_test_setup('valgrind',
                 exe_wrapper : [ valgrind,
                   '--leak-check=full',
                   '--gen-suppressions=all',
                   '--error-exitcode=3',
                   ],
                 env :  valgrind_env,
                 timeout_multiplier : 3)
endif
