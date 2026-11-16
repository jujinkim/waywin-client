"""Local source packaging and extraction contracts; no Linux compatibility claims."""
from pathlib import Path
import hashlib
import io
import json
import shutil
import subprocess
import tarfile
import tempfile
import unittest

from tools.package_review import archive_freerdp

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("git"), "Git required for source archive contracts")
class SourceArchiveTests(unittest.TestCase):
    def test_export_uses_locked_commit_and_excludes_local_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            subprocess.run(["git", "init", "-q", str(source)], check=True)
            (source / "LICENSE").write_text("fixture license\n")
            (source / "source.txt").write_text("committed\n")
            subprocess.run(["git", "-C", str(source), "add", "."], check=True)
            subprocess.run(["git", "-C", str(source), "-c", "user.name=Test",
                            "-c", "user.email=test@example.invalid", "-c", "commit.gpgsign=false",
                            "commit", "-qm", "fixture"], check=True)
            commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
            expected = subprocess.check_output(["git", "-C", str(source), "archive", "--format=tar",
                                                "--prefix=FreeRDP-3.33.1/", commit])
            dependency = {"tag": "3.33.1", "commit": commit,
                          "source_archive_sha256": hashlib.sha256(expected).hexdigest()}
            (source / "source.txt").write_text("local patch\n")
            (source / "untracked.txt").write_text("do not ship\n")
            archive = root / "source.tar"
            archive_freerdp(source, archive, dependency)
            self.assertEqual(archive.read_bytes(), expected)
            with tarfile.open(archive) as contents:
                self.assertEqual(contents.extractfile("FreeRDP-3.33.1/source.txt").read(), b"committed\n")
                self.assertIn("FreeRDP-3.33.1/LICENSE", contents.getnames())
                self.assertNotIn("FreeRDP-3.33.1/untracked.txt", contents.getnames())
                self.assertFalse(any(".git" in Path(name).parts for name in contents.getnames()))
            dependency["source_archive_sha256"] = "0" * 64
            with self.assertRaisesRegex(SystemExit, "does not match"):
                archive_freerdp(source, archive, dependency)
            self.assertFalse(archive.exists())

    def test_missing_checkout_is_not_fetched(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaisesRegex(SystemExit, "never downloads"):
                archive_freerdp(root / "missing", root / "source.tar", {})
            self.assertFalse((root / "source.tar").exists())


@unittest.skipUnless(all(shutil.which(name) for name in ("cmake", "git", "ninja")),
                     "CMake, Git and Ninja required for local runtime contracts")
class BundledRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="waywin bundled runtime ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        # Extraction and patching must also work inside an unrelated Git repository.
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        shutil.copytree(ROOT / "cmake", self.root / "cmake")
        patches = self.root / "patches"
        patches.mkdir()
        (patches / "freerdp-3.33.1-probe.patch").write_text(
            "diff --git a/state.txt b/state.txt\n"
            "--- a/state.txt\n+++ b/state.txt\n@@ -1 +1 @@\n-original\n+probe\n")
        (patches / "freerdp-3.33.1-product.patch").write_text(
            "diff --git a/state.txt b/state.txt\n"
            "--- a/state.txt\n+++ b/state.txt\n@@ -1 +1 @@\n-probe\n+product\n"
            "diff --git a/added.txt b/added.txt\nnew file mode 100644\n"
            "--- /dev/null\n+++ b/added.txt\n@@ -0,0 +1 @@\n+product addition\n")
        self.archive = self.root / "FreeRDP-3.33.1.tar"
        with tarfile.open(self.archive, "w") as archive:
            data = b"original\n"
            member = tarfile.TarInfo("FreeRDP-3.33.1/state.txt")
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
        lock = json.loads((ROOT / "dependencies.lock.json").read_text())
        lock["freerdp"]["source_archive_sha256"] = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        (self.root / "dependencies.lock.json").write_text(json.dumps(lock))
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.22)\nproject(LocalRuntime NONE)\n"
            "include(cmake/ProductRuntime.cmake)\n"
            "ExternalProject_Add_StepTargets(waywin-runtime patch)\n")
        # Fail immediately if a regression attempts Git network transport.
        self.git = self.root / "git-offline"
        self.git.write_text(
            "#!/bin/sh\nfor arg in \"$@\"; do\n"
            'case "$arg" in clone|fetch|pull|ls-remote) exit 91 ;; esac\ndone\n'
            'exec "' + shutil.which("git") + '" "$@"\n')
        self.git.chmod(0o755)
        self.build = self.root / "build"

    def configure(self):
        return subprocess.run([
            "cmake", "-S", str(self.root), "-B", str(self.build), "-G", "Ninja",
            f"-DWAYWIN_RUNTIME_ARCHIVE={self.archive}", f"-DGIT_EXECUTABLE={self.git}",
            "-DWAYWIN_RUNTIME_REPOSITORY=https://blocked.invalid/FreeRDP.git",
        ], capture_output=True, text=True)

    def patch(self):
        return subprocess.run(["cmake", "--build", str(self.build), "--target", "waywin-runtime-patch"],
                              capture_output=True, text=True)

    def test_local_archive_extracts_and_patches_without_git_transport(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.patch()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        source = self.build / "waywin-runtime-prefix/src/waywin-runtime"
        self.assertFalse((source / ".git").exists())
        self.assertEqual((source / "state.txt").read_text(), "product\n")
        self.assertEqual((source / "added.txt").read_text(), "product addition\n")
        result = subprocess.run([
            "cmake", f"-DSOURCE={source}", f"-DROOT={self.root}", f"-DGIT={self.git}",
            "-P", str(self.root / "cmake/ApplyRuntimePatches.cmake"),
        ], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((source / "state.txt").read_text(), "product\n")

    def test_missing_archive_stops_configuration_without_fallback(self):
        self.archive.unlink()
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source archive is missing", result.stderr)

    def test_corrupt_archive_stops_configuration_without_fallback(self):
        self.archive.write_bytes(b"damaged")
        result = self.configure()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("failed SHA-256 verification", result.stderr)

    def test_archive_is_checked_again_before_extraction(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.archive.write_bytes(b"damaged after configuration")
        result = self.patch()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("hash", (result.stdout + result.stderr).lower())
        source = self.build / "waywin-runtime-prefix/src/waywin-runtime"
        self.assertFalse((source / "state.txt").exists())


if __name__ == "__main__":
    unittest.main()
