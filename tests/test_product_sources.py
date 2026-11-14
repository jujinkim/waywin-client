"""Product provenance checks; no platform behavior is inferred from these tests."""
from pathlib import Path
import hashlib
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ProductSourceTests(unittest.TestCase):
    def test_runtime_sources_match_shipped_patch(self):
        patch = (ROOT / "patches/freerdp-3.33.1-product.patch").read_text()
        for local in ("runtime/waywin_product.cpp", "runtime/waywin_product.hpp", "protocol/waywin_wire.hpp"):
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


if __name__ == "__main__":
    unittest.main()
