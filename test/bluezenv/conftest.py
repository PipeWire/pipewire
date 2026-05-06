# -*- coding: utf-8; mode: python; eval: (blacken-mode); -*-
# SPDX-FileCopyrightText: Copyright © 2026 Pauli Virtanen
# SPDX-License-Identifier: MIT

import pytest
import warnings

from pytest_bluezenv import Bluetoothd


@pytest.fixture
def paired_hosts(hosts, host_setup):
    """
    Provide two hosts, paired with each other, and the first one Central
    """

    le = any(
        "ControllerMode = le" in (p.conf or "")
        for plugins in host_setup["setup"]
        for p in plugins
        if isinstance(p, Bluetoothd)
    )

    if le:
        yield from _pair_le(hosts)
    else:
        yield from _pair_bredr(hosts)


def _pair_bredr(hosts):
    host0, host1 = hosts

    host0.bluetoothctl.send("scan on\n")
    host0.bluetoothctl.expect(f"Controller {host0.bdaddr.upper()} Discovering: yes")

    host1.bluetoothctl.send("pairable on\n")
    host1.bluetoothctl.expect("Changing pairable on succeeded")
    host1.bluetoothctl.send("discoverable on\n")
    host1.bluetoothctl.expect(f"Controller {host1.bdaddr.upper()} Discoverable: yes")

    host0.bluetoothctl.expect(f"Device {host1.bdaddr.upper()}")
    host0.bluetoothctl.send(f"pair {host1.bdaddr}\n")

    idx, m = host0.bluetoothctl.expect(r"Confirm passkey (\d+).*:")
    key = m[0].decode("utf-8")

    host1.bluetoothctl.expect(f"Confirm passkey {key}")

    host0.bluetoothctl.send("yes\n")
    host1.bluetoothctl.send("yes\n")

    host0.bluetoothctl.expect("Pairing successful")

    yield hosts


def _pair_le(hosts):
    host0, host1 = hosts

    host0.bluetoothctl.send("scan on\n")
    host0.bluetoothctl.expect(f"Controller {host0.bdaddr.upper()} Discovering: yes")

    host1.bluetoothctl.send("advertise on\n")
    host1.bluetoothctl.expect("Advertising object registered")

    host0.bluetoothctl.expect(f"Device {host1.bdaddr.upper()}")
    host0.bluetoothctl.send(f"pair {host1.bdaddr.upper()}\n")

    # BUG!: if controller is power cycled off/on at boot (before bluetoothd)
    # BUG!: which is what the tester here does,
    # BUG!: bluetoothd MGMT command to enable Secure Connections Host Support
    # BUG!: fails and we are left with legacy passkey. It seems we get randomly
    # BUG!: one of these depending on what state controller/kernel were before
    # BUG!: btmgmt power off/on

    idx, m = host0.bluetoothctl.expect(
        [r"\[agent\].*Passkey:.*m(\d+)", r"Confirm passkey (\d+).*:"]
    )
    key = m[0].decode("utf-8")

    if idx == 0:
        warnings.warn(
            "BUG: we got passkey authentication, bluetoothd/kernel should be fixed"
        )
        host1.bluetoothctl.expect(r"\[agent\] Enter passkey \(number in 0-999999\):")
        host1.bluetoothctl.send(f"{key}\n")
    else:
        host1.bluetoothctl.expect(f"Confirm passkey {key}")

        host0.bluetoothctl.send("yes\n")
        host1.bluetoothctl.send("yes\n")

    host0.bluetoothctl.expect("Pairing successful")

    yield hosts
