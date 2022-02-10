if alsa_dep.found()
  subdir('alsa')
endif
if get_option('audioconvert').allowed()
  subdir('audioconvert')
endif
if get_option('audiomixer').allowed()
  subdir('audiomixer')
endif
if get_option('control').allowed()
  subdir('control')
endif
if get_option('audiotestsrc').allowed()
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
if get_option('support').allowed()
  subdir('support')
endif
if get_option('test').allowed()
  subdir('test')
endif
if get_option('videoconvert').allowed()
  subdir('videoconvert')
endif
if get_option('videotestsrc').allowed()
  subdir('videotestsrc')
endif
if get_option('volume').allowed()
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

subdir('aec')