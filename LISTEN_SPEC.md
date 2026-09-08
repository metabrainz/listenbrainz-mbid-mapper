# ListenBrainz listen & mapping-lookup spec

Reference for the **listen-side consumer** of the mapper. Verified against
`listenbrainz-server` source (`listenbrainz/webserver/views/api_tools.py`,
`.../metadata_api.py`, `listenbrainz/mbid_mapping_writer/mbid_mapper.py`). Companion to
`RUST_PLAN.md` (which defines the mapper we intend to build) and `REWRITE_SPEC.md`.

## 1. What a listen looks like (submission format)

Source: `validate_listen` / `validate_basic_metadata` in `api_tools.py`.

Top level — **only** two keys (`listened_at` may be absent for `playing_now`):
```jsonc
{
  "listened_at": 1699999999,     // unix seconds; must be > 1033410600 (2002-10-01)
  "track_metadata": { ... }
}
```
Limits: `MAX_LISTEN_SIZE = 10240` bytes/listen; `MAX_LISTENS_PER_REQUEST = 1000`.

`track_metadata`:
- **Required:** `track_name`, `artist_name` (non-empty strings, stripped).
- **Optional:** `release_name` (dropped if empty).
- **`additional_info`** (all optional, free-form; validated keys below):
  - `duration` (seconds) **or** `duration_ms` — **not both**; positive int; ≤ 24 days.
  - `tags` — ≤ 50 items, each ≤ 64 chars.
  - Single-MBID hints: `recording_mbid`, `release_mbid`, `release_group_mbid`,
    `track_mbid`.
  - Multi-MBID hints: `artist_mbids`, `work_mbids`.
  - Commonly present but not schema-enforced: `isrc`, `spotify_id`,
    `submission_client`, `music_service`, `origin_url`, etc.

Example:
```jsonc
{
  "listened_at": 1699999999,
  "track_metadata": {
    "artist_name": "Portishead",
    "track_name": "Glory Box",
    "release_name": "Dummy",
    "additional_info": {
      "duration_ms": 301000,
      "isrc": "GBAAA9400304",
      "recording_mbid": "…",        // sometimes present (client already knew it)
      "artist_mbids": ["…"]
    }
  }
}
```

## 2. Reality of listen data (why this is the low-signal consumer)

- The **only guaranteed** fields are `artist_name` + `track_name`. `release_name` is
  frequently missing. This is the minimum the mapper must handle (`RUST_PLAN.md` §0/§9).
- `additional_info` is a grab-bag: `duration`/`duration_ms` and `isrc` are *sometimes*
  present (Spotify/Last.fm/desktop scrobblers vary widely); MBID hints appear when the
  submitting client already resolved them.
- Values are noisy: typos, wrong/º missing release, "feat." suffixes, remaster tags,
  non-Latin scripts, unicode — exactly the degradations Picard's eval harness models
  (`RUST_PLAN.md` §8).

## 3. Current mapping-lookup contract (what to be compatible with)

Source: `GET /1/metadata/lookup/` in `metadata_api.py`.
- **Inputs:** `artist_name` (required), `recording_name` (required), `release_name`
  (optional). Total chars of the three ≤ `MAX_MAPPING_QUERY_LENGTH`. Requires an auth
  token.
- **Notably: names only.** The current endpoint does **not** use `duration`, `isrc`, or
  any MBID hint for matching — even though listens often carry them.
- **Output** per match: `recording_mbid`, `release_mbid`, `artist_mbids` (list),
  `recording_name`, `release_name`, `artist_credit_name` (+ optional extra `metadata`).
- **Backend today is typesense**, not the C++ mapper: collections
  `canonical_musicbrainz_data_latest` / `…_release_latest`. Query prep
  (`mbid_mapper.py::prepare_query`): `unidecode(strip [^\w ]+, collapse spaces, strip,
  lower)`. Match tiers by Levenshtein edit distance:
  `exact / high (≤2) / med (≤5) / low / no_match`.

## 4. Implications for the Rust mapper (RUST_PLAN)

1. **The listen path is genuinely name-only in practice**, so robust
   `artist_name + track_name [+ release_name]` fuzzy matching is the baseline that must
   be excellent — most listens will never provide more. (Reinforces `RUST_PLAN.md` §0
   low-signal regime and §6 pipeline.)
2. **`duration`/`isrc`/MBID hints in `additional_info` are an unused opportunity.** The
   current endpoint ignores them; our progressive-precision API (`RUST_PLAN.md` §9)
   should *accept and use* them when present — this is a strict improvement for the
   subset of listens that carry them, at zero cost to those that don't. Note ListenBrainz
   uses `duration`/`duration_ms` and `isrc`; the mapper API should accept both duration
   forms (normalize to ms) and map `isrc` straight through.
3. **MBID hints (`recording_mbid` etc.) are a free fast-path** stronger than fuzzy: if a
   listen already carries a valid `recording_mbid`, verify + return it directly (like the
   ISRC/AcoustID short-circuits). Consider adding `recording_mbid`/`release_mbid` as
   optional lookup inputs alongside isrc/duration/acoustid.
4. **Compatibility target for a drop-in replacement:** field names
   (`artist_name`/`recording_name`/`release_name` in, `*_mbid`/`*_name`/`artist_mbids`
   out) and the auth + length-limit behavior of `/1/metadata/lookup/`. A v1 JSON API
   (`RUST_PLAN.md` §9) can mirror these names so ListenBrainz can switch backends with
   minimal change, then add the optional richer inputs.
5. **Batch matters:** listens arrive in batches (≤1000/submit) and the mbid-mapping
   writer processes them in bulk — the `POST` batch variant (`RUST_PLAN.md` §9) is the
   right shape for this consumer, not one-request-per-listen.
6. **Encoding parity check:** the current listen-side `prepare_query`
   (unidecode + `[^\w ]+` strip + collapse spaces + lower) differs slightly from the C++
   mapper's `encode_string` (which *removes* spaces too). Worth reconciling — the Rust
   encoder (`REWRITE_SPEC.md` §4) should be the single source of truth and both
   consumers use it.

## 5. Building the listen-side eval corpus (RUST_PLAN §8)

- Derive a name-only corpus from real listens: `{artist_name, release_name?,
  track_name [, duration_ms, isrc]} → expected {recording_mbid, release_mbid,
  artist_mbids}`. The mbid-mapping writer's stored matches (`matcher.py`) and dumped
  listens (`dump_listenstore.py`, which already carries
  `l_recording_name`/`l_release_name`/mapped `recording_mbid`) are a natural source of
  labeled pairs.
- Include: missing-release listens, "feat." suffixes, non-Latin, unicode, and the
  `PROBLEMS.txt` hard cases.
- Report listen-side **recall** prominently (most listens are name-only; recall is the
  pain point), separately from Picard-side **precision**.
