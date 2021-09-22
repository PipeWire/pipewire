media_session_sources = []
if get_option('session-managers').contains('media-session')
  sm_logind_src = []
  sm_logind_dep = []
  if systemd.found() and systemd_dep.found()
    sm_logind_src = ['logind.c']
    sm_logind_dep = [systemd_dep]
  endif
  media_session_sources += [
    'access-flatpak.c',
    'access-portal.c',
    'alsa-no-dsp.c',
    'alsa-midi.c',
    'alsa-monitor.c',
    'alsa-endpoint.c',
    'bluez-monitor.c',
    'bluez-endpoint.c',
    'bluez-autoswitch.c',
    'default-nodes.c',
    'default-profile.c',
    'default-routes.c',
    'media-session.c',
    'session-manager.c',
    'match-rules.c',
    'metadata.c',
    'stream-endpoint.c',
    'restore-stream.c',
    'policy-ep.c',
    'policy-node.c',
    'streams-follow-default.c',
    'v4l2-monitor.c',
    'v4l2-endpoint.c',
    'libcamera-monitor.c',
    'suspend-node.c',
  ] + sm_logind_src
  pms = executable('pipewire-media-session',
    media_session_sources,
    install: true,
    dependencies : [dbus_dep, pipewire_dep, alsa_dep, mathlib, sm_logind_dep, libinotify_dep],
)
endif
