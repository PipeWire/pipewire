pwtest_sources = [
  'pwtest.h',
  'pwtest-implementation.h',
  'pwtest.c',
]

pwtest_deps = [
  pipewire_dep,
  mathlib,
  dl_lib,
]

pwtest_c_args = [
  '-DHAVE_CONFIG_H',
  '-DBUILD_ROOT="@0@"'.format(meson.build_root()),
  '-DSOURCE_ROOT="@0@"'.format(meson.source_root()),
]

pwtest_inc = [
  spa_inc,
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

# Compilation only, this is the example file for how pwtest works and most
# of its tests will fail.
executable('test-example',
  'test-example.c',
  include_directories: pwtest_inc,
  link_with: pwtest_lib)

test('test pipewire utils',
    executable('test-pw-utils',
               'test-properties.c',
               'test-array.c',
               include_directories: pwtest_inc,
               link_with: pwtest_lib)
)
test('test lib',
    executable('test-lib',
               'test-lib.c',
               include_directories: pwtest_inc,
               link_with: pwtest_lib)
)

test('test spa',
    executable('test-spa',
               'test-spa-buffer.c',
               'test-spa-json.c',
               include_directories: pwtest_inc,
               link_with: pwtest_lib)
)
