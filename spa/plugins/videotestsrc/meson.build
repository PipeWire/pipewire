videotestsrc_sources = ['videotestsrc.c', 'plugin.c']

videotestsrclib = shared_library('spa-videotestsrc',
  videotestsrc_sources,
  dependencies : [ spa_dep, pthread_lib ],
  install : true,
  install_dir : spa_plugindir / 'videotestsrc')
