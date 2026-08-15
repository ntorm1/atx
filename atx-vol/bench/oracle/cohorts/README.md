# Oracle cohorts

Cohort JSONs select slices of the parquet oracle store
(`C:\atx-cache\oracle\spiderrock\date=YYYY-MM-DD\bucket_et=HHMM\`) for
`atx-vol-oracle-bench`. Bootstrap stage 1 fills them from the aggregate ingest
manifest and top-liquidity list.

Schema per file:

```json
{
  "name": "smoke",
  "dates": ["2026-08-14"],
  "underliers": ["<tk>", "..."],
  "buckets_et": ["1000", "1330"],
  "notes": "why these were chosen"
}
```

Rules (enforced by the capability gate and reviewers):

- `smoke`: one liquid underlier x one bucket (roughly 10-30k rows); seconds-level
  inner loop.
- `tune`: roughly ten underliers x three buckets, stratified across liquidity,
  dividend, borrow, moneyness, and DTE. Measure and Analyst read only aggregate
  smoke/tune results.
- `holdout`: disjoint from tune in both underliers and buckets. Stage 1 writes
  `holdout.sha256`, the SHA-256 of canonical sorted `dates`, `underliers`, and
  `buckets_et`. Every run freezes that hash at Capability and verifies it before
  Ratchet. Analyst never receives its hash, membership, scorecard, or rows.
- `fullday`: everything; periodic sweep only, outside the iteration loop.

Changing holdout membership after stage 1 invalidates ratchet history and is a
hard gate failure, not an ordinary iteration. A deliberate replacement requires
operator approval and a ledger entry explaining why.

Canonical hash encoding is UTF-8 JSON with no BOM or trailing newline, fixed key
order `dates`, `underliers`, `buckets_et`, each array deduplicated and sorted by
ordinal code point, and compact separators (Python `json.dumps(obj,
ensure_ascii=True, separators=(',', ':'))`). `holdout.sha256` contains one
lowercase hex digest plus a newline. Notes and JSON formatting are deliberately
excluded, so comment-only edits do not change membership identity.

Bucket values are ET `HHMM` strings matching partition directories. The 09:30 ET
slice never exists because ingest drops it.
