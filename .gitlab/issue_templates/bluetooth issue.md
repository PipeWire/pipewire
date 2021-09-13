If you are filing this issue with a regular release please try master as it might already be fixed.
If you can, test also with Pulseaudio and list `pulseaudio --version`.


Bluetooth Radio, Bluetooth Headset, Desktop Environment, Distribution, Version (Bluez, Kernel, and PipeWire):

```
# run the following and paste output here
lsusb; bluetoothctl devices; echo $XDG_SESSION_DESKTOP; grep PRETTY /etc/os-release; pipewire --version; bluetoothctl --version; uname -r
```

Description of Problem:


How Reproducible:


Steps to Reproduce:


 1.
 2.
 3.


Actual Results:


Expected Results:


Additional Info (as attachments):

pw-dump output: `pw-dump -N > pw-dump.log`

Bluetooth debug log
https://gitlab.freedesktop.org/pipewire/pipewire/-/wikis/Troubleshooting#bluetooth
