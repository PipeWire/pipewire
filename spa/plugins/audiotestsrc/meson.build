audiotestsrc_sources = ['audiotestsrc.c', 'plugin.c']

audiotestsrclib = shared_library('spa-audiotestsrc',
                          audiotestsrc_sources,
                          dependencies : [ spa_dep, mathlib ],
                          install : true,
                          install_dir : spa_plugindir / 'audiotestsrc')
