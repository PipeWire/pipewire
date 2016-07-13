executable('test-mixer', 'test-mixer.c',
           include_directories : inc,
           dependencies : [dl_lib, pthread_lib],
           link_with : spalib,
           install : false)

executable('test-v4l2', 'test-v4l2.c',
           include_directories : inc,
           dependencies : [dl_lib, sdl_dep, pthread_lib],
           link_with : spalib,
           install : false)
