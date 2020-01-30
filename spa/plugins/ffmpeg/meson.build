ffmpeg_sources = ['ffmpeg.c',
                  'ffmpeg-dec.c',
                  'ffmpeg-enc.c']

ffmpeglib = shared_library('spa-ffmpeg',
                          ffmpeg_sources,
                          include_directories : [spa_inc],
                          dependencies : [ avcodec_dep, avformat_dep ],
                          install : true,
		          install_dir : join_paths(spa_plugindir, 'ffmpeg'))
