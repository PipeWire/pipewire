plugin_dependencies = []
if get_option('spa-plugins').allowed()
  plugin_dependencies += audioconvert_dep
endif

simd_cargs = []
simd_dependencies = []

if have_sse
  filter_graph_sse = static_library('filter_graph_sse',
    ['pffft.c',
     'audio-dsp-sse.c' ],
    include_directories : [configinc],
    c_args : [sse_args, '-O3', '-DHAVE_SSE'],
    dependencies : [ spa_dep ],
    install : false
    )
  simd_cargs += ['-DHAVE_SSE']
  simd_dependencies += filter_graph_sse
endif
if have_avx2
  filter_graph_avx2 = static_library('filter_graph_avx2',
    ['audio-dsp-avx2.c' ],
    include_directories : [configinc],
    c_args : [avx2_args, fma_args,'-O3', '-DHAVE_AVX2'],
    dependencies : [ spa_dep ],
    install : false
    )
  simd_cargs += ['-DHAVE_AVX2']
  simd_dependencies += filter_graph_avx2
endif
if have_neon
  filter_graph_neon = static_library('filter_graph_neon',
    ['pffft.c' ],
    c_args : [neon_args, '-O3', '-DHAVE_NEON'],
    dependencies : [ spa_dep ],
    install : false
    )
  simd_cargs += ['-DHAVE_NEON']
  simd_dependencies += filter_graph_neon
endif

filter_graph_c = static_library('filter_graph_c',
  ['pffft.c',
   'audio-dsp.c',
   'audio-dsp-c.c' ],
  include_directories : [configinc],
  c_args : [simd_cargs, '-O3', '-DPFFFT_SIMD_DISABLE'],
  dependencies : [ spa_dep, fftw_dep],
  install : false
)
simd_dependencies += filter_graph_c

spa_filter_graph = shared_library('spa-filter-graph',
  ['filter-graph.c' ],
  include_directories : [configinc],
  dependencies : [ spa_dep, sndfile_dep, plugin_dependencies, mathlib ],
  install : true,
  install_dir : spa_plugindir / 'filter-graph',
  objects : audioconvert_c.extract_objects('biquad.c'),
  link_with: simd_dependencies
)


filter_graph_dependencies = [
  spa_dep, mathlib, sndfile_dep, plugin_dependencies
]

spa_filter_graph_plugin_builtin = shared_library('spa-filter-graph-plugin-builtin',
  [ 'builtin_plugin.c',
    'convolver.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : spa_plugindir / 'filter-graph',
  dependencies : [ filter_graph_dependencies ],
  objects : audioconvert_c.extract_objects('biquad.c')
)

spa_filter_graph_plugin_ladspa = shared_library('spa-filter-graph-plugin-ladspa',
  [ 'ladspa_plugin.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : spa_plugindir / 'filter-graph',
  dependencies : [ filter_graph_dependencies, dl_lib ]
)

if libmysofa_dep.found()
spa_filter_graph_plugin_sofa = shared_library('spa-filter-graph-plugin-sofa',
  [ 'sofa_plugin.c',
    'convolver.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : spa_plugindir / 'filter-graph',
  dependencies : [ filter_graph_dependencies, libmysofa_dep ]
)
endif

if lilv_lib.found()
spa_filter_graph_plugin_lv2 = shared_library('spa-filter-graph-plugin-lv2',
  [ 'lv2_plugin.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : spa_plugindir / 'filter-graph',
  dependencies : [ filter_graph_dependencies, lilv_lib ]
)
endif

if ebur128_lib.found()
spa_filter_graph_plugin_ebur128 = shared_library('spa-filter-graph-plugin-ebur128',
  [ 'ebur128_plugin.c' ],
  include_directories : [configinc],
  install : true,
  install_dir : spa_plugindir / 'filter-graph',
  dependencies : [ filter_graph_dependencies, lilv_lib, ebur128_lib ]
)
endif

