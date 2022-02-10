aec_null = shared_library('spa-aec-null',
  [ 'aec-null.c' ],
  include_directories : [ configinc ],
  dependencies : [ spa_dep ],
  install : true,
  install_dir : spa_plugindir / 'aec')

if webrtc_dep.found()
  aec_webrtc = shared_library('spa-aec-webrtc',
    [ 'aec-webrtc.cpp' ],
    include_directories : [ configinc ],
    dependencies : [ spa_dep, webrtc_dep ],
    install : true,
    install_dir : spa_plugindir / 'aec')
endif

