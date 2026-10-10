"""Checks the committed catalog fixtures against the schema history in git (W12, ADR 0022).

For every schema version k of src/catalog/src/schema.cpp:
  1. the migration is the same in every commit since the commit that introduced it: its DDL (the raw
     string kSchemaV<k>), its row of kBuiltinMigrations (version and min_reader_version, which the
     migration stamps into catalog_meta) and its apply function (migrate_to_v<k>). A released
     migration was never edited, except for the edits listed in KNOWN_EDITS, each with its reason
     (SEC-08, W12);
  2. applying kSchemaV1 ... kSchemaV<k> to an empty database gives exactly the schema objects (type,
     name, table and SQL text) of the fixture v<k>.catalog, so the fixture's schema is the one a
     version-k build created.

Usage (from the repository root, after regenerating the fixtures):
    python tests/catalog_fixtures/check_historical_ddl.py [--fixtures src/catalog/tests/data/migration]

Prints one line per version and exits with 1 when a check fails. Needs git and Python's sqlite3
(SQLite 3.37 or later, for STRICT tables).
"""

import argparse
import re
import sqlite3
import subprocess
import sys
from pathlib import Path

SCHEMA_FILE = "src/catalog/src/schema.cpp"
RAW_STRING = re.compile(r'constexpr std::string_view kSchemaV(\d+) = R"sql\((.*?)\)sql";', re.DOTALL)
MIGRATION_TABLE = re.compile(r"constexpr Migration kBuiltinMigrations\[\] = \{(.*?)\n\};", re.DOTALL)
MIGRATION_ROW = re.compile(r"\{\s*(\d+)\s*,\s*(\d+)\s*,")
APPLY_FUNCTION = re.compile(r"Status migrate_to_v(\d+)\(Connection& connection\) \{(.*?)\n\}", re.DOTALL)

# Edits of released migrations, accepted with their reason: (version, part, the commit that made
# the edit, the value before it). Commits before that commit must show exactly the old value; the
# edit commit and every later one, HEAD's.
KNOWN_EDITS = [
    (2, "row", "5548046", (2, 1),
     "v2's min_reader_version was raised from 1 to 2 (review APP-01, APP-03: a v1 build would show and "
     "export edited photos as shot) two days after 0491d19 introduced v2, before any release. Catalogs "
     "that W6 development builds wrote in between record min_reader_version 1; v3's migration stamps 2, "
     "so they read like the v2 fixture once migrated, and no build that would misread them was released."),
]


def git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True, encoding="utf-8").stdout


def migrations_at(commit: str) -> dict[int, dict[str, object]]:
    """Each version's DDL, kBuiltinMigrations row (version, min_reader_version) and apply function
    body (whitespace-normalised) at `commit`; a part the file did not have yet is None."""
    text = git("show", f"{commit}:{SCHEMA_FILE}")
    migrations: dict[int, dict[str, object]] = {}
    for version, sql in RAW_STRING.findall(text):
        migrations[int(version)] = {"ddl": sql, "row": None, "apply": None}
    table = MIGRATION_TABLE.search(text)
    for version, min_reader in MIGRATION_ROW.findall(table.group(1) if table else ""):
        if int(version) in migrations:
            migrations[int(version)]["row"] = (int(version), int(min_reader))
    for version, body in APPLY_FUNCTION.findall(text):
        if int(version) in migrations:
            migrations[int(version)]["apply"] = " ".join(body.split())
    return migrations


def is_ancestor(commit: str, of: str) -> bool:
    return subprocess.run(["git", "merge-base", "--is-ancestor", commit, of], capture_output=True).returncode == 0


def unexplained_edits(version: int, part: str, history: dict, current: dict, introduced: list[str]) -> list[str]:
    """Commits (oldest first) whose `part` of `version` differs from HEAD's without a KNOWN_EDITS entry."""
    edited = []
    for commit in introduced:
        value = history[commit][version][part]
        if value == current[version][part]:
            continue
        known = [e for e in KNOWN_EDITS if e[0] == version and e[1] == part]
        explained = any(
            value == old and not commit.startswith(edit) and is_ancestor(commit, edit)
            for (_, _, edit, old, _) in known
        )
        if not explained:
            edited.append(commit)
    return edited


def schema_objects(connection: sqlite3.Connection) -> list[tuple]:
    return connection.execute(
        "SELECT type, name, tbl_name, sql FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%' ORDER BY type, name"
    ).fetchall()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fixtures", default="src/catalog/tests/data/migration")
    args = parser.parse_args()
    fixtures = Path(args.fixtures)

    commits = git("log", "--format=%h", "--", SCHEMA_FILE).split()  # newest first
    history = {commit: migrations_at(commit) for commit in commits}
    current = migrations_at("HEAD")
    failures = 0
    for version in sorted(current):
        introduced = [c for c in reversed(commits) if version in history[c]]
        problems = []
        for part, what in (("ddl", "DDL"), ("row", "kBuiltinMigrations row"), ("apply", "apply function")):
            if current[version][part] is None:
                problems.append(f"HEAD has no {what}")
                continue
            edited = unexplained_edits(version, part, history, current, introduced)
            if edited:
                problems.append(f"its {what} differs from HEAD in {', '.join(edited)}")
        if problems:
            print(f"v{version}: FAILED: {'; '.join(problems)}")
            failures += 1
            continue
        fixture = fixtures / f"v{version}.catalog"
        if not fixture.exists():
            print(f"v{version}: FAILED: {fixture} is missing")
            failures += 1
            continue
        expected = sqlite3.connect(":memory:")
        for step in range(1, version + 1):
            expected.executescript(current[step]["ddl"])
        actual = sqlite3.connect(f"file:{fixture.as_posix()}?mode=ro", uri=True)
        user_version = actual.execute("PRAGMA user_version").fetchone()[0]
        same = schema_objects(expected) == schema_objects(actual) and user_version == version
        known = [e for e in KNOWN_EDITS if e[0] == version]
        print(
            f"v{version}: migration unchanged since {introduced[0] if introduced else 'HEAD'} "
            f"({len(introduced)} commit(s); DDL, migration row, apply function"
            f"{'; known edit ' + ', '.join(f'{e[2]} ({e[1]})' for e in known) if known else ''}); "
            f"fixture schema {'matches' if same else 'DIFFERS'}"
        )
        failures += 0 if same else 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
