if alsa_dep.found() and host_machine.system() == 'linux'
  subdir('alsa')
endif
if get_option('avb').require(host_machine.system() == 'linux', error_message: 'AVB support is only available on Linux').allowed()
  subdir('avb')
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
if bluez_deps_found
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
if have_vulkan
  subdir('vulkan')
endif

v4l2_header_found = cc.has_header('linux/videodev2.h', required: get_option('v4l2'))
summary({'V4L2 kernel header': v4l2_header_found}, bool_yn: true, section: 'Backend')
summary({'V4L2 enabled': v4l2_header_found}, bool_yn: true, section: 'Backend')
if v4l2_header_found
  subdir('v4l2')
endif
if libcamera_dep.found()
  subdir('libcamera')
endif

subdir('aec')
subdir('filter-graph')
