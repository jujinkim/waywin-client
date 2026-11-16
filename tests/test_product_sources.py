"""Product provenance checks; no platform behavior is inferred from these tests."""
from pathlib import Path
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProductSourceTests(unittest.TestCase):
    def test_runtime_sources_match_shipped_patch(self):
        patch = (ROOT / "patches/freerdp-3.33.1-product.patch").read_text()
        for local in ("runtime/waywin_product.cpp", "runtime/waywin_product.hpp", "protocol/waywin_wire.hpp", "protocol/waywin_policy.hpp"):
            name = "client/SDL/SDL3/" + Path(local).name
            match = re.search(r"^--- /dev/null\n\+\+\+ b/" + re.escape(name) + r"\n@@[^\n]*\n(.*?)(?=^--- |\Z)", patch, re.M | re.S)
            self.assertIsNotNone(match, local)
            lines = match.group(1).splitlines(keepends=True)
            self.assertTrue(all(line.startswith("+") for line in lines))
            self.assertEqual("".join(line[1:] for line in lines), (ROOT / local).read_text())

    def test_product_patch_lock(self):
        lock = json.loads((ROOT / "dependencies.lock.json").read_text())
        patch = (ROOT / "patches/freerdp-3.33.1-product.patch").read_bytes()
        self.assertEqual(hashlib.sha256(patch).hexdigest(), lock["freerdp"]["product_patch_sha256"])

    @unittest.skipUnless(shutil.which("cmake"), "CMake required for manifest contract")
    def test_linux_manifest_requires_both_native_backends(self):
        # Exercise the manifest script with synthetic compile metadata, not a Linux build.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build, stage = root / "build", root / "stage"
            build.mkdir()
            (stage / "bin").mkdir(parents=True)
            (stage / "bin/sdl3-freerdp").write_bytes(b"fixture")
            script = root / "manifest-test.cmake"
            script.write_text('set(CMAKE_HOST_SYSTEM_NAME Linux)\ninclude("' +
                              (ROOT / "cmake/RuntimeManifest.cmake").as_posix() + '")\n')
            for definitions, success in (
                ("-DWITH_SDL_WAYLAND_NATIVE", False),
                ("-DWITH_SDL_X11_NATIVE", False),
                ("-DWITH_SDL_WAYLAND_NATIVE -DWITH_SDL_X11_NATIVE", True),
            ):
                with self.subTest(definitions=definitions):
                    (build / "compile_commands.json").write_text(json.dumps([{"command": definitions}]))
                    result = subprocess.run([
                        "cmake", f"-DBUILD={build}",
                        f"-DSTAGE={stage}", f"-DROOT={ROOT}",
                        "-P", str(script),
                    ], capture_output=True, text=True)
                    self.assertEqual(result.returncode == 0, success, result.stderr)
            manifest = json.loads((stage / "waywin-runtime.json").read_text())
            self.assertEqual(manifest["system"], "Linux")
            self.assertEqual(manifest["display_backends"], ["wayland", "x11"])
            self.assertEqual(manifest["files"]["bin/sdl3-freerdp"], hashlib.sha256(b"fixture").hexdigest())


if __name__ == "__main__":
    unittest.main()
