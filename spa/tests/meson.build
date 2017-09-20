executable('test-mixer', 'test-mixer.c',
           include_directories : [spa_inc ],
           dependencies : [dl_lib, pthread_lib],
           install : false)
executable('test-ringbuffer', 'test-ringbuffer.c',
           include_directories : [spa_inc, spa_libinc ],
           dependencies : [dl_lib, pthread_lib],
           install : false)
executable('test-graph', 'test-graph.c',
           include_directories : [spa_inc ],
           dependencies : [dl_lib, pthread_lib],
           install : false)
executable('test-graph2', 'test-graph2.c',
           include_directories : [spa_inc ],
           dependencies : [dl_lib, pthread_lib],
           install : false)
executable('test-perf', 'test-perf.c',
           include_directories : [spa_inc, spa_libinc ],
           dependencies : [dl_lib, pthread_lib],
           install : false)
executable('stress-ringbuffer', 'stress-ringbuffer.c',
           include_directories : [spa_inc, spa_libinc ],
           dependencies : [dl_lib, pthread_lib],
           install : false)
if sdl_dep.found()
  executable('test-v4l2', 'test-v4l2.c',
             include_directories : [spa_inc, spa_libinc ],
             dependencies : [dl_lib, sdl_dep, pthread_lib],
             link_with : spalib,
             install : false)
endif
executable('test-props', 'test-props.c',
           include_directories : [spa_inc, spa_libinc ],
           dependencies : [],
           link_with : spalib,
           install : false)
executable('test-props2', 'test-props2.c',
           include_directories : [spa_inc, spa_libinc ],
           dependencies : [],
           link_with : spalib,
           install : false)
executable('test-props3', 'test-props3.c',
           include_directories : [spa_inc, spa_libinc ],
           dependencies : [],
           link_with : spalib,
           install : false)
