ffmpeg_sources = ['ffmpeg.c',
                  'ffmpeg-dec.c',
                  'ffmpeg-enc.c']

ffmpeglib = shared_library('spa-ffmpeg',
                          ffmpeg_sources,
                          dependencies : [ spa_dep, avcodec_dep ],
                          install : true,
                          install_dir : spa_plugindir / 'ffmpeg')
