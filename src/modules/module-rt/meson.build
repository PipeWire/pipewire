rlimits_install = get_option('rlimits-install')
rlimits_data = configuration_data()
rlimits_data.set('MATCH', get_option('rlimits-match'))
rlimits_data.set('RTPRIO', get_option('rlimits-rtprio'))
rlimits_data.set('NICE', get_option('rlimits-nice'))
rlimits_data.set('MEMLOCK', get_option('rlimits-memlock'))
configure_file(input: '25-pw-rlimits.conf.in',
  output: '25-pw-rlimits.conf',
  install: rlimits_install,
  install_dir: get_option('sysconfdir') / 'security' / 'limits.d',
  configuration: rlimits_data)
summary({'RLIMITs': '@0@ limits.d file affecting matching PAM users'.format(rlimits_install ? 'with' : 'without')})

# The pam-defaults-install related code can be removed once all Linux <5.16 kernels are EOL (projected Dec, 2026)
pam_defaults_install = get_option('pam-defaults-install')
pam_defaults_data = configuration_data()
pam_defaults_data.set('PAM_MEMLOCK', get_option('pam-memlock-default'))
configure_file(input: '20-pw-defaults.conf.in',
  output: '20-pw-defaults.conf',
  install: pam_defaults_install,
  install_dir: get_option('sysconfdir') / 'security' / 'limits.d',
  configuration: pam_defaults_data)
summary({'PAM defaults': '@0@ limits.d file affecting all PAM users (not needed with modern systemd or kernel)'.format(pam_defaults_install ? 'with' : 'without')})
