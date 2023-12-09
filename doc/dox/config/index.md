\page page_config Configuration

One of the design goals of PipeWire is to be able to closely control
and configure all aspects of the processing graph.

A fully configured PipeWire setup runs various pieces, each with their
configuration options and files:

- **pipewire**: The PipeWire main daemon that runs and coordinates the processing.

- **pipewire-pulse**: The PipeWire PulseAudio replacement server. It also configures
  the properties of the PulseAudio clients connecting to it.

- **wireplumber**: Most configuration of devices is performed by the session manager.
  It typically loads ALSA and other devices and configures the profiles, port volumes and more.
  The session manager also configures new clients and links them to the targets, as configured
  in the session manager policy.

- **PipeWire clients**: Each native PipeWire client also loads a configuration file.
  Emulated JACK client also have separate configuration.

# Configuration Settings

Configuration of daemons:

- \ref page_man_pipewire_conf_5
- \ref page_man_pipewire-pulse_conf_5
- [WirePlumber daemon configuration](https://pipewire.pages.freedesktop.org/wireplumber/)
- [Wiki page on PipeWire daemon configuration](https://gitlab.freedesktop.org/pipewire/pipewire/-/wikis/Config-PipeWire)
- [Wiki page on PipeWire PulseAudio daemon configuration](https://gitlab.freedesktop.org/pipewire/pipewire/-/wikis/Config-PulseAudio)

Configuration of devices:

- \ref page_man_pipewire-devices_7
- [WirePlumber configuration](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration.html)
- [Wiki page on device runtime settings](https://gitlab.freedesktop.org/pipewire/pipewire/-/wikis/Config-Devices)

Configuration for client applications, either connecting via the
native PipeWire interface, or the emulated ALSA, JACK, or PulseAudio
interfaces:

- \ref page_man_pipewire-client_conf_5
- \ref page_man_pipewire-jack_conf_5
- \ref page_man_pipewire-pulse_conf_5
- [Wiki page on native clients](https://gitlab.freedesktop.org/pipewire/pipewire/-/wikis/Config-client)
- [Wiki page on ALSA clients](https://gitlab.freedesktop.org/pipewire/pipewire/-/wikis/Config-ALSA)
- [Wiki page on JACK clients](https://gitlab.freedesktop.org/pipewire/pipewire/-/wikis/Config-JACK)
- [Wiki page on PulseAudio clients](https://gitlab.freedesktop.org/pipewire/pipewire/-/wikis/Config-PulseAudio)

# Configuration Index

\ref page_man_pipewire_conf_5 "pipewire.conf"

@SECREF@ pipewire.conf

\ref page_man_pipewire_conf_5 "pipewire-pulse.conf"

@SECREF@ pipewire-pulse.conf

\ref page_man_pipewire_conf_5 "client.conf, client-rt.conf"

@SECREF@ client.conf

\ref page_man_pipewire_conf_5 "jack.conf"

@SECREF@ jack.conf

**Runtime settings**

@SECREF@ pipewire-settings

**Environment variables**

@SECREF@ pipewire-env client-env jack-env pulse-env

**Device properties**

@SECREF@ device-property

**Device parameters**

@SECREF@ device-param
