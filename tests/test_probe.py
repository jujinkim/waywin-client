# SPDX-License-Identifier: Apache-2.0
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from argparse import Namespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("probe", ROOT / "tools/probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class EndpointTests(unittest.TestCase):
    def test_supported_addresses(self):
        for value, expected in {
            "192.0.2.1": "192.0.2.1:3389", "pc.example:3390": "pc.example:3390",
            "rdp://pc.example:3390/": "pc.example:3390", "rdp://pc.example": "pc.example:3389",
            "[2001:db8::1]:3391": "[2001:db8::1]:3391", "2001:db8::1": "[2001:db8::1]:3389",
            "rdp://[::1]": "[::1]:3389", "PC.EXAMPLE": "pc.example:3389",
        }.items():
            with self.subTest(value=value): self.assertEqual(probe.endpoint(value), expected)

    def test_invalid_and_credential_addresses(self):
        for value in ("", "https://pc", "rdp://user:secret@pc", "pc:0", "pc:65536", "pc:",
                      "pc:abc", "pc/path", "pc?x=y", "pc#x", "/p:secret", "pc\n/p:secret",
                      "pc space", "-bad.host", "rdp://[xyz]", "rdp://", "pc..example"):
            with self.subTest(value=value), self.assertRaises(probe.ProbeError): probe.endpoint(value)

    def test_domain_is_separate_and_no_secret_or_redirection_flags(self):
        argv = probe.rdp_args(Path("/bin/sdl3-freerdp"), "rdp://pc:3390", "alice", "ACME", "host-a", "desktop", None)
        self.assertIn("/v:pc:3390", argv)
        self.assertIn("/d:ACME", argv)
        self.assertIn("-clipboard", argv)
        self.assertIn("-auto-reconnect", argv)
        self.assertFalse(any(x.startswith(("/p:", "/cert:", "/drive:", "/microphone")) for x in argv))

    def test_rail_single_program_and_no_options_injection(self):
        for app in (None, "program,cmd:calc", "bad\nname"):
            with self.subTest(app=app), self.assertRaises(probe.ProbeError):
                probe.rdp_args(Path("binary"), "pc", "alice", "", "a", "rail", app)
        argv = probe.rdp_args(Path("binary"), "pc", "alice", "", "a", "rail", "C:\\Program Files\\WayWin\\probe.exe")
        self.assertEqual(argv[-1], "/app:program:C:\\Program Files\\WayWin\\probe.exe")
        with self.assertRaises(probe.ProbeError):
            probe.rdp_args(Path("binary"), "pc", "alice", "", "a", "desktop", "notepad")

    def test_cli_does_not_echo_rejected_password(self):
        result = subprocess.run([sys.executable, ROOT / "tools/probe.py", "run", "--password", "SECRET-SENTINEL"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("SECRET-SENTINEL", result.stdout + result.stderr)


class EventTests(unittest.TestCase):
    def test_receives_numeric_metadata_only(self):
        line = b"WAYWIN_PROBE_V1 type=notify-update window=42 icon=3 flags=4294967295\n"
        self.assertEqual(probe.parse_event(line), {"type": "notify-update", "window": 42, "icon": 3, "flags": 4294967295})
        self.assertEqual(probe.parse_event(b"WAYWIN_PROBE_V1 driver=wayland\n"), {"type": "driver", "driver": "wayland"})

    def test_raw_and_malformed_logs_are_never_persisted(self):
        for line in (b"password=secret\n", b"DEBUG title=secret\n", b"\xff\n",
                     b"WAYWIN_PROBE_V1 type=notify-update window=1 icon=2 flags=3 title=secret\n",
                     b"WAYWIN_PROBE_V1 type=notify-update window=4294967296 icon=2 flags=3\n",
                     b"WAYWIN_PROBE_V1 driver=secret\n", b"prefix WAYWIN_PROBE_V1 driver=wayland\n"):
            with self.subTest(line=line): self.assertIsNone(probe.parse_event(line))

    def test_oversize_line_cannot_smuggle_valid_tail(self):
        valid = b"WAYWIN_PROBE_V1 driver=wayland\n"
        stream = io.BytesIO(b"secret" * 10000 + valid + valid)
        self.assertEqual(list(probe.events(stream)), [{"type": "driver", "driver": "wayland"}])

    def test_output_private_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "run.json"
            probe.private_json(target, {"schema": 1})
            self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o600)
            with self.assertRaises(FileExistsError): probe.private_json(target, {})
            link = Path(directory) / "link.json"
            link.symlink_to(target)
            with self.assertRaises(FileExistsError): probe.private_json(link, {})
            self.assertEqual(json.loads(target.read_text()), {"schema": 1})


class EnvironmentTests(unittest.TestCase):
    def test_macos_not_mistaken_for_target(self):
        with patch.object(probe.platform, "system", return_value="Darwin"):
            self.assertFalse(probe.doctor()["target_ready"])

    def test_real_socket_required_and_x11_rejected(self):
        # A short path also works under macOS's AF_UNIX path limit.
        with tempfile.TemporaryDirectory(dir="/tmp") as directory, socket.socket(socket.AF_UNIX) as sock:
            sock.bind(str(Path(directory) / "wl"))
            env = dict(XDG_RUNTIME_DIR=directory, WAYLAND_DISPLAY="wl", XDG_SESSION_TYPE="wayland", XDG_CURRENT_DESKTOP="ubuntu:GNOME")
            with patch.object(probe.platform, "system", return_value="Linux"), \
                 patch.object(probe.platform, "freedesktop_os_release", return_value={"ID": "ubuntu", "VERSION_ID": "26.04"}), \
                 patch.dict(os.environ, env, clear=True):
                self.assertTrue(probe.doctor()["target_ready"])
                self.assertFalse(probe.doctor()["runtime_verified"])
                os.environ["XDG_SESSION_TYPE"] = "x11"
                self.assertFalse(probe.doctor()["target_ready"])
                os.environ["XDG_SESSION_TYPE"] = "wayland"
                os.environ["WAYLAND_DISPLAY"] = "missing"
                self.assertFalse(probe.doctor()["target_ready"])


class GateTests(unittest.TestCase):
    def complete(self):
        report = probe.report_template()
        report.update(tester="test fixture (synthetic)", date="2026-11-13", applications=[f"App {n} v1" for n in range(5)])
        report["environment"] = {"ubuntu": "26.04", "gnome": "test", "sdl_driver": "wayland",
            "windows_edition_build": "test", "rdp_policy": "test", "freerdp_commit": probe.lock()["freerdp"]["commit"],
            "patch_sha256": probe.sha256(probe.PATCH)}
        for key, item in report["cases"].items():
            if key != "P08": item.update(status="pass", evidence=["synthetic-reference"], notes="Synthetic test only")
        return report

    def test_template_never_passes(self):
        report = probe.report_template()
        self.assertTrue(probe.assess(report))
        self.assertTrue(all(x["status"] == "not-run" for x in report["cases"].values()))

    def test_optional_case_not_required(self):
        self.assertEqual(probe.assess(self.complete()), [])

    def test_every_required_case_fail_closed(self):
        for case in probe.CASES:
            if case == "P08": continue
            for status in ("fail", "blocked", "not-run", "skipped"):
                with self.subTest(case=case, status=status):
                    report = self.complete()
                    report["cases"][case].update(status=status, required=False)
                    self.assertTrue(probe.assess(report))

    def test_receiving_events_is_not_visual_evidence(self):
        report = self.complete()
        report["cases"]["P04"].update(status="received", notes="Only notification orders observed")
        self.assertTrue(probe.assess(report))
        report = self.complete()
        report["cases"]["P04"]["evidence"] = []
        self.assertTrue(probe.assess(report))

    def test_wrong_platform_or_version_rejected(self):
        for key, value in (("sdl_driver", "x11"), ("ubuntu", "24.04"), ("freerdp_commit", "latest"), ("patch_sha256", "wrong")):
            report = self.complete()
            report["environment"][key] = value
            self.assertTrue(probe.assess(report))

    def test_malformed_report_and_duplicate_apps(self):
        for report in ([], {}, {"environment": []}, {"cases": []}, {"cases": {"P01": []}}):
            self.assertTrue(probe.assess(report))
        report = self.complete()
        report["applications"] = ["same"] * 5
        self.assertTrue(probe.assess(report))


@unittest.skipUnless(os.name == "posix", "POSIX process isolation test")
class IsolationTests(unittest.TestCase):
    def test_stopped_child_is_reaped_other_session_survives(self):
        children = [subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"],
                    start_new_session=True) for _ in range(2)]
        try:
            os.kill(children[0].pid, signal.SIGSTOP)
            start = time.monotonic()
            probe.stop_child(children[0], timeout=0.1)
            self.assertLess(time.monotonic() - start, 3)
            self.assertIsNotNone(children[0].poll())
            self.assertIsNone(children[1].poll())
            probe.stop_child(children[0], timeout=0.1)  # idempotent cleanup
        finally:
            for child in children: probe.stop_child(child, timeout=0.1)


class ProvenanceTests(unittest.TestCase):
    def test_patch_lock_matches(self):
        self.assertEqual(probe.sha256(probe.PATCH), probe.lock()["freerdp"]["probe_patch_sha256"])

    def test_binary_tampering_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "binary"
            binary.write_bytes(b"synthetic binary")
            data = {"schema": 1, "binary": "binary", "binary_sha256": probe.sha256(binary),
                    "source_commit": probe.lock()["freerdp"]["commit"],
                    "patch_sha256": probe.sha256(probe.PATCH), "profile_sha256": probe.sha256(probe.PROFILE)}
            manifest = Path(directory) / "build-manifest.json"
            manifest.write_text(json.dumps(data))
            self.assertEqual(probe.verified_binary(manifest)[0], binary.resolve())
            binary.write_bytes(b"changed binary")
            with self.assertRaises(probe.ProbeError): probe.verified_binary(manifest)


@unittest.skipUnless(os.name == "posix", "POSIX probe launcher")
class LaunchTests(unittest.TestCase):
    def test_end_to_end_evidence_discards_raw_logs_and_separates_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "synthetic-session"
            executable.write_text(f"#!{sys.executable}\n" +
                "import os, sys\n"
                "assert os.environ['SDL_VIDEO_DRIVER'] == 'wayland'\n"
                "assert os.environ['WLOG_APPENDER'] == 'CONSOLE'\n"
                "assert 'WLOG_FILEAPPENDER_OUTPUT_FILE_PATH' not in os.environ\n"
                "print('SECRET-SENTINEL', file=sys.stderr)\n"
                "print('WAYWIN_PROBE_V1 driver=wayland', file=sys.stderr)\n"
                "print('WAYWIN_PROBE_V1 type=window-create window=42 icon=0 flags=1', file=sys.stderr)\n")
            executable.chmod(0o700)
            provenance = {"system": "Linux", "native_wayland_helpers": True}
            args = Namespace(manifest=root / "manifest", server="example.test", user="alice", domain="",
                             label="host-a", mode="rail", app="||probe", output=root / "runs")
            with patch.object(probe, "doctor", return_value={"target_ready": True}), \
                 patch.object(probe, "verified_binary", return_value=(executable, provenance)), \
                 patch.dict(os.environ, {"WLOG_APPENDER": "FILE", "WLOG_FILEAPPENDER_OUTPUT_FILE_PATH": "secret.log"}), \
                 patch("sys.stdout", new_callable=io.StringIO):
                self.assertEqual(probe.run(args), 0)
                self.assertEqual(probe.run(args), 0)
            directories = list(args.output.iterdir())
            self.assertEqual(len(directories), 2)
            ids = []
            for run in directories:
                self.assertEqual(stat.S_IMODE(run.stat().st_mode), 0o700)
                data = [json.loads(line) for line in (run / "events.jsonl").read_text().splitlines()]
                self.assertEqual([item["sequence"] for item in data], [1, 2])
                ids.append(data[0]["run_id"])
                self.assertEqual(data[0]["run_id"], data[1]["run_id"])
                self.assertFalse(json.loads((run / "run.json").read_text())["pass"])
                self.assertNotIn("SECRET-SENTINEL", "".join(p.read_text() for p in run.iterdir()))
            self.assertNotEqual(*ids)


if __name__ == "__main__": unittest.main()
