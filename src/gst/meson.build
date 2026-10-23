pipewire_gst_sources = [
  'gstpipewire.c',
  'gstpipewirecore.c',
  'gstpipewireclock.c',
  'gstpipewireformat.c',
  'gstpipewirepool.c',
  'gstpipewiresink.c',
  'gstpipewiresrc.c',
  'gstpipewirestream.c',
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
  'gstpipewirestream.h',
]

pipewire_gst = shared_library('gstpipewire',
    pipewire_gst_sources,
    include_directories : [ configinc ],
    dependencies : [ spa_dep, gst_dep, pipewire_dep, mathlib ],
    install : true,
    install_dir : '@0@/gstreamer-1.0'.format(get_option('libdir')),
)

plugins = [pipewire_gst]

if get_option('tests').allowed()
  format_test = executable('gst-pipewire-format-test',
    ['test-gstpipewireformat.c', 'gstpipewireformat.c'],
    include_directories : [ configinc ],
    dependencies : [ spa_dep, gst_dep, pipewire_dep, mathlib ],
    install : false,
  )
  foreach format_case : [
    'enumeration-dmabuf',
    'enumeration-forward',
    'enumeration-roundtrip',
    'empty',
    'jpeg-preferred',
    'audio-rate-list',
    'dmabuf-list',
    'fixed',
    'preferred-size',
    'scalar-range',
    'range-scalar',
    'fraction-list',
    'independent-lists',
    'scalar-step',
    'step-scalar',
    'channels-list',
    'channels-step',
    'format-list',
    'all-preferences',
  ]
    test('gst-format-' + format_case, format_test,
      args : [format_case],
      env : ['G_DEBUG=fatal-criticals'],
      suite : 'gstreamer',
    )
  endforeach
endif
