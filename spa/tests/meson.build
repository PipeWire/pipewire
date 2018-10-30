subdir('auto')

executable('test-mixer', 'test-mixer.c',
           include_directories : [spa_inc ],
           dependencies : [dl_lib, pthread_lib, mathlib],
           install : false)
executable('test-convert', 'test-convert.c',
           include_directories : [spa_inc ],
           dependencies : [dl_lib, pthread_lib, mathlib],
           install : false)
executable('test-convert2', 'test-convert2.c',
           include_directories : [spa_inc ],
           dependencies : [dl_lib, pthread_lib, mathlib],
           install : false)
executable('test-bluez5', 'test-bluez5.c',
           include_directories : [spa_inc ],
           dependencies : [dl_lib, pthread_lib, mathlib, dbus_dep],
           install : false)
executable('test-ringbuffer', 'test-ringbuffer.c',
           include_directories : [spa_inc ],
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
           include_directories : [spa_inc ],
           dependencies : [dl_lib, pthread_lib],
           install : false)
executable('stress-ringbuffer', 'stress-ringbuffer.c',
           include_directories : [spa_inc ],
           dependencies : [dl_lib, pthread_lib],
           install : false)
if sdl_dep.found()
  executable('test-v4l2', 'test-v4l2.c',
             include_directories : [spa_inc ],
             dependencies : [dl_lib, sdl_dep, pthread_lib],
             install : false)
endif
executable('test-props', 'test-props.c',
           include_directories : [spa_inc ],
           dependencies : [],
           install : false)
executable('test-props2', 'test-props2.c',
           include_directories : [spa_inc ],
           dependencies : [],
           install : false)
#executable('test-props4', 'test-props4.c',
#           include_directories : [spa_inc ],
#           dependencies : [],
#           install : false)
#executable('test-props5', 'test-props5.c',
#           include_directories : [spa_inc ],
#           dependencies : [],
#           install : false)
executable('test-control', 'test-control.c',
           include_directories : [spa_inc ],
           dependencies : [dl_lib, pthread_lib, mathlib],
           install : false)
