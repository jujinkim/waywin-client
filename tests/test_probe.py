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
import threading
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
                      "pc space", "-bad.host", "rdp://[xyz]", "rdp://", "pc..example",
                      "999.1.1.1", "1.2.3", "0x7f.1", "2130706433", "pc.123"):
            with self.subTest(value=value), self.assertRaises(probe.ProbeError): probe.endpoint(value)

    def test_domain_is_separate_and_no_secret_or_redirection_flags(self):
        argv = probe.rdp_args(Path("/bin/sdl3-freerdp"), "rdp://pc:3390", "alice", "ACME", "host-a", "desktop", None)
        self.assertIn("/v:pc:3390", argv)
        self.assertIn("/d:ACME", argv)
        self.assertIn("-clipboard", argv)
        self.assertIn("-auto-reconnect", argv)
        # Only the protocol without certificate validation is switched off; TLS and NLA stay available.
        self.assertEqual([x for x in argv if x.startswith("/sec:")], ["/sec:rdp:off"])
        self.assertFalse(any(x.startswith(("/p:", "/cert:", "/drive:", "/microphone")) for x in argv))

    def test_rail_single_program_and_no_options_injection(self):
        for app in (None, "program,cmd:calc", "bad\nname"):
            with self.subTest(app=app), self.assertRaises(probe.ProbeError):
                probe.rdp_args(Path("binary"), "pc", "alice", "", "a", "rail", app)
        argv = probe.rdp_args(Path("binary"), "pc", "alice", "", "a", "rail", "C:\\Program Files\\WayWin\\probe.exe")
        self.assertEqual(argv[-1], "/app:program:C:\\Program Files\\WayWin\\probe.exe")
        with self.assertRaises(probe.ProbeError):
            probe.rdp_args(Path("binary"), "pc", "alice", "", "a", "desktop", "notepad")

    def test_text_options_reject_control_characters_and_embedded_domain(self):
        base = dict(server="pc", user="alice", domain="ACME", label="host-a")
        for field, value in (("user", "ACME\\alice"), ("user", ""), ("user", "a\nb"), ("user", "a" * 257),
                             ("domain", "AC\x7fME"), ("label", "host\ta"), ("label", "")):
            with self.subTest(field=field, value=value), self.assertRaises(probe.ProbeError):
                options = {**base, field: value}
                probe.rdp_args(Path("binary"), options["server"], options["user"], options["domain"],
                               options["label"], "desktop", None)
        argv = probe.rdp_args(Path("binary"), "pc", "alice@example.test", "", "host-a", "desktop", None)
        self.assertIn("/u:alice@example.test", argv)

    def test_cli_does_not_echo_rejected_password(self):
        result = subprocess.run([sys.executable, ROOT / "tools/probe.py", "run", "--password", "SECRET-SENTINEL"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("SECRET-SENTINEL", result.stdout + result.stderr)


class DisplayTests(unittest.TestCase):
    def argv(self, mode="desktop", **options):
        return probe.rdp_args(Path("binary"), "pc", "alice", "", "host-a", mode,
                              "||probe" if mode == "rail" else None, **options)

    def test_desktop_window_and_rail_layout_have_distinct_defaults(self):
        desktop = self.argv()
        self.assertIn("/size:1280x720", desktop)
        self.assertIn("-multimon", desktop)
        rail = self.argv("rail")
        self.assertIn("/multimon", rail)
        self.assertFalse(any(arg.startswith("/size:") for arg in rail))

    def test_selected_primary_order_is_preserved(self):
        args = self.argv(display_mode="multimon", monitors="3,1")
        self.assertIn("/monitors:3,1", args)
        self.assertNotIn("/size:1280x720", args)
        self.assertIn("/monitors:2", self.argv("rail", monitors="2"))
        self.assertIn("/monitors:65535", self.argv("rail", monitors="65535"))

    def test_invalid_ids_and_contradictory_modes_rejected(self):
        for monitors in ("0", "1,1", "-1", "65536", "4294967296", "1,", "1,2/p:secret", "1, 2", "", ",".join(str(n) for n in range(1, 18))):
            with self.subTest(monitors=monitors), self.assertRaises(probe.ProbeError):
                self.argv(display_mode="multimon", monitors=monitors)
        for mode, options in (("rail", {"display_mode": "window"}), ("desktop", {"monitors": "1"}),
                              ("desktop", {"display_mode": "multimon", "size": "1280x720"})):
            with self.subTest(mode=mode, options=options), self.assertRaises(probe.ProbeError):
                self.argv(mode, **options)

    def test_dynamic_layout_is_explicit(self):
        self.assertIn("-dynamic-resolution", self.argv("rail"))
        self.assertIn("+dynamic-resolution", self.argv("rail", dynamic=True))
        self.assertIn("/size:1920x1080", self.argv(size="1920x1080"))
        for size in ("100x100", "8193x800", "1920,1080", "1920x1080/p:secret"):
            with self.subTest(size=size), self.assertRaises(probe.ProbeError): self.argv(size=size)

    def test_monitor_inventory_never_connects(self):
        with patch.object(probe, "doctor", return_value={"target_ready": True}), \
             patch.object(probe, "verified_binary", return_value=(Path("/test/binary"),
                          {"system": "Linux", "native_wayland_helpers": True})), \
             patch.object(probe.subprocess, "run") as launch:
            launch.return_value.returncode = 0
            self.assertEqual(probe.list_monitors(Path("manifest")), 0)
            self.assertEqual(launch.call_args.args[0], ["/test/binary", "/list:monitor"])
            self.assertEqual(launch.call_args.kwargs["env"]["SDL_VIDEO_DRIVER"], "wayland")


class EventTests(unittest.TestCase):
    def test_receives_numeric_metadata_only(self):
        line = b"WAYWIN_PROBE_V1 type=notify-update window=42 icon=3 flags=4294967295\n"
        self.assertEqual(probe.parse_event(line), {"type": "notify-update", "window": 42, "icon": 3, "flags": 4294967295})
        self.assertEqual(probe.parse_event(b"WAYWIN_PROBE_V1 driver=wayland\n"), {"type": "driver", "driver": "wayland"})

    def test_raw_and_malformed_logs_are_never_persisted(self):
        for line in (b"password=secret\n", b"DEBUG title=secret\n", b"\xff\n",
                     b"WAYWIN_PROBE_V1 type=notify-update window=1 icon=2 flags=3 title=secret\n",
                     b"WAYWIN_PROBE_V1 type=notify-update window=4294967296 icon=2 flags=3\n",
                     b"prefix WAYWIN_PROBE_V1 driver=wayland\n"):
            with self.subTest(line=line): self.assertIsNone(probe.parse_event(line))

    def test_unrecognised_driver_is_reported_without_its_text(self):
        for name in (b"secret", b"kmsdrm", b"(null)", b"", b"\xff"):
            with self.subTest(name=name):
                self.assertEqual(probe.parse_event(b"WAYWIN_PROBE_V1 driver=" + name + b"\n"),
                                 {"type": "driver", "driver": "unknown"})
        for event in probe.EVENT_TYPES:
            line = f"WAYWIN_PROBE_V1 type={event} window=1 icon=2 flags=3\n".encode()
            self.assertEqual(probe.parse_event(line), {"type": event, "window": 1, "icon": 2, "flags": 3})

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
        for item in report["shell_surfaces"].values():
            item.update(status="pass", evidence=["synthetic-reference"], notes="Synthetic test only",
                        remote_ui_visible=True, interactive=True, same_session=True)
        for item in report["monitor_matrix"].values():
            item.update(status="pass", evidence=["synthetic-reference"], notes="Synthetic test only",
                        host_physical_layout="synthetic host", client_layout="synthetic client",
                        rdp_session_layout="synthetic session")
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

    def test_shell_launch_success_does_not_count_as_mirroring(self):
        for surface in probe.SHELL_CASES:
            for field in ("remote_ui_visible", "interactive", "same_session"):
                report = self.complete()
                report["shell_surfaces"][surface][field] = False
                self.assertTrue(probe.assess(report))
        report = self.complete()
        report["shell_surfaces"] = []
        self.assertTrue(probe.assess(report))

    def test_each_monitor_layout_and_scenario_is_required(self):
        for case in probe.MONITOR_CASES:
            for field in ("host_physical_layout", "client_layout", "rdp_session_layout"):
                report = self.complete()
                report["monitor_matrix"][case][field] = ""
                self.assertTrue(probe.assess(report))
            report = self.complete()
            report["monitor_matrix"][case]["status"] = "not-run"
            self.assertTrue(probe.assess(report))

    def test_legacy_report_cannot_skip_new_requirements(self):
        report = self.complete()
        report["schema"] = 1
        del report["shell_surfaces"]
        del report["monitor_matrix"]
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

    def test_patch_comparison_ignores_user_git_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            clean = {**os.environ, "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}
            clean.pop("GIT_DIFF_OPTS", None)
            def git(*arguments, env=clean):
                return subprocess.run(["git", "-C", source, "-c", "user.name=test", "-c", "user.email=test@localhost",
                                       *arguments], check=True, capture_output=True, text=True, env=env).stdout
            lines = [f"line {number}\n" for number in range(40)]
            (source / "file.txt").write_text("".join(lines))
            git("init", "--quiet")
            git("add", "file.txt")
            git("commit", "--quiet", "--message", "base")
            lines[5] = lines[30] = "changed\n"
            (source / "file.txt").write_text("".join(lines))
            approved = git("diff", "--binary", "--no-ext-diff", "HEAD")
            settings = root / "gitconfig"
            settings.write_text("[diff]\n\tnoprefix = true\n\tmnemonicPrefix = true\n\tcontext = 7\n"
                                "\tinterHunkContext = 5\n\talgorithm = patience\n\tsuppressBlankEmpty = true\n"
                                "\tindentHeuristic = false\n"
                                f"[core]\n\tabbrev = 12\n\tattributesFile = {root / 'attributes'}\n"
                                "[color]\n\tui = always\n")
            (root / "attributes").write_text("*.txt -diff\n")
            hostile = {"GIT_CONFIG_GLOBAL": str(settings), "GIT_DIFF_OPTS": "--unified=9"}
            self.assertNotEqual(git("diff", "--binary", "--no-ext-diff", "HEAD", env={**clean, **hostile}), approved)
            with patch.object(probe, "SOURCE", source), patch.dict(os.environ, {**clean, **hostile}):
                self.assertEqual(probe.source_diff(), approved)

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


class PrepareTests(unittest.TestCase):
    """prepare() against a throwaway upstream repository; no network."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        root = Path(self.directory.name).resolve()
        self.clean = {**os.environ, "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}
        self.upstream = root / "upstream"
        self.upstream.mkdir()
        (self.upstream / "file.txt").write_text("".join(f"line {number}\n" for number in range(20)))
        self.git(self.upstream, "init", "--quiet")
        self.git(self.upstream, "add", "file.txt")
        self.git(self.upstream, "commit", "--quiet", "--message", "base")
        self.git(self.upstream, "tag", "v1")
        commit = self.git(self.upstream, "rev-parse", "HEAD").strip()
        (self.upstream / "file.txt").write_text("".join(
            "observed\n" if number == 7 else f"line {number}\n" for number in range(20)))
        self.patch_file = root / "probe.patch"
        self.patch_file.write_text(self.git(self.upstream, "diff", "--binary", "--no-ext-diff", "HEAD"))
        self.git(self.upstream, "checkout", "--quiet", "--", "file.txt")
        self.source = root / "cache" / "source"
        dependency = {"repository": self.upstream.as_uri(), "tag": "v1", "commit": commit,
                      "probe_patch_sha256": probe.sha256(self.patch_file)}
        for name, value in (("SOURCE", self.source), ("PATCH", self.patch_file)):
            patcher = patch.object(probe, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        for patcher in (patch.object(probe, "lock", return_value={"freerdp": dependency}),
                        patch.dict(os.environ, self.clean)):
            patcher.start()
            self.addCleanup(patcher.stop)
        self.dependency = dependency

    def git(self, directory, *arguments):
        return subprocess.run(["git", "-C", directory, "-c", "user.name=test", "-c", "user.email=test@localhost",
                               *arguments], check=True, capture_output=True, text=True, env=self.clean).stdout

    def test_fetches_applies_once_and_is_repeatable(self):
        probe.prepare()
        self.assertIn("observed\n", (self.source / "file.txt").read_text())
        self.assertFalse(self.source.with_name("source.partial").exists())
        before = (self.source / "file.txt").read_bytes()
        probe.prepare()
        self.assertEqual((self.source / "file.txt").read_bytes(), before)

    def test_other_changes_and_untracked_files_are_kept_and_rejected(self):
        probe.prepare()
        (self.source / "file.txt").write_text((self.source / "file.txt").read_text() + "developer change\n")
        with self.assertRaises(probe.ProbeError): probe.prepare()
        self.assertIn("developer change", (self.source / "file.txt").read_text())
        self.git(self.source, "checkout", "--quiet", "--", "file.txt")
        probe.prepare()
        (self.source / "note.txt").write_text("untracked")
        with self.assertRaises(probe.ProbeError): probe.prepare()
        self.assertTrue((self.source / "note.txt").exists())

    def test_wrong_patch_or_commit_is_rejected(self):
        self.dependency["probe_patch_sha256"] = "0" * 64
        with self.assertRaises(probe.ProbeError): probe.prepare()
        self.assertFalse(self.source.exists())
        self.dependency["probe_patch_sha256"] = probe.sha256(self.patch_file)
        self.dependency["commit"] = "0" * 40
        with self.assertRaises(probe.ProbeError): probe.prepare()
        self.assertNotIn("observed\n", (self.source / "file.txt").read_text())

    def test_interrupted_fetch_is_recovered_and_incomplete_cache_is_refused(self):
        leftover = self.source.with_name("source.partial")
        leftover.mkdir(parents=True)
        (leftover / "half-written").write_text("x")
        probe.prepare()
        self.assertFalse(leftover.exists())
        self.assertIn("observed\n", (self.source / "file.txt").read_text())
        # A directory that is not its own repository must not be answered for by an enclosing one.
        outer = Path(self.directory.name).resolve() / "outer"
        outer.mkdir()
        self.git(outer, "init", "--quiet")
        empty = outer / "cache" / "source"
        empty.mkdir(parents=True)
        with patch.object(probe, "SOURCE", empty), self.assertRaises(probe.ProbeError) as raised:
            probe.prepare()
        self.assertIn("not a complete checkout", str(raised.exception))

    def test_failures_name_the_step_without_arguments(self):
        self.dependency["repository"] = (Path(self.directory.name) / "SECRET-SENTINEL-missing").as_uri()
        with patch("sys.stderr", new_callable=io.StringIO), self.assertRaises(probe.ProbeError) as raised:
            probe.prepare()
        self.assertIn("Fetching the pinned FreeRDP source failed", str(raised.exception))
        self.assertNotIn("SECRET-SENTINEL", str(raised.exception))
        with patch.object(probe.subprocess, "run", side_effect=FileNotFoundError), \
             self.assertRaises(probe.ProbeError) as raised:
            probe.command(["missing-program", "SECRET-SENTINEL"], step="Example step")
        self.assertEqual(str(raised.exception), "Example step: the program is not installed or not on PATH")

    @unittest.skipUnless(os.name == "posix", "advisory lock is POSIX only")
    def test_second_prepare_or_build_is_refused_while_one_runs(self):
        with probe.exclusive_cache():
            with self.assertRaises(probe.ProbeError), probe.exclusive_cache():
                pass
        with probe.exclusive_cache():
            pass


class CommandLineTests(unittest.TestCase):
    def cli(self, *arguments):
        return subprocess.run([sys.executable, ROOT / "tools/probe.py", *arguments], capture_output=True, text=True)

    def test_exit_codes_and_report_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "report.json"
            self.assertEqual(self.cli("new-report", report).returncode, 0)
            self.assertEqual(self.cli("new-report", report).returncode, 2)  # never overwritten
            result = self.cli("assess", report)
            self.assertEqual(result.returncode, 2)
            output = json.loads(result.stdout)
            self.assertFalse(output["record_complete"])
            self.assertFalse(output["independently_verified"])
            self.assertTrue(output["problems"])
            missing = self.cli("assess", Path(directory) / "SECRET-SENTINEL.json")
            self.assertEqual(missing.returncode, 2)
            self.assertNotIn("SECRET-SENTINEL", missing.stdout + missing.stderr)
            self.assertNotIn("Traceback", missing.stderr)

    def test_example_report_matches_the_template(self):
        example = json.loads((ROOT / "docs/feasibility-report.example.json").read_text(encoding="utf-8"))
        self.assertEqual(example, probe.report_template())


@unittest.skipUnless(os.name == "posix", "POSIX probe launcher")
class LaunchTests(unittest.TestCase):
    def test_wrong_driver_aborts_and_reaps_real_child(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "wrong-driver"
            executable.write_text(f"#!{sys.executable}\nimport sys, time\n"
                                  "print('WAYWIN_PROBE_V1 driver=x11', file=sys.stderr, flush=True)\n"
                                  "time.sleep(60)\n")
            executable.chmod(0o700)
            args = Namespace(manifest=root / "manifest", server="example.test", user="alice", domain="",
                             label="host-a", mode="rail", app="||probe", output=root / "runs",
                             display_mode="auto", monitors=None, size=None, dynamic_resolution=False)
            children = []
            original_popen = subprocess.Popen
            def spawn(*argv, **kwargs):
                child = original_popen(*argv, **kwargs)
                children.append(child)
                return child
            with patch.object(probe, "doctor", return_value={"target_ready": True}), \
                 patch.object(probe, "verified_binary", return_value=(executable,
                         {"system": "Linux", "native_wayland_helpers": True})), \
                 patch.object(probe.subprocess, "Popen", side_effect=spawn), \
                 patch("sys.stdout", new_callable=io.StringIO):
                with self.assertRaises(probe.ProbeError): probe.run(args)
            self.assertEqual(len(children), 1)
            self.assertIsNotNone(children[0].poll())
            run = next(args.output.iterdir())
            record = json.loads((run / "exit.json").read_text())
            self.assertEqual((record["ended"], record["exit_code"], record["sdl_driver_observed"]),
                             ("aborted", None, "x11"))
            self.assertEqual(json.loads((run / "events.jsonl").read_text())["driver"], "x11")

    def test_repeated_interrupt_still_kills_child_and_records_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "ignores-sigterm"
            executable.write_text(f"#!{sys.executable}\nimport signal, sys, time\n"
                                  "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                                  "print('WAYWIN_PROBE_V1 driver=wayland', file=sys.stderr, flush=True)\n"
                                  "time.sleep(60)\n")
            executable.chmod(0o700)
            args = Namespace(manifest=root / "manifest", server="example.test", user="alice", domain="",
                             label="host-a", mode="rail", app="||probe", output=root / "runs",
                             display_mode="auto", monitors=None, size=None, dynamic_resolution=False)
            children = []
            original_popen, original_stop = subprocess.Popen, probe.stop_child
            def spawn(*argv, **kwargs):
                child = original_popen(*argv, **kwargs)
                children.append(child)
                return child
            cleaning = threading.Event()
            def stop(child):
                cleaning.set()
                original_stop(child, timeout=1.0)
            def interrupt_twice():
                # The first event proves the child ignores SIGTERM and run() is inside its try block.
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline and not any(
                        path.stat().st_size for path in args.output.glob("*/events.jsonl")):
                    time.sleep(0.02)
                os.kill(os.getpid(), signal.SIGINT)
                if cleaning.wait(10):  # Now inside stop_child's grace period.
                    os.kill(os.getpid(), signal.SIGINT)
                    os.kill(os.getpid(), signal.SIGTERM)
            sender = threading.Thread(target=interrupt_twice)
            handlers = [signal.getsignal(number) for number in (signal.SIGINT, signal.SIGTERM)]
            try:
                with patch.object(probe, "doctor", return_value={"target_ready": True}), \
                     patch.object(probe, "verified_binary", return_value=(executable,
                             {"system": "Linux", "native_wayland_helpers": True})), \
                     patch.object(probe.subprocess, "Popen", side_effect=spawn), \
                     patch.object(probe, "stop_child", side_effect=stop), \
                     patch("sys.stdout", new_callable=io.StringIO):
                    sender.start()
                    try:
                        self.assertEqual(probe.run(args), 130)
                    except KeyboardInterrupt:
                        self.fail("a repeated signal aborted the cleanup")
                sender.join()
                self.assertEqual([signal.getsignal(number) for number in (signal.SIGINT, signal.SIGTERM)], handlers)
                self.assertIsNotNone(children[0].poll())
                record = json.loads(next(args.output.glob("*/exit.json")).read_text())
                self.assertEqual(record, {"ended": "interrupted", "exit_code": None,
                                          "sdl_driver_observed": "wayland", "events_seen": 1,
                                          "events_truncated": False, "runtime_verified": False})
            finally:
                sender.join()
                for child in children: original_stop(child, timeout=0.1)

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
                             label="host-a", mode="rail", app="||probe", output=root / "runs",
                             display_mode="auto", monitors="2,1", size=None, dynamic_resolution=True)
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
                run_record = json.loads((run / "run.json").read_text())
                self.assertFalse(run_record["pass"])
                self.assertEqual(run_record["requested_display"], {"mode": "multimon", "monitor_ids": [2, 1],
                    "window_size": None, "dynamic_resolution": True})
                self.assertNotIn("SECRET-SENTINEL", "".join(p.read_text() for p in run.iterdir()))
                self.assertEqual(json.loads((run / "exit.json").read_text()), {
                    "ended": "session-exit", "exit_code": 0, "sdl_driver_observed": "wayland",
                    "events_seen": 2, "events_truncated": False, "runtime_verified": False})
            self.assertNotEqual(*ids)

    def test_hangup_ends_the_session(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "waits"
            executable.write_text(f"#!{sys.executable}\nimport sys, time\n"
                                  "print('WAYWIN_PROBE_V1 driver=wayland', file=sys.stderr, flush=True)\n"
                                  "time.sleep(60)\n")
            executable.chmod(0o700)
            args = Namespace(manifest=root / "manifest", server="example.test", user="alice", domain="",
                             label="host-a", mode="rail", app="||probe", output=root / "runs",
                             display_mode="auto", monitors=None, size=None, dynamic_resolution=False)
            children = []
            original_popen = subprocess.Popen
            def spawn(*argv, **kwargs):
                child = original_popen(*argv, **kwargs)
                children.append(child)
                return child
            def hang_up():
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline and not any(
                        path.stat().st_size for path in args.output.glob("*/events.jsonl")):
                    time.sleep(0.02)
                os.kill(os.getpid(), signal.SIGHUP)
            sender = threading.Thread(target=hang_up)
            before = signal.getsignal(signal.SIGHUP)
            # Without a handler the default action would end the test process itself.
            signal.signal(signal.SIGHUP, signal.SIG_IGN)
            try:
                with patch.object(probe, "doctor", return_value={"target_ready": True}), \
                     patch.object(probe, "verified_binary", return_value=(executable,
                             {"system": "Linux", "native_wayland_helpers": True})), \
                     patch.object(probe.subprocess, "Popen", side_effect=spawn), \
                     patch("sys.stdout", new_callable=io.StringIO):
                    sender.start()
                    self.assertEqual(probe.run(args), 130)
                self.assertIsNotNone(children[0].poll())
                self.assertEqual(json.loads(next(args.output.glob("*/exit.json")).read_text())["ended"], "interrupted")
            finally:
                sender.join()
                signal.signal(signal.SIGHUP, before)
                for child in children: probe.stop_child(child, timeout=0.1)

    def test_leftover_process_cannot_hold_the_run_open(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "leaves-a-process"
            executable.write_text(f"#!{sys.executable}\nimport subprocess, sys\n"
                "other = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
                f"open({str(root / 'other.pid')!r}, 'w').write(str(other.pid))\n"
                "print('WAYWIN_PROBE_V1 driver=wayland', file=sys.stderr, flush=True)\n")
            executable.chmod(0o700)
            args = Namespace(manifest=root / "manifest", server="example.test", user="alice", domain="",
                             label="host-a", mode="rail", app="||probe", output=root / "runs",
                             display_mode="auto", monitors=None, size=None, dynamic_resolution=False)
            other = None
            try:
                with patch.object(probe, "doctor", return_value={"target_ready": True}), \
                     patch.object(probe, "verified_binary", return_value=(executable,
                             {"system": "Linux", "native_wayland_helpers": True})), \
                     patch.object(probe, "GROUP_GRACE", 0.2), \
                     patch("sys.stdout", new_callable=io.StringIO):
                    start = time.monotonic()
                    self.assertEqual(probe.run(args), 0)
                    self.assertLess(time.monotonic() - start, 20)
                other = int((root / "other.pid").read_text())
                record = json.loads(next(args.output.glob("*/exit.json")).read_text())
                self.assertEqual((record["ended"], record["exit_code"]), ("session-exit", 0))
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    try:
                        os.kill(other, 0)
                    except ProcessLookupError:
                        break
                    time.sleep(0.05)
                else:
                    self.fail("the leftover process survived the run")
            finally:
                if other is None and (root / "other.pid").exists():
                    other = int((root / "other.pid").read_text())
                if other:
                    try:
                        os.kill(other, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_event_cap_and_failed_session_are_recorded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "flood"
            executable.write_text(f"#!{sys.executable}\nimport sys\n"
                "for number in range(1, 6):\n"
                "    print(f'WAYWIN_PROBE_V1 type=window-create window={number} icon=0 flags=1', file=sys.stderr)\n"
                "sys.exit(7)\n")
            executable.chmod(0o700)
            args = Namespace(manifest=root / "manifest", server="example.test", user="alice", domain="",
                             label="host-a", mode="desktop", app=None, output=root / "runs",
                             display_mode="auto", monitors=None, size=None, dynamic_resolution=False)
            with patch.object(probe, "doctor", return_value={"target_ready": True}), \
                 patch.object(probe, "verified_binary", return_value=(executable,
                         {"system": "Linux", "native_wayland_helpers": True})), \
                 patch.object(probe, "EVENT_LIMIT", 3), \
                 patch("sys.stdout", new_callable=io.StringIO):
                self.assertEqual(probe.run(args), 1)
            run = next(args.output.iterdir())
            stored = [json.loads(line) for line in (run / "events.jsonl").read_text().splitlines()]
            self.assertEqual([item["window"] for item in stored], [1, 2, 3])
            self.assertEqual(json.loads((run / "exit.json").read_text()), {
                "ended": "session-exit", "exit_code": 7, "sdl_driver_observed": None,
                "events_seen": 5, "events_truncated": True, "runtime_verified": False})


if __name__ == "__main__": unittest.main()
