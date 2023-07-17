libcamera_sources = [
  'libcamera.c',
  'libcamera-manager.cpp',
  'libcamera-device.cpp',
  'libcamera-source.cpp'
]

libcameralib = shared_library('spa-libcamera',
  libcamera_sources,
  include_directories : [ configinc ],
  dependencies : [ spa_dep, libudev_dep, libcamera_dep, pthread_lib  ],
  install : true,
  install_dir : spa_plugindir / 'libcamera')
