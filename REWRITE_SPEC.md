# Rust Reimplementation Specification

This document specifies exactly what a from-scratch Rust reimplementation of the
MBID mapper must reproduce. It is the *contract*: match these and the Rust version is
correct. Read `DESIGN.md` first for the conceptual overview; this document assumes it.

Everything here is extracted from the current C++ source. Where behavior is subtle
(floating point, encoding, tie-breaking) it is called out explicitly, because those
are exactly the places a reimplementation silently diverges.

---

## 0. Scope decisions (decide these first)

These are the make-or-break architectural choices for the port. Recommended answers
given; change them consciously, not by accident.

1. **Index blob format: DO NOT reproduce.** The current per-artist indexes are
   `cereal` binary blobs wrapping nmslib's `SerializeIndex` output (see
   `FuzzyIndex::save`/`load` in `fuzzy_index.hpp`). This format is C++/nmslib
   specific and not worth reproducing in Rust. **Treat indexes as a rebuildable
   cache.** Keep `mapping.db`'s data tables compatible; regenerate the `index_cache`
   blobs with the Rust format. Consequence: after a Rust port you re-run the
   index-build step; you cannot reuse existing blobs.

2. **`mapping.db` data tables: DO reproduce exactly** (schema in §2). This lets the
   Rust `make_mapping` and the Rust `make_indexes`/`server` interoperate, and lets
   you diff against a C++-generated DB.

3. **Similarity engine: reproduce the *math*, not the library.** The current code
   uses nmslib `simple_invindx` over `negdotprod_sparse_fast`. This is an **exact**
   (brute-force inverted index) cosine/dot-product search, **not** an approximate ANN.
   A Rust port should implement an exact sparse dot-product top-K, not swap in an ANN
   crate (HNSW etc.) — ANN would change recall and break the golden tests. See §4.

4. **Fidelity target.** Define "correct" as: for the golden test set (§8) the Rust
   version returns the same MBIDs as the C++ version. Bit-identical confidence scores
   are NOT required; ranking equivalence on the test set IS.

---

## 1. Components & CLI contract

Six binaries today (`mapper/CMakeLists.txt`). A Rust port may use one binary with
subcommands, but must preserve these behaviors:

| Binary | Purpose | Args | Required env |
|--------|---------|------|--------------|
| `make_mapping` | Build `mapping.db` from Postgres | none | `INDEX_DIR`, `CANONICAL_MUSICBRAINZ_DATA_CONNECT` |
| `make_indexes` | Build fuzzy indexes | `--skip-artists`, `--force-rebuild` | `INDEX_DIR`; `CANONICAL_MUSICBRAINZ_DATA_CONNECT` unless `--skip-artists`; `NUM_BUILD_THREADS` (opt) |
| `server` | HTTP search | none | `INDEX_DIR`; optional: `HOST`,`PORT`,`TEMPLATE_DIR`,`NUM_THREADS`,`MAX_CACHE_ITEMS`,`CACHE_TRIM_COUNT`,`CACHE_CLEANER_DELAY`,`TIMEOUT` |
| `update` | Incremental update from Postgres | (see §7) | `INDEX_DIR`, `CANONICAL_MUSICBRAINZ_DATA_CONNECT` |
| `explore` | Interactive CLI debugging | readline REPL | `INDEX_DIR` |
| `test` | Test runner | Catch2 | — |

Behavioral rules to preserve:
- `--skip-artists` + `--force-rebuild` together is an error.
- `make_mapping` refuses to run if `INDEX_DIR/mapping.db` already exists.
- All binaries load a `.env` file first, then real env vars override it.
- `INDEX_DIR` and `CANONICAL_MUSICBRAINZ_DATA_CONNECT` empty/unset → error + usage.

### Env var semantics (defaults from `.env-local-dev` / `server.cpp`)
```
CANONICAL_MUSICBRAINZ_DATA_CONNECT  libpq conn string, e.g.
    "dbname=musicbrainz_db user=musicbrainz host=localhost port=5432 password=musicbrainz"
INDEX_DIR            dir holding mapping.db and index files (server default "/data")
HOST                 default 0.0.0.0
PORT                 default 5000
TEMPLATE_DIR         default "/mapper/templates"
NUM_THREADS          server workers, 0 = all cores
NUM_BUILD_THREADS    index build threads, 0 = all cores (fallback 4)
MAX_CACHE_ITEMS      default 50000
CACHE_TRIM_COUNT     default 10000  (trim to MAX-TRIM below max)
CACHE_CLEANER_DELAY  seconds, default 60 (30 in local .env)
TIMEOUT              server conn timeout seconds, default 30
```

---

## 2. Data model — SQLite `mapping.db` (reproduce exactly)

Created by `make_mapping` (`make_mapping.cpp`). Two tables built at mapping time,
plus indexes.

### Table `mapping`
```sql
CREATE TABLE mapping (
    artist_credit_id         INTEGER NOT NULL,
    artist_mbids             TEXT    NOT NULL,   -- comma-separated UUIDs (see §3)
    artist_credit_name       TEXT    NOT NULL,
    artist_credit_sortname   TEXT,
    release_id               INTEGER NOT NULL,
    release_mbid             TEXT    NOT NULL,
    release_artist_credit_id INTEGER NOT NULL,
    release_name             TEXT,
    recording_id             INTEGER NOT NULL,
    recording_mbid           TEXT    NOT NULL,
    recording_name           TEXT,
    score                    INTEGER NOT NULL    -- lower = more canonical
);
CREATE INDEX artist_credit_id_ndx          ON mapping(artist_credit_id);
CREATE INDEX release_artist_credit_id_ndx  ON mapping(release_artist_credit_id);
CREATE INDEX release_id_ndx                ON mapping(release_id);
CREATE INDEX recording_id_ndx             ON mapping(recording_id);
CREATE INDEX release_id_recording_id_ndx  ON mapping(release_id, recording_id);
```

### Table `index_cache`
```sql
CREATE TABLE index_cache (
    entity_id  INTEGER NOT NULL UNIQUE,   -- artist_credit_id
    index_data BLOB    NOT NULL            -- serialized ReleaseRecordingIndex
);
CREATE INDEX entity_id_idx ON index_cache(entity_id);
```
`index_data` format is Rust's choice (§0 point 1). It must round-trip a
`ReleaseRecordingIndex` (§6).

### Sentinel values
- Standalone recordings (no release) use `release_id = 4294967295`
  (`0xFFFFFFFF`), `release_artist_credit_id = 4294967295`, `release_mbid = ''`,
  `release_name = ''`, `score = 4294967295`.
- `artist_credit_id` 1 and 2 are special ("Various Artists" etc.);
  `VARIOUS_ARTISTS_ARTIST_CREDIT_ID = 2`. Index building must skip
  `artist_credit_id <= 2` (queries use `WHERE artist_credit_id > 2`).

### `update_metadata` table
Verified schema (`changed_data.hpp`):
```sql
CREATE TABLE IF NOT EXISTS update_metadata (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
-- row: key='last_updated', value = MusicBrainz replication timestamp (see §10)
```
Seeded by `make_mapping` (`initialize_update_timestamp`) with
`musicbrainz.replication_control.last_replication_date`. Used by `update` as the
incremental cursor (§10).

### SQLite PRAGMAs used for bulk build (performance, not correctness)
```
synchronous=OFF, journal_mode=OFF, cache_size=-2097152 (2GB),
temp_store=MEMORY, mmap_size=8589934592 (8GB), threads=16
```
Commit every 500,000 rows during bulk insert.

---

## 3. Canonical data extraction (Postgres → mapping rows)

Source: `canonical_musicbrainz_data.hpp`. This is the business logic that defines
*what* gets mapped. Reproduce precisely.

### Prerequisites in Postgres
- Full MusicBrainz schema (`musicbrainz.*`).
- ListenBrainz canonical data built: table `mapping.canonical_release` must exist
  (produced by `mbid_mapper/manage.py canonical-data --use-mb-conn`).

### Two source queries

**Query 1 — recordings with releases** (one row per recording×release):
```sql
SELECT DISTINCT
       ac.id AS artist_credit_id,
       s.artist_mbids,
       ac.name AS artist_credit_name,
       s.artist_sortnames,
       rl.id AS release_id,
       rl.gid::TEXT AS release_mbid,
       rl.artist_credit AS release_artist_credit_id,
       rl.name AS release_name,
       r.id AS recording_id,
       r.gid::TEXT AS recording_mbid,
       r.name AS recording_name,
       cr.id AS score
  FROM musicbrainz.recording r
  JOIN musicbrainz.artist_credit ac ON r.artist_credit = ac.id
  JOIN musicbrainz.track t          ON t.recording = r.id
  JOIN musicbrainz.medium m         ON m.id = t.medium
  JOIN musicbrainz.release rl       ON rl.id = m.release
  JOIN mapping.canonical_release cr ON rl.id = cr.release
  JOIN (SELECT artist_credit,
               array_agg(a.gid       ORDER BY position) AS artist_mbids,
               array_agg(a.sort_name ORDER BY position) AS artist_sortnames
          FROM musicbrainz.artist_credit_name acn2
          JOIN musicbrainz.artist a ON acn2.artist = a.id
      GROUP BY acn2.artist_credit) s ON ac.id = s.artist_credit
 WHERE 1=1
 ORDER BY cr.id, ac.id;      -- score ascending: best canonical first
```

**Query 2 — standalone recordings** (no track/release):
```sql
SELECT DISTINCT
       ac.id AS artist_credit_id,
       s.artist_mbids,
       ac.name AS artist_credit_name,
       s.artist_sortnames,
       4294967295 AS release_id,
       ''::TEXT   AS release_mbid,
       4294967295 AS release_artist_credit_id,
       ''         AS release_name,
       r.id AS recording_id,
       r.gid::TEXT AS recording_mbid,
       r.name AS recording_name,
       4294967295 AS score
  FROM musicbrainz.recording r
  JOIN musicbrainz.artist_credit ac ON r.artist_credit = ac.id
  JOIN (SELECT artist_credit,
               array_agg(a.gid       ORDER BY position) AS artist_mbids,
               array_agg(a.sort_name ORDER BY position) AS artist_sortnames
          FROM musicbrainz.artist_credit_name acn2
          JOIN musicbrainz.artist a ON acn2.artist = a.id
      GROUP BY acn2.artist_credit) s ON ac.id = s.artist_credit
 WHERE NOT EXISTS (SELECT 1 FROM musicbrainz.track t WHERE t.recording = r.id)
 ORDER BY ac.id;
```

- For incremental update, an `AND <artist_credit_filter>` is appended to the WHERE
  (e.g. `ac.id = ANY($1::int[])`).
- A minimal test dataset filter is `ac.id IN (1160983, 49627, 65, 21238)`.
- Read via server-side cursors, `FETCH 10000` at a time.

### Row post-processing (exact)
Per row (`process_rows`):
1. **Deduplicate by `combined_lookup`.** Since rows arrive ordered by `score` asc,
   the first occurrence of a given `combined_lookup` wins (lowest score kept); later
   duplicates are dropped. `combined_lookup` =
   `unidecode(artist_credit_name + recording_name + release_name)`, then keep only
   `[A-Za-z0-9_]`, then lowercase. (Concatenation order: artist + recording +
   release. See `compute_combined_lookup`.)
2. **`artist_mbids`**: Postgres array text `{uuid1,uuid2}` → strip the braces → store
   the inner comma-separated string verbatim (so the SQLite column is
   `uuid1,uuid2`).
3. **`artist_credit_sortname`**: take the *first* element of the `artist_sortnames`
   Postgres array, handling quoted elements and `""`-escaped quotes.
4. Integers parsed with `strtoll`; empty → 0.

### Postgres side-effect table
`make_mapping` also creates, in Postgres,
`mapping.mapper_canonical_musicbrainz_data (artist_credit_id INTEGER)` as an
`UNLOGGED` table, bulk-loaded (`COPY`) with the DISTINCT `artist_credit_id`s that
made it into `mapping.db`, then indexed + `ANALYZE`d. The artist-index build (§5)
joins against this to fetch artist names/aliases only for credits that are actually
in the mapping.

### Build prerequisites in Postgres (VERIFIED — run before the §3 mapping queries)

**Custom sort tables** (`custom_sorts.hpp`, `create_custom_sort_tables`, one
transaction). All are hardcoded lookup tables in the `mapping` schema:
- `format_sort(format INTEGER, sort INTEGER)` — medium-format preference. Sort order
  is **digital formats first, then video, then analog**, each group in a fixed
  format-id order (e.g. CD=1 ranks first; Digital Media; then DVD/Blu-ray…; then
  Vinyl/Cassette/Shellac…). Full id lists in `custom_sorts.hpp`
  (`DIGITAL_FORMATS`/`VIDEO_FORMATS`/`ANALOG_FORMATS`).
- `release_group_combined_type_sort(sort, primary_type, secondary_type)` — the
  Cartesian product of primary types {Album1, Single2, EP3, Other11, Broadcast12,
  NULL} × secondary types {NULL, Soundtrack2, Mixtape9, Remix7, Audiobook5, …},
  numbered in that nested order, **except Single(NULL) is hoisted to position 1 and
  EP(NULL) to position 2** so bare Singles/EPs outrank Albums that carry a secondary
  type. Reproduce this exact ordering — it drives canonical-release ranking.
- `release_group_secondary_type_sort(sort, secondary_type)` — secondary types in the
  listed order (built but the combined table is what the ranking joins).

**`mapping.canonical_release` table** (`canonical_release.hpp`, `CanonicalRelease`):
```sql
CREATE TABLE mapping.canonical_release (
    id           SERIAL,          -- THIS is the `score` (lower = more canonical)
    release      INTEGER NOT NULL,
    release_mbid UUID    NOT NULL
);
-- indexes on (release) and (id)
```
Built by inserting **ALL releases** (not one per group), **deduplicated by
`release_id` only**, ordered by:
```
ORDER BY release_group_combined_type_sort.sort NULLS LAST,
         format_sort.sort NULLS LAST,
         to_date(date_year || '-' || COALESCE(date_month,12) || '-' ||
                 COALESCE(date_day,28), 'YYYY-MM-DD'),
         country, release_group.artist_credit, release_group.name, release.id
```
Run as **two passes**: first `WHERE rg.artist_credit != 1` (exclude Various Artists),
then `WHERE rg.artist_credit = 1` (only VA), appending rows in that order. The
insertion order defines the `SERIAL id`, i.e. the `score` used everywhere downstream
(the `mapping.score` column is this `cr.id`). Keeping every release per group (not
just the canonical one) is intentional so bonus tracks on non-standard editions are
findable. **NB:** the incremental updater diverges from this — see §10.

---

## 4. Text encoding (exact — reproduce byte-for-byte where possible)

Source: `encode.hpp` (`EncodeSearchData`). Both index-time and query-time text go
through this. Getting it wrong changes every match.

### `encode_string(text)` — the normal path
1. If empty → return empty.
2. Remove all runs of non-word chars: regex `[^\w]+` → `""` (global). `\w` here is
   PCRE2 default = `[A-Za-z0-9_]` (ASCII; the pattern uses no Unicode modifier).
3. `unidecode` the result (transliterate non-ASCII → ASCII).
4. Lowercase (ASCII `tolower`).
5. Remove runs of spaces/underscores: regex `[ _]+` → `""` (global). (unidecode can
   introduce spaces.)

Examples: `"Björk"` → `"bjork"`, `"Godspeed You! Black Emperor"` →
`"godspeedyoublackemperor"`.

### `encode_string_keep_non_word(text)` — the "stupid" path
Used only when `encode_string` yields empty (name is all symbols):
1. If empty → return empty.
2. Remove all whitespace: regex `[\s]+` → `""` (global).
3. Lowercase.
(No unidecode; punctuation kept.)

### Unidecode caveat (must document in the port)
The C++ `unidecode` library's exact transliteration table will **not** match a Rust
crate (`deunicode`, `any_ascii`, etc.) byte-for-byte for all inputs. This affects
non-ASCII names. Options, in order of preference:
- Port/embed the same unidecode data table used here (`mapper/deps/unidecode`) so
  output matches exactly. **Recommended** for fidelity.
- Use a Rust crate and accept small divergences on non-Latin text; the golden test
  set (§8) must then tolerate those or exclude them.

### `reduce_aliases(aliases, encoded_name)`
Encode each alias with `encode_string`; drop empties and any equal to the primary
`encoded_name`; return the set (dedup). Used when building artist index text.

---

## 5. TF-IDF vectorizer (exact algorithm)

Source: `tfidf_vectorizer.cpp/.hpp`. Character-trigram TF-IDF, sklearn-style. This is
fully specified below; reproduce exactly.

Constructor used in code: `TfIdfVectorizer(binary=false, lowercase=false)` in
`FuzzyIndex` (note: lowercase is already done by the encoder, so the vectorizer's
lowercase flag is off). Defaults: `use_idf=true`, `max_features=-1` (all),
`norm="l2"`, `sublinear_tf=false`.

### Tokenization — character 3-grams
For a document string `d`:
- If `len(d) < 3`: right-pad with spaces to length 3, emit that single token.
- Else: emit every length-3 substring `d[i..i+3]` for `i in 0..len-2` (sliding
  window, overlapping, includes duplicates).

Note: tokenization operates on **bytes** of the encoded (ASCII) string. Index and
query strings are truncated to `MAX_ENCODED_STRING_LENGTH = 30` bytes before
vectorizing (§6).

### Vocabulary
Union of all tokens across the fitted documents, assigned ids in **sorted (set)
order** — deterministic. (Vocabulary order only affects internal indices, not
results, but keep it deterministic.)

### IDF (sklearn "smooth_idf" style)
For token `t` with document frequency `df(t)` over `N` documents:
```
idf(t) = ln( (N + 1) / (df(t) + 1) ) + 1
```

### TF
Per document, term count / number of tokens in that document (i.e. relative
frequency), since `binary=false` and `sublinear_tf=false`. (If `binary` were true, tf
∈ {0,1}; not used here.)

### Weight & normalize
- Cell weight = `tf(t,d) * idf(t)` (since `use_idf=true`).
- Then L2-normalize each document vector (divide by its Euclidean norm; skip if norm
  is 0). `norm="l2"`.

Result: a sparse vector per document. At query time, `transform` (not `fit`) is used:
tokens not in the fitted vocabulary/idf are skipped.

### Determinism note
Because vectors are L2-normalized, a perfect match scores dot product = 1.0 in exact
arithmetic. Floating-point reordering can make it 0.9999999 — the search code
compensates with an epsilon (§4 of `PRECISION_FIXES.txt`, and §6 below). The Rust
port should compute in `f32` to match, and apply the same epsilon.

---

## 6. Fuzzy index & search (exact algorithm)

Source: `fuzzy_index.hpp`, `defs.hpp`.

### Constants
```
MAX_ENCODED_STRING_LENGTH = 30
NUM_FUZZY_SEARCH_RESULTS  = 10   (base K)
PERFECT_MATCH_EPSILON     = 1e-6 (f32)
max_k                     = 1000
```

### Build (`FuzzyIndex::build(ids, texts)`)
- `index_ids = ids`, `index_texts = texts` (full, untruncated — kept for long-query
  rescoring).
- Vectorize `texts` **truncated to 30 bytes each** via `fit_transform`.
- Build an exact sparse inverted index over the resulting vectors using
  negative-dot-product distance (i.e. similarity = dot product; distance = −sim).

### Search (`FuzzyIndex::search(query, min_confidence, source) -> Vec<IndexResult>`)
1. Truncate `query` to 30 bytes, `transform` (not fit) into a sparse vector.
2. Adaptive-K KNN loop starting at `k = 10`:
   - Run exact top-`k` by dot product.
   - Collect results where `sim >= min_confidence`. `confidence = sim` (the negated
     top distance). Track `has_long` if the matched index text was longer than 30
     bytes (it was truncated at index time).
   - If any result has `sim < 1.0 - 1e-6` (a non-perfect match), OR there are no
     results, OR `results.len() < k` (k exceeded index size) → stop.
   - Otherwise all top-k are perfect ties (score 1.0): `k += 10` and repeat, up to
     `max_k = 1000`. (This is the fix for equal-score matches being truncated — see
     `PRECISION_FIXES.txt`.)
3. Reverse results (KNN queue pops worst-first; reversal yields best-first-ish; final
   ordering is re-sorted by callers, §7).
4. **Long-query rescoring**: if `len(query) > 30` OR `has_long`, call
   `post_process_long_query`.

### `post_process_long_query(query, results, min_confidence, source)`
For each candidate, compute **Levenshtein** edit distance between the *full* query and
the *full* index text (`levenshtein.cpp`, `lev_edit_distance`, unit cost):
```
if dist == 0:  conf = 1.0
else:          conf = 1.0 - |dist / len(query)|     // note: over query length
keep if conf >= min_confidence
```
This replaces the truncated-TF-IDF confidence for long strings. `IndexResult` =
`{ id, result_index, confidence, source }` where `source` is a char tag
(`'l'`,`'c'`,`'t'`,`'s'`,`'r'`) identifying which index produced it.

### `ReleaseRecordingIndex` (per artist credit) — `defs.hpp`
Holds:
- `recording_index`, `release_index` — normal `FuzzyIndex`es.
- `stupid_recording_index`, `stupid_release_index` — fallback indexes built from
  `encode_string_keep_non_word` text, used when a query encodes to empty.
- `links: map<recording_result_index, Vec<ReleaseRecordingLink>>` where
  `ReleaseRecordingLink = { release_index, release_id, rank, recording_index,
  recording_id }`. `rank` orders releases for a recording (lower = preferred).
This is the object serialized into `index_cache.index_data` (Rust format, §0).

---

## 7. Search pipeline (server request → SearchMatch)

**VERIFIED — it is a finite state machine with backtracking, not a
collect-all-then-pick-best loop.** Source: `fsm.hpp` (`MappingSearch`, the top-level
orchestrator) + `search.hpp` (`SearchFunctions`, the per-step primitives). Thresholds
(`search.hpp`):
```
artist_threshold    = 0.7
release_threshold   = 0.3   // low: some artists have tiny release vocab
recording_threshold = 0.7
```

Input: `artist_credit_name` (required), `release_name` (optional), `recording_name`
(required).

### Semantics that matter for the port
- **Returns the FIRST complete, linkable match found**, walking candidates in
  confidence-sorted order — NOT the global maximum-confidence match. Ordering of
  candidates is therefore part of the contract (confidence desc, then id asc).
- **Nested backtracking:** artist candidates (outer) → recording candidates (middle)
  → release candidates (inner). Failure at any level falls back to the next candidate
  one level up. Concretely, the FSM transitions:
  `select_artist_match → recording_search → select_recording_match →
  has_release_argument → release_search → select_release_match → evaluate_match →
  success_fetch_metadata`, with `doesnt_meet_threshold`/`no_matches` edges looping
  back up (e.g. recording below threshold → back to next artist).

### Steps (as FSM states)
1. **Artist name check** (`do_artist_name_check`): `encode_string`; if empty, fall
   back to `encode_string_keep_non_word` and take the "stupid" artist path.
2. **Artist search** (`do_artist_search`): query **two** indexes —
   `single_artist_index` (source `'s'`) and `multiple_artist_index` (source `'m'`) —
   merge, sort by confidence desc / id asc, keep ≥ `artist_threshold`. (Stupid path:
   `stupid_artist_index`, threshold 0.7.) These are three distinct artist indexes,
   not one (see §9).
3. **Clean-and-retry** (`do_clean_artist_name`): if artist search finds nothing and
   the name was not yet cleaned, run `lb_matching_tools::MetadataCleaner::clean_artist`
   and retry artist search once.
4. **Select artist match** (`do_select_artist_match`): advance to next artist
   candidate ≥ threshold; on switch, invalidate recording/release matches and release
   the previous artist's cached index. Below threshold / exhausted → fail.
5. **Recording search/select** (`do_recording_search`, `do_select_recording_match`):
   load the artist's `ReleaseRecordingIndex` via `IndexCache` (load-on-demand from the
   SQLite BLOB; another thread may have inserted first — use returned pointer;
   refcounted). Search recordings (normal source `'c'`, stupid `'s'`), sort, walk
   candidates ≥ `recording_threshold`. Exhausted → back to next artist.
6. **Release branch** (`do_has_release_argument`):
   - If a release name was given → `do_release_search` (normal source `'l'`, stupid
     `'t'`), then `do_select_release_match`: walk release candidates ≥
     `release_threshold`, and for each call `find_match` to confirm a link to the
     current recording; first linkable release wins. No linkable release → back to
     next recording.
   - If no release name → `do_lookup_canonical_release`: `get_canonical_release_id`
     (the release with lowest `score` for that artist+recording, source `'r'`).
7. **Evaluate match** (`do_evaluate_match` / `find_match`): build the `SearchMatch`
   with `score = (rec_conf + rel_conf) / 2`. Link resolution by release source:
   - `'r'`/`'t'`: binary-search link vector by `release_id`.
   - `'l'`: among links with that `release_index`, pick lowest `rank`.
   - Stupid recording (`'s'`): translate its `result_index` into the normal index by
     matching `recording_id` first.
8. **Fetch metadata** (`do_success_fetch_metadata`):
   - With release: `SELECT artist_mbids, artist_credit_name, release_mbid,
     release_name, recording_mbid, recording_name FROM mapping WHERE release_id=? AND
     recording_id=?`.
   - Without release: same columns `WHERE recording_id=? ORDER BY score LIMIT 1`.
   - `artist_mbids` split on `,` into a list.

Output (`SearchMatch`): `artist_credit_id`, `artist_credit_mbids: [uuid]`,
`artist_credit_name`, `release_mbid`, `release_name`, `recording_mbid`,
`recording_name`, `confidence`.

**Port note:** a Rust reimplementation does not need to copy the FSM's function-pointer
structure, but it MUST preserve the *backtracking order and first-match semantics* —
they determine which result is returned when multiple candidates exist. A naive
"best confidence overall" implementation will return different results than the C++
version and fail the golden set.

---

## 8. HTTP API contract (server)

Framework today: Crow (C++). Routes (`server.cpp`):

- `GET /` — main search + HTML result page. **Query params**:
  `artist_credit_name` (required), `recording_name` (required), `release_name`
  (optional). Search runs only if `artist_credit_name` AND `recording_name` present.
  Renders `index.html` (mustache) with, on match: artist credit name, artist MBID
  list, artist_credit_id, release name/MBID, recording name/MBID, confidence
  (`%.2f`), and `search_time_ms` (`%.1f`). On no match: `has_match=false`.
- `GET /docs` — renders `docs.html`.
- `GET /supported` — renders `supported.html`.

Status codes tracked: 200/400/404/500/503. `503` while `g_ready == false` (indexes
still loading). Templates live in `TEMPLATE_DIR`.

**Reimplementation note:** the current API is HTML-form oriented (params via query
string, HTML response). If the Rust port should expose JSON, that is a *new*
contract — define it explicitly (suggest `GET /1/search?artist=&release=&recording=`
→ JSON with the `SearchMatch` fields). Do not assume a JSON API exists today; it does
not.

**Known drift — load test vs server:** the Locust harness (`load_test/locustfile.py`,
§12) calls `GET /mapping/lookup?artist_credit_name=&recording_name=&release_name=`
and treats `200`/`404` as success and `503` as not-ready. The current `server.cpp`
does **not** implement `/mapping/lookup` (only `/`, `/docs`, `/supported`) and `/`
returns HTML, not a status/JSON API. So the harness targets an *intended* API that
the server has not (yet) exposed. If the Rust port defines a JSON API, `GET
/mapping/lookup` with those three params and `200`/`404`/`503` semantics is the
already-implied contract to adopt — it will make the existing load test work as-is.

Concurrency model to preserve: worker thread pool (`NUM_THREADS`), one
`MappingSearch` per thread (thread-local), a single shared bounded `IndexCache`, a
single shared read-only artist index, one read-only SQLite connection per thread
(lazy). A background cache-cleaner thread trims the cache to
`MAX_CACHE_ITEMS - CACHE_TRIM_COUNT` every `CACHE_CLEANER_DELAY` seconds.

**Serving-side SQLite tuning (fix in the port):** the current C++ server opens
`OPEN_READONLY` but sets **no PRAGMAs** — the large `mmap_size`/`cache_size` tuning
only exists in the build path (`make_mapping`). The Rust server should configure each
read-only connection for warm-read performance:
`PRAGMA mmap_size=<large>`, `PRAGMA cache_size=<negative KB>`,
`PRAGMA query_only=ON`, `PRAGMA temp_store=MEMORY`. The hot path is point/range
lookups on indexed integer keys (§7); SQLite is the cheap part — per-query cost is
dominated by `index_cache` blob deserialization on cache miss and the fuzzy search,
not the SQL. Also confirm (`EXPLAIN QUERY PLAN`) the `... ORDER BY score LIMIT 1`
queries use `recording_id_ndx` rather than sorting a large group.

---

## 9. Artist index (global)

Source: `artist_index.hpp`, built in `make_indexes` before recording indexes.
**VERIFIED.** Three `FuzzyIndex`es, persisted as negative-`entity_id` rows in
`index_cache`:
- `single_artist_index`  — `entity_id = -1` (`SINGLE_ARTIST_INDEX_ENTITY_ID`)
- `multiple_artist_index` — `entity_id = -2`
- `stupid_artist_index`  — `entity_id = -3`

Normal artist search queries single (source `'s'`) + multiple (source `'m'`) and
merges; the stupid index is the fallback for names that encode to empty.

Population SQL (all CTE-join to `mapping.mapper_canonical_musicbrainz_data` so only
mapped credits are indexed; all filter `a.id > 1` to exclude the special artist):
- **single**: `artist_credit` where `artist_count = 1`, selecting `ac.name`,
  `array_agg(sort_name ORDER BY position)`, `array_agg(join_phrase ORDER BY position)`.
- **multiple**: identical but `artist_count > 1`.
- **aliases**: `musicbrainz.artist_alias.name` for `artist_count = 1` credits — added
  as extra searchable texts for the single index.

Text handling:
- Names/aliases are encoded via `encode_string`, then deduplicated
  (`encode_and_dedup_artist_names`): if a name encodes to empty it is kept raw
  (feeds the stupid index), otherwise the encoded form is used; stored in a `set`
  (dedup).
- `is_transliterated(name, sortname)`: detects a name mixing Latin + non-Latin where
  the sort_name is pure Latin (U+0000–U+024F) — used to also index the romanized
  sort_name so e.g. CJK/Cyrillic names are findable by their Latin sort form.
- `index_ids` = `artist_credit_id`s; blob insert uses
  `INSERT ... ON CONFLICT(entity_id) DO UPDATE SET index_data=excluded.index_data`.

This is why `make_indexes` needs Postgres unless `--skip-artists`.

---

## 10. Incremental update (`update`)

Source: `update.cpp`, `changed_data.hpp`, `mapping_update_fetcher.hpp`,
`mapping_batch_updater.hpp`, `canonical_release_updater.hpp`. **Verified below.**

`update` is a **long-running daemon**, not a one-shot. It opens `mapping.db`
`OPEN_READWRITE` (the *only* writer; the server is read-only) and Postgres, then loops:
sleep until **:15 past each hour** (`update_minute_offset = 15`), run one incremental
pass, repeat. Flags: `--dry-run` (report only) and `--apply` (write).

### Timestamp / change detection (`changed_data.hpp`)
- Metadata table: `update_metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL)`; the
  cursor is row `key='last_updated'`. `make_mapping` seeds it via
  `initialize_update_timestamp`. **This is the exact `update_metadata` schema** (§2
  can now be treated as resolved).
- The timestamp is MusicBrainz's **data replication point**, not wall clock:
  `SELECT last_replication_date::TEXT FROM musicbrainz.replication_control`.
- First run with no `last_updated` → refuses ("run a full build first").
- Change collection compares `last_updated` against each source table's
  `last_updated`/`created`, unioning into three sets:
  - **changed release_groups** (from `release.last_updated`,
    `release_group.last_updated`, `medium.last_updated`) → drive `canonical_release`
    re-eval.
  - **changed artist_credit_ids** (from `recording.last_updated`, recordings on
    updated releases, updated releases' own `artist_credit`,
    `artist.last_updated` → their `artist_credit_name`s, and
    `artist_credit.created`) → drive mapping + index updates.
  - **changed recording_ids** (fine-grained, optional).
- Artist credits `1..=2` (Various Artists etc.) are filtered out of the change set.

### Update pass (`run_update`)
1. Collect changed data (above). If none → save current timestamp, done.
2. If `--dry-run` → print counts, exit without writing.
3. Update `mapping.canonical_release` in Postgres for changed release groups
   (`canonical_release_updater`).
4. Process changed artist_credit_ids in **batches of 5000** (`BATCH_SIZE`). Per batch,
   **strict ordering**:
   a. Fetch fresh mapping rows from Postgres (`MappingUpdateFetcher`, §3 queries with
      an `ac.id` filter).
   b. Update the SQLite `mapping` table **first** (`update_mapping_only`) — so index
      rebuild reads fresh rows.
   c. Rebuild fuzzy indexes for those artists (`build_indexes_for_update`).
   d. Update the SQLite `index_cache` blobs (`update_index_cache_only`).
   On any batch failure it aborts (a batch may leave `mapping` updated but
   `index_cache` not — a partial-write hazard to handle in the port, e.g. transaction
   per batch).
5. **Rebuild ALL artist indexes wholesale** (`ArtistIndex::build`) — nmslib has no
   incremental index update, so the global artist index is fully rebuilt (~3 min per
   the code comment) every pass that has changes.
6. Save the new `last_updated` timestamp.

### Verified updater SQL & transaction boundaries
- **`MappingUpdateFetcher::fetch`** (`mapping_update_fetcher.hpp`): runs the §3
  queries with filter `ac.id = ANY($1::int[])` (Postgres int-array param), both the
  with-releases and standalone variants, and **re-applies the same `combined_lookup`
  dedup** as the full build (with-releases first so best scores win). Row parsing is
  identical to §3 (array-brace stripping, first sortname).
- **`MappingBatchUpdater`** (`mapping_batch_updater.hpp`): each of
  `update_mapping_only`, `update_index_cache_only`, the combined `update`, and
  `delete_only` runs inside a single `SQLite::Transaction` that commits or rolls back
  atomically. Pattern is **delete-then-insert** keyed on the batch's artist_credit_ids
  built into an `IN (...)` clause:
  - mapping: `DELETE FROM mapping WHERE artist_credit_id IN (...)` then
    `INSERT INTO mapping VALUES (?×12)` per row.
  - cache: `DELETE FROM index_cache WHERE entity_id IN (...)` then
    `INSERT INTO index_cache (entity_id, index_data) VALUES (?, ?)` per blob.
  Because `update.cpp` calls `update_mapping_only` and `update_index_cache_only` as
  **two separate transactions** (mapping committed before indexes are built), a crash
  between them leaves mapping updated but `index_cache` stale for that batch — the
  partial-write hazard. The port should wrap a batch's mapping+cache writes in one
  transaction (the combined `update()` already does this; `update.cpp` just doesn't
  use it because it must build indexes from committed mapping rows in between).
- **`CanonicalReleaseUpdater`** (`canonical_release_updater.hpp`): per changed
  release-group set, `DELETE FROM mapping.canonical_release WHERE release IN (SELECT
  r.id FROM musicbrainz.release r WHERE r.release_group = ANY($1::int[]))`, then an
  `INSERT ... SELECT` using `ROW_NUMBER() OVER (PARTITION BY rg.id ORDER BY
  <release_group_combined_type_sort>, <format_sort>, <date>, country, artist_credit,
  name, release_id) WHERE rnum = 1` — i.e. keeps **one** canonical release per group.
  Batched (default 10000 groups) each in its own Postgres `BEGIN/COMMIT`.

  **⚠ VERIFIED INCONSISTENCY between full build and incremental update.** The
  full-build `CanonicalRelease` (`canonical_release.hpp`, §3.x below) does **NOT** keep
  one release per group — it inserts **ALL** releases (deduplicated by `release_id`
  only), ordered by the same ranking, and uses the `SERIAL id` as the score (lower =
  better). Its own comment: keeping all releases lets bonus tracks on non-standard
  editions be found. The updater instead collapses to `rnum = 1` (one release per
  group). So after an incremental update, changed release groups have a *different*
  canonical_release shape (single row) than freshly-built ones (all rows). This is a
  real divergence in the current code — the Rust port must pick one behavior for both
  paths (recommended: match the full build — keep all releases — since search relies
  on multiple releases per group being present, e.g. the `'l'` fuzzy release match and
  the link `rank`). Flag and resolve; do not replicate the inconsistency.

### CRITICAL open problem — reader/writer coherence
`update` writes `mapping.db` while the read-only `server` has it open. **There is no
mechanism for a running server to observe the update.** Verified in `index_cache.hpp`:
- `IndexCache` only ever *adds*; it has **no invalidation, no versioning, no staleness
  check, and no signal from `update`.**
- The only removal path is `trim()`, triggered **solely when the cache is full**
  (`count >= max_cache_items`), evicting oldest `ref_count==0` entries to reclaim
  memory — never to refresh.
Consequence: an artist index already cached in RAM is served **stale indefinitely**
until (a) the cache fills and that entry is LRU-evicted and later reloaded, or (b) the
server restarts. Newly-updated `mapping` rows are seen only for artists not currently
cached (or after eviction). The OS page cache adds a second staleness layer.

**The Rust port MUST design this explicitly.** Options:
- Have `update` bump a version/generation counter (e.g. in `update_metadata`); server
  checks it and invalidates/reloads changed `index_cache` entries. Requires `update`
  to record *which* `artist_credit_id`s changed each pass (it already has the set).
- Build-and-swap: `update` writes a new DB file, server atomically reopens (needs a
  reload signal / SIGHUP / health-gated rolling restart).
- Read-through with per-entry staleness: cache stores the source row's version;
  invalidate on mismatch.
- Enable SQLite **WAL** so the writer doesn't block readers and readers get consistent
  snapshots (helps concurrency, does *not* by itself fix the in-memory cache
  staleness).
Pick one and specify it; the current C++ behavior (implicit, eventual, eviction-driven)
should not be treated as the intended contract.

---

## 11. Recommended Rust crate mapping

| Concern | C++ today | Rust suggestion |
|---------|-----------|-----------------|
| SQLite | SQLiteCpp | `rusqlite` (bundled) |
| Postgres | libpq | `postgres` / `tokio-postgres` (server-side cursors via `COPY`/`FETCH` or portal) |
| HTTP | Crow | `axum` or `actix-web` |
| Templates | mustache | `askama` / `minijinja` (or JSON API, §8) |
| Sparse linear algebra | armadillo `sp_mat` | `sprs`, or hand-rolled `Vec<(u32,f32)>` |
| Fuzzy index | nmslib `simple_invindx` | hand-rolled exact sparse inverted index (§6) |
| unidecode | deps/unidecode | port the table (recommended) or `deunicode` crate |
| Levenshtein | levenshtein.cpp | `strsim` or hand-rolled (unit cost) |
| Serialization | cereal | `serde` + `bincode` (new blob format) |
| Regex | jpcre2/PCRE2 | `regex` (patterns are ASCII `\w`, trivial) |
| Threads | std::thread + thread_local | `rayon` (build), `tokio`/thread pool (serve) |

---

## 12. Golden test set (definition of done)

Build a fixture of `(artist, release, recording) -> {artist_mbid, release_mbid,
recording_mbid}` from:
- `mapper/test_cases.hpp` (the existing Catch2 cases), and
- the hard cases documented in `PROBLEMS.txt` (Harry Nilsson / "Without You",
  Godspeed You! Black Emperor / "Lift Your Skinny Fists…", etc.) with their known
  correct MBIDs.

Acceptance: run the same inputs through the C++ `server` and the Rust `server` over
the *same* `mapping.db`; the returned MBID triples must match on the golden set.
Confidence values need not be identical (§0.4). Track any divergences caused by the
unidecode table choice (§4) separately.

Use the `use_minimal_dataset` filter (`ac.id IN (1160983, 49627, 65, 21238)`) to
build a tiny `mapping.db` for fast CI without the full MusicBrainz dump.

### 12.1 Load testing / performance

There is a Locust harness at `load_test/` today; **no benchmark results are recorded
in the repo** (nothing captured for latency, throughput, or memory). The Rust port
can reuse it as-is if it exposes the implied `GET /mapping/lookup` API (§8).

Harness contract (from `load_test/locustfile.py`):
- Input data: JSONL file (`TEST_DATA_FILE`, default `../listen_test_data/listens.jsonl`),
  each line `{"track_metadata": {"artist_name","release_name","track_name"}}`. Falls
  back to a few hard-coded famous tracks if absent.
- Request: `GET /mapping/lookup?artist_credit_name=&recording_name=[&release_name=]`.
  Success = `200` or `404`; `503` = not ready (failure).
- Task weights: with-release 10, without-release 5, health (`GET /`) 1.
- User classes: `MappingUser` (0.5–2 s wait), `HighLoadUser` (0.1–0.5 s).
- Example headless run:
  `locust -f locustfile.py --host http://localhost:5000 --headless --users 100
  --spawn-rate 10 --run-time 5m --csv=results/load_test`.

Metrics worth capturing (none exist yet), driven by the scaling analysis in
`DESIGN.md`:
- Lookup p50/p95/p99, warm vs cold cache.
- Throughput vs `NUM_THREADS`.
- Cache hit rate vs `MAX_CACHE_ITEMS` under a *realistic skewed* listen distribution
  (uniform-over-all-artists is a worst case that thrashes the cache).
- `make_mapping` / `make_indexes` wall time and **peak RAM** at full scale (the
  `make_mapping` dedup set is linear in unique rows — multi-GB at 38M+).
- Behavior on pathologically large artist credits and high name-collision inputs
  (the adaptive-K loop, §6).

A realistic benchmark must use a full `mapping.db`; the minimal-dataset build is only
meaningful for correctness (golden set), not performance.

### 12.2 Storage sizing (ESTIMATED — to be verified)

No `mapping.db` has been built on record; sizes below are estimates. See `DESIGN.md`
"Storage sizing" for the reasoning. Rough full-scale estimate: **~20–50 GB**, split
across the `mapping` table (~10 GB), its five B-tree indexes (~several GB), and the
`index_cache` BLOBs (largest, low-single-digit to low-tens of GB).

**How to measure (do this instead of trusting the estimate):**
- Build a tiny DB with the minimal-dataset filter (`ac.id IN (1160983, 49627, 65,
  21238)`), then run `make_indexes`, to get real per-row and per-artist-blob sizes.
- Per-table / per-index byte breakdown via SQLite's `dbstat` virtual table:
  ```sql
  SELECT name, SUM(pgsize) AS bytes
    FROM dbstat
   GROUP BY name
   ORDER BY bytes DESC;
  ```
  This separates the `mapping` table, each of its five indexes, and `index_cache`.
- Average `index_cache` blob size:
  ```sql
  SELECT COUNT(*) AS n, AVG(LENGTH(index_data)) AS avg_bytes,
         SUM(LENGTH(index_data)) AS total_bytes FROM index_cache;
  ```
- Extrapolate to full artist/row counts (~2.8M artists, tens of millions of mapping
  rows). The dominant uncertainty is the average per-artist blob size.

Note: the Rust port uses a **different `index_cache` blob format** (§0), so its
`index_cache` size will differ from the C++ build — measure both if comparing.

---

## 13. Items to verify against source before implementing

This spec is now derived from a **full read of every file on the build, serve, and
update paths.** Verified and quoted in this document: CLI/env (§1); SQLite schema +
sentinels + `update_metadata` (§2); canonical extraction queries, dedup, custom sort
tables, and the `canonical_release` build (§3); encoding (§4); TF-IDF (§5); fuzzy
index + adaptive-K + long-query rescoring (§6); the FSM search with backtracking /
first-match / three artist indexes (§7); HTTP routes + load-test drift (§8); artist
index population SQL (§9); the `update` daemon, change-detection, updater SQL, and the
coherence gap (§10).

**No files remain unread.** What remains are **design decisions the port must make**,
not unverified code:
- **Canonical-release full-build vs incremental inconsistency** (§10): full build keeps
  ALL releases per group (dedup by release_id); the updater keeps ONE (rnum=1). Pick
  one for both paths — recommended: keep all (search depends on it).
- **`score` semantics** (§3): `mapping.score` = `canonical_release.id` (a `SERIAL`
  assigned in ranking order across the two VA/non-VA passes). The Rust port must
  reproduce the *insertion order* to get equivalent scores, or define its own stable
  canonical ordering and accept different absolute score values (only the *ordering*
  matters downstream via `ORDER BY score`).
- **Index blob format** (§0): new Rust format; do not read C++ cereal blobs.
- **unidecode table** (§4) and **similarity engine fidelity** (§0.4): match the C++
  outputs or accept/measure divergence against the golden set (§12).
- **Reader/writer coherence** (§10) and **serving PRAGMAs** (§8): design explicitly.
- **Proposed signals** (§14): ISRC/duration/AcoustID are additive, not yet implemented.


---

## 14. PROPOSED: additional matching signals (AcoustID / ISRC / duration)

**Status: proposed, not implemented.** Nothing in §1–§13 depends on this. These are
additive and optional — absent signals contribute nothing, so name-only input
behaves exactly as the current system. Principle (see `DESIGN.md`): **strong
identifiers short-circuit the pipeline; weak signals only re-rank candidates that
already clear the name thresholds.**

Signal strength, strongest first: AcoustID > ISRC > names (current) > duration.

### 14.1 Input contract additions (all optional)
Search input gains optional fields:
```
isrc:        Option<String>     // e.g. "USRC17607839"
duration_ms: Option<u32>        // track length in milliseconds
acoustid:    Option<Uuid>       // AcoustID track UUID (Picard already has this)
// future/weak: year, label, track_number
```

### 14.2 Schema additions to `mapping.db`

Add a duration column to the mapping row (source `musicbrainz.recording.length`, ms):
```sql
ALTER TABLE mapping ADD COLUMN recording_length INTEGER;  -- ms, nullable
```
(Or a side table `recording_length(recording_id INTEGER PRIMARY KEY, length INTEGER)`
if you prefer not to widen the hot table.)

ISRC side table (source `musicbrainz.isrc`, recording↔ISRC is many-to-many):
```sql
CREATE TABLE isrc (
    isrc         TEXT    NOT NULL,
    recording_id INTEGER NOT NULL
);
CREATE INDEX isrc_ndx ON isrc(isrc);
```

AcoustID (proposed, heavier — decide operational model):
- Option A: call the AcoustID web service at query time (recording MBIDs come back
  directly). No local table; adds a network dependency.
- Option B: local table `acoustid(acoustid TEXT/uuid, recording_id INTEGER)` indexed
  on `acoustid`, populated from an AcoustID data dump. Self-contained but large and
  needs its own update cycle.
```sql
CREATE TABLE acoustid (
    acoustid     TEXT    NOT NULL,
    recording_id INTEGER NOT NULL
);
CREATE INDEX acoustid_ndx ON acoustid(acoustid);
```

Populate `recording_length` and `isrc` during `make_mapping` (extra columns/joins on
the §3 queries: `r.length`; and a separate pass over `musicbrainz.isrc`). No fuzzy
index rebuild needed for these — they are exact-lookup tables.

### 14.3 Fast path (before fuzzy matching)
In the search pipeline (§7), before step 1:
```
if acoustid present:
    ids = lookup recording_id(s) by acoustid   (table or web service)
    if hit: resolve recording MBID directly; confidence = HIGH (e.g. 1.0);
            optionally fuzzy-match only the release for release MBID; RETURN.
if isrc present:
    ids = SELECT recording_id FROM isrc WHERE isrc = ?
    filter/confirm the artist is plausible (encode+compare artist name, or
        require the recording's artist_credit to fuzzy-match >= artist_threshold)
    if unambiguous hit: resolve directly, confidence = HIGH;
            optionally fuzzy-match the release; RETURN.
    if ISRC maps to multiple recordings / artist mismatch: FALL THROUGH to fuzzy.
```
Both are high-confidence, not infallible (an ID may map to multiple recordings), so
always fall back to the fuzzy pipeline on conflict rather than returning a guess.

### 14.4 Duration re-ranking (soft, never a filter)
Applies **only** in `find_match` (`search.hpp`), at the single scoring site currently:
```
score = (rec_result.confidence + rel_result.confidence) / 2.0
```
Proposed, gated on `duration_ms` being present AND the candidate already passing name
thresholds:
```
base = (rec_conf + rel_conf) / 2.0
if duration_ms.is_some() and candidate.recording_length.is_some():
    delta = |duration_ms - recording_length|
    // tolerance window: e.g. within 3000 ms OR 3% of length -> bonus,
    // grossly off -> mild penalty, in-between -> neutral
    score = base + duration_adjustment(delta, recording_length)   // small, bounded
else:
    score = base
```
Requirements:
- Duration must be **bounded and small** relative to the name score — it breaks ties
  among good matches, it must not promote a poor name match above a good one.
- Missing on either side → no effect (neutral).
- Tolerance must be generous (fades, silence, ms/s rounding, bad metadata). Exact
  window/curve is a tunable; pick with the golden set (§14.6).
- This requires `find_match` to have access to the candidate's `recording_length`
  (add it to the link/lookup, or fetch alongside metadata).

### 14.5 HTTP API additions
Extend the search endpoint with the optional query params `isrc`, `duration_ms`,
`acoustid` (and later `year`, `label`, `track_number`). Purely additive; existing
callers unaffected. If/when a JSON API is defined (§8), include these there too, plus
a field indicating *which signal produced the match* (`match_source`:
`acoustid`|`isrc`|`name`) so callers like Picard can reason about confidence.

### 14.6 Golden test additions (prove the signals help)
The current golden set (§12) is name-solvable. Add cases that are **unsolvable by
names alone** but solvable with the new signals, otherwise the additions can't be
validated:
- Same-titled recordings by one artist distinguished only by duration
  (studio/live/remix, "Intro", "Untitled").
- Name-colliding inputs where the ISRC resolves the correct recording.
- Picard-style cases where AcoustID pins the recording regardless of messy tags.
Each case: assert the correct MBID triple AND (optionally) the expected
`match_source`.

### 14.7 Open decisions
- AcoustID operational model (web service vs local dump) — affects deployment and
  update cadence.
- Duration tolerance curve and weight — tune empirically.
- ISRC artist-confirmation strictness (how much to trust ISRC alone vs require artist
  agreement).
- Whether duration should also lightly influence the *release* choice among a
  recording's releases (a recording's length is release-independent, so probably no).
