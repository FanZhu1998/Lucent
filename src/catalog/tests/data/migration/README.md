# Catalog fixtures of every historical schema version

One Lucent catalog per schema version the product has released (W12, ADR 0022; the release gate
"Update compatibility": prior catalog and recipe fixtures migrate safely). `fixtures.json` lists
them with their SHA-256 and row counts. They are committed and **never change**: a later schema
version adds its own fixture.

| File | Schema | Min. reader | Written by | Content |
|---|---|---|---|---|
| `v1.catalog` | 1 | 1 | W4 (catalog and import) | 9 sources (referenced, a copy from a card with the original's identity, a removable card file, a missing and a changed file, a content duplicate under a non-ASCII name, a two-image DNG, an unsupported file), 10 items (picks, ratings, Supported, Limited and Unsupported with W4's reasons), 4 imports (completed, a copy, interrupted, still running) |
| `v2.catalog` | 2 | 2 | W6 (recipes and editor) | v1 and the capture settings; 8 recipe revisions, all `lucent.develop` version 1 (edits, an undo and a new branch, a lock, a recorded reset, a paste, an edit of the missing photo) |
| `v3.catalog` | 3 | 2 | W7 (durable batch) | v2 and 4 export batches: completed with receipts, cancelled after one output, paused with a failed, a waiting and an already exported photo, and one whose session ended mid-render (a leased job with its temporary output in the journal); the batch documents as W7 wrote them |
| `v4.catalog` | 4 | 2 | W7's closing review | v3, a single export (a batch of one), an interruption count, and finished batches with and without their report |
| `v5.catalog` | 5 | 2 | W8 to W12 | v4, `lucent.develop` version 2 recipes (geometry version 1 with a crop, a radial mask and colour noise reduction; geometry version 2), the editor's Auto and an automatic and an analyse-only batch (real analysis documents and records from `run_automatic`), camera support records (rules 2) naming database 2.0.1 |

All source paths are on `Z:` and `E:` and do not exist: the files of the photographs are never
needed (the presence check marks them Missing).

## How they were made

By driving the older migration states (`tests/catalog_fixtures/generate_catalog_fixtures.cpp`,
built as `lucent_catalog_fixture_generator`):

1. Each era's content is written through the catalog's API, and the application's own document
   writers, into a scratch catalog: rows are what the catalog's code writes for those columns, whose
   meaning never changed after they were introduced (that would itself need a migration). Documents
   are each era's: version-1 recipes before W9, the batch policy without the members W7's closing
   review added (`retry.max_interruptions`, `single_export`) for v3, receipts without the support
   state W10 added before v5, the decoder version without the decoded-RAW contract before W9.
2. The fixture file is created by exactly the migrations a build of that version ran on an empty
   file: the first *v* entries of `detail::builtin_migrations()`.
3. Every table is filled from the scratch catalog with the columns schema *v* has, foreign keys and
   the schema's triggers enforced; the history is stamped with each version's release time; the file
   is vacuumed into one rollback-journal file and checked (`integrity_check`, `foreign_key_check`).

`tests/catalog_fixtures/check_historical_ddl.py` then verifies against git history that each
migration's DDL (`kSchemaV1` ... `kSchemaV5` in `src/catalog/src/schema.cpp`) is unchanged since the
commit that introduced it (b2951f7, 0491d19, 6711a6a, 93f72ca, 7df00b8) and that applying them to an
empty database (with Python's own SQLite) gives exactly each fixture's schema objects. Result on
2026-10-09: all five unchanged, all five schemas identical. Regenerating with the same build
reproduces all five files byte for byte (the automatic run's measured durations are fixed).

```powershell
build\w12-upd\bin\lucent_catalog_fixture_generator.exe src\catalog\tests\data\migration
python tests\catalog_fixtures\check_historical_ddl.py
```

## What the tests check

- `src/catalog/tests/migration_fixture_test.cpp`: each fixture has the schema that version's
  migrations create (a released migration is never edited); a copy migrates after a verified
  pre-migration backup that holds the fixture as it was (and restores to it), every value of every
  column that existed is unchanged, added columns hold their defaults (v4's `reported_at` equals
  `finished_at` for finished batches), integrity and foreign keys hold, everything reads through the
  API and the catalog accepts writes; a catalog of a newer Lucent opens read-only with the reason and
  is never written; one whose minimum reader version this build does not reach is refused.
- `src/application/tests/update_compatibility_test.cpp`: the migrated fixtures through the
  application: the library loads every item and its support state, every recipe (versions 1 and 2)
  reads, every batch document and receipt reads, recipes of a newer Lucent are reported and never
  rendered as if their unknown operations were absent, and a missing or replaced camera support
  database is never substituted.
