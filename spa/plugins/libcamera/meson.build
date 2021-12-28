libcamera_sources = [
  'libcamera.c',
  'libcamera-manager.cpp',
  'libcamera-device.cpp',
  'libcamera-source.cpp'
]

libdrm_dep = dependency('libdrm', version : '>= 2.4.98',
                        required : get_option('libcamera'))
summary({'libdrm': libdrm_dep.found()}, bool_yn: true, section: 'Backend')
if libdrm_dep.found()
  libcameralib = shared_library('spa-libcamera',
    libcamera_sources,
    dependencies : [ spa_dep, libudev_dep, libcamera_dep, pthread_lib, libdrm_dep  ],
    install : true,
    install_dir : spa_plugindir / 'libcamera')
endif
