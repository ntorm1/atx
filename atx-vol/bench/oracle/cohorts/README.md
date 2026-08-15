# Oracle cohorts

Cohort JSONs select slices of the parquet oracle store
(`C:\atx-cache\oracle\spiderrock\date=YYYY-MM-DD\bucket_et=HHMM\`) for
`atx-vol-oracle-bench`. Filled in by bootstrap stage 1 from
`oracle_manifest_<date>.json` (ingest prints the top-liquidity underliers).

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

Rules (enforced by reviewers, not code):

- `smoke` — 1 liquid underlier × 1 bucket (~10-30k rows). Inner-loop speed: seconds.
- `tune` — ~10 underliers × 3 buckets, stratified: mega-cap ETF, index-style, high-div
  single names (dividend stress), hard-to-borrow names (sdiv stress), a low-liquidity
  tail name. The attribution stage reads ONLY smoke/tune results.
- `holdout` — disjoint from `tune` in BOTH underliers and buckets. Never read by
  vol-analyst; only the Ratchet stage touches it. Changing holdout membership after
  iteration 0 invalidates ratchet history — don't, without a ledger line explaining why.
- `fullday` — everything; periodic sweep, not in the iteration loop.

Bucket values are ET HHMM strings matching the partition dirs (9:30 slice never exists —
dropped at ingest).
