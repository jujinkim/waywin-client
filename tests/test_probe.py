# SPDX-License-Identifier: Apache-2.0
import importlib.util
import io
import json
import os
from pathlib import Path
import re
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
            "fe80::1%eth0": "[fe80::1%eth0]:3389", "[fe80::1%wlp2s0]:3390": "[fe80::1%wlp2s0]:3390",
        }.items():
            with self.subTest(value=value): self.assertEqual(probe.endpoint(value), expected)

    def test_invalid_and_credential_addresses(self):
        for value in ("", "https://pc", "rdp://user:secret@pc", "pc:0", "pc:65536", "pc:",
                      "pc:abc", "pc/path", "pc?x=y", "pc#x", "/p:secret", "pc\n/p:secret",
                      "pc space", "-bad.host", "rdp://[xyz]", "rdp://", "pc..example",
                      "999.1.1.1", "1.2.3", "0x7f.1", "2130706433", "pc.123",
                      "fe80::1%]:1", "fe80::1%a,b", "::1%x:y", "::1%@", "[fe80::1%a b]", "rdp://[fe80::1%a,b]"):
            with self.subTest(value=value), self.assertRaises(probe.ProbeError): probe.endpoint(value)

    def test_domain_is_separate_and_no_secret_or_redirection_flags(self):
        argv = probe.rdp_args(Path("/bin/sdl3-freerdp"), "rdp://pc:3390", "alice", "ACME", "host-a", "desktop", None)
        self.assertIn("/v:pc:3390", argv)
        self.assertIn("/d:ACME", argv)
        self.assertIn("-clipboard", argv)
        self.assertIn("-auto-reconnect", argv)
        # Only the protocol without certificate validation is switched off; TLS and NLA stay available.
        self.assertEqual([x for x in argv if x.startswith("/sec:")], ["/sec:rdp:off"])
        self.assertEqual([x for x in argv if x.startswith("/proxy")], ["/proxy:ignore"])
        self.assertFalse(any(x.startswith(("/p:", "/cert:", "/drive:", "/microphone")) for x in argv))

    def test_rail_single_program_and_no_options_injection(self):
        for app in (None, "program,cmd:calc", "bad\nname"):
            with self.subTest(app=app), self.assertRaises(probe.ProbeError):
                probe.rdp_args(Path("binary"), "pc", "alice", "", "a", "rail", app)
        argv = probe.rdp_args(Path("binary"), "pc", "alice", "", "a", "rail", "C:\\Program Files\\WayWin\\probe.exe")
        self.assertEqual(argv[-1], "/app:program:C:\\Program Files\\WayWin\\probe.exe")
        with self.assertRaises(probe.ProbeError):
            probe.rdp_args(Path("binary"), "pc", "alice", "", "a", "desktop", "notepad")

    def test_rail_can_join_without_starting_a_program(self):
        argv = probe.rdp_args(Path("binary"), "pc", "alice", "", "a", "rail", None, launch=False)
        self.assertEqual(argv[-1], "/app:program:")
        for mode, app in (("rail", "||probe"), ("desktop", None), ("desktop", "notepad")):
            with self.subTest(mode=mode, app=app), self.assertRaises(probe.ProbeError):
                probe.rdp_args(Path("binary"), "pc", "alice", "", "a", mode, app, launch=False)

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
    def test_session_environment_drops_variables_that_redirect_the_client(self):
        dropped = {"FREERDP_ASKPASS": "/tmp/askpass", "https_proxy": "http://proxy.test:3128",
                   "HTTPS_PROXY": "http://proxy.test:3128", "no_proxy": "*", "all_proxy": "socks5://proxy.test",
                   "LD_PRELOAD": "/tmp/x.so", "LD_LIBRARY_PATH": "/tmp", "DYLD_INSERT_LIBRARIES": "/tmp/x.dylib",
                   "OPENSSL_CONF": "/tmp/openssl.cnf", "OPENSSL_MODULES": "/tmp", "SSL_CERT_FILE": "/tmp/ca.pem",
                   "SSL_CERT_DIR": "/tmp", "SSLKEYLOGFILE": "/tmp/keys", "WLOG_APPENDER": "FILE",
                   "WLOG_FILEAPPENDER_OUTPUT_FILE_PATH": "/tmp", "WINPR_X": "1", "WTSAPI_LIBRARY": "/tmp/x.so",
                   "SDL_VIDEODRIVER": "x11", "GTK_MODULES": "x", "GIO_EXTRA_MODULES": "/tmp", "TZDIR": "/tmp"}
        kept = {"WAYLAND_DISPLAY": "wayland-0", "XDG_RUNTIME_DIR": "/run/user/1000", "HOME": "/home/test",
                "XDG_CONFIG_HOME": "/home/test/.config", "LANG": "ko_KR.UTF-8", "XMODIFIERS": "@im=ibus",
                "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus", "PATH": "/usr/bin", "TZ": "Asia/Seoul"}
        with patch.dict(os.environ, {**dropped, **kept}, clear=True):
            env = probe.session_environment()
        self.assertEqual({key: env[key] for key in kept}, kept)
        self.assertEqual(set(env) - set(kept), {"SDL_VIDEO_DRIVER", "WAYWIN_PROBE_EVENTS", "WLOG_LEVEL", "WLOG_APPENDER"})
        for value, expected in (("fcitx", "fcitx"), ("ibus", "ibus"), ("/tmp/module.so", None)):
            with patch.dict(os.environ, {"SDL_IM_MODULE": value}, clear=True):
                self.assertEqual(probe.session_environment().get("SDL_IM_MODULE"), expected)
        self.assertEqual((env["SDL_VIDEO_DRIVER"], env["WLOG_APPENDER"], env["WLOG_LEVEL"]), ("wayland", "CONSOLE", "ERROR"))

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

    def test_group_members_are_ended_after_the_leader_has_exited(self):
        with tempfile.TemporaryDirectory() as directory:
            record = Path(directory) / "member.pid"
            leader = subprocess.Popen([sys.executable, "-c",
                "import subprocess, sys\n"
                "member = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'],\n"
                "    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)\n"
                f"open({str(record)!r}, 'w').write(str(member.pid))\n"], start_new_session=True)
            leader.wait()
            member = int(record.read_text())
            try:
                os.kill(member, 0)  # still running, and the leader is gone
                probe.stop_child(leader, timeout=0.1)
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    try:
                        os.kill(member, 0)
                    except ProcessLookupError:
                        break
                    time.sleep(0.05)
                else:
                    self.fail("a member of the session's group survived stop_child")
            finally:
                try:
                    os.kill(member, signal.SIGKILL)
                except ProcessLookupError:
                    pass


class ProvenanceTests(unittest.TestCase):
    def test_patch_lock_matches(self):
        self.assertEqual(probe.sha256(probe.PATCH), probe.lock()["freerdp"]["probe_patch_sha256"])

    def test_patch_observes_the_driver_once_for_every_session_mode(self):
        text = probe.PATCH.read_text(encoding="utf-8")
        files = re.findall(r"^diff --git a/(\S+) b/\1$", text, re.MULTILINE)
        self.assertEqual(files, ["client/SDL/SDL3/sdl_freerdp.cpp", "client/SDL/SDL3/sdl_rail.cpp"])
        sections = dict(zip(files, re.split(r"^diff --git .*$", text, flags=re.MULTILINE)[1:]))
        added = {name: [line[1:] for line in body.splitlines()
                        if line.startswith("+") and not line.startswith("+++")]
                 for name, body in sections.items()}
        # Emitted right after SDL_Init in main(), before any connection, so desktop runs are
        # observed too; RAIL initialisation no longer repeats it.
        driver = [line for lines in added.values() for line in lines if "driver=" in line]
        self.assertEqual(len(driver), 1)
        self.assertIn("backend ? backend : \"\"", driver[0])
        self.assertTrue(any("driver=" in line for line in added["client/SDL/SDL3/sdl_freerdp.cpp"]))
        # Both observers stay behind the same opt-in variable and never print without it.
        for name, lines in added.items():
            with self.subTest(file=name):
                self.assertIn('WAYWIN_PROBE_EVENTS', "\n".join(lines))
                self.assertIn('"1"', "\n".join(lines))

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
            for name in ("a.txt", "file.txt"):
                (source / name).write_text("".join(lines))
            git("init", "--quiet")
            git("add", "a.txt", "file.txt")
            git("commit", "--quiet", "--message", "base")
            lines[5] = lines[30] = "changed\n"
            for name in ("a.txt", "file.txt"):
                (source / name).write_text("".join(lines))
            approved = git("diff", "--binary", "--no-ext-diff", "HEAD")
            # A hook's GIT_DIR/GIT_INDEX_FILE name another repository with nothing changed in it.
            other = root / "other"
            other.mkdir()
            subprocess.run(["git", "-C", other, "-c", "user.name=test", "-c", "user.email=test@localhost",
                            "init", "--quiet"], check=True, env=clean)
            settings = root / "gitconfig"
            settings.write_text("[diff]\n\tnoprefix = true\n\tmnemonicPrefix = true\n\tcontext = 7\n"
                                "\tinterHunkContext = 5\n\talgorithm = patience\n\tsuppressBlankEmpty = true\n"
                                f"\tindentHeuristic = false\n\torderFile = {root / 'order'}\n"
                                f"[core]\n\tabbrev = 12\n\tattributesFile = {root / 'attributes'}\n"
                                "[color]\n\tui = always\n")
            (root / "attributes").write_text("*.txt -diff\n")
            (root / "order").write_text("file.txt\na.txt\n")
            hostile = {"GIT_CONFIG_GLOBAL": str(settings), "GIT_DIFF_OPTS": "--unified=9",
                       "GIT_DIR": str(other / ".git"), "GIT_INDEX_FILE": str(other / ".git" / "index"),
                       "GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "diff.context", "GIT_CONFIG_VALUE_0": "5",
                       "GIT_CONFIG_PARAMETERS": "'diff.noprefix'='true'"}
            for name in ("GIT_CONFIG_GLOBAL", "GIT_DIFF_OPTS", "GIT_DIR", "GIT_CONFIG_COUNT", "GIT_CONFIG_PARAMETERS"):
                single = {name: hostile[name]}
                if name == "GIT_DIR":
                    single["GIT_INDEX_FILE"] = hostile["GIT_INDEX_FILE"]
                if name == "GIT_CONFIG_COUNT":
                    single.update(GIT_CONFIG_KEY_0="diff.context", GIT_CONFIG_VALUE_0="5")
                with self.subTest(setting=name):
                    self.assertNotEqual(subprocess.run(
                        ["git", "-C", source, "diff", "--binary", "--no-ext-diff", "HEAD"], check=False,
                        capture_output=True, text=True, env={**clean, **single}).stdout, approved)
            with patch.object(probe, "SOURCE", source), patch.dict(os.environ, {**clean, **hostile}):
                self.assertEqual(probe.source_diff(), approved)
            # The checkout's own attributes cannot be overridden, so they are refused instead.
            local = source / ".git" / "info" / "attributes"
            local.parent.mkdir(exist_ok=True)
            local.write_text("# comments and blank lines are harmless\n\n")
            with patch.object(probe, "SOURCE", source), patch.dict(os.environ, clean):
                self.assertEqual(probe.source_diff(), approved)
                local.write_text("*.txt -diff\n")
                with self.assertRaises(probe.ProbeError) as raised:
                    probe.source_diff()
            self.assertIn("info/attributes", str(raised.exception))

    def manifest(self, directory):
        (directory / "bin").mkdir()
        (directory / "lib").mkdir()
        binary = directory / "bin" / "client"
        binary.write_bytes(b"synthetic binary")
        library = directory / "lib" / "libfreerdp3.so.3.33.1"
        library.write_bytes(b"synthetic library")
        (directory / "lib" / "libfreerdp3.so.3").symlink_to("libfreerdp3.so.3.33.1")
        (directory / "lib" / "libwinpr3.3.33.1.dylib").write_bytes(b"other library")
        (directory / "CMakeFiles").mkdir()
        (directory / "CMakeFiles" / "probe.so").write_bytes(b"configure check, not a product")
        data = {"schema": 2, "binary": "bin/client", "binary_sha256": probe.sha256(binary),
                "libraries": probe.runtime_libraries(directory),
                "source_commit": probe.lock()["freerdp"]["commit"],
                "patch_sha256": probe.sha256(probe.PATCH), "profile_sha256": probe.sha256(probe.PROFILE)}
        path = directory / "build-manifest.json"
        path.write_text(json.dumps(data))
        return path, data, binary, library

    def test_matching_build_is_accepted_and_libraries_are_listed(self):
        with tempfile.TemporaryDirectory() as name:
            path, data, binary, _ = self.manifest(Path(name).resolve())
            self.assertEqual(data["libraries"], {
                "lib/libfreerdp3.so.3": "-> lib/libfreerdp3.so.3.33.1",
                "lib/libfreerdp3.so.3.33.1": probe.sha256(directory_library := Path(name).resolve() / "lib/libfreerdp3.so.3.33.1"),
                "lib/libwinpr3.3.33.1.dylib": probe.sha256(directory_library.with_name("libwinpr3.3.33.1.dylib"))})
            self.assertEqual(probe.verified_binary(path)[0], binary)

    def test_binary_tampering_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            path, _, binary, _ = self.manifest(Path(name).resolve())
            binary.write_bytes(b"changed binary")
            with self.assertRaises(probe.ProbeError): probe.verified_binary(path)

    def test_library_changes_are_rejected(self):
        def modified(directory, library): library.write_bytes(b"changed library")
        def added(directory, library): (directory / "lib" / "libextra.so").write_bytes(b"x")
        def removed(directory, library): library.unlink()
        def redirected(directory, library):
            outside = directory.parent / (directory.name + "-outside.so")
            outside.write_bytes(b"synthetic library")
            self.addCleanup(outside.unlink)
            (directory / "lib" / "libfreerdp3.so.3").unlink()
            (directory / "lib" / "libfreerdp3.so.3").symlink_to(outside)
        def repointed_inside(directory, library):
            (directory / "payload.bin").write_bytes(b"not a built library")
            (directory / "lib" / "libfreerdp3.so.3").unlink()
            (directory / "lib" / "libfreerdp3.so.3").symlink_to("../payload.bin")
        def repointed_to_other_library(directory, library):
            (directory / "lib" / "libfreerdp3.so.3").unlink()
            (directory / "lib" / "libfreerdp3.so.3").symlink_to("libwinpr3.3.33.1.dylib")
        def link_added(directory, library): (directory / "lib" / "libssl.so.3").symlink_to("libfreerdp3.so.3.33.1")
        def looped(directory, library): (directory / "lib" / "libloop.so").symlink_to("libloop.so")
        for change in (modified, added, removed, redirected, repointed_inside, repointed_to_other_library,
                       link_added, looped):
            with self.subTest(change=change.__name__), tempfile.TemporaryDirectory() as name:
                directory = Path(name).resolve()
                path, _, _, library = self.manifest(directory)
                change(directory, library)
                with self.assertRaises(probe.ProbeError): probe.verified_binary(path)

    def test_stale_or_malformed_manifest_is_rejected(self):
        changes = ({"schema": 1}, {"source_commit": "0" * 40}, {"patch_sha256": "0" * 64},
                   {"profile_sha256": "0" * 64}, {"binary": "../outside"}, {"binary": 7}, {"libraries": None})
        for change in changes:
            with self.subTest(change=change), tempfile.TemporaryDirectory() as name:
                directory = Path(name).resolve()
                path, data, binary, _ = self.manifest(directory)
                if change.get("binary") == "../outside":
                    outside = directory.parent / "outside"
                    outside.write_bytes(binary.read_bytes())
                    self.addCleanup(outside.unlink)
                path.write_text(json.dumps({**data, **change}))
                with self.assertRaises(probe.ProbeError): probe.verified_binary(path)
        with tempfile.TemporaryDirectory() as name:
            for content in (None, "not json", "[]"):
                path = Path(name) / "build-manifest.json"
                if content is not None: path.write_text(content)
                with self.subTest(content=content), self.assertRaises(probe.ProbeError) as raised:
                    probe.verified_binary(path)
                self.assertIn("run build first", str(raised.exception))


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

    def test_user_apply_and_repository_settings_do_not_change_the_result(self):
        # Trailing whitespace in an added line: apply.whitespace=fix would strip it.
        (self.upstream / "file.txt").write_text("".join(
            "observed  \n" if number == 7 else f"line {number}\n" for number in range(20)))
        self.patch_file.write_text(self.git(self.upstream, "diff", "--binary", "--no-ext-diff", "HEAD"))
        self.git(self.upstream, "checkout", "--quiet", "--", "file.txt")
        self.dependency["probe_patch_sha256"] = probe.sha256(self.patch_file)
        settings = Path(self.directory.name) / "gitconfig"
        settings.write_text("[apply]\n\twhitespace = fix\n")
        hook = Path(self.directory.name).resolve() / "hook-repository"
        hook.mkdir()
        self.git(hook, "init", "--quiet")
        with patch.dict(os.environ, {"GIT_CONFIG_GLOBAL": str(settings), "GIT_DIR": str(hook / ".git"),
                                     "GIT_WORK_TREE": str(hook)}):
            probe.prepare()
            probe.prepare()
        self.assertIn("observed  \n", (self.source / "file.txt").read_text())
        self.assertEqual(self.git(hook, "status", "--porcelain"), "")
        # Files hidden by the user's global ignore file, or attributes hidden by the checkout's
        # own exclude list, are still found.
        ignore = Path(self.directory.name) / "ignore"
        ignore.write_text("*.local\n")
        settings.write_text(f"[core]\n\texcludesFile = {ignore}\n")
        (self.source / "notes.local").write_text("untracked")
        with patch.dict(os.environ, {"GIT_CONFIG_GLOBAL": str(settings)}), self.assertRaises(probe.ProbeError):
            probe.prepare()
        (self.source / "notes.local").unlink()
        (self.source / ".git" / "info").mkdir(exist_ok=True)
        (self.source / ".git" / "info" / "exclude").write_text(".gitattributes\n")
        (self.source / ".gitattributes").write_text("*.txt -diff\n")
        with self.assertRaises(probe.ProbeError) as raised:
            probe.prepare()
        self.assertIn(".gitattributes", str(raised.exception))

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
        # The lock file is never truncated and never followed through a link.
        lock = self.source.parent / "probe.lock"
        lock.unlink()
        victim = self.source.parent / "victim.txt"
        victim.write_text("important")
        lock.symlink_to(victim)
        with self.assertRaises(probe.ProbeError), probe.exclusive_cache():
            pass
        self.assertEqual(victim.read_text(), "important")


FAKE_CMAKE = """\
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(args) + "\\n")
def status(name):
    code = int(os.environ.get(name, "0"))
    if code:
        sys.exit(code)
if "-S" in args:
    status("FAKE_CONFIGURE_STATUS")
    build = Path(args[args.index("-B") + 1])
    deps = Path(os.environ["FAKE_DEPS"])
    (build / "CMakeCache.txt").write_text(
        f"SDL3_DIR:PATH={deps / 'sdl3'}\\nOPENSSL_INCLUDE_DIR:PATH={deps / 'include'}\\n"
        "//comment: not an entry\\n")
    flags = "-DWITH_SDL_WAYLAND_NATIVE" if os.environ.get("FAKE_NATIVE") == "1" else ""
    (build / "compile_commands.json").write_text(json.dumps([{"command": "c++ " + flags}]))
elif "--build" in args:
    status("FAKE_BUILD_STATUS")
    build = Path(args[args.index("--build") + 1])
    library = build / "libfreerdp" / "libfreerdp3.so.3.33.1"
    library.parent.mkdir(exist_ok=True)
    library.write_bytes(b"synthetic library")
    link = library.with_name("libfreerdp3.so.3")
    if not link.is_symlink():
        link.symlink_to(library.name)
    if os.environ.get("FAKE_NO_BINARY") != "1":
        client = build / "client" / "SDL" / "SDL3" / "sdl3-freerdp"
        client.parent.mkdir(parents=True, exist_ok=True)
        client.write_text("#!/bin/sh\\necho \\"$FAKE_VERSION\\"\\n")
        client.chmod(0o755)
"""


@unittest.skipUnless(os.name == "posix", "the fake toolchain uses POSIX scripts and links")
class BuildTests(unittest.TestCase):
    """build() through a fake cmake on PATH: configure, build, version and manifest steps."""

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        tools = self.root / "bin"
        tools.mkdir()
        cmake = tools / "cmake"
        cmake.write_text(f"#!{sys.executable}\n" + FAKE_CMAKE)
        cmake.chmod(0o755)
        deps = self.root / "deps"
        (deps / "sdl3").mkdir(parents=True)
        (deps / "sdl3" / "SDL3ConfigVersion.cmake").write_text('set(PACKAGE_VERSION "3.4.18")\n')
        (deps / "include" / "openssl").mkdir(parents=True)
        (deps / "include" / "openssl" / "opensslv.h").write_text(
            '# define OPENSSL_VERSION_TEXT "OpenSSL 4.0.3 1 Oct 2026"\n')
        self.build = self.root / "build"
        self.log = self.root / "calls.jsonl"
        self.manifest = self.build / "build-manifest.json"
        self.environment = {"PATH": f"{tools}{os.pathsep}{os.environ.get('PATH', '')}",
                            "FAKE_LOG": str(self.log), "FAKE_DEPS": str(deps), "FAKE_NATIVE": "1",
                            "FAKE_VERSION": "This is FreeRDP version 3.33.1 (3.33.1)"}
        for patcher in (patch.object(probe, "BUILD", self.build), patch.object(probe, "prepare"),
                        patch.dict(os.environ, self.environment)):
            patcher.start()
            self.addCleanup(patcher.stop)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def failed_build(self, message, **environment):
        # A manifest from an earlier build must not survive a failed one.
        self.build.mkdir(exist_ok=True)
        self.manifest.write_text("{}")
        with patch.dict(os.environ, environment), self.assertRaises(probe.ProbeError) as raised:
            probe.build(3)
        self.assertIn(message, str(raised.exception))
        self.assertFalse(self.manifest.exists())
        return str(raised.exception)

    def test_successful_build_writes_a_manifest_that_verifies(self):
        with patch("sys.stdout", new_callable=io.StringIO) as output:
            probe.build(3)
        self.assertIn("No RDP compatibility test was performed", output.getvalue())
        configure, compile_ = self.calls()
        self.assertEqual(configure, ["-S", str(probe.SOURCE), "-B", str(self.build), "-G", "Ninja",
                                     "-C", str(probe.PROFILE), "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"])
        self.assertEqual(compile_, ["--build", str(self.build), "--target", "sdl3-freerdp", "--parallel", "3"])
        self.assertEqual(stat.S_IMODE(self.manifest.stat().st_mode), 0o600)
        binary, manifest = probe.verified_binary(self.manifest)
        self.assertEqual(binary, self.build / "client/SDL/SDL3/sdl3-freerdp")
        self.assertEqual(manifest["dependency_versions"], {"SDL3": "3.4.18", "OpenSSL": "OpenSSL 4.0.3 1 Oct 2026"})
        self.assertEqual(manifest["libraries"]["libfreerdp/libfreerdp3.so.3"], "-> libfreerdp/libfreerdp3.so.3.33.1")
        self.assertTrue(manifest["native_wayland_helpers"])
        self.assertIn("3.33.1", manifest["version"])
        # A second build replaces the manifest instead of failing on the existing file.
        with patch("sys.stdout", new_callable=io.StringIO):
            probe.build(1)
        self.assertEqual(probe.verified_binary(self.manifest)[0], binary)

    def test_each_failing_step_is_named_and_leaves_no_manifest(self):
        with patch("sys.stderr", new_callable=io.StringIO):
            self.assertEqual(self.failed_build("CMake configure failed with exit status 3",
                                               FAKE_CONFIGURE_STATUS="3"),
                             "CMake configure failed with exit status 3; see its error output above")
            self.assertEqual(len(self.calls()), 1)
            self.failed_build("Building sdl3-freerdp failed with exit status 4", FAKE_BUILD_STATUS="4")
        self.failed_build("SDL3 executable missing", FAKE_NO_BINARY="1")
        self.failed_build("Unexpected FreeRDP version", FAKE_VERSION="This is FreeRDP version 3.34.0")
        self.failed_build("not installed or not on PATH", PATH=str(self.root / "empty"))

    def test_linux_build_requires_native_wayland_helpers(self):
        with patch.object(probe.platform, "system", return_value="Linux"):
            self.failed_build("SDL native Wayland helpers missing", FAKE_NATIVE="0")
            with patch("sys.stdout", new_callable=io.StringIO):
                probe.build(2)
        self.assertEqual(json.loads(self.manifest.read_text())["system"], "Linux")


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
                self.assertTrue(run_record["program_start_requested"])
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
            # Without a handler the default action would end the test process itself. An ignored
            # SIGHUP is left alone by run(), so this must be a handler and not SIG_IGN.
            signal.signal(signal.SIGHUP, lambda number, frame: None)
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

    def test_ignored_hangup_stays_ignored(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "quick"
            executable.write_text(f"#!{sys.executable}\nimport sys\n"
                                  "print('WAYWIN_PROBE_V1 driver=wayland', file=sys.stderr)\n")
            executable.chmod(0o700)
            args = Namespace(manifest=root / "manifest", server="example.test", user="alice", domain="",
                             label="host-a", mode="rail", app="||probe", output=root / "runs",
                             display_mode="auto", monitors=None, size=None, dynamic_resolution=False)
            before = signal.signal(signal.SIGHUP, signal.SIG_IGN)
            installed = []
            original = signal.signal
            def spy(number, handler):
                installed.append(number)
                return original(number, handler)
            try:
                with patch.object(probe, "doctor", return_value={"target_ready": True}), \
                     patch.object(probe, "verified_binary", return_value=(executable,
                             {"system": "Linux", "native_wayland_helpers": True})), \
                     patch.object(probe.signal, "signal", side_effect=spy), \
                     patch("sys.stdout", new_callable=io.StringIO):
                    self.assertEqual(probe.run(args), 0)
                self.assertNotIn(signal.SIGHUP, installed)
                self.assertEqual(signal.getsignal(signal.SIGHUP), signal.SIG_IGN)
            finally:
                signal.signal(signal.SIGHUP, before)

    def test_detached_process_holding_the_pipe_does_not_block(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executable = root / "leaves-a-detached-process"
            executable.write_text(f"#!{sys.executable}\nimport subprocess, sys\n"
                "other = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'],\n"
                "                         start_new_session=True)\n"
                f"open({str(root / 'other.pid')!r}, 'w').write(str(other.pid))\n"
                "print('WAYWIN_PROBE_V1 driver=wayland', file=sys.stderr, flush=True)\n"
                "sys.exit(7)\n")
            executable.chmod(0o700)
            args = Namespace(manifest=root / "manifest", server="example.test", user="alice", domain="",
                             label="host-a", mode="rail", app="||probe", output=root / "runs",
                             display_mode="auto", monitors=None, size=None, dynamic_resolution=False)
            try:
                with patch.object(probe, "doctor", return_value={"target_ready": True}), \
                     patch.object(probe, "verified_binary", return_value=(executable,
                             {"system": "Linux", "native_wayland_helpers": True})), \
                     patch.object(probe, "GROUP_GRACE", 0.2), \
                     patch("sys.stdout", new_callable=io.StringIO):
                    start = time.monotonic()
                    self.assertEqual(probe.run(args), 1)
                    self.assertLess(time.monotonic() - start, 20)
                record = json.loads(next(args.output.glob("*/exit.json")).read_text())
                self.assertEqual((record["ended"], record["exit_code"], record["events_seen"]), ("session-exit", 7, 1))
            finally:
                # It left the session's group on purpose, so the tool cannot and does not end it.
                os.kill(int((root / "other.pid").read_text()), signal.SIGKILL)

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
            with patch.object(probe, "doctor", return_value={"target_ready": True}), \
                 patch.object(probe, "verified_binary", return_value=(executable,
                         {"system": "Linux", "native_wayland_helpers": True})), \
                 patch.object(probe, "EVENT_LIMIT", 5), \
                 patch("sys.stdout", new_callable=io.StringIO):
                probe.run(args)
            exact = next(path for path in args.output.iterdir() if path != run)
            self.assertFalse(json.loads((exact / "exit.json").read_text())["events_truncated"])
            self.assertEqual(len((exact / "events.jsonl").read_text().splitlines()), 5)
            stored = [json.loads(line) for line in (run / "events.jsonl").read_text().splitlines()]
            self.assertEqual([item["window"] for item in stored], [1, 2, 3])
            self.assertEqual(json.loads((run / "exit.json").read_text()), {
                "ended": "session-exit", "exit_code": 7, "sdl_driver_observed": None,
                "events_seen": 5, "events_truncated": True, "runtime_verified": False})


if __name__ == "__main__": unittest.main()
