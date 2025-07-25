pipewire_daemon_sources = [
  'pipewire.c',
]

conf_config = configuration_data()
conf_config.set('VERSION', '"@0@"'.format(pipewire_version))
conf_config.set('PIPEWIRE_CONFIG_DIR', pipewire_configdir)
conf_config.set('session_manager_path', pipewire_bindir / 'pipewire-media-session')
conf_config.set('session_manager_args', '')
conf_config.set('pipewire_path', pipewire_bindir / 'pipewire')
conf_config.set('pipewire_pulse_path', pipewire_bindir / 'pipewire-pulse')
conf_config.set('sm_comment', '#')
conf_config.set('pulse_comment', '#')
conf_config.set('rtprio_server', get_option('rtprio-server'))
conf_config.set('rtprio_client', get_option('rtprio-client'))

conf_config_uninstalled = conf_config
conf_config_uninstalled.set('pipewire_path',
  meson.project_build_root() / 'src' / 'daemon' / 'pipewire')
conf_config_uninstalled.set('pipewire_pulse_path',
  meson.project_build_root() / 'src' / 'daemon' / 'pipewire-pulse')
conf_config_uninstalled.set('pulse_comment', '')

build_ms = 'media-session' in get_option('session-managers')
build_wp = 'wireplumber' in get_option('session-managers')
default_sm = get_option('session-managers').get(0, '')

build_vk = get_option('vulkan').enabled()

summary({'Build media-session': build_ms,
         'Build wireplumber': build_wp,
         'Default session-manager': default_sm},
         section: 'Session managers',
         bool_yn: true)

if build_wp
  wp_proj = subproject('wireplumber', required : true)
endif
if build_ms
  ms_proj = subproject('media-session', required : true)
endif

if default_sm == ''
  summary({'No session manager': 'pw-uninstalled.sh will not work out of the box!'})
elif default_sm == 'media-session'
  ms_bindir = ms_proj.get_variable('media_session_bin_dir', pipewire_bindir)
  conf_config.set('session_manager_path', ms_bindir / 'pipewire-media-session')

  ms_uninstalled = ms_proj.get_variable('media_session_uninstalled')
  conf_config_uninstalled.set('session_manager_path', ms_uninstalled.full_path())
  conf_config_uninstalled.set('session_manager_args', 'pipewire-media-session')
  conf_config_uninstalled.set('sm_comment', '')
elif default_sm == 'wireplumber'
  wp_bindir = wp_proj.get_variable('wireplumber_bin_dir', pipewire_bindir)
  conf_config.set('session_manager_path', wp_bindir / 'wireplumber')

  wp_uninstalled = wp_proj.get_variable('wireplumber_uninstalled')
  conf_config_uninstalled.set('session_manager_path', wp_uninstalled.full_path())
  conf_config_uninstalled.set('session_manager_args', 'wireplumber')
  conf_config_uninstalled.set('sm_comment', '')
else
  conf_config_uninstalled.set('session_manager_path', default_sm)
  conf_config_uninstalled.set('sm_comment', '')
endif

conf_files = [
 'pipewire.conf',
 'client.conf',
 'filter-chain.conf',
 'jack.conf',
 'minimal.conf',
 'pipewire-pulse.conf',
 'pipewire-avb.conf',
 'pipewire-aes67.conf',
]

if build_vk
  conf_files += 'pipewire-vulkan.conf'
endif

foreach c : conf_files
  res = configure_file(input : '@0@.in'.format(c),
                 output : c,
                 configuration : conf_config,
                 install_dir : pipewire_confdatadir)
  test(f'validate-json-@c@', spa_json_dump_exe, args : res)
endforeach

res = configure_file(input : 'pipewire.conf.in',
               output : 'pipewire-uninstalled.conf',
               configuration : conf_config_uninstalled)
test('validate-json-pipewire-uninstalled.conf', spa_json_dump_exe, args : res)

conf_avail_folders = [
  'pipewire.conf.avail',
  'client.conf.avail',
  'pipewire-pulse.conf.avail',
]

foreach c : conf_avail_folders
  subdir(c)
endforeach

pipewire_exec = executable('pipewire',
  pipewire_daemon_sources,
  install: true,
  include_directories : [ configinc ],
  dependencies : [ spa_dep, pipewire_dep, ],
)

ln = find_program('ln')

pipewire_aliases = [
  'pipewire-pulse',
  'pipewire-avb',
  'pipewire-aes67',
]

if build_vk
 pipewire_aliases += 'pipewire-vulkan'
endif

foreach alias : pipewire_aliases
  custom_target(
    alias,
    build_by_default: true,
    install: false,
    command: [ln, '-sf', meson.project_build_root() + '/@INPUT@', '@OUTPUT@'],
    input: pipewire_exec,
    output: alias,
  )
  install_symlink(
    alias,
    pointing_to: pipewire_exec.name(),
    install_dir: pipewire_bindir,
  )
endforeach

custom_target('pipewire-uninstalled',
  build_by_default: true,
  install: false,
  input: pipewire_exec,
  output: 'pipewire-uninstalled',
  command: [ln, '-fs', meson.project_build_root() + '/@INPUT@', '@OUTPUT@'],
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
subdir('systemd')
