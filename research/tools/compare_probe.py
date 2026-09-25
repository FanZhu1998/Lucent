#!/usr/bin/env python3
"""Compare the parity blocks of two probe reports (schema ``lucent.probe-report`` version 1).

Usage::

    compare_probe.py LUCENT.json REFERENCE.json [--allowances FILE] [--require-raw-hash]

``LUCENT.json`` is written by Lucent's native probe (``lucent-probe``); ``REFERENCE.json`` by
``probe_reference.py`` (a full report or a compact ``fixtures/reference/<id>.parity.json``).

Rules (see research/README.md, "Probe report contract"):

* Both files must be ``lucent.probe-report``; a ``schema_version`` other than 1 is rejected.
* ``file_size`` must be equal, and ``file_sha256`` too when both reports carry it; otherwise the
  reports describe different files and nothing is compared.
* Integers, strings and CFA patterns compare exactly; ``null`` only equals ``null``.
* Floats compare with ``|a - b| <= max(1e-6 * max(|a|, |b|), 1e-9)``. Integral JSON numbers are
  accepted in float fields. The non-finite markers ``"nan"``, ``"inf"``, ``"-inf"`` only equal
  themselves.
* ``raw_data_sha256`` is compared when both reports carry it. It is only produced when decoding
  flat raws, so its absence is reported as "not compared" unless ``--require-raw-hash`` is given.
* Any other field present on only one side is a mismatch. Fields outside the v1 contract are
  still compared (exact, or float tolerance for numbers) and flagged as "extra".
* When neither report has a parity block (both decoders refused the file) the result is a match
  of failures and the recorded errors are printed; when only one side has it, that is a mismatch.

Allowances (``--allowances FILE``) document justified, reviewed differences::

    {
      "schema": "lucent.parity-allowances",
      "schema_version": 1,
      "allowances": [
        {
          "field": "daylight_whitebalance",      # parity field name (required)
          "files": ["*.RAF"],                    # optional fnmatch patterns on file_name
          "action": "tolerance",                 # "tolerance" (float fields) or "ignore"
          "rel_tol": 1e-4, "abs_tol": 1e-9,      # for "tolerance"; abs_tol defaults to 1e-9
          "reason": "why the difference is correct and who reviewed it"   # required
        }
      ]
    }

Exit status: 0 when every compared field matches or is allowed, 1 on any mismatch,
2 for usage, file or schema errors.
"""

from __future__ import annotations

import argparse
import dataclasses
import fnmatch
import json
import math
import sys
from pathlib import Path
from typing import Any, Sequence

REPORT_SCHEMA = "lucent.probe-report"
REPORT_SCHEMA_VERSION = 1
ALLOWANCE_SCHEMA = "lucent.parity-allowances"
ALLOWANCE_SCHEMA_VERSION = 1

DEFAULT_REL_TOL = 1e-6
DEFAULT_ABS_TOL = 1e-9
NON_FINITE = ("nan", "inf", "-inf")

# Field kinds of the v1 parity contract, in report order.
INT = "int"
FLOAT = "float"
STR = "str"
FLOAT_LIST = "float_list"
FLOAT_MATRIX = "float_matrix"
INT_LIST = "int_list"
INT_MATRIX = "int_matrix"

PARITY_FIELDS: dict[str, tuple[str, bool]] = {
    # name: (kind, nullable)
    "raw_width": (INT, False),
    "raw_height": (INT, False),
    "width": (INT, False),
    "height": (INT, False),
    "top_margin": (INT, False),
    "left_margin": (INT, False),
    "iwidth": (INT, False),
    "iheight": (INT, False),
    "flip": (INT, False),
    "num_colors": (INT, False),
    "color_desc": (STR, False),
    "raw_pattern": (INT_MATRIX, True),
    "black_level_per_channel": (INT_LIST, False),
    "white_level": (INT, False),
    "camera_white_level_per_channel": (INT_LIST, True),
    "camera_whitebalance": (FLOAT_LIST, False),
    "daylight_whitebalance": (FLOAT_LIST, False),
    "rgb_xyz_matrix": (FLOAT_MATRIX, False),
    "color_matrix": (FLOAT_MATRIX, False),
    "raw_type": (STR, False),
    "raw_data_sha256": (STR, False),
}
FLOAT_KINDS = (FLOAT, FLOAT_LIST, FLOAT_MATRIX)


class UsageError(Exception):
    """Invalid input files or arguments (exit status 2)."""


@dataclasses.dataclass(frozen=True)
class Allowance:
    index: int
    field: str
    files: tuple[str, ...]
    action: str
    rel_tol: float
    abs_tol: float
    reason: str

    def applies_to(self, file_name: str) -> bool:
        if not self.files:
            return True
        return any(fnmatch.fnmatch(file_name.lower(), pattern.lower()) for pattern in self.files)


@dataclasses.dataclass
class FieldResult:
    field: str
    lucent: Any
    reference: Any
    status: str  # ok | MISMATCH | allowed | not compared | extra ok | EXTRA MISMATCH
    detail: str = ""

    @property
    def failed(self) -> bool:
        return self.status in ("MISMATCH", "EXTRA MISMATCH")


# ---- loading ----------------------------------------------------------------------------------


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise UsageError(f"cannot read {path}: {exc}") from exc
    except ValueError as exc:
        raise UsageError(f"{path} is not valid JSON: {exc}") from exc


def load_report(path: Path) -> dict[str, Any]:
    report = load_json(path)
    if not isinstance(report, dict) or report.get("schema") != REPORT_SCHEMA:
        raise UsageError(f"{path}: not a {REPORT_SCHEMA} document")
    version = report.get("schema_version")
    if not isinstance(version, int) or isinstance(version, bool):
        raise UsageError(f"{path}: schema_version must be an integer")
    if version != REPORT_SCHEMA_VERSION:
        raise UsageError(
            f"{path}: schema_version {version} is not supported (this tool reads {REPORT_SCHEMA_VERSION})"
        )
    parity = report.get("parity")
    if parity is not None and not isinstance(parity, dict):
        raise UsageError(f"{path}: 'parity' must be an object or null")
    return report


def load_allowances(path: Path) -> list[Allowance]:
    document = load_json(path)
    if not isinstance(document, dict) or document.get("schema") != ALLOWANCE_SCHEMA:
        raise UsageError(f"{path}: not a {ALLOWANCE_SCHEMA} document")
    if document.get("schema_version") != ALLOWANCE_SCHEMA_VERSION:
        raise UsageError(f"{path}: unsupported schema_version {document.get('schema_version')!r}")
    entries = document.get("allowances")
    if not isinstance(entries, list):
        raise UsageError(f"{path}: 'allowances' must be an array")

    allowances = []
    for index, entry in enumerate(entries):
        where = f"{path}: allowances[{index}]"
        if not isinstance(entry, dict):
            raise UsageError(f"{where} must be an object")
        unknown_keys = set(entry) - {"field", "files", "action", "rel_tol", "abs_tol", "reason"}
        if unknown_keys:
            raise UsageError(f"{where}: unknown key(s) {sorted(unknown_keys)}")
        field = entry.get("field")
        if not isinstance(field, str) or not field:
            raise UsageError(f"{where}: 'field' is required")
        reason = entry.get("reason")
        if not isinstance(reason, str) or not reason.strip():
            raise UsageError(f"{where}: a non-empty 'reason' is required")
        files = entry.get("files", [])
        if not isinstance(files, list) or not all(isinstance(p, str) and p for p in files):
            raise UsageError(f"{where}: 'files' must be an array of non-empty patterns")
        action = entry.get("action")
        rel_tol, abs_tol = DEFAULT_REL_TOL, DEFAULT_ABS_TOL
        if action == "tolerance":
            kind = PARITY_FIELDS.get(field, (None, False))[0]
            if kind not in FLOAT_KINDS:
                raise UsageError(f"{where}: 'tolerance' applies only to float fields, not {field!r}")
            rel_tol = entry.get("rel_tol")
            abs_tol = entry.get("abs_tol", DEFAULT_ABS_TOL)
            for name, value in (("rel_tol", rel_tol), ("abs_tol", abs_tol)):
                if not _is_number(value) or not math.isfinite(value) or value < 0:
                    raise UsageError(f"{where}: '{name}' must be a finite number >= 0")
        elif action != "ignore":
            raise UsageError(f"{where}: 'action' must be 'tolerance' or 'ignore'")
        allowances.append(
            Allowance(index, field, tuple(files), action, float(rel_tol), float(abs_tol), reason.strip())
        )
    return allowances


# ---- comparison -------------------------------------------------------------------------------


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def floats_close(a: Any, b: Any, rel_tol: float, abs_tol: float) -> tuple[bool, str]:
    """Compares two float-field values (numbers or non-finite markers)."""
    if isinstance(a, str) or isinstance(b, str):
        ok = isinstance(a, str) and isinstance(b, str) and a == b and a in NON_FINITE
        return ok, "" if ok else f"{a!r} vs {b!r}"
    if not (_is_number(a) and _is_number(b)):
        return False, f"non-numeric value {a!r} vs {b!r}"
    a, b = float(a), float(b)
    diff = abs(a - b)
    limit = max(rel_tol * max(abs(a), abs(b)), abs_tol)
    if diff <= limit:
        return True, ""
    scale = max(abs(a), abs(b))
    rel = diff / scale if scale else math.inf
    return False, f"{a!r} vs {b!r} (abs {diff:.3g}, rel {rel:.3g})"


def _shape(value: Any) -> Any:
    if isinstance(value, list):
        return [len(value)] + ([_shape(value[0])] if value and isinstance(value[0], list) else [])
    return None


def compare_values(kind: str, a: Any, b: Any, rel_tol: float, abs_tol: float) -> tuple[bool, str]:
    """Returns (equal, detail) for two non-null values of the given field kind."""
    if kind == INT:
        if not (_is_int(a) and _is_int(b)):
            return False, f"expected integers, got {a!r} and {b!r}"
        return a == b, "" if a == b else f"{a} vs {b}"
    if kind == FLOAT:
        return floats_close(a, b, rel_tol, abs_tol)
    if kind == STR:
        if not (isinstance(a, str) and isinstance(b, str)):
            return False, f"expected strings, got {a!r} and {b!r}"
        return a == b, "" if a == b else f"{a!r} vs {b!r}"
    if kind in (INT_LIST, FLOAT_LIST, INT_MATRIX, FLOAT_MATRIX):
        rows_a = a if kind in (INT_MATRIX, FLOAT_MATRIX) else [a]
        rows_b = b if kind in (INT_MATRIX, FLOAT_MATRIX) else [b]
        if not (isinstance(rows_a, list) and isinstance(rows_b, list)) or len(rows_a) != len(rows_b):
            return False, f"shape {_shape(a)} vs {_shape(b)}"
        problems = []
        for r, (row_a, row_b) in enumerate(zip(rows_a, rows_b)):
            if not (isinstance(row_a, list) and isinstance(row_b, list)) or len(row_a) != len(row_b):
                return False, f"shape {_shape(a)} vs {_shape(b)}"
            for c, (x, y) in enumerate(zip(row_a, row_b)):
                if kind in FLOAT_KINDS:
                    ok, why = floats_close(x, y, rel_tol, abs_tol)
                else:
                    ok = _is_int(x) and _is_int(y) and x == y
                    why = "" if ok else f"{x!r} vs {y!r}"
                if not ok:
                    index = f"[{r}][{c}]" if kind in (INT_MATRIX, FLOAT_MATRIX) else f"[{c}]"
                    problems.append(f"{index}: {why}")
        return not problems, "; ".join(problems[:4]) + (f"; ... {len(problems) - 4} more" if len(problems) > 4 else "")
    raise AssertionError(f"unknown kind {kind}")


def _guess_kind(a: Any, b: Any) -> str:
    """Kind for fields outside the v1 contract: numbers use float tolerance unless both are ints."""
    sample = a if a is not None else b
    if _is_int(a) and _is_int(b):
        return INT
    if _is_number(sample):
        return FLOAT
    if isinstance(sample, list):
        if sample and isinstance(sample[0], list):
            return FLOAT_MATRIX if any(_is_number(v) and not _is_int(v) for row in sample for v in row) else INT_MATRIX
        return FLOAT_LIST if any(_is_number(v) and not _is_int(v) for v in sample) else INT_LIST
    return STR


def compare_parity(
    lucent: dict[str, Any],
    reference: dict[str, Any],
    file_name: str,
    allowances: Sequence[Allowance],
    require_raw_hash: bool,
) -> tuple[list[FieldResult], set[int]]:
    """Compares two parity blocks. Returns per-field results and the indices of used allowances."""
    used: set[int] = set()
    results: list[FieldResult] = []
    names = list(PARITY_FIELDS) + sorted((set(lucent) | set(reference)) - set(PARITY_FIELDS))
    for name in names:
        in_l, in_r = name in lucent, name in reference
        if not in_l and not in_r:
            if name == "raw_data_sha256" and require_raw_hash:
                results.append(FieldResult(name, None, None, "MISMATCH", "absent on both sides"))
            continue
        a, b = lucent.get(name), reference.get(name)
        extra = name not in PARITY_FIELDS
        if name == "raw_data_sha256" and not (in_l and in_r) and not require_raw_hash:
            results.append(FieldResult(name, a, b, "not compared", "present on one side only"))
            continue

        applicable = [al for al in allowances if al.field == name and al.applies_to(file_name)]
        kind = None
        if not (in_l and in_r):
            equal, detail = False, "missing from " + ("reference" if in_l else "lucent")
        elif a is None or b is None:
            nullable = PARITY_FIELDS.get(name, (None, True))[1]
            equal = a is None and b is None
            detail = "" if equal else f"{a!r} vs {b!r}"
            if equal and not nullable:
                equal, detail = False, "null is not allowed for this field"
        else:
            kind = PARITY_FIELDS[name][0] if not extra else _guess_kind(a, b)
            equal, detail = compare_values(kind, a, b, DEFAULT_REL_TOL, DEFAULT_ABS_TOL)

        status = ("extra ok" if extra else "ok") if equal else ("EXTRA MISMATCH" if extra else "MISMATCH")
        if not equal and kind in FLOAT_KINDS:
            for allowance in (al for al in applicable if al.action == "tolerance"):
                within, _ = compare_values(kind, a, b, allowance.rel_tol, allowance.abs_tol)
                if within:
                    used.add(allowance.index)
                    status = "allowed"
                    detail = f"within allowance[{allowance.index}] tolerance (rel {allowance.rel_tol:g}): {detail}"
                    break
        if status in ("MISMATCH", "EXTRA MISMATCH"):
            ignore = next((al for al in applicable if al.action == "ignore"), None)
            if ignore is not None:
                used.add(ignore.index)
                status, detail = "allowed", f"ignored by allowance[{ignore.index}]: {detail}"
        results.append(FieldResult(name, a, b, status, detail))
    return results, used


# ---- output -----------------------------------------------------------------------------------


def _short(value: Any, width: int = 34) -> str:
    text = "-" if value is None else json.dumps(value, separators=(",", ":"))
    return text if len(text) <= width else text[: width - 3] + "..."


def _generator_label(report: dict[str, Any]) -> str:
    generator = report.get("generator")
    if not isinstance(generator, dict):
        return "(no generator block)"
    parts = [str(generator.get("name", "?"))]
    for key in ("rawpy", "libraw", "version"):
        if key in generator:
            parts.append(f"{key}={_short(generator[key], 40)}")
    return " ".join(parts)


def _error_label(report: dict[str, Any]) -> str:
    error = report.get("error")
    if isinstance(error, dict):
        return f"{error.get('stage', '?')}: {error.get('type', '?')}: {error.get('message', '')}"
    return f"status={report.get('status', '?')}"


def run(
    lucent_path: Path, reference_path: Path, allowances_path: Path | None, require_raw_hash: bool
) -> int:
    lucent = load_report(lucent_path)
    reference = load_report(reference_path)
    allowances = load_allowances(allowances_path) if allowances_path else []

    file_name = str(reference.get("file_name") or lucent.get("file_name") or "")
    print(f"lucent:    {lucent_path}  [{_generator_label(lucent)}]")
    print(f"reference: {reference_path}  [{_generator_label(reference)}]")
    print(f"file:      {file_name}")
    if lucent.get("file_name") != reference.get("file_name"):
        print(f"note: file names differ ({lucent.get('file_name')!r} vs {reference.get('file_name')!r})")

    identity_problems = []
    if lucent.get("file_size") != reference.get("file_size"):
        identity_problems.append(f"file_size {lucent.get('file_size')!r} vs {reference.get('file_size')!r}")
    if "file_sha256" in lucent and "file_sha256" in reference and lucent["file_sha256"] != reference["file_sha256"]:
        identity_problems.append(f"file_sha256 {lucent['file_sha256']} vs {reference['file_sha256']}")
    if identity_problems:
        print("MISMATCH: the reports describe different files: " + "; ".join(identity_problems))
        return 1

    parity_l, parity_r = lucent.get("parity"), reference.get("parity")
    if parity_l is None or parity_r is None:
        if parity_l is None and parity_r is None:
            print("both sides report no parity block (decoder refused the file); consistent")
            print(f"  lucent:    {_error_label(lucent)}")
            print(f"  reference: {_error_label(reference)}")
            return 0
        missing, report = ("lucent", lucent) if parity_l is None else ("reference", reference)
        print(f"MISMATCH: {missing} has no parity block ({_error_label(report)}); the other side decoded the file")
        return 1

    results, used = compare_parity(parity_l, parity_r, file_name, allowances, require_raw_hash)
    print()
    print(f"{'field':32} {'lucent':34} {'reference':34} result")
    print(f"{'-' * 32} {'-' * 34} {'-' * 34} {'-' * 14}")
    for result in results:
        print(f"{result.field:32} {_short(result.lucent):34} {_short(result.reference):34} {result.status}")
    details = [r for r in results if r.detail and r.status != "ok"]
    if details:
        print()
        for result in details:
            print(f"{result.status}: {result.field}: {result.detail}")
    for allowance in allowances:
        if allowance.index in used:
            print(f"allowance[{allowance.index}] used for {allowance.field}: {allowance.reason}")
        elif allowance.applies_to(file_name):
            print(f"note: allowance[{allowance.index}] for {allowance.field} was not needed")

    failures = [r for r in results if r.failed]
    compared = sum(1 for r in results if r.status != "not compared")
    print()
    print(f"{compared} fields compared, {len(failures)} mismatched, "
          f"{sum(1 for r in results if r.status == 'allowed')} allowed")
    return 1 if failures else 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compare the parity blocks of a Lucent probe report and a rawpy reference report."
    )
    parser.add_argument("lucent", type=Path, help="report written by lucent-probe")
    parser.add_argument("reference", type=Path, help="report written by probe_reference.py")
    parser.add_argument("--allowances", type=Path, help="lucent.parity-allowances file")
    parser.add_argument(
        "--require-raw-hash",
        action="store_true",
        help="fail unless both reports carry an equal raw_data_sha256",
    )
    args = parser.parse_args(argv)
    try:
        return run(args.lucent, args.reference, args.allowances, args.require_raw_hash)
    except UsageError as exc:
        print(f"compare_probe: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
