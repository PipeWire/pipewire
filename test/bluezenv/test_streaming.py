# -*- coding: utf-8; mode: python; eval: (blacken-mode); -*-
# SPDX-FileCopyrightText: Copyright © 2026 Pauli Virtanen
# SPDX-License-Identifier: MIT
"""
Tests for PipeWire audio streaming

To use uninstalled version of PipeWire, run the tests in PipeWire
devenv::

    meson devenv -C ../pipewire/builddir -w . python3 -mpytest test/bluezenv -v
"""

import sys
import os
import re
import pytest
import signal
import subprocess
import tempfile
import time
import logging
import json
import dbus
import threading
from pathlib import Path

import pytest

from pytest_bluezenv import (
    HostPlugin,
    host_config,
    find_exe,
    Bluetoothd,
    Bluetoothctl,
    DbusSession,
    LogStream,
    wait_until,
    mainloop_wrap,
)

pytestmark = [pytest.mark.vm]

log = logging.getLogger(__name__)

# Use larger VM instances in case ASAN is enabled
VM_MEM = "784M"


class PipeWire(HostPlugin):
    """
    Launch PipeWire in VM instance
    """

    name = "pipewire"
    depends = [DbusSession(), Bluetoothd()]

    def __init__(
        self,
        uuids=(
            "0000110a-0000-1000-8000-00805f9b34fb",
            "0000110b-0000-1000-8000-00805f9b34fb",
        ),
        roles="a2dp_sink a2dp_source",
        config=None,
    ):
        self.uuids = tuple(uuids)
        self.roles = str(roles)
        self.config = config

        # For running PipeWire from build directory
        self.devenv = {}
        if os.environ.get("PW_UNINSTALLED"):
            devenv_keys = [
                "WIREPLUMBER_MODULE_DIR",
                "WIREPLUMBER_CONFIG_DIR",
                "WIREPLUMBER_DATA_DIR",
                "PIPEWIRE_CONFIG_DIR",
                "PIPEWIRE_MODULE_DIR",
                "SPA_PLUGIN_DIR",
                "SPA_DATA_DIR",
                "ACP_PATHS_DIR",
                "ACP_PROFILES_DIR",
                "GST_PLUGIN_PATH",
                "ALSA_PLUGIN_DIR",
                "LD_LIBRARY_PATH",
                "PW_UNINSTALLED",
                "PW_BUILDDIR",
                "PATH",
            ]
            for key in devenv_keys:
                value = os.environ.get(key)
                if value is not None:
                    self.devenv[key] = value

    def presetup(self, config):
        try:
            self.exe_pw = find_exe("", "pipewire")
            self.exe_wp = find_exe("", "wireplumber")
            self.exe_dump = find_exe("", "pw-dump")
            self.exe_cli = find_exe("", "pw-cli")
            self.exe_play = find_exe("", "pw-play")
            self.exe_record = find_exe("", "pw-record")

            env = dict(os.environ)
            env["ASAN_OPTIONS"] = "detect_leaks=0:leak_check_at_exit=0"

            # get versions
            res = subprocess.run(
                [self.exe_pw, "--version"],
                stdout=subprocess.PIPE,
                encoding="utf-8",
                check=True,
                env=env,
            )
            m = re.search("libpipewire ([0-9.]+)", res.stdout)
            if m:
                pw_version = tuple(int(x) for x in m.group(1).split("."))
            else:
                raise ValueError(
                    f"pipewire {self.exe_pw} version unknown: {res.stdout}"
                )

            res = subprocess.run(
                [self.exe_wp, "--version"],
                stdout=subprocess.PIPE,
                encoding="utf-8",
                check=True,
                env=env,
            )
            m = re.search("libwireplumber ([0-9.]+)", res.stdout)
            if m:
                wp_version = tuple(int(x) for x in m.group(1).split("."))
            else:
                raise ValueError(
                    f"wireplumber {self.exe_wp} version unknown: {res.stdout}"
                )

            # check versions
            if pw_version >= (1, 6, 0) and pw_version <= (1, 6, 2):
                raise ValueError("buggy pipewire version")
            if pw_version < (1, 4, 9):
                raise ValueError("pipewire too old")
            if wp_version < (0, 5, 8):
                raise ValueError("wireplumber too old")
        except (FileNotFoundError, ValueError) as exc:
            pytest.skip(reason=f"PipeWire: {exc}")

    @mainloop_wrap
    def setup(self, impl):
        self.play = None
        self.record = None
        self.log = logging.getLogger(self.name)

        self.tmpdir = tempfile.TemporaryDirectory(prefix="pipewire-", dir="/run")
        conf_dir = Path(self.tmpdir.name) / "config"
        runtime_dir = Path(self.tmpdir.name) / "runtime"
        state_dir = Path(self.tmpdir.name) / "state"

        dropin_dir = conf_dir / "wireplumber" / "wireplumber.conf.d"
        wp_conf = dropin_dir / "01-config.conf"
        wp_extra_conf = dropin_dir / "02-extra-config.conf"

        conf_dir.mkdir()
        runtime_dir.mkdir()
        dropin_dir.mkdir(parents=True)
        state_dir.mkdir()

        self.environ = environ = dict(os.environ)
        environ.update(self.devenv)

        environ["XDG_CONFIG_HOME"] = str(conf_dir)
        environ["XDG_STATE_HOME"] = str(runtime_dir)
        environ["XDG_RUNTIME_HOME"] = str(runtime_dir)
        environ["PIPEWIRE_RUNTIME_DIR"] = str(runtime_dir)
        environ["XDG_STATE_HOME"] = str(state_dir)
        environ["PIPEWIRE_DEBUG"] = "2"
        environ["WIREPLUMBER_DEBUG"] = (
            "spa.bluez5.iso:3,spa.bluez5*:4,s-monitors:4,m-lua-scripting:4,s-linking:4,s-device:4"
        )

        # Handle devenv
        if "WIREPLUMBER_CONFIG_DIR" in environ:
            environ["WIREPLUMBER_CONFIG_DIR"] = (
                environ["WIREPLUMBER_CONFIG_DIR"] + ":" + str(conf_dir / "wireplumber")
            )

        with open(wp_conf, "w") as f:
            text = f"""
            monitor.bluez.properties = {{
               bluez5.roles = [ {self.roles} ]
               bluez5.decode-buffer.latency = 4096
            }}
            """
            f.write(text)

        if self.config is not None:
            with open(wp_extra_conf, "w") as f:
                f.write(self.config)

        log.info(f"Starting pipewire: {self.exe_pw}")

        self.logger = LogStream("pipewire")
        self.pw = subprocess.Popen(
            self.exe_pw,
            env=environ,
            stdout=self.logger.stream,
            stderr=subprocess.STDOUT,
        )

        log.debug(f"Wait for pipewire...")

        def cond():
            try:
                ret = subprocess.run(
                    [self.exe_dump],
                    stdout=subprocess.DEVNULL,
                    env=self.environ,
                    timeout=5,
                )
            except subprocess.TimeoutExpired:
                return False
            return ret.returncode == 0

        wait_until(cond)

        log.info(f"Starting wireplumber: {self.exe_wp}")

        self.wp_logger = LogStream("wireplumber")
        self.wp = subprocess.Popen(
            self.exe_wp,
            env=environ,
            stdout=self.wp_logger.stream,
            stderr=subprocess.STDOUT,
        )

        # Wait for PipeWire's bluetooth services
        log.info("Wait for PipeWire endpoints...")
        bus = dbus.SystemBus()
        bus.set_exit_on_disconnect(False)
        adapter = dbus.Interface(
            bus.get_object("org.bluez", "/org/bluez/hci0"),
            "org.freedesktop.DBus.Properties",
        )

        def cond():
            self.check_running()

            uuids = [str(uuid) for uuid in adapter.Get("org.bluez.Adapter1", "UUIDs")]
            return all(uuid in uuids for uuid in self.uuids)

        wait_until(cond)

        os.environ["PIPEWIRE_RUNTIME_DIR"] = str(runtime_dir)

        # Wait for wireplumber session services
        text = None

        def cond():
            nonlocal text

            self.check_running()

            text = self.pw_dump()
            try:
                data = json.loads(text)
            except:
                return False
            for item in data:
                if item.get("type", None) != "PipeWire:Interface:Client":
                    continue
                if item["info"]["props"]["application.name"] != "WirePlumber":
                    continue
                if "api.bluez" in item["info"]["props"].get("session.services", ""):
                    return True
            return False

        try:
            wait_until(cond)
        except:
            raise TimeoutError(f"PipeWire not ready\n{text}")

        log.info("PipeWire ready")

    def check_running(self):
        if self.pw.poll() is not None:
            raise RuntimeError("PipeWire process terminated")
        if self.wp.poll() is not None:
            raise RuntimeError("Wireplumber process terminated")

    def pw_dump(self):
        try:
            ret = subprocess.run(
                [self.exe_dump],
                stdout=subprocess.PIPE,
                encoding="utf-8",
                env=self.environ,
                timeout=5,
            )
        except subprocess.TimeoutExpired:
            return "ERROR: timeout"

        return ret.stdout

    def pw_set_profile(self, profile):
        """
        Set profile of the Bluetooth device, by profile name prefix
        """
        for item in json.loads(self.pw_dump()):
            if item.get("type", None) != "PipeWire:Interface:Device":
                continue
            if item["info"]["props"].get("device.api", None) != "bluez5":
                continue
            for entry in item["info"]["params"].get("EnumProfile", []):
                if not entry["name"].startswith(profile):
                    continue
                subprocess.run(
                    [
                        self.exe_cli,
                        "set-param",
                        str(item["id"]),
                        "Profile",
                        json.dumps({"index": entry["index"], "save": False}),
                    ],
                    env=self.environ,
                    check=True,
                    # The device may block in a synchronous transport release
                    # and codec switch; a loaded VM can take a while to
                    # process the request. Completion is verified by
                    # check_pipewire_devices_exist().
                    timeout=30,
                )
                return entry["name"]

        raise ValueError(f"no profile {profile} on the bluez5 device")

    def pw_play(self):
        self.play = subprocess.Popen(
            [
                self.exe_play,
                "--raw",
                "--rate",
                "4000",
                "--channels",
                "1",
                "--format",
                "s8",
                "-",
            ],
            stdin=subprocess.PIPE,
            env=self.environ,
        )
        self.play_thread = threading.Thread(
            target=self._play_thread, args=(self.play.stdin,)
        )
        self.play_thread.start()

    def _play_thread(self, stream):
        block = bytes([j % 256 for j in range(4096)])
        while True:
            try:
                stream.write(block)
            except:
                self.log.info("pw_play ended")
                break

    def pw_record(self):
        self.record = subprocess.Popen(
            [
                self.exe_record,
                "-P",
                "media.class=Audio/Sink",
                "--raw",
                "--format",
                "s8",
                "--rate",
                "4000",
                "--channels",
                "1",
                "-",
            ],
            stdout=subprocess.PIPE,
            env=self.environ,
        )
        self.record_thread = threading.Thread(
            target=self._record_thread, args=(self.record.stdout,)
        )
        self.record_thread.start()
        self.record_signal = threading.Event()

    def _record_thread(self, stream):
        while True:
            try:
                block = stream.read(256)
                if not block:
                    break
            except:
                self.log.info("pw_record failed")
                break

            # If we get anything nonzero, some signal is getting
            # through.  Can't check exactness due to encoding and
            # possibly heavy underruns in VM environment.
            if any(list(block)):
                self.log.info("pw_record signal found")
                self.record_success = True
                self.record_signal.set()
                return
            else:
                self.log.debug("pw_record: waiting for signal")

        self.log.error("pw_record: no signal found")
        self.record_success = False
        self.record_signal.set()

    def pw_record_wait_signal(self, timeout=160):
        res = self.record_signal.wait(timeout=timeout)
        return res and self.record_success

    def pw_stop_streams(self):
        # Terminate before closing the pipes: the IO threads may be blocked
        # on them, and hold the locks close() needs.
        if self.play is not None:
            self.play.terminate()
            self.play_thread.join()
            try:
                self.play.stdin.close()
            except BrokenPipeError:
                pass
            self.play = None
        if self.record is not None:
            self.record.terminate()
            self.record_thread.join()
            try:
                self.record.stdout.close()
            except BrokenPipeError:
                pass
            self.record = None

    def _terminate(self):
        log.info("Stop pipewire")
        self.pw.terminate()
        self.wp.terminate()
        self.pw_stop_streams()

    def shutdown(self):
        """Terminate PipeWire/WirePlumber and check they exited cleanly."""
        self._terminate()

        for name, proc in (("pipewire", self.pw), ("wireplumber", self.wp)):
            rc = proc.wait()
            if rc < 0:
                try:
                    sig = signal.Signals(-rc).name
                except ValueError:
                    sig = str(-rc)
                raise RuntimeError(f"{name} terminated by signal {sig}")
            if rc != 0:
                raise RuntimeError(f"{name} exited with code {rc}")

    def teardown(self):
        self._terminate()
        self.tmpdir.cleanup()


class BluetoothdWithRestart(Bluetoothd):
    """Bluetoothd plugin that can be restarted via ``restart()``."""

    def restart(self):
        # SIGUSR1 kills bluetoothd abruptly (no graceful D-Bus cleanup), which
        # may crash PipeWire/WirePlumber.
        self.log.info("Restart bluetoothd")
        self.job.send_signal(signal.SIGUSR1)
        try:
            self.job.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.job.kill()
            self.job.wait()
        self.teardown()
        self.setup(None)  # setup() ignores impl


a2dp_host = [Bluetoothctl(), PipeWire(roles="a2dp_sink a2dp_source")]


@host_config(a2dp_host, a2dp_host, mem=VM_MEM)
def test_pipewire_a2dp(paired_hosts):
    host0, host1 = paired_hosts

    # Connect
    host1.bluetoothctl.send(f"trust {host0.bdaddr}\n")
    host0.bluetoothctl.send(f"connect {host1.bdaddr}\n")

    # Wait for pipewire devices to appear
    check_pipewire_devices_exist(host0, "a2dp-sink")

    # Test streaming
    host1.pipewire.pw_record()
    host0.pipewire.pw_play()

    assert host1.pipewire.pw_record_wait_signal()


pw_client_host = [BluetoothdWithRestart(), PipeWire(), Bluetoothctl()]

a2dp_server_host = [Bluetoothctl(), PipeWire(roles="a2dp_sink a2dp_source")]


@host_config(pw_client_host, a2dp_server_host, mem=VM_MEM)
def test_pipewire_bluez_restart(paired_hosts):
    host0, host1 = paired_hosts

    # Connect to the A2DP server on host1
    host1.bluetoothctl.send(f"trust {host0.bdaddr}\n")
    host0.bluetoothctl.send(f"connect {host1.bdaddr}\n")

    # Wait for the bluez card to appear
    check_pipewire_devices_exist(host0, "a2dp-sink")

    host0.bluetoothd.restart()

    host0.pipewire.shutdown()


bap_ucast_host = [
    Bluetoothd(conf="[General]\nControllerMode = le\n", args=["-E", "-K"]),
    Bluetoothctl(),
    PipeWire(
        roles="bap_sink bap_source", uuids=("00001850-0000-1000-8000-00805f9b34fb",)
    ),
]


@host_config(bap_ucast_host, bap_ucast_host, mem=VM_MEM)
def test_pipewire_bap_ucast(paired_hosts):
    host0, host1 = paired_hosts

    # Connect
    host1.bluetoothctl.send(f"trust {host0.bdaddr}\n")

    host0.bluetoothctl.send(f"scan off\n")
    host0.bluetoothctl.send(f"connect {host1.bdaddr}\n")

    # Wait for pipewire devices to appear
    check_pipewire_devices_exist(host0, "bap-sink")

    # Test streaming
    host1.pipewire.pw_record()
    host0.pipewire.pw_play()

    assert host1.pipewire.pw_record_wait_signal()


bcast_src_config = """
monitor.bluez.properties = {
  bluez5.bcast_source.config = [
    {
      "broadcast_code": "Test",
      "encryption": false,
      "bis": [ { "qos_preset": "16_2_1" } ]
    }
  ]
}
"""

bap_bcast_src_host = [
    Bluetoothd(conf="[General]\nControllerMode = le\n", args=["-E", "-K"]),
    Bluetoothctl(),
    PipeWire(
        roles="bap_bcast_source",
        uuids=("00001850-0000-1000-8000-00805f9b34fb",),
        config=bcast_src_config,
    ),
]

bap_bcast_snk_host = [
    Bluetoothd(conf="[General]\nControllerMode = le\n", args=["-E", "-K"]),
    Bluetoothctl(),
    PipeWire(roles="bap_bcast_sink", uuids=("00001850-0000-1000-8000-00805f9b34fb",)),
]


# BUG!: the bcast test is sometimes flaky because BlueZ has a hardcoded
# BUG!: 3 sec DBus timeout and Wireplumber on the VM may not boot up
# BUG!: fast enough


@host_config(bap_bcast_src_host, bap_bcast_snk_host, mem=VM_MEM)
def test_pipewire_bap_bcast(hosts):
    host0, host1 = hosts

    # Start broadcasting
    check_pipewire_devices_exist(host0, "bap-sink")
    host0.pipewire.pw_play()

    # Connect
    host1.bluetoothctl.send(f"scan on\n")

    host0.bluetoothctl.send(f"advertise on\n")

    host1.pipewire.pw_record()

    idx, m = host1.bluetoothctl.expect(f"Transport (/org/bluez/hci0/.+)")
    transport = m[0].decode("utf-8")

    # BUG!: issuing transport select immediately causes failure
    # BUG!: as it tries to enter broadcasting state while config(1)
    # BUG!: is not finished and BROADCASTING state gets cancelled via
    # BUG!:    transport.c:bap_state_changed()
    # BUG!:    -> transport_update_playing(transport, FALSE)
    # BUG!:    -> transport_set_state(transport, TRANSPORT_STATE_IDLE)

    # TODO: fix the bug and go to transport.select without waiting here
    check_pipewire_devices_exist(host1, "device")

    host1.bluetoothctl.send(f"transport.select {transport}\n")

    check_pipewire_devices_exist(host1, "bap-source")

    # Test streaming
    assert host1.pipewire.pw_record_wait_signal()


hfp_hf_host = [
    Bluetoothctl(),
    PipeWire(
        roles="hfp_hf",
        uuids=("0000111e-0000-1000-8000-00805f9b34fb",),
    ),
]

hfp_ag_host = [
    Bluetoothctl(),
    PipeWire(
        roles="hfp_ag hfp_hf hsp_ag hsp_hs",
        uuids=(
            "0000111f-0000-1000-8000-00805f9b34fb",
            "0000111e-0000-1000-8000-00805f9b34fb",
            "00001108-0000-1000-8000-00805f9b34fb",
            "00001112-0000-1000-8000-00805f9b34fb",
        ),
    ),
]


@host_config(hfp_ag_host, hfp_hf_host, mem=VM_MEM)
def test_pipewire_hfp(paired_hosts):
    host0, host1 = paired_hosts

    # Connect
    host1.bluetoothctl.send(f"trust {host0.bdaddr}\n")

    host0.bluetoothctl.send(f"scan off\n")
    host0.bluetoothctl.send(f"connect {host1.bdaddr}\n")

    # Wait for pipewire devices to appear
    check_pipewire_devices_exist(host0, "hfp")

    # Test streaming
    host1.pipewire.pw_record()
    host0.pipewire.pw_play()

    assert host1.pipewire.pw_record_wait_signal()


# WirePlumber profile autoswitch reverts profiles that no stream asks for,
# after a delay that starts counting when the device appears. The test
# switches profiles itself, so the policy must not act on the host doing
# the switching.
no_profile_autoswitch_config = """
wireplumber.settings = {
  bluetooth.autoswitch-to-headset-profile = false
}
"""

a2dp_hfp_ag_host = [
    Bluetoothctl(),
    PipeWire(
        roles="a2dp_source hfp_ag",
        uuids=(
            "0000110a-0000-1000-8000-00805f9b34fb",
            "0000111f-0000-1000-8000-00805f9b34fb",
        ),
        config=no_profile_autoswitch_config,
    ),
]

a2dp_hfp_hf_host = [
    Bluetoothctl(),
    PipeWire(
        roles="a2dp_sink hfp_hf",
        uuids=(
            "0000110b-0000-1000-8000-00805f9b34fb",
            "0000111e-0000-1000-8000-00805f9b34fb",
        ),
    ),
]


@host_config(a2dp_hfp_ag_host, a2dp_hfp_hf_host, mem=VM_MEM)
def test_pipewire_profile_switch(paired_hosts, caplog):
    """
    Switching profiles while streaming releases the transport under a
    started node, which must not be counted as a transport error.
    """
    host0, host1 = paired_hosts

    # Connect
    host1.bluetoothctl.send(f"trust {host0.bdaddr}\n")

    host0.bluetoothctl.send(f"scan off\n")
    host0.bluetoothctl.send(f"connect {host1.bdaddr}\n")

    check_pipewire_devices_exist(host0, "a2dp-sink")

    # Switch profiles back and forth, while streaming
    host0.pipewire.pw_play()

    # Three, which is what it takes to exhaust TRANSPORT_ERROR_MAX_RETRY
    # if the releases are counted as errors
    for _ in range(3):
        host0.pipewire.pw_set_profile("headset-head-unit")
        check_pipewire_devices_exist(host0, "hfp")

        host0.pipewire.pw_set_profile("a2dp-sink")
        check_pipewire_devices_exist(host0, "a2dp-sink")

    assert not [
        r for r in caplog.records if "Bluetooth audio transport" in r.getMessage()
    ]

    host0.pipewire.pw_stop_streams()

    # Test streaming still works
    host1.pipewire.pw_record()
    host0.pipewire.pw_play()

    assert host1.pipewire.pw_record_wait_signal()


@host_config(bap_ucast_host, bap_ucast_host, mem=VM_MEM)
def test_pipewire_bap_profile_switch(paired_hosts, caplog):
    """
    As above, for BAP: the transports are released the same way, and linked
    transports make their release decisions from transport->acquired.

    Only one switch: switching back to the duplex profile does not complete
    here, the BAP reconfiguration gets no reply from BlueZ and times out.
    """
    host0, host1 = paired_hosts

    # Connect
    host1.bluetoothctl.send(f"trust {host0.bdaddr}\n")

    host0.bluetoothctl.send(f"scan off\n")
    host0.bluetoothctl.send(f"connect {host1.bdaddr}\n")

    check_pipewire_devices_exist(host0, "bap-sink")

    # Stream, and wait until the sink is actually started: pw_play() returns
    # as soon as the process exists, which is before that
    host1.pipewire.pw_record()
    host0.pipewire.pw_play()

    assert host1.pipewire.pw_record_wait_signal()

    # Switch profile, while streaming
    host0.pipewire.pw_set_profile("bap-sink")
    check_pipewire_devices_exist(host0, "bap-sink")

    assert not [
        r for r in caplog.records if "Bluetooth audio transport" in r.getMessage()
    ]


def check_pipewire_devices_exist(host, profile="a2dp-sink"):
    factories = {
        "a2dp-sink": ("api.bluez5.a2dp.sink",),
        "a2dp-source": ("api.bluez5.a2dp.source",),
        "hfp": ("api.bluez5.sco.sink", "api.bluez5.sco.source"),
        "bap-sink": ("api.bluez5.media.sink",),
        "bap-source": ("api.bluez5.media.source",),
        "bap-duplex": ("api.bluez5.media.sink", "api.bluez5.media.source"),
        "device": ("bluez5",),
    }[profile]

    text = ""

    def cond():
        nonlocal text

        host.pipewire.check_running()

        text = host.pipewire.pw_dump()
        try:
            data = json.loads(text)
        except:
            return False

        seen = set()
        for item in data:
            if item.get("type", None) == "PipeWire:Interface:Node":
                props = item["info"]["props"]
                seen.add(props.get("factory.name", None))
                continue
            if item.get("type", None) == "PipeWire:Interface:Device":
                props = item["info"]["props"]
                seen.add(props.get("device.api", None))
                continue

        if not set(factories).difference(seen):
            return True

        return False

    try:
        wait_until(cond)
    except TimeoutError:
        assert False, f"pipewire devices not seen within timeout:\n{text}"
