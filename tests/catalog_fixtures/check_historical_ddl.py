"""Checks the committed catalog fixtures against the schema history in git (W12, ADR 0022).

For every schema version k of src/catalog/src/schema.cpp:
  1. the migration's DDL (the raw string kSchemaV<k>) is the same in every commit since the commit
     that introduced it: a released migration was never edited;
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


def git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True, encoding="utf-8").stdout


def migrations_at(commit: str) -> dict[int, str]:
    text = git("show", f"{commit}:{SCHEMA_FILE}")
    return {int(version): sql for version, sql in RAW_STRING.findall(text)}


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
        edited = [c for c in introduced if history[c][version] != current[version]]
        if edited:
            print(f"v{version}: FAILED: the migration differs from HEAD in {', '.join(edited)}")
            failures += 1
            continue
        fixture = fixtures / f"v{version}.catalog"
        if not fixture.exists():
            print(f"v{version}: FAILED: {fixture} is missing")
            failures += 1
            continue
        expected = sqlite3.connect(":memory:")
        for step in range(1, version + 1):
            expected.executescript(current[step])
        actual = sqlite3.connect(f"file:{fixture.as_posix()}?mode=ro", uri=True)
        user_version = actual.execute("PRAGMA user_version").fetchone()[0]
        same = schema_objects(expected) == schema_objects(actual) and user_version == version
        print(
            f"v{version}: migration unchanged since {introduced[0] if introduced else 'HEAD'} "
            f"({len(introduced)} commit(s)); fixture schema {'matches' if same else 'DIFFERS'}"
        )
        failures += 0 if same else 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
