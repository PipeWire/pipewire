pipewire_daemon_sources = [
  'pipewire.c',
]

pipewire_c_args = [
  '-DG_LOG_DOMAIN=g_log_domain_pipewire',
]

conf_config = configuration_data()
conf_config.set('VERSION', '"@0@"'.format(pipewire_version))
conf_config.set('PIPEWIRE_CONFIG_DIR', pipewire_configdir)
conf_config.set('session_manager_path', pipewire_bindir / 'pipewire-media-session')
conf_config.set('session_manager_args', '')
conf_config.set('pipewire_path', pipewire_bindir / 'pipewire')
conf_config.set('pipewire_pulse_path', pipewire_bindir / 'pipewire-pulse')
conf_config.set('comment', '#')

conf_config_uninstalled = conf_config
conf_config_uninstalled.set('session_manager_path',
  meson.build_root() / 'src' / 'examples' / 'pipewire-media-session')
conf_config_uninstalled.set('pipewire_path',
  meson.build_root() / 'src' / 'daemon' / 'pipewire')
conf_config_uninstalled.set('pipewire_pulse_path',
  meson.build_root() / 'src' / 'daemon' / 'pipewire-pulse')
conf_config_uninstalled.set('comment', '')

build_ms = get_option('session-managers').contains('media-session')
build_wp = get_option('session-managers').contains('wireplumber')
sm_choice = get_option('default-session-manager')

if sm_choice == 'media-session'
  if get_option('session-managers') == []
    summary({'No session manager': 'pw-uninstalled.sh will not work out of the box!'})
  elif not build_ms
    error('media-session is the chosen session manager but it won\'t be built')
  endif
endif

if not build_wp and (sm_choice == 'wireplumber')
  error('building of the wireplumber subproject is disabled, ' +
        'but the requested session-manager is wireplumber')
elif build_wp
  wp_proj = subproject('wireplumber', required : true)
  if sm_choice == 'wireplumber'
    wp_bindir = wp_proj.get_variable('wireplumber_bin_dir', pipewire_bindir)

    conf_config.set('session_manager_path', wp_bindir / 'wireplumber')

    # wp-uninstalled.sh -b path/to/wp/build/root wireplumber
    conf_config_uninstalled.set('session_manager_path',
      meson.source_root() / 'subprojects' / 'wireplumber' / 'wp-uninstalled.sh')
    conf_config_uninstalled.set('session_manager_args',
      '-b ' + meson.build_root() / 'subprojects' / 'wireplumber' + ' wireplumber')
  endif
endif

conf_files = [
 'pipewire.conf',
 'client.conf',
 'client-rt.conf',
 'jack.conf',
 'pipewire-pulse.conf',
]

foreach c : conf_files
  configure_file(input : '@0@.in'.format(c),
                 output : c,
                 configuration : conf_config,
                 install_dir : pipewire_confdatadir)
endforeach

configure_file(input : 'pipewire.conf.in',
               output : 'pipewire-uninstalled.conf',
               configuration : conf_config_uninstalled)

pipewire_exec = executable('pipewire',
  pipewire_daemon_sources,
  install: true,
  c_args : pipewire_c_args,
  include_directories : [configinc, spa_inc],
  dependencies : [pipewire_dep, ],
)

executable('pipewire-pulse',
  pipewire_daemon_sources,
  install: true,
  c_args : pipewire_c_args,
  include_directories : [configinc, spa_inc],
  dependencies : [pipewire_dep, ],
)

ln = find_program('ln')

custom_target('pipewire-uninstalled',
  build_by_default: true,
  install: false,
  input: pipewire_exec,
  output: 'pipewire-uninstalled',
  command: [ln, '-fs', meson.build_root() + '/@INPUT@', '@OUTPUT@'],
)

#desktop_file = i18n.merge_file(
#  input : 'pipewire.desktop.in',
#  output : 'pipewire.desktop',
#  po_dir : po_dir,
#  type : 'desktop',
#  install : true,
#  install_dir : pipewire_sysconfdir / 'xdg' / 'autostart'
#)
#
#desktop_utils = find_program('desktop-file-validate', required: false)
#if desktop_utils.found()
#  test('Validate desktop file', desktop_utils,
#    args: [ desktop_file ],
#  )
#endif

subdir('filter-chain')
if build_ms
  subdir('media-session.d')
endif
if systemd.found()
  subdir('systemd')
endif
