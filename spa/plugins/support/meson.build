spa_support_sources = [
  'cpu.c',
  'logger.c',
  'log-patterns.c',
  'loop.c',
  'node-driver.c',
  'null-audio-sink.c',
  'plugin.c',
  'system.c'
]

simd_cargs = []

if have_sse
  simd_cargs += [sse_args, '-DHAVE_SSE']
endif

spa_support_lib = shared_library('spa-support',
  spa_support_sources,
  c_args : [ simd_cargs ],
  dependencies : [ spa_dep, pthread_lib, epoll_shim_dep ],
  install : true,
  install_dir : spa_plugindir / 'support')
spa_support_dep = declare_dependency(link_with: spa_support_lib)

if get_option('evl').allowed()
  evl_inc = include_directories('/usr/evl/include')
  evl_lib = cc.find_library('evl',
                            dirs: ['/usr/evl/lib/'],
                            required: get_option('evl'))

  spa_evl_sources = ['evl-system.c', 'evl-plugin.c']

  spa_evl_lib = shared_library('spa-evl',
    spa_evl_sources,
    include_directories : [ evl_inc],
    dependencies : [ spa_dep, pthread_lib, evl_lib ],
    install : true,
    install_dir : spa_plugindir / 'support')
endif

if dbus_dep.found()
  spa_dbus_sources = ['dbus.c']

  spa_dbus_lib = shared_library('spa-dbus',
    spa_dbus_sources,
    dependencies : [ spa_dep, dbus_dep ],
    install : true,
    install_dir : spa_plugindir / 'support')
  spa_dbus_dep = declare_dependency(link_with: spa_dbus_lib)
else
  spa_dbus_dep = declare_dependency()
endif


if systemd_dep.found()
  spa_journal_sources = [
    'journal.c',
    'log-patterns.c',
  ]

  spa_journal_lib = shared_library('spa-journal',
    spa_journal_sources,
    dependencies : [ spa_dep, systemd_dep ],
    install : true,
    install_dir : spa_plugindir / 'support')
  spa_journal_dep = declare_dependency(link_with: spa_journal_lib)
else
  spa_journal_dep = declare_dependency()
endif
