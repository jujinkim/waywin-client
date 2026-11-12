#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Stage-0 developer tooling. No stored credentials, arbitrary RDP flags or pass claims."""
from __future__ import annotations

import argparse
import datetime
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import sys
import time
import uuid
from urllib.parse import urlsplit

if sys.version_info < (3, 10):
    raise SystemExit("WayWin probe tooling requires Python 3.10 or newer.")

ROOT = Path(__file__).resolve().parents[1]
LOCK_PATH = ROOT / "dependencies.lock.json"
PATCH = ROOT / "patches/freerdp-3.33.1-probe.patch"
PROFILE = ROOT / "cmake/FreeRDPProbe.cmake"
SOURCE = ROOT / ".cache/FreeRDP-3.33.1"
BUILD = ROOT / "build/freerdp-probe"
EVENT_TYPES = ("window-create", "window-delete", "notify-create", "notify-update",
               "notify-delete", "desktop-monitored", "desktop-unmonitored")
EVENT_PATTERN = re.compile(
    rb"WAYWIN_PROBE_V1 type=(" + b"|".join(t.encode() for t in EVENT_TYPES) +
    rb") window=([0-9]{1,10}) icon=([0-9]{1,10}) flags=([0-9]{1,10})\n")
CASES = {
    "P01": "Native Wayland: display, move, resize, input, popups",
    "P02": "Existing AND new windows discovered; representative app inventory",
    "P03": "Occluded/minimized windows restore and repaint",
    "P04": "Tray create/update/delete AND click/context menu",
    "P05": "Helper launch AND desktop/seamless switch preserves session and work",
    "P06": "Disconnect/recovery without duplicate launch or logoff",
    "P07": "Two sessions: stopping/killing one leaves the other responsive",
    "P08": "Mixed/duplicate desktop and selected windows (optional)",
    "P09": "Actual Windows Start, IME settings and Quick Settings mirrored and interactive",
    "P10": "Host/client multi-monitor matrix, mixed DPI and topology recovery",
}
MONITOR_CASES = {
    "M01": "One host physical display / one client display",
    "M02": "Multiple host physical displays / one client display",
    "M03": "One host physical display / multiple client displays",
    "M04": "Multiple host physical displays / multiple client displays",
    "M05": "Mixed 100/150/200% DPI, negative coordinates, portrait and selected primary",
    "M06": "Unplug/replug, primary change and reconnect without lost windows",
}
SHELL_CASES = {"start": "Start menu", "ime": "IME settings and input UI", "quick": "Quick Settings"}


class ProbeError(Exception):
    pass


class PrivateArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        # argparse normally repeats unknown values, potentially including a password.
        self.exit(2, "Invalid or unsupported arguments. Use --help. Enter passwords only in the SDL dialog.\n")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def lock() -> dict:
    return json.loads(LOCK_PATH.read_text())


def command(args: list[str | Path], *, capture: bool = False) -> str:
    result = subprocess.run([str(a) for a in args], check=True,
                            stdout=subprocess.PIPE if capture else None,
                            text=True)
    return result.stdout or ""


def private_json(path: Path, data: dict) -> None:
    # Never overwrite a previous run; don't follow an existing file/symlink.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as stream:
        json.dump(data, stream, indent=2, ensure_ascii=False)
        stream.write("\n")


def prepare() -> None:
    dependency = lock()["freerdp"]
    if sha256(PATCH) != dependency["probe_patch_sha256"]:
        raise ProbeError("Probe patch differs from dependencies.lock.json")
    if not SOURCE.exists():
        SOURCE.parent.mkdir(parents=True, exist_ok=True)
        command(["git", "clone", "--depth", "1", "--branch", dependency["tag"],
                 dependency["repository"], SOURCE])
    if command(["git", "-C", SOURCE, "rev-parse", "HEAD"], capture=True).strip() != dependency["commit"]:
        raise ProbeError("Unexpected source commit; use a separate clean cache")
    # Do not discard developer changes or apply the patch twice.
    if command(["git", "-C", SOURCE, "ls-files", "--others", "--exclude-standard"], capture=True):
        raise ProbeError("Untracked files in dependency source; inspect them first")
    changes = command(["git", "-C", SOURCE, "diff", "--binary", "--no-ext-diff", "HEAD"], capture=True)
    expected = PATCH.read_text()
    if changes and changes != expected:
        raise ProbeError("Dependency has changes other than the approved observer patch")
    if not changes:
        command(["git", "-C", SOURCE, "apply", "--check", PATCH])
        command(["git", "-C", SOURCE, "apply", PATCH])
    if command(["git", "-C", SOURCE, "diff", "--binary", "--no-ext-diff", "HEAD"], capture=True) != expected:
        raise ProbeError("Applied patch differs from expected patch")


def dependency_inventory(cache: str) -> dict:
    entries = dict(re.findall(r"^([^/#\n:]+):[^=\n]+=(.*)$", cache, re.MULTILINE))
    result = {}
    for name in ("SDL3", "SDL3_ttf", "json-c", "cJSON"):
        directory = Path(entries.get(name + "_DIR", "missing"))
        for version_file in directory.glob("*[Vv]ersion.cmake"):
            match = re.search(r'set\(PACKAGE_VERSION "([^"]+)"\)', version_file.read_text())
            if match:
                result[name] = match[1]
                break
    for name, key, relative, pattern in (
            ("OpenSSL", "OPENSSL_INCLUDE_DIR", "openssl/opensslv.h", r'OPENSSL_VERSION_TEXT\s+"([^"]+)"'),
            ("zlib", "ZLIB_INCLUDE_DIR", "zlib.h", r'ZLIB_VERSION\s+"([^"]+)"')):
        header = Path(entries.get(key, "missing")) / relative
        if header.is_file():
            match = re.search(pattern, header.read_text())
            if match:
                result[name] = match[1]
    return result


def build(jobs: int) -> None:
    prepare()
    BUILD.mkdir(parents=True, exist_ok=True)
    # Invalidate previous provenance before any operation that can fail.
    manifest = BUILD / "build-manifest.json"
    manifest.unlink(missing_ok=True)
    command(["cmake", "-S", SOURCE, "-B", BUILD, "-G", "Ninja", "-C", PROFILE,
             "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"])
    command(["cmake", "--build", BUILD, "--target", "sdl3-freerdp", "--parallel", str(jobs)])
    binary = BUILD / "client/SDL/SDL3/sdl3-freerdp"
    if not binary.is_file():
        raise ProbeError("SDL3 executable missing; no verified manifest was written")
    version = command([binary, "/version"], capture=True).strip()
    if not re.search(r"\b3\.33\.1\b", version):
        raise ProbeError("Unexpected FreeRDP version")
    compiles = (BUILD / "compile_commands.json").read_text()
    native = "WITH_SDL_WAYLAND_NATIVE" in compiles
    if platform.system() == "Linux" and not native:
        raise ProbeError("SDL native Wayland helpers missing; install wayland-client/scanner/protocols")
    cache = (BUILD / "CMakeCache.txt").read_text()
    private_json(manifest, {
        "schema": 1, "source_commit": lock()["freerdp"]["commit"],
        "patch_sha256": sha256(PATCH), "profile_sha256": sha256(PROFILE),
        "binary": str(binary.relative_to(BUILD)), "binary_sha256": sha256(binary),
        "version": version, "system": platform.system(), "machine": platform.machine(),
        "native_wayland_helpers": native, "dependency_versions": dependency_inventory(cache),
        "cache_sha256": sha256(BUILD / "CMakeCache.txt"),
    })
    print(f"Built {binary}\nProvenance: {manifest}\nNo RDP compatibility test was performed.")


def doctor() -> dict:
    system = platform.system()
    release = platform.freedesktop_os_release() if system == "Linux" else {}
    checks = {
        "linux": system == "Linux",
        "ubuntu_26_04": release.get("ID") == "ubuntu" and release.get("VERSION_ID") == "26.04",
        "gnome": "gnome" in os.environ.get("XDG_CURRENT_DESKTOP", "").lower(),
        "wayland_session": os.environ.get("XDG_SESSION_TYPE") == "wayland",
    }
    display = os.environ.get("WAYLAND_DISPLAY", "")
    runtime = os.environ.get("XDG_RUNTIME_DIR", "")
    socket = Path(display) if Path(display).is_absolute() else Path(runtime) / display
    checks["wayland_socket"] = bool(display and runtime and socket.is_socket())
    return {"system": system, "machine": platform.machine(), "checks": checks,
            "target_ready": all(checks.values()), "runtime_verified": False}


def endpoint(value: str) -> str:
    """Accept DNS, IPv4, bracketed or bare IPv6, or rdp://host[:port]."""
    if not value or any(c.isspace() or ord(c) < 32 for c in value):
        raise ProbeError("Server must be an IP/DNS address, optionally with a port")
    if "://" not in value and value.count(":") > 1 and not value.startswith("["):
        try:
            return f"[{ipaddress.IPv6Address(value)}]:3389"
        except ValueError:
            raise ProbeError("Use [IPv6]:port for an IPv6 address with a port") from None
    try:
        parsed = urlsplit(value if "://" in value else "rdp://" + value)
        port = parsed.port if parsed.port is not None else 3389
        host = parsed.hostname
    except ValueError:
        raise ProbeError("Invalid RDP address or port") from None
    if (parsed.scheme != "rdp" or parsed.username is not None or parsed.password is not None
            or parsed.path not in ("", "/") or parsed.query or parsed.fragment
            or not host or not 1 <= port <= 65535 or parsed.netloc.endswith(":")):
        raise ProbeError("Use rdp://host:port without credentials, path, query or fragment")
    try:
        address = ipaddress.ip_address(host)
        host = f"[{address}]" if address.version == 6 else str(address)
    except ValueError:
        if ":" in host or "%" in host or parsed.netloc.startswith("["):
            raise ProbeError("Invalid IP address") from None
        try:
            host = host.encode("idna").decode("ascii")
        except UnicodeError:
            raise ProbeError("Invalid DNS name") from None
        if len(host) > 253 or not all(re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", part)
                                      for part in host.rstrip(".").split(".")):
            raise ProbeError("Invalid DNS name")
    return f"{host}:{port}"


def text_option(value: str, name: str) -> str:
    if not value or len(value) > 256 or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ProbeError(f"Invalid {name}")
    return value


def display_config(mode: str, display_mode: str = "auto", monitors: str | None = None,
                   size: str | None = None, dynamic: bool = False) -> dict:
    if mode not in ("desktop", "rail") or display_mode not in ("auto", "window", "multimon"):
        raise ProbeError("Invalid display mode")
    resolved = ("multimon" if mode == "rail" else "window") if display_mode == "auto" else display_mode
    if mode == "rail" and resolved == "window":
        raise ProbeError("RAIL uses monitor layout; select one SDL monitor ID for a single-display test")
    ids = []
    if monitors is not None:
        if resolved != "multimon" or not re.fullmatch(r"[0-9]{1,10}(?:,[0-9]{1,10}){0,15}", monitors):
            raise ProbeError("Monitor selection requires multimon and 1..16 SDL display IDs")
        ids = [int(value) for value in monitors.split(",")]
        if len(set(ids)) != len(ids) or any(not 1 <= value <= 0xFFFFFFFF for value in ids):
            raise ProbeError("SDL display IDs must be unique positive 32-bit values")
    if resolved == "multimon" and size is not None:
        raise ProbeError("A fixed window size cannot be combined with monitor layout")
    if resolved == "window":
        size = size or "1280x720"
        if not re.fullmatch(r"[0-9]{3,4}x[0-9]{3,4}", size) or not all(
                200 <= int(value) <= 8192 for value in size.split("x")):
            raise ProbeError("Window size must be WIDTHxHEIGHT, each dimension 200..8192")
    return {"mode": resolved, "monitor_ids": ids, "window_size": size, "dynamic_resolution": dynamic}


def rdp_args(binary: Path, server: str, user: str, domain: str, label: str,
             mode: str, app: str | None, *, display_mode: str = "auto", monitors: str | None = None,
             size: str | None = None, dynamic: bool = False) -> list[str]:
    display = display_config(mode, display_mode, monitors, size, dynamic)
    args = [str(binary), f"/v:{endpoint(server)}", f"/u:{text_option(user, 'user')}",
            f"/t:[{text_option(label, 'label')}] WayWin probe",
            "-clipboard", "/audio-mode:2", "-auto-reconnect", "/timeout:9000",
            "/log-level:ERROR"]
    if display["mode"] == "multimon":
        args.append("/multimon")
        if display["monitor_ids"]:
            args.append("/monitors:" + ",".join(str(value) for value in display["monitor_ids"]))
    else:
        args.extend(["-multimon", "/size:" + display["window_size"]])
    args.append("+dynamic-resolution" if dynamic else "-dynamic-resolution")
    if domain:
        args.append(f"/d:{text_option(domain, 'domain')}")
    if mode == "rail":
        if not app or "," in app:
            raise ProbeError("RAIL requires one registered alias or executable path without commas")
        args.append(f"/app:program:{text_option(app, 'app')}")
    elif mode != "desktop" or app:
        raise ProbeError("An app is accepted only in rail mode")
    return args


def parse_event(line: bytes) -> dict | None:
    match = EVENT_PATTERN.fullmatch(line)
    if match:
        numbers = [int(n) for n in match.groups()[1:]]
        if any(n > 0xFFFFFFFF for n in numbers):
            return None
        return dict(type=match[1].decode("ascii"), window=numbers[0], icon=numbers[1], flags=numbers[2])
    match = re.fullmatch(rb"WAYWIN_PROBE_V1 driver=(wayland|x11|cocoa|windows|dummy|offscreen)\n", line)
    return {"type": "driver", "driver": match[1].decode("ascii")} if match else None


def events(stream):
    # Bound line buffers even if an upstream diagnostic has no newline.
    while line := stream.readline(512):
        if not line.endswith(b"\n"):
            while line and not line.endswith(b"\n"):
                line = stream.readline(512)
            continue
        event = parse_event(line)
        if event:
            yield event


def stop_child(child: subprocess.Popen, timeout: float = 3.0) -> None:
    if child.poll() is None:
        try:
            os.killpg(child.pid, signal.SIGTERM)
            child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait()
        except ProcessLookupError:
            child.wait()


def verified_binary(manifest_path: Path) -> tuple[Path, dict]:
    manifest = json.loads(manifest_path.read_text())
    binary = (manifest_path.parent / manifest["binary"]).resolve()
    if (manifest.get("schema") != 1 or manifest.get("source_commit") != lock()["freerdp"]["commit"]
            or manifest.get("patch_sha256") != sha256(PATCH)
            or manifest.get("profile_sha256") != sha256(PROFILE)
            or manifest.get("binary_sha256") != sha256(binary)):
        raise ProbeError("Build provenance does not match; rebuild with this checkout")
    return binary, manifest


def session_environment() -> dict:
    env = os.environ.copy()
    # Raw upstream file logging must not be inherited by a probe or inventory command.
    for key in list(env):
        if key.startswith("WLOG_") or key.startswith("SDL_"):
            del env[key]
    env.update(SDL_VIDEO_DRIVER="wayland", WAYWIN_PROBE_EVENTS="1", WLOG_LEVEL="ERROR",
               WLOG_APPENDER="CONSOLE")
    return env


def list_monitors(manifest_path: Path) -> int:
    if not doctor()["target_ready"]:
        raise ProbeError("Monitor inventory requires the Ubuntu GNOME Wayland target")
    binary, provenance = verified_binary(manifest_path)
    if provenance["system"] != "Linux" or not provenance["native_wayland_helpers"]:
        raise ProbeError("Monitor inventory requires a Linux native Wayland build")
    # No server or credentials; the SDL client exits after enumerating local displays.
    return subprocess.run([str(binary), "/list:monitor"], env=session_environment(),
                          stdin=subprocess.DEVNULL, timeout=15, check=False).returncode


def run(args) -> int:
    environment = doctor()
    if not environment["target_ready"]:
        raise ProbeError("Run requires Ubuntu 26.04 GNOME Wayland; use doctor for missing checks")
    binary, provenance = verified_binary(args.manifest)
    if provenance["system"] != "Linux" or not provenance["native_wayland_helpers"]:
        raise ProbeError("This binary was not built with Linux native Wayland helpers")
    display = display_config(args.mode, args.display_mode, args.monitors, args.size, args.dynamic_resolution)
    argv = rdp_args(binary, args.server, args.user, args.domain, args.label, args.mode, args.app,
                    display_mode=args.display_mode, monitors=args.monitors, size=args.size,
                    dynamic=args.dynamic_resolution)
    run_id = str(uuid.uuid4())
    directory = args.output / run_id
    directory.mkdir(parents=True, mode=0o700)
    private_json(directory / "run.json", {
        "schema": 1, "run_id": run_id, "host_label": args.label, "mode": args.mode,
        "requested_display": display,
        "environment": environment, "build": provenance, "pass": False,
        "note": "Observer events establish reception only; complete manual cases separately.",
    })
    env = session_environment()
    print(f"Run {run_id}\nEvidence: {directory}\nEnter the password only in the SDL dialog. "
          "Cancel there or press Ctrl-C here to disconnect.", flush=True)
    count = 0
    started = time.monotonic()
    child = None
    exit_code = None
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    previous_term = signal.signal(signal.SIGTERM, interrupted)
    try:
        fd = os.open(directory / "events.jsonl", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as output:
            child = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                     start_new_session=True)
            print(f"Session process: {child.pid}", flush=True)
            for event in events(child.stderr):
                if event["type"] == "driver" and event["driver"] != "wayland":
                    raise ProbeError("SDL did not select the native Wayland driver")
                # Bounded evidence storage; continue draining after the cap.
                if count < 100000:
                    event.update(run_id=run_id, sequence=count + 1,
                                 elapsed_ms=int((time.monotonic() - started) * 1000))
                    output.write(json.dumps(event) + "\n")
                    output.flush()
                count += 1
            exit_code = child.wait()
    except KeyboardInterrupt:
        exit_code = 130
    finally:
        signal.signal(signal.SIGTERM, previous_term)
        if child:
            stop_child(child)
            if child.stderr:
                child.stderr.close()
        private_json(directory / "exit.json", {"exit_code": exit_code, "events_seen": count,
                     "events_truncated": count > 100000, "runtime_verified": False})
    print(f"Disconnected (exit={exit_code}, events={count}). No compatibility verdict was inferred.")
    return 0 if exit_code == 0 else 1


def report_template() -> dict:
    return {"schema": 2, "environment": {"ubuntu": "", "gnome": "", "sdl_driver": "",
            "windows_edition_build": "", "rdp_policy": "", "freerdp_commit": "",
            "patch_sha256": ""}, "tester": "", "date": "", "applications": [],
            "cases": {case: {"description": desc, "status": "not-run", "evidence": [],
                              "notes": "", "required": case != "P08"}
                      for case, desc in CASES.items()},
            "shell_surfaces": {key: {"description": desc, "status": "not-run", "evidence": [],
                                     "notes": "", "remote_ui_visible": False, "interactive": False,
                                     "same_session": False} for key, desc in SHELL_CASES.items()},
            "monitor_matrix": {key: {"description": desc, "status": "not-run", "evidence": [],
                                     "notes": "", "host_physical_layout": "", "client_layout": "",
                                     "rdp_session_layout": ""} for key, desc in MONITOR_CASES.items()}}


def manual_pass(item) -> bool:
    return (isinstance(item, dict) and item.get("status") == "pass"
            and isinstance(item.get("evidence"), list) and bool(item["evidence"])
            and all(isinstance(e, str) and e.strip() for e in item["evidence"])
            and isinstance(item.get("notes"), str) and bool(item["notes"].strip()))


def assess(report: dict) -> list[str]:
    """Completeness check, not independent attestation of manually supplied evidence."""
    problems = []
    if not isinstance(report, dict):
        return ["Report must be an object"]
    if report.get("schema") != 2:
        problems.append("Unsupported report schema")
    environment = report.get("environment", {})
    if not isinstance(environment, dict):
        return ["Environment must be an object"]
    for key in report_template()["environment"]:
        if not isinstance(environment.get(key), str) or not environment[key].strip():
            problems.append(f"Missing environment: {key}")
    if environment.get("ubuntu") != "26.04" or environment.get("sdl_driver") != "wayland":
        problems.append("Target must be Ubuntu 26.04 with actual SDL wayland driver")
    if environment.get("freerdp_commit") != lock()["freerdp"]["commit"]:
        problems.append("Incorrect FreeRDP source commit")
    if environment.get("patch_sha256") != sha256(PATCH):
        problems.append("Incorrect probe patch hash")
    for key in ("tester", "date"):
        if not isinstance(report.get(key), str) or not report[key].strip():
            problems.append(f"Missing {key}")
    try:
        datetime.date.fromisoformat(report.get("date", ""))
    except (ValueError, TypeError):
        problems.append("Date must be YYYY-MM-DD")
    apps = report.get("applications")
    if (not isinstance(apps, list) or not all(isinstance(app, str) and app.strip() for app in apps)
            or len({app.strip().casefold() for app in apps}) < 5):
        problems.append("Record at least five representative apps and versions")
    cases = report.get("cases", {})
    if not isinstance(cases, dict):
        return problems + ["Cases must be an object"]
    for case in CASES:
        if case == "P08":
            continue
        item = cases.get(case, {})
        if not isinstance(item, dict):
            problems.append(f"{case}: must be an object")
            continue
        if not manual_pass(item):
            problems.append(f"{case}: needs manual pass, evidence references and observations")
    surfaces = report.get("shell_surfaces", {})
    for key in SHELL_CASES:
        item = surfaces.get(key) if isinstance(surfaces, dict) else None
        if not manual_pass(item) or not all(item.get(field) is True for field in
                ("remote_ui_visible", "interactive", "same_session")):
            problems.append(f"Shell {key}: needs the actual interactive remote UI in the same session")
    matrix = report.get("monitor_matrix", {})
    for key in MONITOR_CASES:
        item = matrix.get(key) if isinstance(matrix, dict) else None
        if not manual_pass(item) or not all(isinstance(item.get(field), str) and item[field].strip()
                for field in ("host_physical_layout", "client_layout", "rdp_session_layout")):
            problems.append(f"{key}: needs observed host, client and RDP session layouts with evidence")
    return problems


def main() -> int:
    parser = PrivateArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("prepare", help="Fetch and verify the pinned source; apply observer patch")
    builder = sub.add_parser("build", help="Build a local SDL3 probe, never install system-wide")
    builder.add_argument("--jobs", type=int, choices=range(1, 65), default=4, metavar="1..64")
    sub.add_parser("doctor", help="Check target environment without a connection")
    inventory = sub.add_parser("list-monitors", help="List local SDL display IDs without an RDP connection")
    inventory.add_argument("--manifest", type=Path, default=BUILD / "build-manifest.json")
    runner = sub.add_parser("run", help="Launch one isolated native Wayland RDP probe")
    runner.add_argument("--server", required=True)
    runner.add_argument("--user", required=True)
    runner.add_argument("--domain", default="")
    runner.add_argument("--label", required=True, help="Non-sensitive host label, e.g. host-a")
    runner.add_argument("--mode", choices=["desktop", "rail"], required=True)
    runner.add_argument("--app")
    runner.add_argument("--display-mode", choices=["auto", "window", "multimon"], default="auto",
                        help="auto: desktop window or RAIL monitor layout")
    runner.add_argument("--monitors", help="Comma-separated SDL IDs from list-monitors; first is primary")
    runner.add_argument("--size", help="Windowed desktop WIDTHxHEIGHT; default 1280x720")
    runner.add_argument("--dynamic-resolution", action="store_true", help="Exercise display-control changes")
    runner.add_argument("--manifest", type=Path, default=BUILD / "build-manifest.json")
    runner.add_argument("--output", type=Path, default=ROOT / "out/runs")
    template = sub.add_parser("new-report", help="Create an entirely NOT-RUN manual report")
    template.add_argument("path", type=Path)
    assessment = sub.add_parser("assess", help="Check manual report completeness; no automatic attestation")
    assessment.add_argument("path", type=Path)
    args = parser.parse_args()
    try:
        if args.action == "prepare":
            prepare()
        elif args.action == "build":
            build(args.jobs)
        elif args.action == "doctor":
            result = doctor()
            print(json.dumps(result, indent=2))
            return 0 if result["target_ready"] else 2
        elif args.action == "run":
            return run(args)
        elif args.action == "list-monitors":
            return list_monitors(args.manifest)
        elif args.action == "new-report":
            private_json(args.path, report_template())
        elif args.action == "assess":
            problems = assess(json.loads(args.path.read_text()))
            print(json.dumps({"record_complete": not problems, "problems": problems,
                              "independently_verified": False}, indent=2))
            return 2 if problems else 0
    except (ProbeError, OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        # Never echo command arguments or source text from input on errors.
        message = str(error) if isinstance(error, ProbeError) else type(error).__name__
        print(f"Probe error: {message}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
