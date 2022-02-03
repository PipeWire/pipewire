pipewire_gst_sources = [
  'gstpipewire.c',
  'gstpipewirecore.c',
  'gstpipewireclock.c',
  'gstpipewireformat.c',
  'gstpipewirepool.c',
  'gstpipewiresink.c',
  'gstpipewiresrc.c',
]

if get_option('gstreamer-device-provider').allowed()
  pipewire_gst_sources += [ 'gstpipewiredeviceprovider.c' ]
endif

pipewire_gst_headers = [
  'gstpipewireclock.h',
  'gstpipewirecore.h',
  'gstpipewiredeviceprovider.h',
  'gstpipewireformat.h',
  'gstpipewirepool.h',
  'gstpipewiresink.h',
  'gstpipewiresrc.h',
]

pipewire_gst = shared_library('gstpipewire',
    pipewire_gst_sources,
    include_directories : [ configinc ],
    dependencies : [ spa_dep, gst_dep, pipewire_dep ],
    install : true,
    install_dir : '@0@/gstreamer-1.0'.format(get_option('libdir')),
)

plugins = [pipewire_gst]
