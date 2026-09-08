# Improvements

Assessment of the current codebase, derived from a full read of the build, serve, and
update paths (see `DESIGN.md` for architecture, `REWRITE_SPEC.md` for exact contracts).

**Meta-point:** the core architecture is sound and scales well — per-artist-scoped
fuzzy indexes, denormalized SQLite, hierarchical matching. It does *not* need
rearchitecting. The serious problems are concentrated in the **update path's
correctness**, the **absence of any matching-accuracy measurement**, and
**operability**. Names-only matching is the biggest opportunity but is an enhancement,
not a bug.

**Confidence note:** items marked ✅ were verified directly in the source this session.
Items marked 📊 are reasoned from code but **not measured** (no end-to-end run or
profiling was done) — measuring them is itself part of I8.

---

## Top 10 (by priority)

### 1. Fix update coherence — the server serves stale data ✅
**Severity: critical (correctness).** `update` writes `mapping.db` hourly, but the
read-only server's `IndexCache` has **no invalidation** (`index_cache.hpp`: `get`/`add`
only add; `trim()` fires only when the cache is full, for memory reclamation). A cached
artist index is served stale indefinitely until it happens to be LRU-evicted or the
server restarts. No signal, no versioning, no reload. A long-running server drifts
further from the DB over time.
**Fix:** bump a generation/version counter in `update_metadata` per changed artist;
server checks it on cache hit and invalidates/reloads. Or build-and-swap a new DB file
with an atomic reopen (SIGHUP / health-gated rolling restart). Enabling SQLite WAL
helps reader/writer concurrency but does **not** fix the in-memory staleness.

### 2. Add a golden accuracy test + a match-quality metric ✅
**Severity: high (you can't safely improve anything else without it).** Matching
accuracy — the entire point of the system — is effectively untested. `test_cases.hpp`
exists but there is no golden `(artist, release, recording) → {MBIDs}` regression set
wired to the hard cases in `PROBLEMS.txt`, and no precision/recall measurement.
**Fix:** golden set in CI on a minimal DB (`ac.id IN (1160983, 49627, 65, 21238)`);
track match rate / precision / recall as a first-class metric so every change is
measurable. Prerequisite for I3, I6, I7.

### 3. Add duration + ISRC matching signals 📊
**Severity: high (biggest accuracy win).** Matching is names-only — the root cause of
every collision case in `PROBLEMS.txt`. ISRC gives a direct-key fast path (exact
recording resolution); duration is a soft tiebreaker for same-named recordings
(studio/live/remix). Both optional and backward-compatible.
**Fix:** see `REWRITE_SPEC.md` §14 — ISRC side table + `recording_length` column,
ISRC fast-path before fuzzy, duration as a bounded re-rank term in `find_match`.

### 4. Reconcile the canonical-release build/update inconsistency ✅
**Severity: high (silent data drift).** Full build (`canonical_release.hpp`) keeps
**all** releases per group (dedup by release_id); the incremental updater
(`canonical_release_updater.hpp`) keeps only **one** (`ROW_NUMBER()=1`). Any release
group touched by an update silently changes shape, degrading release matching for
recently-edited tracks.
**Fix:** make both paths keep all releases (search relies on multiple releases per
group for the `'l'` fuzzy release match and link `rank`).

### 5. Fix the partial-write hazard in the update batch ✅
**Severity: high (correctness).** `update.cpp` commits the `mapping` update and the
`index_cache` update as **separate** transactions (it must, to build indexes from
committed rows in between). A crash between them leaves `mapping` updated but the index
stale for that batch, with no repair path.
**Fix:** stage to temp and swap in one transaction; or record per-batch progress and
make the pass resumable/idempotent so a re-run heals partial state.

### 6. Length-adaptive thresholds ✅
**Severity: medium-high (accuracy).** `artist=0.7, release=0.3, recording=0.7` are
hardcoded and acknowledged wrong for short strings (`search.hpp` TODO; Nilsson/Godspeed
cases in `PROBLEMS.txt`). Short names need stricter checks.
**Fix:** thresholds as a function of encoded length. **Requires I2** to tune safely —
do not tune blind.

### 7. Real JSON API + versioning ✅
**Severity: medium-high (usability/correctness of contract).** The server serves HTML
from `/`; the load test (and presumably clients) expect `GET /mapping/lookup` returning
structured data — an endpoint the server does not implement. No JSON, no versioning, no
documented contract.
**Fix:** `GET /1/mapping/lookup?artist=&release=&recording=` → JSON with the
`SearchMatch` fields + a `match_source` field. Aligns server, load test, and clients.

### 8. Observability / monitoring 📊
**Severity: medium-high (operability).** `TODO.txt` literally lists "add monitoring."
Today only per-thread HTTP status counters exist. No match rate, confidence
distribution, cache hit rate, latency percentiles, or update lag — so you cannot tell
whether a change improved or regressed matching.
**Fix:** metrics endpoint (Prometheus-style): match/no-match rate, confidence
histogram, cache hit rate, p50/p95/p99 latency, update lag (now − `last_updated`).

### 9. Serving-side SQLite tuning ✅
**Severity: medium (cheap, high-value performance).** The read-only server sets **no
PRAGMAs** — the 8 GB `mmap_size` / 2 GB `cache_size` tuning exists only in the build
path (`make_mapping`). Warm reads are left slower than necessary.
**Fix:** on each read-only connection: `PRAGMA mmap_size=<large>`,
`cache_size=<negative KB>`, `query_only=ON`, `temp_store=MEMORY`. Confirm via
`EXPLAIN QUERY PLAN` that `... ORDER BY score LIMIT 1` uses `recording_id_ndx`.

### 10. Harden and pin the build ✅
**Severity: medium (reproducibility/maintainability).** The build vendors a large,
unpinned dependency stack (nmslib, armadillo, pcre2, jpcre2, unidecode, cereal, Crow,
asio, standalone libpq, SQLiteCpp, Catch2), `sed`-patches a dependency's CMakeLists to
disable tests ("a horrible hack"), refuses to run if `build/` exists, and pins nothing
(`# TODO: Pin all dependencies`). Fragile and hard to reproduce.
**Fix:** pin all dependency versions; remove the sed hack (patch upstream or use a
proper option); make reconfigure non-destructive. (A Rust rewrite collapses most of
this — see `REWRITE_SPEC.md`.)

---

## Full list

### A. Correctness & reliability
- **A1.** Update coherence / cache invalidation (= Top 1). ✅
- **A2.** Canonical-release full-build vs incremental inconsistency (= Top 4). ✅
- **A3.** Partial-write hazard across mapping/index_cache transactions (= Top 5). ✅
- **A4.** Floating-point determinism: tie handling relies on float equality + epsilon
  (`PRECISION_FIXES.txt`); `-Ofast`/`-ffast-math` in deps changed surviving results.
  **Fix:** canonicalize tie handling — stable sort by id, integer-quantized scores —
  rather than float-equality + epsilon. ✅
- **A5.** `score` semantics are positional (`mapping.score` = `canonical_release.id`
  SERIAL, assigned across two VA/non-VA passes). Any reproduction must preserve
  insertion order or define a stable alternative; fragile and undocumented. ✅
- **A6.** Empty/degenerate source rows (`TODO.txt` "empty rows in source mapping"). ✅
- **A7.** No per-thread crash isolation (`TODO.txt` "thread level segfault isolation?"):
  one bad request can take down a worker. ✅

### B. Matching accuracy
- **B1.** Golden accuracy test + precision/recall metric (= Top 2). ✅
- **B2.** Duration + ISRC signals (= Top 3); AcoustID later for Picard
  (`REWRITE_SPEC.md` §14). 📊
- **B3.** Length-adaptive thresholds (= Top 6). ✅
- **B4.** First-match-with-backtracking may not return the globally best match; revisit
  whether best-of-candidates (with signals) is more accurate — but only with B1 in
  place. ✅
- **B5.** unidecode fidelity: transliteration affects non-ASCII matching; document and
  test the table (esp. for any reimplementation). ✅

### C. API & operability
- **C1.** Real JSON API + versioning (= Top 7). ✅
- **C2.** Observability / metrics (= Top 8). 📊
- **C3.** Load test targets a non-existent endpoint and no benchmark results are
  recorded; align harness with the real API and capture baselines
  (`DESIGN.md` "Performance"). ✅
- **C4.** No documented deployment/reload story for picking up `update` writes (ties to
  A1): define swap/restart/invalidation operationally. ✅
- **C5.** `503`-while-loading is handled, but there's no readiness/health/liveness
  endpoint contract beyond the HTML root. ✅

### D. Performance & scaling
- **D1.** Serving-side SQLite PRAGMAs (= Top 9). ✅
- **D2.** Cache behavior under uniform (non-skewed) traffic thrashes; blob
  deserialization on miss is the real latency spike — measure and consider a
  larger/tiered cache or lighter blob format. 📊
- **D3.** `make_mapping` in-memory dedup set is linear RAM (multi-GB at 38M+ rows);
  move to on-disk/streaming dedup as the catalog grows. ✅
- **D4.** Wholesale artist-index rebuild every update pass (~3 min; nmslib has no
  incremental update) — consider an index engine that supports incremental updates, or
  rebuild off-line and swap. ✅
- **D5.** Storage sizing is only estimated (~20–50 GB); measure via `dbstat`
  (`REWRITE_SPEC.md` §12.2). 📊

### E. Build & maintainability
- **E1.** Pin dependencies + remove the sed/CMake hacks (= Top 10). ✅
- **E2.** Manual C-style resource management (raw `new`/`delete`, manual `PQclear`,
  `reinterpret_cast` on nmslib internals) — leak/error-prone; debug build needs ASan.
  Wrap in RAII / smart pointers. ✅
- **E3.** The search FSM (`fsm.hpp`) is clever but opaque: function-pointer tables,
  subtle backtracking/first-match invariants, undocumented in-code. It's also where
  thresholds/signals/scoring converge (highest-churn code). Document invariants; ideally
  restructure into an explicit, testable candidate-ranking pipeline. ✅
- **E4.** Sparse in-code documentation; `DESIGN.md`/`REWRITE_SPEC.md` (added) help, but
  the matching internals still lack rationale comments. ✅
- **E5.** `TODO.txt` items: thresholding, empty rows, thread isolation, monitoring,
  zero-length index handling — fold into tracked issues. ✅

### F. Enhancements (not bugs)
- **F1.** AcoustID lookup for Picard integration (`REWRITE_SPEC.md` §14.1/§14.3). 📊
- **F2.** Additional weak signals: year, label, track number, as re-rank terms. 📊
- **F3.** Rust reimplementation (`REWRITE_SPEC.md`) — collapses the dependency/build
  fragility (E1/E2) and lets A1/A3/E3 be designed correctly from the start. 📊

---

## Suggested sequencing
1. **A1** (coherence) — active correctness bug.
2. **B1** (golden test + metric) — unblocks safe iteration on everything else.
3. **B2** (duration + ISRC) — biggest accuracy win; short-circuits collision cases.
4. **A2 + A3** (canonical inconsistency + partial write) — stop silent data drift.
5. **C1 + C2 + D1** (JSON API + observability + serving PRAGMAs) — make it operable and
   measurable.

Then iterate on B3/B4 (thresholds) with the metric in place, and treat E1–E3 / F3 as
the maintainability track (or the trigger for the Rust rewrite).
