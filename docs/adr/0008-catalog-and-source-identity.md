# ADR 0008: Persistent library: catalog, source identity, duplicates, relinking and thumbnail cache

- **Status:** Accepted (amended 2026-10-03 after the W4 review: identity matching, copies, relink,
  absent volumes, thumbnail size)
- **Date:** 2026-10-03
- **Workflow:** W4 (Catalog and import)

## Context

Blueprint section 2 ("Import and organize") asks for a library that references files in place by
default, offers **Copy into library folder** with a destination the user chooses ("verifies each
copy before registering it, and leaves the source intact"), keeps a removable-card source visibly
identified, keeps missing files in the catalog with a relink action, and detects files with
identical contents while letting intentional duplicates remain separate items. Section 7 requires
the source identity to be the content hash plus subimage ("File path alone is not image identity"),
stable-read checks before and after hashing, re-import of a changed source "instead of using stale
diagnostics", and a live database "on a local disk, not an SMB/network share" with "a single
coordinated writer". Section 8 places the catalog in per-user application data, thumbnails in a
disposable cache, asks for migrations that run "transactionally after backing up the catalog",
and for catalog backup and restore. Section 10's release gate "Original preservation" requires
source hashes to be unchanged after import.

W3 (ADR 0007) had an in-memory `SessionLibrary` behind the `app::Library` interface. W4 added, in
parallel, the `lucent_catalog` module (SQLite, schema v1, migrations, backups, repository API) and
core storage utilities (`core::storage_kind`, `core::copy_file_verified`, per-user directories,
atomic writes) and the on-disk `app::ThumbnailCache`. This ADR records how they form the
persistent library and the decisions about identity, duplicates, relinking and caching.

## Decision

1. **Locations.** The catalog is `<root>\data\lucent.catalog`, where `<root>` is
   `core::default_app_directories()` (`%LOCALAPPDATA%\Lucent`), so it lives with the other
   per-user application data the blueprint lists; its backups go to `<root>\data\backups`
   (`catalog::default_backup_directory`). The thumbnail cache is `<root>\cache\thumbnails`. The
   whole `<root>` is one of Lucent's own folders that an import never scans
   (`catalog::default_catalog_path()` names the same file). `Lucent.exe --data-root <folder>`
   (developer-only) uses another root, and keeps its settings (`settings.ini`) there too, so
   screenshots and smoke tests never touch the user's library or settings.

2. **Catalog storage and concurrency** (implemented by `lucent_catalog`, `catalog.hpp`). SQLite is
   private to `lucent_catalog`. Only local disks are accepted for the live catalog: network
   locations and drives whose type cannot be determined are refused; removable drives are
   allowed, with a warning. WAL journaling with `synchronous=NORMAL`: a commit is atomic and
   survives a crash of Lucent; after a power loss the newest commits since the last checkpoint
   may be lost, never consistency (`Durability::Full` exists for one flush per commit). One writer
   connection behind a mutex, a bounded pool of read-only connections, and an exclusive
   `<catalog>.lock` file held while a catalog is open for writing, so a second Lucent cannot become
   a second writer.

3. **Versions, migrations, backups.** `PRAGMA application_id` ('LUCN') marks a Lucent catalog and
   `PRAGMA user_version` is its schema version; `catalog_meta` (schema_version,
   min_reader_version, created_by/at, written_by) and `schema_history` are a forward-compatibility
   contract that never changes. Each migration runs in its own transaction with foreign keys
   verified before commit, after a verified online backup (`sqlite3_backup`) with per-kind
   retention. A catalog written by a newer Lucent is never migrated or written: it opens read-only
   when its `min_reader_version` allows this build to read it, otherwise it is refused. A schema
   version that changes what renders or exports of existing items mean raises
   `min_reader_version` to itself, and it never goes back down (`validate_migrations`); only a
   purely additive change keeps it. Schema v2 (W6, ADR 0010) is the first: an older build would
   show and export edited photos as shot, so v2 needs a v2 reader. Enumerations
   are stored as text without CHECK constraints so values can only be appended; lists use keyset
   paging. Restore verifies the backup, takes a pre-restore backup (or moves a damaged catalog
   aside) and installs a verified copy. The Library menu offers **Back up catalog** (written on the
   Background lane); a restore user interface is not part of W4.

4. **The library is a write-through mirror of the catalog** (`app::CatalogLibrary`). It implements
   `app::Library`, so import, previews, export and the UI adapters are unchanged. `open()` loads all
   sources and items in one read transaction (10,000 items in 0.11 s); every read (`items`,
   `find`, `find_source`) is served from memory. Each mutation is one catalog write transaction (an
   import batch of `add()` is one commit); memory changes and observers are notified only after the
   commit, so a failed write changes nothing. Item ids are the catalog's ids, stable across
   restarts. Schema v1 does not store ISO, exposure time, f-number, focal length or the file's
   image count; they are known for items probed in the running session only (the image count is
   restored from the catalogued images). Support reasons are stored in an application-owned JSON
   array (`src/application/src/catalog_mapping.hpp`); unknown spellings from a newer Lucent degrade
   to generic wording, and the stored support status stays authoritative.

5. **Source identity.** A source is one file: its canonical path (links resolved; compared with
   the file system's case rules via `catalog::make_path_key`), its `core::FileIdentity` (volume
   serial, file id, size, last write time) when it was catalogued, and its SHA-256 once hashed
   with a stable read. An item is one photograph of a source (its subimage). The hash, identity,
   presence state, storage kind, import mode and origin (path and, for copies, the original's
   identity during the copy) are facts of the source shared by its items. A file version is
   matched by the whole identity, never by the file id alone: FAT and exFAT, which most camera
   cards use, derive file ids from directory positions and reuse them for new photos. The
   library's version of a file found at another path is a duplicate while the library's path
   still holds it (a hard link), and the same file moved when it does not (renamed or moved within
   its volume, or a card back under another drive letter): an import then moves the item there.
   Paths that are not valid Unicode (NTFS allows unpaired surrogates) cannot be stored; the
   importer skips such files one by one (`SkipReason::InvalidName`).

6. **Change detection; stale diagnostics are never reused.**
   - Import of a file already in the library at the same path with another identity re-probes it
     and refreshes its items in place (ids and flags kept; identity, probe and support replaced;
     the old hash cleared; preview caches of those items dropped). Import of a Missing or Changed
     file found again with its catalogued identity makes it Present again, and the library's
     version found at another path moves the item there (decision 5); the import summary counts
     both as "found again", not as duplicates.
   - After the window is shown (`LibraryService::start`), a background check stats every source
     (no content is read): not found, or its volume absent (a card reader without its card or a
     disconnected drive: `ERROR_NOT_READY` and similar, `core::is_volume_absent_win32_error`) →
     **Missing**; the catalogued identity → Present (a returning card restores Missing files);
     another identity → **Changed**, then re-probed through the import pipeline as above. The
     same check runs again whenever a volume arrives or leaves while Lucent runs
     (`VolumeWatcher`, `WM_DEVICECHANGE`). Opening the library reads only the catalog; thumbnails
     come from the disk cache (decision 12), and a photo without a cached thumbnail (not hashed
     yet, evicted, or the cache was deleted) is read for its camera preview when the contact
     sheet shows it, so a missing photo without a cached thumbnail shows none.
   - Hashing compares the identity `hash_file` reports with the catalogued one, and with
     verification also the hash: a difference marks the file Changed and queues a re-read.
   - Renders and exports keep refusing a file whose identity changed (`SourceChanged`, ADR 0007).

7. **Hashing in the background** (`app::MaintenanceService`). New and refreshed items are queued
   after each import, unhashed items after the start-up check. Work runs on the executor's
   Background lane in steps of about 512 MiB, each a separate task, so imports and exports queued
   meanwhile run in between; it is cancellable, reports progress in photos ("Verifying photos… 12
   of 340" in the header) and counts as busy. Copies made by an import are already hashed.

8. **Duplicates are flagged, never merged.** Items of different sources with the same SHA-256 and
   the same subimage are content duplicates: each shows a **Duplicate** badge and the details list
   the other files. Nothing is removed or merged; the photographer's **Pick** / **Reject** choice
   is kept per item in the catalog (written on the Quick lane), so intentional duplicates remain
   separate library items with their own state.

9. **Copy into library folder.** The Import dialog (after Add photos, Add folder or a drop) shows
   the selected location, **Include subfolders** and **Copy into library folder** with a
   destination chosen by the user (remembered between sessions). Each file is copied with
   `core::copy_file_verified` (stable-read hash of the source, flushed copy read back without the
   system cache and compared, no-clobber publication) into the destination, below a folder named
   after the import folder and mirroring its subfolders; only the verified copy is probed and
   registered (`ImportMode::Copied`, origin path, the original's identity during the copy, hash).
   The source is only read. An original copied before is skipped only when the earlier copy holds
   the photo the original holds now: the original's identity recorded with the copy equals its
   current identity (under any path or drive letter, without reading it), or, for a copy found by
   origin path and size only, the original's SHA-256 equals the copy's. Otherwise it is a new
   photo (cameras restart their numbering after a format, many RAW formats have a fixed size) and
   is copied; an earlier copy that is Missing is made again and its item moves there. A file
   already in the library as a referenced original moves to its copy (`Library::relocate_source`,
   keeping its id and pick; the copy's hash must be the item's, or for an item never hashed the
   original's identity during the copy must be the item's); one whose file changed (a reused
   card) is refreshed in place first and then moved to the copy of the new version, so no item is
   left on the card after a copy was asked for. Files already inside the destination are
   referenced in place. A copy that fails is reported per file with its reason and leaves nothing
   behind; a verified copy that does not become a library file (no RAW photo in it, a cancelled
   probe, a batch the catalog refused) is removed again (`core::remove_verified_copy`, checked
   through the deleting handle). A selected file with a non-RAW extension is probed before it is
   copied.

10. **Removable media.** Every registered file records `core::storage_kind` (cards, USB sticks and
    USB/SD/IEEE 1394 disks that Windows reports as fixed count as removable). Such items carry a
    **Removable** badge, and the import summary names the drives with advice to keep them
    connected or copy the photos into a library folder.

11. **Missing files and relink.** Missing items stay listed with their cached thumbnail (dimmed),
    a **Missing** badge and a count in the Library toolbar. **Relink…** (the toolbar for all
    missing photos, the details panel for one) lets the user pick a folder; files with the same
    name (case-insensitively) and size are candidates (found with loop protection), and a path
    changes **only on proof** (`relocate_source`: ids, picks, probe and hash kept): the candidate
    is the missing file's own version (its whole identity equals the catalogued one: moved within
    its volume or a card under another drive letter; nothing is read), or its SHA-256 equals the
    catalogued hash. A file that was never hashed and whose own version is not found cannot be
    confirmed and stays missing; the result reports relinked and unmatched counts (and how many
    could not be verified). Relinked files without a hash are queued for hashing.

12. **Thumbnail cache.** Camera thumbnails are cached on disk (`app::ThumbnailCache`, format v1,
    LRU within 1 GiB) under the source's content hash, subimage, generator `EmbeddedJpeg` v2 and
    size class `Large`: the camera's JPEG chosen for a 1024-pixel long edge and reduced to that
    long edge (`exporter::make_jpeg_thumbnail`: DCT-domain scaling, area averaging, quality 85;
    many cameras embed only a full-size JPEG of several MB besides a tiny thumbnail; a smaller
    preview is kept as it is up to 256 KiB and re-encoded above). Entries are
    written when a file is hashed (the file is still in the system cache) and when a camera
    preview of a hashed item is read. `PreviewService` serves requests up to 1024 pixels from this
    cache before opening the source, so the contact sheet of a reopened library, and of missing
    files, appears without source I/O for every photo whose thumbnail is cached. The cache is
    disposable (Lucent starts without it; thumbnails are then read from the sources again) and is
    keyed by content, so a changed file can never show a stale entry. Lucent's own renders are not
    cached on disk yet.

13. **Threads and fallbacks.** No catalog write, copy, hash or file check runs on the GUI thread:
    imports, maintenance and backups use the Background lane, picks the Quick lane. The initial load
    runs before the window is created. When the catalog cannot be opened (open in another Lucent,
    damaged, not on a local disk, folders unavailable) the session runs on an in-memory
    `SessionLibrary` and the header shows "Library not saved" with the reason; a newer catalog is
    shown read-only with the same notice.

## Consequences

Measured on the reference laptop (Ryzen 5 5500U, 12 GB) on 2026-10-03:

| What | Result |
|---|---|
| Catalog of 10,000 items: open, integrity check and load into memory | 0.11 s (adding them in batches of 500: 0.42 s) |
| Lucent.exe, 5 smoke fixtures (154 MB), first session: window loaded / first thumbnail after the import started | 1.01 s / 0.25 s |
| Same library after a restart: window loaded / all 5 thumbnails from the disk cache, before the background check | 0.67 s / 0.12 s after the window |
| UI thread during import, hashing and the start-up check: largest gap between 5 ms timer ticks | 6 to 25 ms |
| Copy into library folder + verification of the 5 fixtures, then verification re-hash (warm system cache) | 1.3 s for the whole fixture test |
| Thumbnail cache after importing all 34 fixtures (33 camera previews), reduced thumbnails | 5.1 MB in all: mean 152 KiB, largest 286 KiB (39.3 MB, largest 6.1 MiB, when stored verbatim) |

- The release gate "Original preservation" is tested on the smoke fixtures (in place, hashed,
  checked, copied, verified: originals' hashes and identities unchanged) and on synthetic DNGs.
- A content change that keeps size, modification time and file id (a memory-mapped writer, the
  limitation documented for `core::hash_file`) is found only by an explicit verification
  (`verify_hashed`); the start-up check is a stat.
- Relink finds files by name and size: a renamed file is not found (an import of its folder still
  finds it by its identity when it stayed on its volume). Files missing before they were ever
  hashed can be relinked only to their own file version (same volume, file id, size and time).
- A file replaced at the same path (a reused card imported in place) is treated as a new version
  of the same photograph: its items are refreshed and keep their flags (decision 6). With **Copy
  into library folder** the new version is copied and the item moves to the copy.
- FAT and exFAT store local times: after a daylight-saving change card files can look Changed and
  are re-read and hashed again; nothing is lost. A hard-linked file edited through another link
  than the catalogued one becomes a second item and the first stays Changed until it is removed.
- Duplicates are known only once both files are hashed (seconds to minutes after an import).
- Exposure details of reloaded items are empty until schema v2 stores them (request to the catalog
  owner); the shell shows what it knows.
- A second Lucent window works on an unsaved library rather than refusing to start; single-instance
  handling belongs to W5.
- W6 must decide whether recipes cascade with a removed source (catalog `ON DELETE CASCADE`), and
  should key Lucent-render thumbnails by recipe revision in the same cache. W7's job journal
  references sources and items by these ids.

## Alternatives considered

- **Path as identity.** Rejected by the blueprint: moving or renaming files would orphan edits,
  and a replaced file would reuse stale diagnostics.
- **Hashing during the import.** Rejected: hashing a card (tens of GB) before photos appear would
  make import minutes slower. Photos appear after the probe; hashing follows in the background.
- **Recognising earlier copies by origin path and size, or by file id alone.** Rejected after
  review: both lose photos (a reused card, FAT file-id reuse). The original's whole identity is
  recorded with each copy, and a weaker match is confirmed by content.
- **Storing camera thumbnails verbatim.** Rejected after review: full-size embedded JPEGs (up to
  6 MB in the fixture set, 1.1 MB on average) let 256 MiB hold about 225 photos.
- **Merging duplicates or keeping only one.** Rejected by the blueprint ("intentional duplicates can
  remain separate library items").
- **Reading every Library query from SQLite.** Rejected: the preview, export and import services
  call `find()` on hot paths and the UI must never wait on the database; the memory mirror is small
  (0.11 s and a few MB for 10,000 items) and consistent because all writes go through it.
- **Re-probing every file at start-up.** Rejected: it reads every file. A stat per file finds what
  changed; only those are re-probed.
- **Thumbnails as catalog BLOBs.** Rejected: the catalog is small, critical and backed up; thumbnails
  are disposable and belong in the cache directory with their own size limit.
- **Relinking by hashing every file in the chosen folder.** Rejected as the first step: name and
  size select a handful of candidates; the hash then confirms.
- **Storing exposure facts in the support-reasons JSON.** Rejected: that column has a documented
  meaning; a schema change is the right place.
