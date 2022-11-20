## BLE MIDI & SELinux

The SELinux configuration on Fedora 37 (as of 2022-11-10) does not
permit access to the bluetoothd APIs needed for BLE MIDI.

As a workaround, hopefully to be not necessary in future, you can
permit such access by creating a file `blemidi.te` with contents:

    policy_module(blemidi, 1.0);

    require {
        type system_dbusd_t;
        type unconfined_t;
        type bluetooth_t;
    }

    allow bluetooth_t unconfined_t:unix_stream_socket { read write };
    allow system_dbusd_t bluetooth_t:unix_stream_socket { read write };

Then having package `selinux-policy-devel` installed, running
`make -f /usr/share/selinux/devel/Makefile blemidi.pp`, and finally
to insert the rules via `sudo semodule -i blemidi.pp`.

The policy change can be removed by `sudo semodule -r blemidi`.
