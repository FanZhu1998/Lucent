"""Builds or checks the bundled lens database asset, assets/lens-database (ADR 0023).

The asset is the Lensfun lens database in data format version 1 (the format Lensfun 0.3.x reads), as
the Lensfun project publishes it for lensfun-update-data, unmodified, with the licence text of the
database (CC BY-SA 3.0) and a provenance record. Every byte is pinned here:

  * the published archive https://lensfun.github.io/db/version_1.tar.bz2 (versions.json timestamp
    1791482221 = 2026-10-08T17:57:01Z, retrieved 2026-10-10) has the SHA-256 ARCHIVE_SHA256; its 56
    XML files are byte-identical to what tools/update_database/generate_db.py of lensfun commit
    43f9e001ab66c4fcdd4f400463f13b72f94b2288 (master, 2026-10-08) writes from that commit's data/db
    (checked on 2026-10-10 with lxml 6.1.3), so the commit is the reproducible source;
  * the licence text is data/COPYING.CC_BY-SA_3.0 of the same commit (SHA-256 LICENCE_SHA256).

Usage (Python 3.10+, standard library only):
  python tests/tools/lens_database_asset.py --archive version_1.tar.bz2 --licence COPYING.CC_BY-SA_3.0 \\
      --output assets/lens-database
  python tests/tools/lens_database_asset.py --check --output assets/lens-database [--archive ...]

Without --archive / --licence the files are downloaded from the pinned URLs (the archive URL is the
live one; a newer upload fails the SHA-256 check, which is the point: a new database is a new
asset version, reviewed and pinned here). --check rebuilds in memory and compares with the folder
(exit 1 on any difference). The manifest is written in the canonical form of
src/assets/include/lucent/assets/asset_manifest.hpp (one-space indentation, files sorted by path,
LF, final newline); the test AssetsLensDatabase.TheCommittedManifestIsCanonical checks it.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import sys
import tarfile
import urllib.request
from pathlib import Path

ARCHIVE_URL = "https://lensfun.github.io/db/version_1.tar.bz2"
ARCHIVE_SHA256 = "97c9f8059accae0128a413bc610a04a4548e8da2764a6d3d60d82bf917639457"
ARCHIVE_SIZE = 434744
ARCHIVE_TIMESTAMP = 1791482221  # versions.json: [1791482221, [0, 1, 2], []]
SOURCE_REPOSITORY = "https://github.com/lensfun/lensfun"
SOURCE_COMMIT = "43f9e001ab66c4fcdd4f400463f13b72f94b2288"
LICENCE_URL = f"https://raw.githubusercontent.com/lensfun/lensfun/{SOURCE_COMMIT}/data/COPYING.CC_BY-SA_3.0"
LICENCE_SHA256 = "3f941b3b89cf7b8370ceb83cc76d2120d471b58735d8ca60238a751a48d7f72f"
LICENCE_FILE = "COPYING.CC_BY-SA_3.0"
PROVENANCE_FILE = "provenance.json"
XML_FILES = 56

ASSET_ID = "lens-database"
ASSET_VERSION = "1.0.0"
MINIMUM_APP_VERSION = "0.1.0"
CONTENT_SCHEMA = "lensfun.database"
CONTENT_SCHEMA_VERSION = 1


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=60) as response:  # noqa: S310 (pinned https URLs)
        return response.read()


def provenance() -> bytes:
    record = {
        "schema": "lucent.lens-database-provenance",
        "schema_version": 1,
        "description": "The Lensfun lens database, data format version 1, as published by the Lensfun "
                       "project for Lensfun 0.3.x (lensfun-update-data). The XML files are unmodified.",
        "licence": "CC-BY-SA-3.0",
        "licence_file": LICENCE_FILE,
        "attribution": "Lens database (c) the Lensfun project and its contributors "
                       "(https://lensfun.github.io/), licensed under the Creative Commons "
                       "Attribution-ShareAlike 3.0 Unported licence.",
        "modifications": "none",
        "source": {
            "archive_url": ARCHIVE_URL,
            "archive_sha256": ARCHIVE_SHA256,
            "archive_size": ARCHIVE_SIZE,
            "archive_timestamp": ARCHIVE_TIMESTAMP,
            "archive_timestamp_utc": "2026-10-08T17:57:01Z",
            "retrieved": "2026-10-10",
            "repository": SOURCE_REPOSITORY,
            "commit": SOURCE_COMMIT,
            "commit_date": "2026-10-08T19:57:01+02:00",
            "generator": "tools/update_database/generate_db.py (lensfun master, same commit), from data/db",
            "data_format_version": 1,
            "licence_url": LICENCE_URL,
        },
    }
    return (json.dumps(record, indent=1, ensure_ascii=True) + "\n").encode("ascii")


def manifest(files: dict[str, bytes]) -> bytes:
    entries = [{"path": path, "size": len(data), "sha256": sha256(data)} for path, data in sorted(files.items())]
    document = {
        "schema": "lucent.asset-manifest",
        "schema_version": 1,
        "id": ASSET_ID,
        "version": ASSET_VERSION,
        "minimum_app_version": MINIMUM_APP_VERSION,
        "content": {"schema": CONTENT_SCHEMA, "schema_version": CONTENT_SCHEMA_VERSION},
        "files": entries,
    }
    return (json.dumps(document, indent=1, ensure_ascii=True) + "\n").encode("ascii")


def build(archive: bytes, licence: bytes) -> dict[str, bytes]:
    if sha256(archive) != ARCHIVE_SHA256 or len(archive) != ARCHIVE_SIZE:
        raise SystemExit(f"the archive has SHA-256 {sha256(archive)} ({len(archive)} bytes); pinned "
                         f"{ARCHIVE_SHA256} ({ARCHIVE_SIZE} bytes)")
    if sha256(licence) != LICENCE_SHA256:
        raise SystemExit(f"the licence text has SHA-256 {sha256(licence)}; pinned {LICENCE_SHA256}")
    files: dict[str, bytes] = {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:bz2") as tar:
        for member in tar.getmembers():
            if not member.isfile() or "/" in member.name or not member.name.endswith(".xml"):
                raise SystemExit(f"unexpected archive member {member.name!r}")
            extracted = tar.extractfile(member)
            assert extracted is not None
            files[member.name] = extracted.read()
    if len(files) != XML_FILES:
        raise SystemExit(f"the archive holds {len(files)} XML files, expected {XML_FILES}")
    for name, data in files.items():
        if b'<lensdatabase version="1">' not in data[:200]:
            raise SystemExit(f"{name} is not data format version 1")
    files[LICENCE_FILE] = licence
    files[PROVENANCE_FILE] = provenance()
    files["manifest.json"] = manifest({k: v for k, v in files.items()})
    return files


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--archive", type=Path, help="a local copy of version_1.tar.bz2")
    parser.add_argument("--licence", type=Path, help="a local copy of data/COPYING.CC_BY-SA_3.0")
    parser.add_argument("--output", type=Path, default=Path("assets/lens-database"))
    parser.add_argument("--check", action="store_true", help="compare with the folder instead of writing")
    args = parser.parse_args()
    archive = args.archive.read_bytes() if args.archive else fetch(ARCHIVE_URL)
    licence = args.licence.read_bytes() if args.licence else fetch(LICENCE_URL)
    files = build(archive, licence)
    if args.check:
        problems = []
        present = {p.name for p in args.output.iterdir()} if args.output.is_dir() else set()
        for name, data in sorted(files.items()):
            path = args.output / name
            if not path.is_file():
                problems.append(f"missing {name}")
            elif path.read_bytes() != data:
                problems.append(f"differs: {name}")
        for name in sorted(present - set(files) - {".gitattributes"}):
            problems.append(f"unexpected {name}")
        for p in problems:
            print(p)
        print("OK" if not problems else f"{len(problems)} difference(s)")
        return 1 if problems else 0
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in sorted(files.items()):
        (args.output / name).write_bytes(data)
    print(f"wrote {len(files)} files to {args.output} (manifest SHA-256 {sha256(files['manifest.json'])})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
