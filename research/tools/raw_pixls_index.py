#!/usr/bin/env python3
"""Query the raw.pixls.us sample archive and cross-check Lucent's fixture manifest against it.

raw.pixls.us publishes its catalogue in two machine-readable forms (discovered from the
homepage, https://raw.pixls.us/):

* ``https://raw.pixls.us/json/getrepository.php?set=all`` -- the JSON that feeds the homepage
  table. Each row is ``[make, model, mode, megapixels, remark, license_html, date,
  raw_link_html, exif_link_html]``. ``raw_link_html`` holds the per-file download link
  ``https://raw.pixls.us/getfile.php/<id>/nice/<display name>`` plus the published SHA-256 and
  an approximate size; ``license_html`` names the per-file licence (most files are CC0, some
  imported files are CC BY-NC-SA 4.0).
* ``https://raw.pixls.us/data/filelist.sha256`` -- ``<sha256> *<Make>/<Model>/<original name>``
  for the static mirror under ``https://raw.pixls.us/data/``.

Joining the two by SHA-256 gives, for every sample, a stable id-based download URL, a
human-readable mirror URL and the camera's original file name.

Subcommands::

    search  REGEX...        list samples whose "make | model | mode | remark | name" match all REGEXes
    describe ID...          print draft manifest entries (without sha256/size_bytes) for sample ids
    verify-manifest [PATH]  check every manifest entry against the live index (licence, hash, id)

Only the Python standard library is used, so the tool also runs outside research/.venv.
Exit status: 0 success, 1 verification failure, 2 usage or network/parse error.
"""

from __future__ import annotations

import argparse
import dataclasses
import html
import json
import posixpath
import re
import sys
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Iterable, Sequence

INDEX_URL = "https://raw.pixls.us/json/getrepository.php?set=all"
FILELIST_URL = "https://raw.pixls.us/data/filelist.sha256"
MIRROR_BASE = "https://raw.pixls.us/data/"
HOMEPAGE_URL = "https://raw.pixls.us/"
USER_AGENT = "Lucent-research/1 (+https://github.com/FanZhu1998/Lucent)"

# Licence titles used by the archive's per-file licence column, mapped to SPDX identifiers.
LICENSE_TITLES = {
    "Creative Commons 0 - Public Domain": "CC0-1.0",
    "Creative Commons - Attribution, Non-Commercial, ShareAlike 4.0": "CC-BY-NC-SA-4.0",
}

# Characters allowed in a fixture file name (mirrors tools/fetch-fixtures.ps1).
SAFE_FILE_NAME = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9._()+-]*$")

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MANIFEST = REPO_ROOT / "fixtures" / "manifest.json"


class IndexError_(RuntimeError):
    """The archive index could not be fetched or parsed."""


@dataclasses.dataclass(frozen=True)
class Sample:
    """One row of the raw.pixls.us index, joined with the mirror file list."""

    sample_id: int
    make: str
    model: str
    mode: str
    remark: str
    license_spdx: str
    date: str
    getfile_url: str  # percent-encoded
    display_name: str
    published_sha256: str | None
    approx_size_mb: float | None
    mirror_path: str | None  # "<Make>/<Model>/<original name>", not encoded

    @property
    def original_name(self) -> str | None:
        return posixpath.basename(self.mirror_path) if self.mirror_path else None

    @property
    def mirror_url(self) -> str | None:
        if not self.mirror_path:
            return None
        return MIRROR_BASE + urllib.parse.quote(self.mirror_path, safe="/")

    @property
    def source_page(self) -> str:
        if not self.mirror_path:
            return HOMEPAGE_URL + "#repo"
        return MIRROR_BASE + urllib.parse.quote(posixpath.dirname(self.mirror_path), safe="/") + "/"


def _http_get(url: str, timeout: float = 60.0) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:  # noqa: S310 (fixed https URLs)
            return response.read()
    except OSError as exc:  # URLError and socket errors derive from OSError
        raise IndexError_(f"cannot fetch {url}: {exc}") from exc


def _encode_url(raw_url: str) -> str:
    """Percent-encodes the path of an archive URL that the index publishes unencoded."""
    parts = urllib.parse.urlsplit(raw_url)
    path = urllib.parse.quote(urllib.parse.unquote(parts.path), safe="/")
    return urllib.parse.urlunsplit((parts.scheme, parts.netloc, path, parts.query, parts.fragment))


def parse_filelist(text: str) -> dict[str, list[str]]:
    """Maps SHA-256 -> mirror paths from the ``filelist.sha256`` text."""
    by_hash: dict[str, list[str]] = {}
    for line_number, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        match = re.fullmatch(r"([0-9a-f]{64}) \*(.+)", line.strip())
        if not match:
            raise IndexError_(f"filelist.sha256 line {line_number} is malformed: {line!r}")
        by_hash.setdefault(match.group(1), []).append(match.group(2))
    return by_hash


def parse_index(index_json: str, filelist: dict[str, list[str]]) -> list[Sample]:
    """Parses the getrepository.php JSON and joins it with the mirror file list."""
    try:
        rows = json.loads(index_json)["data"]
    except (ValueError, KeyError, TypeError) as exc:
        raise IndexError_(f"index JSON is not in the expected shape: {exc}") from exc

    samples: list[Sample] = []
    for row in rows:
        if not isinstance(row, list) or len(row) < 8:
            raise IndexError_(f"unexpected index row: {row!r}")
        make, model, mode, _mp, remark, license_html, date, raw_html = row[:8]
        link = re.search(r"href='([^']+)'", raw_html)
        if not link:
            raise IndexError_(f"index row without download link: {row!r}")
        raw_url = html.unescape(link.group(1)).replace("\\/", "/")
        id_match = re.search(r"/getfile\.php/(\d+)/nice/(.+)$", raw_url)
        if not id_match:
            raise IndexError_(f"download link has an unknown layout: {raw_url!r}")
        sha = re.search(r"title='sha256 Checksum'>([0-9a-f]{64})<", raw_html)
        size = re.search(r"\(([0-9.]+)MB\)", raw_html)
        title = re.search(r"title='([^']+)'", license_html)
        license_title = html.unescape(title.group(1)) if title else ""
        published = sha.group(1) if sha else None

        mirror_path = None
        if published and published in filelist:
            candidates = filelist[published]
            # Prefer the copy stored under this row's make/model directory.
            preferred = [p for p in candidates if p.split("/")[:2] == [make, model]]
            mirror_path = (preferred or candidates)[0]

        samples.append(
            Sample(
                sample_id=int(id_match.group(1)),
                make=make,
                model=model,
                mode=mode,
                remark=html.unescape(remark),
                license_spdx=LICENSE_TITLES.get(license_title, f"UNKNOWN({license_title})"),
                date=date,
                getfile_url=_encode_url(raw_url),
                display_name=id_match.group(2),
                published_sha256=published,
                approx_size_mb=float(size.group(1)) if size else None,
                mirror_path=mirror_path,
            )
        )
    return samples


def load_samples() -> list[Sample]:
    """Downloads and parses the live archive index."""
    filelist = parse_filelist(_http_get(FILELIST_URL).decode("utf-8"))
    return parse_index(_http_get(INDEX_URL).decode("utf-8"), filelist)


def draft_entry(sample: Sample) -> dict:
    """Builds a manifest entry skeleton; sha256/size_bytes are filled after download."""
    original = sample.original_name
    if original and SAFE_FILE_NAME.match(original):
        file_name = original
    else:
        stem = re.sub(r"[^A-Za-z0-9._()+-]+", "_", posixpath.splitext(sample.display_name)[0]).strip("_")
        file_name = f"{stem}{posixpath.splitext(sample.display_name)[1]}"
    extension = posixpath.splitext(file_name)[1].lstrip(".").upper()
    entry = {
        "id": "",
        "tier": "qualification",
        "make": sample.make,
        "model": sample.model,
        "format": extension,
        "mode": sample.mode,
        "url": sample.getfile_url,
        "mirror_url": sample.mirror_url,
        "file_name": file_name,
        "license": sample.license_spdx,
        "source_page": sample.source_page,
        "raw_pixls_id": sample.sample_id,
        "notes": sample.remark,
    }
    return {k: v for k, v in entry.items() if v is not None}


def verify_manifest(manifest: dict, samples: Sequence[Sample]) -> list[str]:
    """Returns a list of problems; empty when every entry agrees with the archive index."""
    problems: list[str] = []
    by_id = {s.sample_id: s for s in samples}
    for entry in manifest.get("fixtures", []):
        fid = entry.get("id", "<no id>")
        if entry.get("license") != "CC0-1.0":
            problems.append(f"{fid}: manifest licence is {entry.get('license')!r}; only CC0-1.0 is accepted")
        sample_id = entry.get("raw_pixls_id")
        if not isinstance(sample_id, int):
            problems.append(f"{fid}: no raw_pixls_id; cannot cross-check against the archive")
            continue
        sample = by_id.get(sample_id)
        if sample is None:
            problems.append(f"{fid}: raw.pixls.us sample {sample_id} no longer exists in the index")
            continue
        if sample.license_spdx != "CC0-1.0":
            problems.append(f"{fid}: archive now lists sample {sample_id} as {sample.license_spdx}")
        if f"/getfile.php/{sample_id}/" not in entry.get("url", ""):
            problems.append(f"{fid}: url does not point at getfile.php/{sample_id}")
        manifest_sha = entry.get("sha256")
        if not manifest_sha:
            problems.append(f"{fid}: manifest has no sha256 yet")
        elif sample.published_sha256 and manifest_sha != sample.published_sha256:
            problems.append(
                f"{fid}: manifest sha256 {manifest_sha} differs from the archive's {sample.published_sha256}"
            )
        if (entry.get("make"), entry.get("model")) != (sample.make, sample.model):
            problems.append(
                f"{fid}: make/model {entry.get('make')!r}/{entry.get('model')!r} differ from the archive's "
                f"{sample.make!r}/{sample.model!r}"
            )
    return problems


def _format_row(sample: Sample) -> str:
    size = f"{sample.approx_size_mb:7.2f}" if sample.approx_size_mb is not None else "      ?"
    return (
        f"{sample.sample_id:5d}  {sample.license_spdx:16.16}  {sample.make[:12]:12}  {sample.model[:24]:24}  "
        f"{sample.mode[:30]:30}  {size} MB  {(sample.original_name or sample.display_name)[:50]:50}  "
        f"{sample.remark[:60]}"
    )


def _cmd_search(patterns: Iterable[str], samples: Sequence[Sample]) -> int:
    compiled = [re.compile(p, re.IGNORECASE) for p in patterns]
    for sample in samples:
        haystack = " | ".join(
            [sample.make, sample.model, sample.mode, sample.remark, sample.original_name or sample.display_name]
        )
        if all(p.search(haystack) for p in compiled):
            print(_format_row(sample))
    return 0


def _cmd_describe(ids: Iterable[int], samples: Sequence[Sample]) -> int:
    by_id = {s.sample_id: s for s in samples}
    entries = []
    for sample_id in ids:
        if sample_id not in by_id:
            print(f"error: sample {sample_id} is not in the archive index", file=sys.stderr)
            return 2
        entries.append(draft_entry(by_id[sample_id]))
    print(json.dumps(entries, indent=2, ensure_ascii=False))
    return 0


def _cmd_verify(manifest_path: Path, samples: Sequence[Sample]) -> int:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"error: cannot read manifest {manifest_path}: {exc}", file=sys.stderr)
        return 2
    problems = verify_manifest(manifest, samples)
    for problem in problems:
        print(f"MISMATCH {problem}")
    count = len(manifest.get("fixtures", []))
    print(f"{count - len({p.split(':')[0] for p in problems})}/{count} entries agree with the archive index")
    return 1 if problems else 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p_search = sub.add_parser("search", help="list samples matching all regular expressions")
    p_search.add_argument("patterns", nargs="+")
    p_describe = sub.add_parser("describe", help="print draft manifest entries for sample ids")
    p_describe.add_argument("ids", nargs="+", type=int)
    p_verify = sub.add_parser("verify-manifest", help="cross-check a fixture manifest with the index")
    p_verify.add_argument("manifest", nargs="?", type=Path, default=DEFAULT_MANIFEST)
    args = parser.parse_args(argv)

    try:
        samples = load_samples()
    except IndexError_ as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if args.command == "search":
        return _cmd_search(args.patterns, samples)
    if args.command == "describe":
        return _cmd_describe(args.ids, samples)
    return _cmd_verify(args.manifest, samples)


if __name__ == "__main__":
    sys.exit(main())
