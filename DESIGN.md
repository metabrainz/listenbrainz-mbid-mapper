# Design

## Goal

Given loosely-typed music metadata — an **artist name**, a **release (album) name**,
and a **recording (track) name** — return the correct MusicBrainz IDs (MBIDs) for
that artist, release, and recording.

The input is messy: typos, punctuation, non-ASCII characters, aliases, wrong
capitalization, extra words. The mapper's job is to match that fuzzy input against
canonical MusicBrainz data quickly and accurately, and hand back stable MBIDs that
downstream systems (e.g. ListenBrainz listen submission) can rely on.

Two things matter most:

- **Precision** — a wrong MBID is worse than no MBID.
- **Speed** — this runs at listen-submission scale, so lookups must be fast and
  memory-bounded.

## Data source

Everything is derived from a full MusicBrainz PostgreSQL database that also has the
ListenBrainz **canonical** data built into it
(`mbid_mapper/manage.py canonical-data --use-mb-conn`). "Canonical" means: for each
logical recording, MusicBrainz has many releases/duplicates, and the canonical step
picks one preferred release/recording so we map to a single consistent answer.

Postgres is only needed at *build* time. The running server never touches Postgres —
it reads from a self-contained local SQLite database plus prebuilt indexes.

## The three build/run stages

The system is a pipeline of small standalone binaries (see `mapper/CMakeLists.txt`).
Each stage produces artifacts consumed by the next.

```
PostgreSQL (full MB + canonical data)
        |
        |  make_mapping
        v
INDEX_DIR/mapping.db   (SQLite: the flat "mapping" table + index_cache)
        |
        |  make_indexes
        v
INDEX_DIR/*            (fuzzy artist index + per-artist recording/release indexes,
                        cached back into mapping.db's index_cache table)
        |
        |  server
        v
HTTP search API  ->  { artist_mbid, release_mbid, recording_mbid, confidence }
```

### 1. `make_mapping` — build the flat mapping table

`mapper/make_mapping.cpp`

- Connects to Postgres via `CANONICAL_MUSICBRAINZ_DATA_CONNECT`.
- Builds custom sort tables (`mapping.format_sort`,
  `mapping.release_group_combined_type_sort`) — hardcoded preference orderings:
  formats rank digital > video > analog; release-group types rank by primary type,
  with bare Single/EP hoisted above albums that carry a secondary type.
- Builds `mapping.canonical_release` in Postgres: **all** releases (deduped by
  release_id), ordered by (combined-type sort, format sort, date, country,
  artist_credit, name, release_id), first excluding Various Artists then only VA. The
  `SERIAL id` assigned in that order **is** the `score` (lower = more canonical) used
  everywhere downstream. Keeping all releases (not one per group) lets bonus tracks on
  alternate editions be matched.
- Streams the canonical data into a local SQLite database `INDEX_DIR/mapping.db`.

The core table is denormalized on purpose so lookups are single-row reads:

```
mapping(
    artist_credit_id, artist_mbids, artist_credit_name, artist_credit_sortname,
    release_id,   release_mbid,   release_name, release_artist_credit_id,
    recording_id, recording_mbid, recording_name,
    score            -- lower score = more canonical/preferred
)
```

It also creates an `index_cache` table (`entity_id`, `index_data` BLOB) used by the
next stage to persist prebuilt indexes, and B-tree indexes on the id columns.

### 2. `make_indexes` — build the fuzzy search indexes

`mapper/make_indexes.cpp`, `artist_index.hpp`, `indexer_thread.hpp`, `recording_index.hpp`

- Builds one global **artist index** over all artist-credit names.
- Builds, per artist credit, a **release index** and a **recording index** (the
  `ReleaseRecordingIndex`, see `defs.hpp`) plus the release↔recording links.
- These per-artist indexes are serialized (via `cereal`) and stored as BLOBs in the
  `index_cache` table, so they can be loaded on demand at query time instead of
  rebuilt.
- Recording index building is multithreaded (`NUM_BUILD_THREADS`). nmslib is
  initialized once in the main thread first because its library init is not
  thread-safe.
- Supports `--skip-artists` and `--force-rebuild` (drops the cache, rebuilds all).

`artist_credit_id` 1/2 ("Various Artists" etc.) are skipped — they have millions of
releases and would blow up the index (`VARIOUS_ARTISTS_ARTIST_CREDIT_ID` in
`defs.hpp`).

### 3. `server` — serve lookups

`mapper/server.cpp`, `search.hpp`, `index_cache.hpp`

Read-only. Opens `mapping.db` (`OPEN_READONLY`), keeps an in-memory `IndexCache` of
per-artist indexes, and answers search requests over HTTP (Crow). The cache is
bounded (`MAX_CACHE_ITEMS`, trimmed by `CACHE_TRIM_COUNT`, cleaned every
`CACHE_CLEANER_DELAY` seconds) so memory stays flat under load.

## The matching algorithm

The search is a **hierarchical, three-level fuzzy match**: artist first, then
release and recording within that artist. This scoping is what makes it both fast
(small per-artist indexes) and precise (no cross-artist confusion).

### Text encoding

`encode.hpp` (`EncodeSearchData`). Every name — indexed or queried — is normalized
the same way so fuzzy comparison is apples-to-apples:

- strip non-word characters / punctuation,
- `unidecode` non-ASCII to a romanized ASCII equivalent,
- lowercase,
- remove spaces/underscores.

So `"Björk"` -> `"bjork"`, `"Godspeed You! Black Emperor"` ->
`"godspeedyoublackemperor"`.

There is also an `encode_string_keep_non_word` fallback used by the "stupid" indexes
(below) for names that are *entirely* punctuation/symbols and would otherwise encode
to an empty string.

### Fuzzy index

`fuzzy_index.hpp`, `tfidf_vectorizer.hpp`. Names are turned into TF-IDF vectors over
character n-grams and searched with **nmslib**'s `simple_inverted_index` on the
`negdotprod_sparse_fast` space (cosine-like similarity via negative dot product). A
search returns the top-N candidates with a confidence in `[0, 1]`.

Each `ReleaseRecordingIndex` actually holds *two* indexes for releases and two for
recordings:

- the normal index (encoded, word characters only), and
- a **"stupid" index** built from `encode_string_keep_non_word`, used only when the
  query encodes to an empty string. It's the fallback for symbol-only names.

### Search flow

The top level is a **finite state machine with backtracking** (`fsm.hpp`,
`MappingSearch`), driving the per-step primitives in `search.hpp`. Crucially it
returns the **first** complete, linkable match found while walking candidates in
confidence order — not a global "best of all candidates" pick. The nesting is
artist → recording → release, and failure at any level backtracks to the next
candidate one level up.

1. **Artist search** — encode the artist name, query the artist indexes. There are
   **three**: `single_artist_index` and `multiple_artist_index` (queried together and
   merged for normal names) and `stupid_artist_index` (fallback for names that encode
   to empty). Keep candidates ≥ `artist_threshold`, sorted by confidence then id. If
   nothing matches and the name wasn't cleaned yet, run the metadata cleaner and retry.
2. Pick the next artist candidate; **load its `ReleaseRecordingIndex`** from the
   `index_cache` (in-memory, backed by the SQLite BLOB).
3. **Recording search** within that artist (≥ `recording_threshold`); walk candidates.
4. **Release branch:** if a release name was given, **release search**
   (≥ `release_threshold`) and, for each candidate, confirm a link to the current
   recording via `find_match` — first linkable release wins. If no release name, look
   up the canonical release for that (artist, recording) (lowest `score`).
5. **Evaluate match** — build the `SearchMatch` with confidence
   `(recording_conf + release_conf) / 2`. If no linkable release/recording, backtrack.
6. **Fetch metadata** for the winning `(release_id, recording_id)` from `mapping` to
   get the MBIDs and canonical names.

Thresholds (`search.hpp`) are currently fixed:

- `artist_threshold = 0.7`
- `release_threshold = 0.3` (lower — some artists have a tiny release vocabulary)
- `recording_threshold = 0.7`

Making these **length-adaptive** (short names need stricter checks) is a known TODO.
Note the first-match-with-backtracking semantics are part of the behavior: a
reimplementation that instead picks the global-highest-confidence match will return
different results.

## Incremental updates

`update.cpp`, `changed_data.hpp`, `mapping_update_fetcher.hpp`,
`mapping_batch_updater.hpp`, `canonical_release_updater.hpp`.

**"Read-only" applies to the `server`, not the file.** `mapping.db` is a normal
writable SQLite file. The `server` opens it `OPEN_READONLY` and never writes; a
*separate* `update` process opens the same file `OPEN_READWRITE` and is the sole
writer. Two different programs, different times.

`update` is a **daemon**: it wakes at :15 past each hour, runs one incremental pass,
and sleeps again (`--dry-run` to preview, `--apply` to write). Each pass:
1. Reads the `last_updated` cursor from `update_metadata` — a MusicBrainz *replication*
   timestamp (`replication_control.last_replication_date`), not wall clock. First run
   with no cursor refuses (a full build must exist).
2. Diffs Postgres against that timestamp to collect changed **release groups** and
   **artist credits** (Various Artists id ≤ 2 filtered out). No changes → just advance
   the timestamp.
3. Updates `canonical_release` for changed release groups.
4. In batches of 5000 artist credits: fetch fresh rows → update the `mapping` table
   → rebuild those artists' fuzzy indexes → update their `index_cache` blobs (in that
   order, so indexes read fresh data).
5. Rebuilds the **entire** global artist index wholesale (nmslib has no incremental
   index update; ~3 min).
6. Saves the new timestamp.

**Open problem — a running server does not reliably see updates.** The in-memory
`IndexCache` has no invalidation: `get`/`add` only add, and eviction happens *only when
the cache is full* (LRU, to reclaim memory). Nothing signals the server that `update`
changed a blob. So a cached artist index is served **stale** until it happens to be
evicted-and-reloaded or the server restarts; the OS page cache adds a second staleness
layer. This is currently implicit/eventual, not a designed contract — see
`REWRITE_SPEC.md` §10 for options (version/generation counter + targeted invalidation,
build-and-swap with reload signal, read-through staleness checks, and/or WAL).

## Key design decisions (and why)

- **SQLite + prebuilt index BLOBs, no runtime Postgres.** Keeps the serving path
  self-contained, fast, and easy to deploy; Postgres is a heavy build-time
  dependency only.
- **Denormalized `mapping` table.** The final metadata fetch is a single indexed row
  read — no joins on the hot path.
- **Per-artist indexes loaded on demand + bounded LRU-style cache.** The full index
  set is far too large to hold in RAM; scoping by artist keeps each index small and
  memory usage flat.
- **Hierarchical artist -> release/recording matching.** Narrows the search space
  massively and prevents matching a track to the wrong artist.
- **Shared TF-IDF encoding for index and query.** Guarantees the fuzzy comparison is
  consistent.
- **"Stupid" fallback indexes.** Handle pathological names that normalize to nothing.

## Matching signals: current and proposed

Everything above describes the **current** system, which matches on **names only**
(artist, release, recording strings). That is the weakest possible signal set and is
the root cause of the hard cases in `PROBLEMS.txt` — they are all string collisions.

The design should treat matching as a set of signals of differing strength. The
guiding principle:

> **Strong identifiers short-circuit the pipeline; weak signals only re-rank
> candidates that already clear the name thresholds. Every extra signal is optional —
> absent signals contribute nothing, so name-only input behaves exactly as today.**

Signals, strongest to weakest:

1. **AcoustID** (proposed, for Picard use) — an acoustic fingerprint of the audio.
   The strongest signal: it identifies the actual audio, independent of any text
   metadata. A fingerprint/lookup resolves to MusicBrainz recording MBID(s) directly.
   Ideal fast path when the caller has the audio (Picard does). Like ISRC it can map
   to multiple recordings, so it's very-high-confidence, not infallible; fall back to
   the rest of the pipeline to disambiguate. Requires either the AcoustID web service
   or a local fingerprint→recording table; heavier to operate than ISRC/duration.

2. **ISRC** (proposed) — a globally unique recording code. If present, do a **direct
   key lookup** instead of fuzzy matching: ISRC → recording MBID, then optionally
   fuzzy-match only the release for the release MBID. Sidesteps all name-collision
   problems. Data is cheap (`musicbrainz.isrc`). Caveat: an ISRC can be attached to
   multiple recordings, so confirm the artist is plausible and fall back on conflict.

3. **Name fuzzy match** (current) — the backbone for the common case where no IDs are
   provided. Stays exactly as designed.

4. **Duration** (proposed) — a **soft tiebreaker**, never a filter. Its value is
   precisely in the low-signal regime where names collide (Intro/Untitled/live vs
   studio vs remix of the same title). Used as a scoring bonus/penalty within a
   tolerance window (durations are noisy: fades, silence, ms-vs-s rounding, bad
   metadata). Data is cheap (`musicbrainz.recording.length`).

5. Other weak optional signals (year, label, track number) could enter the same
   re-ranking step later if useful.

Architecturally this means two additions to the current flow:
- A **fast path** before fuzzy matching: if AcoustID or ISRC is present, resolve
  directly and use fuzzy only as fallback / for the release side.
- A **re-ranking term** inside `find_match` (`search.hpp`), where the score is
  currently `(rec_conf + rel_conf) / 2`. Duration (and other weak signals) adjust
  this score *after* candidates pass the name thresholds — they must not rescue a bad
  name match, only order good ones.

See `REWRITE_SPEC.md` §14 for the concrete proposed schema, scoring, and API changes.

## Performance, scaling & benchmarking

### How it scales

The whole point of the per-artist scoping is that **query cost tracks the size of a
single artist credit, not the size of the whole database.** For scale context,
MusicBrainz is ~2.8M artists / ~5.4M releases / ~38M recordings (2026), and growing.

- **Per lookup** — one global artist-index search, one per-artist index load
  (SQLite `entity_id` lookup, `O(log A)`), then release/recording search *within that
  one artist* (scales with that artist's release/recording count, typically tens to
  low thousands). Growing the total catalog barely changes a typical lookup.
- **Artist search** is an exact sparse inverted-index query (nmslib
  `simple_invindx`): it visits postings for the query's character trigrams, so cost
  grows with the number of artists sharing trigrams with the query, sub-linearly in
  total artist count.
- **Storage** — `mapping.db` grows linearly with (recording × canonical-release)
  rows after dedup (tens of millions of rows, tens of GB). Index blobs grow linearly
  with total but live on disk and are paged in on demand.

### Storage sizing (ESTIMATED — to be verified)

No `mapping.db` has been built on record, so the following are **reasoned estimates,
not measured values.** Verify with the `dbstat` method in `REWRITE_SPEC.md` §12.2.
The file has three cost centers, all roughly linear in catalog size:

- **`mapping` table (row data).** One row per (recording × canonical-release) after
  dedup — order tens of millions of rows. Per row ≈ 5 small integers + 2×36-byte
  MBID UUIDs + `artist_mbids` (~36–74 B) + 4 variable name strings (~20–40 B each) ≈
  **~200–300 bytes/row**. At ~250 B × ~40M rows ≈ **~10 GB**.
- **Five B-tree indexes on `mapping`** (`artist_credit_id`,
  `release_artist_credit_id`, `release_id`, `recording_id`, and composite
  `release_id, recording_id`). Narrow integer keys over tens of millions of rows —
  index overhead here typically rivals the base table: **~several GB combined**.
- **`index_cache` BLOBs** — the wildcard and likely the *largest* component. One
  serialized `ReleaseRecordingIndex` per indexed artist credit (~2.8M minus skipped):
  TF-IDF vocabulary + sparse vectors + nmslib inverted index + full index texts +
  the link map. Plausibly single-digit-to-tens of KB per artist → **low-single-digit
  to low-tens of GB** overall, dominated by prolific artists.

**Total estimate: roughly ~20–50 GB** for a full-scale build, dominated by
`index_cache` and the `mapping` table + its indexes. Widest uncertainty is the
average per-artist blob size — trust only once measured. Note the running `server`
does **not** load all of this into RAM: `mapping.db` is opened read-only and blobs
are paged in on demand behind the bounded `IndexCache`.

### Scaling weak points (all about per-artist cardinality, not total count)

- **Huge artist credits.** The model assumes small per-artist indexes. "Various
  Artists" would produce an enormous single index — hence `artist_credit_id <= 2` is
  hard-skipped (`VARIOUS_ARTISTS_ARTIST_CREDIT_ID`). Any very large credit degrades
  the same way; worst-case cost is bounded by the *largest* artist, not the average.
- **Name-collision density.** The adaptive-K loop in `FuzzyIndex::search` re-searches
  with larger K (up to 1000) when the top results are all perfect-score ties. More
  near-duplicate encoded names = more work; this grows with catalog density.
- **Cache breadth.** The in-memory `IndexCache` holds `MAX_CACHE_ITEMS` (default 50k)
  of ~2.8M possible artist indexes. Skewed/hot-set traffic caches well; traffic
  spread uniformly across all artists thrashes and reloads blobs from SQLite (memory
  stays bounded, latency rises).

### Build-time scaling (offline)

- `make_mapping` is linear in canonical rows, and holds an in-memory dedup set
  (`seen_lookups`) with one entry per unique row — **linear RAM**, multiple GB at
  38M+ rows. Watch this on the build host.
- `make_indexes` is `O(artists)`, embarrassingly parallel (`NUM_BUILD_THREADS`).
- `update` exists to avoid full rebuilds — it reprocesses only changed artist
  credits.

### Why SQLite, and is it fast enough?

Yes — SQLite is a good fit here, not a compromise, and storage size (even ~100 GB on
a 2 TB NVMe with room to grow) is not the constraint. The reason it fits:

- **The serving path is read-only, single-process, no writers** (`search.hpp` /
  `server.cpp` open with `OPEN_READONLY`, one connection per thread). SQLite's known
  weaknesses — concurrent writes and multi-client/network access — are entirely
  absent. Read-only embedded access is SQLite's best case: no network hop, no IPC,
  just in-process B-tree lookups against a memory-mapped file. Many reader threads
  don't block each other.
- **Every hot-path query is a point/range lookup on an indexed integer key returning
  a few rows** — no joins, no scans, no aggregation:
  - `WHERE release_id=? AND recording_id=?` → composite `release_id_recording_id_ndx`
  - `WHERE recording_id=? ORDER BY score LIMIT 1` → `recording_id_ndx`
  - `WHERE artist_credit_id=? AND recording_id=? ORDER BY score LIMIT 1`
  - `index_cache WHERE entity_id=?` → UNIQUE index, one blob row
  A B-tree over tens of millions of rows is only ~4–5 levels deep, and depth grows
  logarithmically — 40M vs 400M rows barely differ. These resolve in microseconds
  once pages are warm.

**SQLite is the cheap part.** The dominant per-query cost is *not* the SQL; it is
(1) deserializing an `index_cache` blob on a cache miss, and (2) the fuzzy search
(TF-IDF + nmslib). Those are what to profile/optimize; SQLite just needs to hand over
bytes quickly, which it does.

**The one concrete gap:** the serving path currently sets **no PRAGMAs** — the 8 GB
`mmap_size` / 2 GB `cache_size` tuning only exists in the *build* path
(`make_mapping`). For serving, the server should also set a large `mmap_size` (and
rely on the OS page cache holding the hot working set) so warm reads are
memory-speed. This is a cheap, high-value change. Recommended on the read-only
connection(s): `PRAGMA mmap_size` (large, e.g. covering the DB or several GB),
`PRAGMA cache_size` (negative = KB), `PRAGMA query_only=ON`, `PRAGMA temp_store=MEMORY`.

**Caveats to keep in mind (none are SQLite throughput problems):**
- Cold-cache first access is a disk read (~tens of µs on NVMe); mitigated by
  `mmap_size` + enough RAM for the page cache.
- Blob deserialization on `IndexCache` miss is the real latency spike and scales with
  the artist's index size — that's why the bounded cache exists. Skewed (realistic)
  traffic keeps hot artists cached; uniform-random-over-all-artists traffic pays
  deserialization constantly. A cache/algorithm concern, not a DB one.
- Verify the planner uses `recording_id_ndx` for the `ORDER BY score LIMIT 1` queries
  rather than sorting a large group for recordings with many releases (`EXPLAIN QUERY
  PLAN`); expected fine, worth confirming.

SQLite would only become the bottleneck if the access pattern changed to something it
is bad at — heavy concurrent writes, or full-table/analytical scans on the hot path —
and this design does neither.


### Benchmarking

There is a **Locust load-test harness** in `load_test/` (`locustfile.py`), but **no
benchmark results are recorded anywhere in the repo** — nothing has been captured for
latency, throughput, or resource use.

The harness:
- Reads test cases from a JSONL listens file
  (`TEST_DATA_FILE`, default `../listen_test_data/listens.jsonl`), format
  `{"track_metadata": {"artist_name","release_name","track_name"}}`; falls back to a
  handful of hard-coded famous tracks if the file is missing.
- Two user classes: `MappingUser` (0.5–2 s think time) and `HighLoadUser` (0.1–0.5 s,
  stress). Task weights: with-release 10, without-release 5, health 1.
- Run: `locust -f locustfile.py --host http://localhost:5000 --headless --users 100
  --spawn-rate 10 --run-time 5m --csv=results/load_test`.

**Caveat / known drift:** the harness targets `GET /mapping/lookup` and expects
`503` (not-ready) and `404` (no match) statuses, but the current `server.cpp` only
implements `/`, `/docs`, `/supported`, and `/` returns HTML rather than a JSON/status
API. The load test appears to target an intended API that the server does not
currently expose (see `REWRITE_SPEC.md` §8). Reconcile this before trusting load-test
runs.

Suggested things to actually measure (none captured yet): p50/p95/p99 lookup latency
warm vs cold cache, throughput vs `NUM_THREADS`, cache hit rate vs `MAX_CACHE_ITEMS`
under a realistic (skewed) listen distribution, `make_mapping`/`make_indexes` wall
time and peak RAM at full scale.

## Known issues / caveats

- Thresholds are static; short strings are under-checked (`search.hpp` TODO,
  `PROBLEMS.txt`).
- Release/debug builds could disagree on tie-breaking due to floating-point
  precision in nmslib's inverted index — see `PRECISION_FIXES.txt` for the fix and
  rationale.
- Empty/degenerate source rows and per-thread crash isolation are open items
  (`TODO.txt`).
- **Update coherence:** the read-only `server` has no way to observe `update`'s writes
  deterministically — `IndexCache` has no invalidation, so updated artist indexes are
  served stale until eviction or restart (see Incremental updates above and
  `REWRITE_SPEC.md` §10).
- **Canonical-release build vs update inconsistency:** the full build
  (`canonical_release.hpp`) keeps *all* releases per group (dedup by release_id), but
  the incremental updater (`canonical_release_updater.hpp`) keeps only *one* per group
  (`ROW_NUMBER()=1`). So incrementally-updated release groups end up shaped differently
  than freshly-built ones. A real divergence to resolve (see `REWRITE_SPEC.md` §10).

## Component map

| File | Role |
|------|------|
| `make_mapping.cpp` / `.hpp` | Build `mapping.db` from Postgres canonical data |
| `make_indexes.cpp` | Build artist + per-artist recording/release indexes |
| `server.cpp` | HTTP search server (read-only) |
| `update.cpp` | Incremental updates from Postgres |
| `explore.cpp` | Interactive/CLI exploration + debugging (readline) |
| `test.cpp`, `test_cases.hpp` | Tests (Catch2) |
| `fsm.hpp` | Top-level search orchestration — the `MappingSearch` state machine |
| `search.hpp` | Per-step search primitives (artist/release/recording search, find_match, fetch) + thresholds |
| `encode.hpp` | Text normalization/encoding |
| `fuzzy_index.hpp`, `tfidf_vectorizer.*` | TF-IDF + nmslib fuzzy index |
| `artist_index.hpp` | Global artist-credit index |
| `recording_index.hpp`, `indexer_thread.hpp` | Per-artist recording/release indexes |
| `index_cache.hpp` | In-memory bounded cache of loaded indexes |
| `defs.hpp` | Shared structs (`ReleaseRecordingIndex`, `SearchMatch`, constants) |
| `levenshtein.*`, `custom_sorts.hpp` | Edit-distance + sort helpers |
| `canonical_release*.hpp`, `changed_data.hpp` | Canonical release derivation + change tracking |
