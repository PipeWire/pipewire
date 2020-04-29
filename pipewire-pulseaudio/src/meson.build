pipewire_pulseaudio_sources = [
  'bitset.c',
  'channelmap.c',
  'context.c',
  'core-format.c',
  'direction.c',
  'error.c',
  'ext-device-manager.c',
  'ext-device-restore.c',
  'ext-stream-restore.c',
  'format.c',
  'introspect.c',
  'json.c',
  'mainloop.c',
  'mainloop-signal.c',
  'operation.c',
  'proplist.c',
  'rtclock.c',
  'sample.c',
  'scache.c',
  'stream.c',
  'strbuf.c',
  'subscribe.c',
  'thread-mainloop.c',
  'timeval.c',
  'utf8.c',
  'util.c',
  'version.c',
  'volume.c',
  'xmalloc.c',
  'pipewire-pulseaudio.c',
]

pipewire_mainloop_glib_sources = [
  'mainloop-glib.c',
]

pipewire_mainloop_simple_sources = [
  'simple.c',
]

pipewire_pulseaudio_c_args = [
  '-DHAVE_CONFIG_H',
  '-D_GNU_SOURCE',
  '-DPIC',
]

mapfile = 'map-file'
vflag = '-Wl,--version-script,@0@/@1@'.format(meson.current_source_dir(), mapfile)

libpulse_path = get_option('libpulse-path')
if libpulse_path == ''
  libpulse_path = join_paths(modules_install_dir, 'pulse')
endif


pipewire_pulseaudio = shared_library('pulse',
    pipewire_pulseaudio_sources,
    soversion : soversion,
    version : libversion,
    c_args : pipewire_pulseaudio_c_args,
    link_args : vflag,
    include_directories : [configinc],
    dependencies : [pipewire_dep, pulseaudio_dep, mathlib],
    install : true,
    install_dir : libpulse_path,
)

pipewire_pulseaudio = shared_library('pulse-mainloop-glib',
    pipewire_mainloop_glib_sources,
    soversion : soversion,
    version : libversion,
    c_args : pipewire_pulseaudio_c_args,
    link_args : vflag,
    include_directories : [configinc],
    dependencies : [pipewire_dep, pulseaudio_dep, mathlib, glib_dep],
    install : true,
    install_dir : libpulse_path,
)

pipewire_pulseaudio = shared_library('pulse-simple',
    pipewire_mainloop_simple_sources,
    soversion : soversion,
    version : libversion,
    c_args : pipewire_pulseaudio_c_args,
    link_args : vflag,
    include_directories : [configinc],
    dependencies : [pipewire_dep, pulseaudio_dep, mathlib, glib_dep],
    install : true,
    install_dir : libpulse_path,
)
