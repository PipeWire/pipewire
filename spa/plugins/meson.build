if get_option('alsa')
  subdir('alsa')
endif
if get_option('audioconvert')
  subdir('audioconvert')
endif
if get_option('audiomixer')
  subdir('audiomixer')
endif
if get_option('audiotestsrc')
  subdir('audiotestsrc')
endif
if get_option('bluez5') and sbc_dep.found() and bluez_dep.found()
  subdir('bluez5')
endif
if get_option('ffmpeg') and avcodec_dep.found()
  subdir('ffmpeg')
endif
if get_option('support')
  subdir('support')
endif
if get_option('test')
  subdir('test')
endif
if get_option('videotestsrc')
  subdir('videotestsrc')
endif
if get_option('volume')
  subdir('volume')
endif
if get_option('v4l2')
  subdir('v4l2')
endif
