ffmpeg_sources = ['ffmpeg.c',
                  'ffmpeg-dec.c',
                  'ffmpeg-enc.c']

ffmpeglib = shared_library('spa-ffmpeg',
                          ffmpeg_sources,
                          include_directories : [spa_inc, spa_libinc],
                          dependencies : [ avcodec_dep, avformat_dep ],
                          link_with : spalib,
                          install : true,
                          install_dir : '@0@/spa'.format(get_option('libdir')))
