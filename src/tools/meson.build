tools_sources = [
  [ 'pw-mon', [ 'pw-mon.c' ] ],
  [ 'pw-config', [ 'pw-config.c' ] ],
  [ 'pw-container', [ 'pw-container.c' ] ],
  [ 'pw-dot', [ 'pw-dot.c' ] ],
  [ 'pw-dump', [ 'pw-dump.c' ] ],
  [ 'pw-profiler', [ 'pw-profiler.c' ] ],
  [ 'pw-mididump', [ 'pw-mididump.c', 'midifile.c', 'midievent.c', 'midiclip.c' ] ],
  [ 'pw-metadata', [ 'pw-metadata.c' ] ],
  [ 'pw-loopback', [ 'pw-loopback.c' ] ],
  [ 'pw-link', [ 'pw-link.c' ] ],
]

foreach t : tools_sources
  executable(t.get(0),
    t.get(1),
    install: true,
    dependencies : [pipewire_dep, mathlib],
  )
endforeach

executable('pw-cli',
  'pw-cli.c',
  install: true,
  dependencies: [pipewire_dep, readline_dep]
)

if ncurses_dep.found()
  executable('pw-top',
    'pw-top.c',
    install: true,
    dependencies : [pipewire_dep, ncurses_dep],
  )
endif

build_pw_cat = false
build_pw_cat_with_ffmpeg = false
pwcat_deps = [ sndfile_dep ]

if get_option('pw-cat').allowed() and sndfile_dep.found()
  build_pw_cat = true

  if pw_cat_ffmpeg.allowed() and avcodec_dep.found() and avformat_dep.found() and avutil_dep.found()
    pwcat_deps += [avcodec_dep, avformat_dep, avutil_dep]
    build_pw_cat_with_ffmpeg = true
  endif

  pwcat_sources = [
    'pw-cat.c',
    'midifile.c',
    'midiclip.c',
    'midievent.c',
    'dfffile.c',
    'dsffile.c',
  ]

  pwcat_aliases = [
    'pw-play',
    'pw-record',
    'pw-midiplay',
    'pw-midirecord',
    'pw-midi2play',
    'pw-midi2record',
    'pw-dsdplay',
    'pw-encplay',
    'pw-sysex',
  ]

  pw_cat = executable('pw-cat',
    pwcat_sources,
    install: true,
    dependencies : [pwcat_deps, pipewire_dep, mathlib],
  )

  foreach alias : pwcat_aliases
    custom_target(
      alias,
      build_by_default: true,
      install: false,
      command: [ln, '-sf', meson.project_build_root() + '/@INPUT@', '@OUTPUT@'],
      input: pw_cat,
      output: alias,
    )
    install_symlink(
      alias,
      pointing_to: pw_cat.name(),
      install_dir: pipewire_bindir,
    )
  endforeach
elif not sndfile_dep.found() and get_option('pw-cat').enabled()
  error('pw-cat is enabled but required dependency `sndfile` was not found.')
endif
summary({'Build pw-cat tool': build_pw_cat}, bool_yn: true, section: 'pw-cat/pw-play/pw-dump tool')
if build_pw_cat
  summary({'Build pw-cat with FFmpeg integration': build_pw_cat_with_ffmpeg}, bool_yn: true, section: 'pw-cat/pw-play/pw-dump tool')
endif

if sndfile_dep.found()
  executable('pw-audioconvert',
    'pw-audioconvert.c',
    install: true,
    dependencies : [pipewire_dep, sndfile_dep, mathlib],
  )
endif

build_avb_virtual = get_option('avb-virtual').require(
    host_machine.system() == 'linux',
    error_message: 'AVB support is only available on Linux'
).allowed()

if build_avb_virtual
  avb_tool_inc = include_directories('../modules')
  avb_tool_sources = [
    'pw-avb-virtual.c',
    '../modules/module-avb/avb.c',
    '../modules/module-avb/adp.c',
    '../modules/module-avb/acmp.c',
    '../modules/module-avb/acmp-cmds-resps/acmp-common.c',
    '../modules/module-avb/acmp-cmds-resps/acmp-legacy-avb.c',
    '../modules/module-avb/acmp-cmds-resps/acmp-milan-v12.c',
    '../modules/module-avb/aecp.c',
    '../modules/module-avb/aecp-aem.c',
    '../modules/module-avb/aecp-vendor-unique-milan-v12.c',
    '../modules/module-avb/aecp-vendor-unique-milan-v12-cmds-resps/cmd-get-milan-info.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-available.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-get-set-control.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-get-set-name.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-get-set-clock-source.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-get-set-sampling-rate.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-deregister-unsolicited-notifications.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-register-unsolicited-notifications.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-get-set-stream-format.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-lock-entity.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-get-set-configuration.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-get-dynamic-info.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-get-set-stream-info.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-start-stop-streaming.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-get-counters.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-get-as-path.c',
    '../modules/module-avb/aecp-aem-cmds-resps/cmd-get-set-max-transit-time.c',
    '../modules/module-avb/aecp-aem-cmds-resps/reply-unsol-helpers.c',
    '../modules/module-avb/es-builder.c',
    '../modules/module-avb/avdecc.c',
    '../modules/module-avb/descriptors.c',
    '../modules/module-avb/maap.c',
    '../modules/module-avb/mmrp.c',
    '../modules/module-avb/mrp.c',
    '../modules/module-avb/msrp.c',
    '../modules/module-avb/mvrp.c',
    '../modules/module-avb/srp.c',
    '../modules/module-avb/stream.c',
  ]
  executable('pw-avb-virtual',
    avb_tool_sources,
    install: true,
    include_directories: [configinc, avb_tool_inc],
    dependencies: [mathlib, dl_lib, rt_lib, pipewire_dep],
  )
endif

if dbus_dep.found()
  executable('pw-reserve',
    'reserve.h',
    'reserve.c',
    'pw-reserve.c',
    install: true,
    dependencies : [dbus_dep, pipewire_dep],
  )
endif
