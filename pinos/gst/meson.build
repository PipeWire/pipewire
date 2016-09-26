pinos_gst_sources = [
  'gstpinos.c',
  'gstpinosclock.c',
  'gstpinosdeviceprovider.c',
  'gstpinosformat.c',
  'gstpinospool.c',
  'gstpinossink.c',
  'gstpinossrc.c',
]

pinos_gst_headers = [
  'gstpinosclock.h',
  'gstpinosdeviceprovider.h',
  'gstpinosformat.h',
  'gstpinospool.h',
  'gstpinossink.h',
  'gstpinossrc.h',
]

pinos_gst_c_args = [
  '-DHAVE_CONFIG_H',
]

pinos_gst = shared_library('gstpinos',
    pinos_gst_sources,
    c_args : pinos_gst_c_args,
    include_directories : [configinc, spa_inc],
    dependencies : [gobject_dep, glib_dep, gio_dep, gst_dep, pinos_dep, pinoscore_dep],
    install : true,
    install_dir : '@0@/gstreamer-1.0'.format(get_option('libdir')),
)
