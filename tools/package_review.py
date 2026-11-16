#!/usr/bin/env python3
"""Build a local review bundle from already-built binaries (developer packaging only)."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / "out/release/waywin-0.3.0-linux-desktops"


def archive_freerdp(source, output, dependency):
    """Export the pinned commit, never the possibly patched working tree or a remote."""
    source, output = Path(source).resolve(), Path(output)
    if not source.is_dir():
        raise SystemExit("Pinned FreeRDP checkout missing. Supply --freerdp-source; packaging never downloads sources.")
    try:
        with output.open("wb") as archive:
            subprocess.run([
                "git", "--no-replace-objects", "-C", str(source), "archive", "--format=tar",
                f"--prefix=FreeRDP-{dependency['tag']}/", dependency["commit"],
            ], stdout=archive, check=True)
        digest = hashlib.sha256(output.read_bytes()).hexdigest()
        if digest != dependency["source_archive_sha256"]:
            raise SystemExit("FreeRDP source archive does not match dependencies.lock.json.")
    except BaseException:
        output.unlink(missing_ok=True)
        raise


def create_bundle(freerdp_archive):
    companion = ROOT / "build/windows-complete/windows/companion/waywin-companion.exe"
    probe = ROOT / "build/windows-complete/windows/probe/waywin-windows-probe.exe"
    if not companion.is_file() or not probe.is_file():
        raise SystemExit("Build the Windows targets in build/windows-complete first.")
    if DEST.exists():
        shutil.rmtree(DEST)  # Only this tool's fixed ignored output directory.
    (DEST / "windows").mkdir(parents=True)
    dependencies = DEST / "ubuntu/dependencies"
    dependencies.mkdir(parents=True)
    shutil.copy2(freerdp_archive, dependencies / "FreeRDP-3.33.1.tar")
    source = DEST / "ubuntu/source"
    source.mkdir(parents=True)
    files = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT).decode().split("\0")
    for name in sorted(set(filter(None, files))):
        original = ROOT / name
        if not original.is_file():
            continue
        target = source / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, target)
    shutil.copy2(companion, DEST / "windows/WayWin Companion.exe")
    shutil.copy2(probe, DEST / "windows/WayWin Windows Probe.exe")
    for name in ("install-linux.sh", "WayWin-Setup.desktop"):
        target = DEST / "ubuntu" / name
        shutil.copy2(ROOT / "packaging" / name, target)
        target.chmod(0o755)
    shutil.copy2(ROOT / "docs/product.md", DEST / "PRODUCT-GUIDE.md")
    shutil.copytree(ROOT / "docs/screenshots", DEST / "screenshots")
    shutil.copy2(ROOT / "LICENSE", DEST / "LICENSE")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT))
    (DEST / "SOURCE-REVISION").write_text(revision + (" (working tree changes)" if dirty else "") + "\n")
    shutil.copy2(ROOT / "packaging/START-HERE.md", DEST / "START-HERE.md")
    hashes = []
    for path in sorted(DEST.rglob("*")):
        if path.is_file():
            hashes.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + path.relative_to(DEST).as_posix())
    (DEST / "SHA256SUMS").write_text("\n".join(hashes) + "\n")
    archive = DEST.with_name(DEST.name + ".tar.gz")
    with tarfile.open(archive, "w:gz") as output:
        output.add(DEST, arcname=DEST.name)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_name(archive.name + ".sha256").write_text(digest + "  " + archive.name + "\n")
    print(f"Review bundle: {archive} ({archive.stat().st_size} bytes)")
    print(f"SHA256: {digest}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--freerdp-source", type=Path, default=ROOT / ".cache/FreeRDP-3.33.1",
                        help="Existing checkout containing the pinned FreeRDP commit (never fetched)")
    args = parser.parse_args()
    dependency = json.loads((ROOT / "dependencies.lock.json").read_text())["freerdp"]
    with tempfile.TemporaryDirectory(prefix="waywin-source-") as directory:
        archive = Path(directory) / "FreeRDP-3.33.1.tar"
        archive_freerdp(args.freerdp_source, archive, dependency)
        create_bundle(archive)


if __name__ == "__main__":
    main()
