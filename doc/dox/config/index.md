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

- \ref page_man_pipewire_conf_5 "PipeWire daemon configuration reference"
- \ref page_man_pipewire-pulse_conf_5 "PipeWire Pulseaudio daemon configuration reference"
- [WirePlumber daemon configuration](https://pipewire.pages.freedesktop.org/wireplumber/)

Configuration of devices:

- [WirePlumber configuration](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration.html)
- \ref page_man_pipewire-props_7 "Object property reference"
- \subpage page_config_xref "Configuration Index"

Configuration for client applications, either connecting via the
native PipeWire interface, or the emulated ALSA, JACK, or PulseAudio
interfaces:

- \ref page_man_pipewire-client_conf_5 "PipeWire native and ALSA client configuration reference"
- \ref page_man_pipewire-jack_conf_5 "PipeWire JACK client configuration reference"
- \ref page_man_pipewire-pulse_conf_5 "PipeWire Pulseaudio client configuration reference"

# Manual Pages

- \subpage page_man_pipewire_conf_5
- \subpage page_man_pipewire-client_conf_5
- \subpage page_man_pipewire-pulse_conf_5
- \subpage page_man_pipewire-jack_conf_5
- \subpage page_man_pipewire-filter-chain_conf_5
- \subpage page_man_pipewire-props_7
- \subpage page_man_pipewire-pulse-modules_7
- \subpage page_man_libpipewire-modules_7
