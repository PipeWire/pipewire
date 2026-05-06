tutorials = [
  'tutorial1',
  'tutorial2',
  'tutorial3',
  'tutorial4',
  'tutorial5',
  'tutorial6',
  'tutorial7',
]

if not get_option('examples').allowed()
  subdir_done()
endif

foreach c : tutorials
  executable(
    c,
    sources: c + '.c',
    dependencies: [ pipewire_dep, mathlib ],
    install: installed_tests_enabled,
    install_dir: installed_tests_execdir / 'examples',
  )
endforeach
