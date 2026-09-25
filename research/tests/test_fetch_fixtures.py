"""End-to-end tests for tools/fetch-fixtures.ps1 against a loopback HTTP server.

Runs the script with Windows PowerShell 5.1 (the compatibility target) and, when installed,
PowerShell 7. Skipped on systems without PowerShell.
"""

from __future__ import annotations

import hashlib
import http.server
import json
import os
import re
import shutil
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "fetch-fixtures.ps1"


def _powershell_executables() -> list[str]:
    found = []
    for name in ("powershell.exe", "pwsh.exe", "pwsh"):
        path = shutil.which(name)
        if path and not any(os.path.samefile(path, other) for other in found):
            found.append(path)
    return found


POWERSHELLS = _powershell_executables()


class _QuietHandler(http.server.SimpleHTTPRequestHandler):
    requests: list[str] = []

    def log_message(self, format, *args):  # noqa: A002 - signature defined by the base class
        pass

    def do_GET(self):  # noqa: N802 - name defined by the base class
        type(self).requests.append(self.path)
        super().do_GET()


@unittest.skipUnless(POWERSHELLS, "PowerShell is not installed")
class FetchFixturesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._served = tempfile.TemporaryDirectory()
        cls.served = Path(cls._served.name)
        cls.payload_a = os.urandom(300_000)
        cls.payload_b = os.urandom(1_000)
        (cls.served / "a.bin").write_bytes(cls.payload_a)
        (cls.served / "b.bin").write_bytes(cls.payload_b)
        handler = lambda *args, **kwargs: _QuietHandler(*args, directory=str(cls.served), **kwargs)  # noqa: E731
        cls.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        cls.base = f"http://127.0.0.1:{cls.server.server_address[1]}"
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls._served.cleanup()

    def setUp(self):
        self._work = tempfile.TemporaryDirectory()
        self.work = Path(self._work.name)
        self.cache = self.work / "cache"
        _QuietHandler.requests.clear()

    def tearDown(self):
        self._work.cleanup()

    # ---- helpers --------------------------------------------------------------------------

    def entry(self, fixture_id="fixture-a", name="a.bin", payload=None, **overrides):
        payload = self.payload_a if payload is None else payload
        entry = {
            "id": fixture_id,
            "tier": "smoke",
            "url": f"{self.base}/{name}",
            "file_name": "sample.RAW",
            "size_bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "license": "CC0-1.0",
        }
        entry.update(overrides)
        return {k: v for k, v in entry.items() if v is not None}

    def manifest(self, *entries, **top):
        document = {"schema": "lucent.fixture-manifest", "schema_version": 1, "fixtures": list(entries)}
        document.update(top)
        path = self.work / "manifest.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def run_script(self, manifest, *args, shell=None):
        command = [
            shell or POWERSHELLS[0], "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
            "-File", str(SCRIPT), "-Manifest", str(manifest), "-CacheDir", str(self.cache), *args,
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        return result.returncode, result.stdout + result.stderr

    def cached(self, fixture_id="fixture-a"):
        return self.cache / fixture_id / "sample.RAW"

    def assert_no_partial_files(self):
        leftovers = [p for p in self.cache.rglob("*") if p.name.endswith(".partial")] if self.cache.exists() else []
        self.assertEqual(leftovers, [])

    # ---- success paths --------------------------------------------------------------------

    def test_download_verify_then_skip_cached(self):
        manifest = self.manifest(self.entry())
        for index, shell in enumerate(POWERSHELLS):
            with self.subTest(shell=shell):
                self.cache = self.work / f"cache-{index}"
                code, out = self.run_script(manifest, shell=shell)
                self.assertEqual(code, 0, out)
                self.assertIn("downloaded, verified", out)
                self.assertEqual(self.cached().read_bytes(), self.payload_a)
                self.assert_no_partial_files()

                requests_before = len(_QuietHandler.requests)
                code, out = self.run_script(manifest, shell=shell)
                self.assertEqual(code, 0, out)
                self.assertIn("cached, verified", out)
                self.assertEqual(len(_QuietHandler.requests), requests_before)

    def test_force_downloads_again(self):
        manifest = self.manifest(self.entry())
        self.assertEqual(self.run_script(manifest)[0], 0)
        count = len(_QuietHandler.requests)
        code, out = self.run_script(manifest, "-Force")
        self.assertEqual(code, 0, out)
        self.assertEqual(len(_QuietHandler.requests), count + 1)
        self.assertIn("downloaded, verified", out)

    def test_corrupted_cache_is_replaced(self):
        manifest = self.manifest(self.entry())
        self.cached().parent.mkdir(parents=True)
        self.cached().write_bytes(b"corrupted" + self.payload_a[9:])
        code, out = self.run_script(manifest)
        self.assertEqual(code, 0, out)
        self.assertIn("does not verify", out)
        self.assertEqual(self.cached().read_bytes(), self.payload_a)

    def test_mirror_url_is_used_when_primary_fails(self):
        manifest = self.manifest(self.entry(url=f"{self.base}/missing.bin", mirror_url=f"{self.base}/a.bin"))
        code, out = self.run_script(manifest)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.cached().read_bytes(), self.payload_a)
        self.assertIn("warning: fixture-a: download from", out)  # the stale primary URL is reported

    def test_tier_and_id_selection(self):
        manifest = self.manifest(
            self.entry(),
            self.entry("fixture-b", name="b.bin", payload=self.payload_b, tier="qualification"),
        )
        code, out = self.run_script(manifest)  # default tier: smoke
        self.assertEqual(code, 0, out)
        self.assertTrue(self.cached("fixture-a").exists())
        self.assertFalse(self.cached("fixture-b").exists())

        code, out = self.run_script(manifest, "-Id", "fixture-b,fixture-a")
        self.assertEqual(code, 0, out)
        self.assertTrue(self.cached("fixture-b").exists())
        self.assertIn("2 selected, 2 ok, 0 failed", out)

        code, out = self.run_script(manifest, "-Tier", "all")
        self.assertEqual(code, 0, out)
        self.assertIn("2 selected", out)

    def test_bootstrap_prints_hash_of_unhashed_entries(self):
        manifest = self.manifest(self.entry(sha256=None, size_bytes=None))
        code, out = self.run_script(manifest)
        self.assertEqual(code, 1, out)
        self.assertIn("no sha256", out)
        self.assertEqual(_QuietHandler.requests, [])
        self.assertFalse(self.cached().exists())

        code, out = self.run_script(manifest, "-Bootstrap")
        self.assertEqual(code, 0, out)
        expected = hashlib.sha256(self.payload_a).hexdigest()
        self.assertIn(f"BOOTSTRAP id=fixture-a sha256={expected} size_bytes={len(self.payload_a)}", out)
        self.assertIn("unverified", out)

        code, out = self.run_script(manifest, "-Bootstrap")  # cached copy is hashed, not downloaded
        self.assertEqual(code, 0, out)
        self.assertIn(f"sha256={expected}", out)

    # ---- failure paths --------------------------------------------------------------------

    def test_hash_mismatch_never_replaces_the_file(self):
        manifest = self.manifest(self.entry(sha256="0" * 64))
        code, out = self.run_script(manifest)
        self.assertEqual(code, 1, out)
        self.assertIn("SHA-256 is", out)
        self.assertFalse(self.cached().exists())
        self.assert_no_partial_files()

    def test_empty_and_html_responses_are_rejected_even_when_bootstrapping(self):
        (self.served / "empty.bin").write_bytes(b"")
        (self.served / "page.html").write_bytes(b"\n  <!DOCTYPE html><html><body>not found</body></html>")
        for name, reason in (("empty.bin", "empty file"), ("page.html", "HTML/XML page")):
            with self.subTest(name):
                manifest = self.manifest(self.entry(url=f"{self.base}/{name}", sha256=None, size_bytes=None))
                code, out = self.run_script(manifest, "-Bootstrap")
                self.assertEqual(code, 1, out)
                self.assertIn(reason, out)
                self.assertNotIn("BOOTSTRAP id=", out)
                self.assertFalse(self.cached().exists())

    def test_size_mismatch_is_rejected(self):
        manifest = self.manifest(self.entry(size_bytes=len(self.payload_a) + 1))
        code, out = self.run_script(manifest)
        self.assertEqual(code, 1, out)
        self.assertIn("size is", out)
        self.assertFalse(self.cached().exists())

    def test_http_error_fails_and_other_entries_continue(self):
        manifest = self.manifest(
            self.entry(url=f"{self.base}/missing.bin"),
            self.entry("fixture-b", name="b.bin", payload=self.payload_b),
        )
        code, out = self.run_script(manifest)
        self.assertEqual(code, 1, out)
        self.assertIn("[FAIL] fixture-a", out)
        self.assertIn("404", out)
        self.assertTrue(self.cached("fixture-b").exists())
        self.assertIn("2 selected, 1 ok, 1 failed", out)

    def test_usage_and_manifest_errors_exit_2(self):
        cases = {
            "unknown id": (self.manifest(self.entry()), ["-Id", "nope"]),
            "id outside tier": (self.manifest(self.entry()), ["-Id", "fixture-a", "-Tier", "qualification"]),
        }
        for label, (manifest, args) in cases.items():
            with self.subTest(label):
                code, out = self.run_script(manifest, *args)
                self.assertEqual(code, 2, out)
        bad_manifests = {
            "newer schema": ((self.entry(),), {"schema_version": 2}),
            "wrong schema": ((self.entry(),), {"schema": "something-else"}),
            "non-CC0": ((self.entry(license="CC-BY-NC-SA-4.0"),), {}),
            "ftp url": ((self.entry(url="ftp://example.com/a.bin"),), {}),
            "plain http to remote host": ((self.entry(url="http://example.com/a.bin"),), {}),
            "unsafe file name": ((self.entry(file_name="..\\evil.RAW"),), {}),
            "uppercase sha": ((self.entry(sha256="A" * 64),), {}),
            "duplicate id": ((self.entry(), self.entry()), {}),
            "bad tier": ((self.entry(tier="Smoke"),), {}),
        }
        for label, (entries, top) in bad_manifests.items():
            with self.subTest(label):
                code, out = self.run_script(self.manifest(*entries, **top))
                self.assertEqual(code, 2, out)
                self.assertRegex(out, re.compile("error", re.IGNORECASE))
        (self.work / "manifest.json").write_text("{ not json", encoding="utf-8")
        self.assertEqual(self.run_script(self.work / "manifest.json")[0], 2)
        self.assertEqual(self.run_script(self.work / "absent.json")[0], 2)
        self.assertEqual(_QuietHandler.requests, [])


if __name__ == "__main__":
    unittest.main()
