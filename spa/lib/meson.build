spalib_sources = ['audio-raw.c',
                  'buffer.c',
                  'control.c',
                  'debug.c',
                  'format.c',
                  'props.c',
                  'ringbuffer.c',
                  'video-raw.c']

spalib = shared_library('spa-lib',
                         spalib_sources,
                         include_directories : spa_inc,
                         install : true)

spalib_dep = declare_dependency(link_with : spalib,
  include_directories : spa_inc,
)
