# Rust Reimplementation Plan

A from-scratch plan for a Rust MBID mapper. It assumes the verified contracts in
`REWRITE_SPEC.md`, addresses the problems in `IMPROVEMENTS.md`, and is driven by the
stated goals: **minimal dependencies, and optimized matching accuracy, latency, disk,
memory, API, and maintainability.**

This is a plan, not code. It states decisions, the reasoning, and a phased path.

---

## 0. Goals & non-goals

**Goals (ranked):**
1. **Matching accuracy first** — a wrong MBID is worse than no MBID. Accuracy is a
   measured, tracked number from day one.
2. **Correctness of the update path** — no stale-serving, no partial-write drift
   (fixes the two worst bugs, `IMPROVEMENTS.md` I1/I5).
3. **Low, predictable latency and memory** under real (skewed) listen traffic.
4. **Small, reproducible build** — few, pinned dependencies; one static binary.
5. **Maintainability** — explicit, testable matching pipeline (not an opaque FSM).

**Non-goals:**
- Bit-compatibility with the C++ index blob format (rebuild indexes; keep only the
  data-table contract where useful).
- Being a general search engine. It does exactly one thing:
  `(artist, release, recording [, isrc, duration, acoustid]) → MBIDs`.

**Definition of done:** on the golden set (§8), the Rust mapper meets or beats the C++
version's match rate, with p99 warm-cache latency and memory within budget (§7).

### The two consumers (they pull in different directions — design for both)

| | **Picard** (`/home/zas/src/picard`) | **ListenBrainz listens** |
|---|---|---|
| Has | audio file + rich tags: multiple ISRCs, precise duration, track number, barcode, can compute **AcoustID** | loose scrobble strings only: artist / release / track names; sometimes duration; rarely IDs |
| Matches | a file/cluster → release/recording among confusable candidates | a listen → MBIDs at high volume |
| Regime | **high-signal, precision-critical** | **low-signal, high-volume, recall-sensitive** |
| Needs | strong-ID fast paths (ISRC/AcoustID), duration tiebreak, tolerance to *degraded* tags | robust name-only fuzzy matching, cheap per-lookup |

Design consequence: this is exactly why the pipeline is **strong-ID-short-circuits,
weak-signals-rerank, everything optional** (§6). The same core must serve a client that
supplies IDs+duration *and* one that supplies only names. The eval must cover **both**
regimes (§8): Picard's harness (`scripts/eval_matching/`) covers the high-signal side
thoroughly; the listen side needs its own name-only corpus.

---

## 1. Guiding decisions (the ones that shape everything)

| Decision | Choice | Why |
|----------|--------|-----|
| Language | Rust, stable, edition 2021 | Memory safety removes the whole class of C++ bugs (I-E2); great perf; single static binary |
| Binary layout | One binary, subcommands (`build`, `index`, `serve`, `update`) | Simpler ops than 6 binaries; shared code |
| Async | Only in the HTTP server; build/update are sync + `rayon` | Avoid async everywhere; it buys nothing for CPU-bound batch work |
| Similarity engine | **Hand-rolled exact sparse inverted index** | nmslib's `simple_invindx` is exact, not ANN; reimplementing it is ~a few hundred lines and drops a huge dependency. ANN crates would change recall and break golden tests |
| Index storage | Rebuildable; **not** cereal-compatible | Blob format is an internal cache; own it with `serde`+`bincode`/`rkyv` |
| Data DB | SQLite via `rusqlite` (bundled) | Read-only, single-node, point lookups — SQLite's best case (see `DESIGN.md`) |
| Coherence | **Build-and-swap + generation counter** | Fixes I1 by construction (see §5) |

**Minimal dependency set (target ~10, all pinned):**
`rusqlite` (bundled SQLite), `tokio-postgres` (build/update only), `axum`+`tokio`
(serve), `serde`+`bincode` (or `rkyv`) for index blobs, `deunicode` (or a vendored
unidecode table — see §4), `rayon` (parallel build), `regex` (trivial ASCII patterns),
`serde_json`, `tracing`+`tracing-subscriber` (logs/metrics), `metrics`+
`metrics-exporter-prometheus` (observability). Everything else is std. No armadillo, no
nmslib, no Crow, no asio, no cereal, no jpcre2, no standalone libpq.

---

## 2. Crate / module structure

```
mbid-mapper/
  Cargo.toml                 # workspace, pinned deps, LTO+codegen-units=1 release profile
  crates/
    mapper-core/             # no I/O: encoding, tfidf, sparse index, scoring, types
      encode.rs              # §4 exact port of EncodeSearchData
      tfidf.rs               # §5 char-3gram TF-IDF (sklearn smooth idf, L2)
      sparse_index.rs        # exact sparse dot-product top-K (replaces nmslib)
      levenshtein.rs         # unit-cost edit distance (long-query rescoring)
      score.rs               # candidate scoring incl. duration/ISRC re-rank
      types.rs               # SearchMatch, IndexResult, ReleaseRecordingIndex
    mapper-db/               # SQLite schema, read + write, index_cache blobs
      schema.rs              # §2 tables + migrations + generation counter
      read.rs                # read-only queries (serve path)
      write.rs               # mapping/index_cache writes (build/update path)
    mapper-mb/               # Postgres extraction (build/update only)
      canonical.rs           # §3 queries, custom sort tables, canonical_release
      changed.rs             # §10 change detection
    mapper-index/            # index building (uses core + db)
      artist_index.rs        # 3 artist indexes (§9)
      recording_index.rs     # per-artist release/recording indexes + links
      cache.rs               # bounded in-memory cache w/ generation-based invalidation
    mapper-search/           # the matching pipeline (replaces fsm.hpp)
      pipeline.rs            # explicit staged candidate ranking (§6)
    mapper-cli/              # the single binary: build|index|serve|update
      main.rs
  golden/                    # golden test fixtures (§8)
  tests/                     # integration tests on a minimal DB
```

Rationale: `mapper-core` is pure (no I/O), so the accuracy-critical logic is trivially
unit-testable and deterministic. Everything else depends on it.

---

## 3. Data model & disk usage

Keep the `mapping.db` **data-table** contract from `REWRITE_SPEC.md` §2 (so you can
diff against C++ output and reuse the golden DB), with disk-focused refinements:

- **`mapping` table:** same columns. Disk optimizations:
  - Store MBIDs as **16-byte BLOB** (UUID bytes), not 36-char text — ~55% smaller per
    MBID, 4 MBIDs/row → meaningful at tens of millions of rows. Format to hyphenated
    text only at API egress.
  - `artist_mbids` as a length-prefixed BLOB of 16-byte UUIDs rather than a
    comma-joined string.
  - Consider `WITHOUT ROWID` only if a natural PK fits; otherwise keep rowid.
- **New columns/tables for signals (I3):** `recording_length INTEGER` (ms) on
  `mapping`; `isrc(isrc TEXT, recording_id INTEGER)` indexed on isrc.
- **`index_cache`:** same `(entity_id, index_data BLOB)` shape, new Rust blob format.
  Store blobs **compressed** (zstd, level ~3–6). Index blobs are the dominant disk
  cost (est. low-tens of GB, `DESIGN.md`); zstd on TF-IDF/text structures typically
  gets 2–4×. Decompress on load (cheap vs the deserialization already happening).
- **`meta` table:** `last_updated` (replication timestamp) **and** a monotonic
  `generation` counter + per-artist `changed_at` generation (for coherence, §5).
- **Blob format:** `rkyv` for zero-copy deserialization (no parse step → kills the
  main per-query latency spike, `IMPROVEMENTS.md` D2) or `bincode` if simpler. Decide
  by benchmark; `rkyv` is the performance play.

Measure real sizes early via `dbstat` (`REWRITE_SPEC.md` §12.2) on a minimal build.

---

## 4. Encoding (must match, or accuracy diverges)

Exact port of `REWRITE_SPEC.md` §4:
- `encode_string`: strip `[^\w]+` (ASCII `\w`), unidecode, lowercase, strip `[ _]+`.
- `encode_string_keep_non_word`: strip `\s`, lowercase.

**Unidecode is the #1 divergence risk.** Plan: **vendor the exact transliteration
table** used by the C++ `deps/unidecode` (it's data, not code) into `mapper-core` and
port the lookup. This guarantees byte-identical output and removes a runtime crate. Add
a differential test: run a large sample of MB names through both C++ and Rust encoders
and assert equality; any mismatch is a known, listed exception.

---

## 5. The update path — designed correct from the start

This is where the C++ version has its worst bugs. Fix them by construction.

**Coherence (I1) — build-and-swap + generation counter:**
- `update` writes to a **staging** area, never mutating the live DB in place for the
  changed rows mid-serve. Two viable models:
  - **A (simple, robust):** write a new `mapping.db` (copy + apply deltas), then the
    server hot-swaps via an atomic file rename + reopen on a `SIGHUP`/inotify signal.
    Readers finish in-flight queries on the old handle; new queries use the new one.
  - **B (finer-grained):** single DB with a `generation` counter; `update` writes
    changed `index_cache`/`mapping` rows and bumps `meta.generation` and each changed
    artist's `changed_at`. The server's cache stores the generation it loaded an entry
    at; on lookup it cheaply checks the artist's `changed_at` and reloads if stale.
- Recommend **B for incremental freshness** with **A as the periodic full-rebuild
  path**. Either way the server is never silently stale.

**Partial-write (I5):** each artist batch's `mapping` + `index_cache` writes happen in
**one** SQLite transaction. Because indexes are built from the new mapping rows, build
the index in memory *first* (from the fetched Postgres rows, not from committed DB
state), then write mapping + index_cache together atomically. Make the whole pass
**idempotent/resumable**: record the highest fully-committed batch; a re-run resumes.

**Canonical inconsistency (I4):** one code path builds `canonical_release`, used by
both full build and incremental update — keep **all** releases per group (dedup by
release_id), never the updater's `ROW_NUMBER()=1`. No two divergent implementations.

**Determinism (I4/A4):** never compare scores by float equality. Quantize similarity to
a fixed integer scale (e.g. round to 1e-4) for tie detection, and always stable-sort by
`(‑score_q, id)`. Eliminates build-type divergence without an epsilon hack.

---

## 6. The matching pipeline — explicit, not an FSM

Replace `fsm.hpp` with a linear, testable pipeline that makes the
**strong-ID-short-circuits, weak-signals-rerank** principle first-class:

```
resolve(query) -> Option<SearchMatch>:
  1. if query.acoustid  -> fast_path_acoustid()  (I: F1; may short-circuit)
  2. if query.isrc      -> fast_path_isrc()       (exact; confirm artist plausibility)
  3. artist_candidates  = artist_search(name)     (3 indexes; clean+retry; ≥ thr)
  4. for each artist candidate (confidence order):
       load ReleaseRecordingIndex (cache, generation-checked)
       rec_candidates = recording_search(...)      (≥ thr)
       rel_candidates = release_search(...) or canonical fallback
       for (rec, rel) linked pairs:
           base   = (rec.conf + rel.conf)/2
           score  = base + duration_adjust(query.duration, rec.length)   (I3)
           collect SearchMatch{score, source}
  5. return best SearchMatch by (score desc, deterministic tiebreak)
```

Key differences from the C++ FSM, all deliberate:
- **Best-of-candidates, not first-match** — with a golden set (§8) to prove it's at
  least as good; falls back to first-match semantics if a case regresses. (Resolves
  `IMPROVEMENTS.md` B4.)
- **Thresholds are length-adaptive** (I6): `thr(len)` functions, tuned against the
  golden set, not constants.
- **Signals are optional inputs** to `score`, absent → no effect (backward compatible).
- Pure functions over loaded indexes → each stage unit-tested in `mapper-core`.

**Sparse index (replaces nmslib):** build a postings map `trigram -> [(doc, weight)]`
from L2-normalized TF-IDF vectors; query accumulates dot products over the query's
trigram postings, keeps top-K by a bounded heap, applies the same adaptive-K perfect-
tie expansion (`REWRITE_SPEC.md` §6) and long-query Levenshtein rescoring. Exact,
deterministic, small.

---

## 7. Performance & memory budget

Targets (to be validated by benchmark, §8 / `IMPROVEMENTS.md` I8):
- **Warm-cache lookup p99 < a few ms**; cold (cache miss) dominated by blob load —
  minimized by `rkyv` zero-copy + zstd (§3).
- **Flat, bounded memory:** the in-memory index cache is capped (configurable), LRU by
  last-access, with per-entry generation for invalidation. Memory is a function of the
  cap, not the catalog.
- **Concurrency:** `axum` + a worker pool; one read-only `rusqlite` connection per
  worker (or a small pool); reads never block reads. Set serving PRAGMAs (I9):
  `mmap_size` large, `cache_size` negative-KB, `query_only=ON`, `temp_store=MEMORY`.
- **Build:** `rayon`-parallel per-artist index build; streaming/on-disk dedup for
  `make_mapping` to avoid the C++ linear-RAM dedup set (`IMPROVEMENTS.md` D3) — e.g.
  dedup by writing to a temp SQLite/`sled` keyed on `combined_lookup`, or sort-based
  external dedup.
- **Release profile:** `lto = "fat"`, `codegen-units = 1`, `panic = "abort"` for the
  serve binary; avoid `-ffast-math`-style nondeterminism (compute TF-IDF in `f32`
  deterministically).

---

## 8. Testing, accuracy metric, and benchmarking (built in from day 1)

This is `IMPROVEMENTS.md` I2, treated as core infrastructure. **Do not reinvent the
eval — Picard already has a mature one at `/home/zas/src/picard/scripts/eval_matching/`
and it is directly applicable.** Reuse its methodology and corpus.

**What Picard's harness already provides (reuse it):**
- A curated **corpus** of confusable releases/recordings with known-correct MBIDs —
  the same hard cases as `PROBLEMS.txt` (Weezer Blue/Green, GY!BE spelling variants,
  Collision Course 23-vs-13 tracks, Beethoven 5th conductors, 椎名林檎 digital vs CD,
  Nirvana studio vs Unplugged, Queen GH editions). `corpus/releases.tsv` is the
  registry; `eval_release_*.json` are cached MB responses.
- A **degradation model** (`eval_matching.py`): takes correct metadata and corrupts it
  (typos, missing/wrong barcode, year-only date, ±3 s / ±15 s length diffs, remaster
  suffix, feat. artists, swapped fields, and realistic combos). This tests robustness
  against *degraded tags* — exactly Picard's regime — far richer than static
  input→output pairs.
- **Signal-specific synthetic fixtures** (`eval_recording_*.json`): `multi_isrc`
  (full/partial/unique/all-wrong ISRC overlap), `same_song`, `zero_length` (missing
  duration), `unicode`, `short_tracks`, `feat_artists`, `parenthetical`,
  `extreme_similarity`. These exercise the ISRC/duration signals from §6/§14 directly.
- A **save/compare delta workflow** (`--save`/`--compare`) reporting improved/regressed
  cases — precisely the CI accuracy-gate we want.
- It has already surfaced a concrete scoring bug to design against: *file has ISRCs but
  a candidate has none → ISRC comparison is skipped, letting the no-ISRC candidate win
  over partially-matching ones.* Our `score.rs` must penalize ISRC mismatch and handle
  the "have ISRC vs candidate has none" asymmetry explicitly.

**Plan:**
- **Picard-side (high-signal):** point the eval harness (or a thin adapter) at the Rust
  mapper's `GET /1/mapping/lookup` so the same corpus + degradations + delta workflow
  gate the mapper. Reuse the fixtures rather than duplicating them; keep them as a git
  submodule or vendored snapshot under `golden/picard/`.
- **Listen-side (low-signal):** a *separate* name-only corpus derived from real
  ListenBrainz listens (`{artist_name, release_name, track_name}` → expected MBIDs),
  since Picard's corpus is ID/duration-rich and does not represent scrobble noise.
  Include the `PROBLEMS.txt` cases and known-messy scrobbles. See `LISTEN_SPEC.md` for
  the verified listen format, the current `/1/metadata/lookup/` contract to stay
  compatible with, and label sources (the mbid-mapping writer's stored matches +
  dumped listens).
- **Accuracy metric:** precision / recall / match-rate per corpus, printed and asserted
  in CI; a regression fails the build. Report the two regimes separately (Picard
  precision-critical; listens recall-sensitive).
- **Minimal DB** for CI: build with `ac.id IN (1160983, 49627, 65, 21238)` — fast, no
  full dump. (Note: the eval corpus artists must exist in the built `mapping.db`; either
  extend the minimal filter to cover the corpus MBIDs or run eval against a fuller DB.)
- **Differential tests vs C++:** same inputs through both over the same `mapping.db`;
  MBID triples must match (confidence need not). Track divergences (esp. unidecode).
- **Property tests:** encoding idempotence, TF-IDF determinism, sparse-index equals a
  brute-force reference on random data.
- **Load/bench:** reuse the Locust harness against the real `GET /1/mapping/lookup`
  (fixes the endpoint drift, `IMPROVEMENTS.md` C3); capture p50/p95/p99, throughput vs
  workers, cache hit rate under a *skewed* listen distribution. Record baselines.

---

## 9. API & operability

### Core API principle: one endpoint, progressive precision

**One lookup endpoint serves both consumers. Everything beyond the minimum is optional,
and precision scales with how much you provide.** The caller sends whatever it has; the
mapper uses every signal available and degrades gracefully when signals are missing —
it never *requires* the rich inputs, and never ignores them when present.

- **Minimum:** `recording` (+ `artist` strongly recommended). Pure name-only fuzzy
  match — the ListenBrainz-listen path.
- **+ `release`:** disambiguates which release/edition → better `release_mbid`.
- **+ `duration_ms`:** breaks ties between same-named recordings (studio/live/remix).
- **+ `isrc` (one or many):** near-exact recording resolution; short-circuits fuzzy.
- **+ `acoustid`:** strongest — identifies the audio itself; the Picard path.

The **signal ladder** (strongest first): `acoustid > isrc > duration > names`. Strong
IDs short-circuit and set high confidence; weaker signals re-rank name candidates
(§6). The response's `confidence` and `match_source` therefore *reflect the input
richness* — a name-only listen might return `confidence 0.72, match_source "name"`,
while a Picard lookup with a matching ISRC returns `confidence ~1.0,
match_source "isrc"`. Same endpoint, same contract, precision proportional to input.

- **JSON API** (I7): `GET /1/mapping/lookup?recording=&artist=[&release=&duration_ms=
  &isrc=&isrc=&acoustid=]` → JSON `{ artist_mbids, artist_credit_name, release_mbid,
  release_name, recording_mbid, recording_name, confidence, match_source }`.
  `match_source ∈ {acoustid, isrc, name}` so callers can reason about trust. `404` on
  no match, `503` until ready, `400` if the minimum inputs are absent.
  `isrc` is **repeatable** (Picard files can carry several); treat the
  ISRC-present-but-candidate-has-none asymmetry explicitly (§8). A `POST` batch variant
  (array of lookups) is worthwhile for the high-volume listen path.
- **Health/readiness** endpoints distinct from search (I C5).
- **Metrics** (I8): Prometheus endpoint — match/no-match rate, confidence histogram,
  cache hit rate, latency percentiles, update lag (`now − last_updated`), current
  generation. `tracing` for structured logs.
- **Config:** same env vars as today for drop-in ops, plus cache sizing and PRAGMA
  knobs; validated at startup.

---

## 10. Phased delivery

**Phase 0 — skeleton & harness (de-risk accuracy):**
`mapper-core` (encode + tfidf + sparse index + levenshtein) with property/differential
tests; wire up **Picard's `eval_matching` corpus** (§8) + a small listen corpus +
minimal-DB CI; the accuracy metric. No server yet. *Exit: encoder matches C++
byte-for-byte; sparse index matches a brute-force reference; eval harness runs against
a stub and reports the metric.*

**Phase 1 — build path:** `mapper-mb` (Postgres extraction, custom sorts, single
canonical_release path) + `mapper-db` writes + `mapper-index` build → produce a real
`mapping.db` from the minimal dataset, then full. *Exit: Rust-built minimal DB matches
C++-built one on the golden set.*

**Phase 2 — serve path:** `mapper-search` pipeline + bounded cache + `axum` JSON API +
PRAGMAs + metrics. *Exit: golden-set match rate ≥ C++; p99 within budget; metrics live.*

**Phase 3 — update path (correct):** change detection + atomic batch writes +
build-and-swap/generation coherence + resumable/idempotent pass. *Exit: no stale
serving under a write-during-serve test; crash-mid-update leaves a consistent DB.*

**Phase 4 — signals & tuning:** ISRC fast path + duration re-rank + length-adaptive
thresholds, tuned against the (now expanded) golden set. *Exit: measurable accuracy
gain over Phase 2 on the signal-dependent cases.*

**Phase 5 — hardening:** AcoustID (optional), full-scale build, storage/perf
measurement, ops docs. *Exit: full-catalog build within memory budget; sizing/bench
numbers recorded.*

Each phase is shippable and measured. Accuracy never regresses (CI gate).

---

## 11. Risks & mitigations

| Risk | Mitigation |
|------|-----------|
| unidecode divergence changes matches | Vendor the exact table; differential test (§4) |
| Hand-rolled sparse index ≠ nmslib results | Brute-force reference test; adaptive-K + epsilon parity (§6) |
| Best-of-candidates regresses vs first-match | Golden set gate; fall back per-case if needed (§6/B4) |
| Postgres extraction subtly differs (dedup, arrays, sortname) | Port exactly per `REWRITE_SPEC.md` §3; row-count + checksum diff vs C++ |
| `score` positional semantics | Single canonical path; if absolute scores matter, reproduce insertion order, else rely only on ordering (§5/A5) |
| rkyv format churn | Version the blob; `generation`-triggered rebuild handles format bumps |
| Scope creep into a general search engine | Non-goals (§0); the API surface is fixed |

---

## 12. What this plan fixes from IMPROVEMENTS.md

- **I1 coherence** → §5 build-and-swap + generation (by construction).
- **I2 accuracy metric** → §8 (Phase 0, CI gate).
- **I3 duration+ISRC** → §3 schema, §6 pipeline, Phase 4.
- **I4 canonical inconsistency + determinism** → §5 single path + integer-quantized ties.
- **I5 partial write** → §5 atomic per-batch + resumable.
- **I6 adaptive thresholds** → §6, Phase 4.
- **I7 JSON API** → §9.
- **I8 observability** → §9 metrics; §8 benchmarks.
- **I9 serving PRAGMAs** → §7.
- **I10 build/deps** → §1 minimal pinned deps, single static binary; §0 non-goal on
  blob compatibility removes the cereal/nmslib burden.
- **E2 memory safety / E3 opaque FSM** → Rust + explicit pipeline (§6).
- **D2 blob deserialization spike / D3 build RAM / D5 sizing** → §3 (rkyv+zstd), §7
  (streaming dedup), §8 (measure).
