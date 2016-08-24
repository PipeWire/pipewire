spalib_sources = ['audio-raw.c',
                  'buffer.c',
                  'control.c',
                  'debug.c',
                  'memory.c',
                  'props.c',
                  'ringbuffer.c',
                  'video-raw.c']

spalib = shared_library('spa-lib',
                         spalib_sources,
                         include_directories : inc,
                         install : true)
spalibs = static_library('spa-lib',
                         spalib_sources,
                         include_directories : inc,
                         install : true)
