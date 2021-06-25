if alsa_dep.found()
  subdir('alsa')
endif
if not get_option('audioconvert').disabled()
  subdir('audioconvert')
endif
if not get_option('audiomixer').disabled()
  subdir('audiomixer')
endif
if not get_option('control').disabled()
  subdir('control')
endif
if not get_option('audiotestsrc').disabled()
  subdir('audiotestsrc')
endif
if bluez_dep.found()
  subdir('bluez5')
endif
if avcodec_dep.found()
  subdir('ffmpeg')
endif
if jack_dep.found()
  subdir('jack')
endif
if not get_option('support').disabled()
  subdir('support')
endif
if not get_option('test').disabled()
  subdir('test')
endif
if not get_option('videoconvert').disabled()
  subdir('videoconvert')
endif
if not get_option('videotestsrc').disabled()
  subdir('videotestsrc')
endif
if not get_option('volume').disabled()
  subdir('volume')
endif
if vulkan_headers
  subdir('vulkan')
endif

v4l2_header_found = cc.has_header('linux/videodev2.h', required: get_option('v4l2'))
v4l2_supported = libudev_dep.found() and v4l2_header_found
summary({'V4L2 kernel header': v4l2_header_found}, bool_yn: true, section: 'Backend')
summary({'V4L2 enabled': v4l2_supported}, bool_yn: true, section: 'Backend')
if v4l2_supported
  subdir('v4l2')
endif
if libcamera_dep.found()
  subdir('libcamera')
endif
