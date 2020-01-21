if sdl_dep.found()
  executable('local-v4l2', 'local-v4l2.c',
             include_directories : [spa_inc ],
             dependencies : [dl_lib, sdl_dep, pthread_lib],
             install : false)
endif

executable('example-control', 'example-control.c',
             include_directories : [spa_inc ],
             dependencies : [dl_lib, pthread_lib, mathlib],
             install : false)

executable('adapter-control', 'adapter-control.c',
             include_directories : [spa_inc ],
             dependencies : [dl_lib, pthread_lib, mathlib],
             install : false)
