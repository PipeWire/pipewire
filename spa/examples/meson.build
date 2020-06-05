if sdl_dep.found()
  executable('local-v4l2', 'local-v4l2.c',
             include_directories : [configinc, spa_inc],
             c_args : ['-D_GNU_SOURCE'],
             dependencies : [dl_lib, sdl_dep, pthread_lib],
             install : false)

  if get_option('libcamera') and libcamera_dep.found()
    executable('local-libcamera', 'local-libcamera.c',
               include_directories : [configinc, spa_inc],
               c_args : ['-D_GNU_SOURCE'],
               dependencies : [dl_lib, sdl_dep, pthread_lib, libcamera_dep],
               install : false)
  endif
endif

executable('example-control', 'example-control.c',
             include_directories : [configinc, spa_inc],
             c_args : ['-D_GNU_SOURCE'],
             dependencies : [dl_lib, pthread_lib, mathlib],
             install : false)

executable('adapter-control', 'adapter-control.c',
             include_directories : [configinc, spa_inc],
             c_args : ['-D_GNU_SOURCE'],
             dependencies : [dl_lib, pthread_lib, mathlib],
             install : false)
