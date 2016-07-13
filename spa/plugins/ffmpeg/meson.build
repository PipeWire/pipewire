ffmpeg_sources = ['ffmpeg.c',
                  'ffmpeg-dec.c',
                  'ffmpeg-enc.c']

ffmpeglib = shared_library('spa-ffmpeg',
                          ffmpeg_sources,
                          include_directories : inc,
                          dependencies : [ avcodec_dep, avformat_dep ],
                          link_with : spalib,
                          install : true)
