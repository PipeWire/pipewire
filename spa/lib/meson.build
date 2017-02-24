spalib_sources = ['audio-raw.c',
                  'debug.c',
                  'mapper.c',
                  'props.c',
                  'video-raw.c']

spalib = shared_library('spa-lib',
                         spalib_sources,
                         include_directories : [ spa_inc, spa_libinc ],
                         install : true)

spalib_dep = declare_dependency(link_with : spalib,
  include_directories : spa_inc,
)
