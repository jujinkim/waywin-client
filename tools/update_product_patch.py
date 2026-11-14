#!/usr/bin/env python3
"""Developer-only: regenerate the product patch from a reviewed runtime checkout."""
from pathlib import Path
import argparse
import difflib
import hashlib
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMMIT = "65bcca1913a28cbdd7eb913599f692bd2b707991"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if revision != COMMIT:
        raise SystemExit("Runtime checkout must be at the pinned commit.")
    files = set(subprocess.check_output(["git", "diff", "--name-only"], cwd=source, text=True).splitlines())
    for name in ("waywin_product.cpp", "waywin_product.hpp", "waywin_wire.hpp"):
        files.add("client/SDL/SDL3/" + name)
    for name in ("waywin_product.cpp", "waywin_product.hpp"):
        if (ROOT / "runtime" / name).read_bytes() != (source / "client/SDL/SDL3" / name).read_bytes():
            raise SystemExit(f"Synchronize runtime/{name} with the reviewed checkout first.")
    if (ROOT / "protocol/waywin_wire.hpp").read_bytes() != (source / "client/SDL/SDL3/waywin_wire.hpp").read_bytes():
        raise SystemExit("Synchronize the shared wire header first.")
    with tempfile.TemporaryDirectory(prefix="waywin-patch-") as directory:
        base = Path(directory)
        subprocess.run(["git", "init", "-q"], cwd=base, check=True)
        for name in sorted(files):
            original = subprocess.run(["git", "show", f"{COMMIT}:{name}"], cwd=source, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            if original.returncode == 0:
                target = base / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(original.stdout)
        subprocess.run(["git", "apply", str(ROOT / "patches/freerdp-3.33.1-probe.patch")], cwd=base, check=True)
        patch = ""
        for name in sorted(files):
            before = base / name
            old = before.read_text().splitlines(keepends=True) if before.exists() else []
            new = (source / name).read_text().splitlines(keepends=True)
            patch += "".join(difflib.unified_diff(old, new, fromfile="a/" + name if before.exists() else "/dev/null", tofile="b/" + name))
        target = ROOT / "patches/freerdp-3.33.1-product.patch"
        target.write_text(patch)
        subprocess.run(["git", "apply", "--check", str(target)], cwd=base, check=True)
        subprocess.run(["git", "apply", str(target)], cwd=base, check=True)
        for name in sorted(files):
            if (base / name).read_bytes() != (source / name).read_bytes():
                raise SystemExit(f"Patch round-trip mismatch: {name}")
    lock = ROOT / "dependencies.lock.json"
    data = json.loads(lock.read_text())
    digest = hashlib.sha256(target.read_bytes()).hexdigest()
    data["freerdp"]["product_patch_sha256"] = digest
    lock.write_text(json.dumps(data, indent=2) + "\n")
    probe = hashlib.sha256((ROOT / "patches/freerdp-3.33.1-probe.patch").read_bytes()).hexdigest()
    (source / ".waywin-patches").write_text(probe + "\n" + digest + "\n")
    print(f"Product patch verified: {digest}")


if __name__ == "__main__":
    main()
