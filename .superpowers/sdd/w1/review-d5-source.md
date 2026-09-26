# Independent W1-D5 source review

Reviewed frozen `a5250e257f150d478a69888316f8e381b238e37a` from Git objects,
including all 13 changed files and the owning report. No build, real-data read,
acquisition or historical membership reconstruction was performed.

Verdict: **changes requested**, limited to the two findings below. No broader
architecture changes or extra historical runs are requested.

## Findings

1. **Causal exclusion clock (blocker).** `bind_types` combines evidence,
   availability and vintage verification into one `verified` bit. The engine
   accepts zero clocks for an unverified row, then permits that row to set
   `PitTypeUnverified` when its asserted `available_at < rank_session_key`.
   Appending a record whose publication clock is not established can therefore
   exclude a line that already had verified common-stock evidence at an earlier
   decision. An unknown clock must not acquire an epoch-zero exclusion effect.
   Keep clock qualification separate: either reject such a projection explicitly
   or leave unknown-clock rows unavailable. Uncertain type evidence may still
   conservatively block under a verified publication/vintage clock. Add a mixed
   prior-common plus unknown-clock record regression; the current lone-unverified
   fixture cannot distinguish this case.

2. **Impossible daily QA totals (provenance defect).** The native QA-v2 consumer
   checks per-date `accepted = accepted_v1 + rescued` and the sum of rescued rows,
   but not the sum of accepted rows against global accepted/unchanged totals.
   Global `accepted=1, changed=1, unchanged=0` with daily
   `accepted_v1=99, rescued=1, accepted=100` passes those checks. Add an
   overflow-safe cumulative daily accepted/global bound (and corresponding
   unchanged bound, or its equivalent) plus this mutation fixture. This defect
   falsifies count provenance; it does not change the loaded close values.

Both findings were sent to the owner and root before any source integration
approval. Fix-only review and C++ execution remain pending.

## Reviewed portions without another blocker

- QA-v2 derives the exact pinned in-window 19-date allowlist, refuses a window
  reaching 2020, rescues only a sole OHLC-order failure, and blanks only O/H/L.
  Duplicate IDs and malformed close/volume/factor remain quarantined. Unaffected
  row bytes and line endings are retained; the native receipt carries V2 rather
  than labeling modified rows as V1. Accepted archive hashes remain bound.
- Available, qualified type records are selected by their valid interval and
  strict publication boundary. Conflicting public types fail closed. Verified
  future records do not remove a previously eligible earlier member. Known finite
  endpoints are asserted as part of the original source payload; the code does not
  backfill a later expiry into an older interval. Source hashes bind assertions,
  not authenticity or acquired historical coverage.
- Actual stage defaults require common-stock-v2 and a bounded, copied type
  projection. The engine's direct-call default stays LegacyV1; the old stage is
  explicitly selectable. V2 uses inclusive $5 and $5m floors while retaining
  rank/band/next-session mechanics. Unknown/non-common types are not promoted.
- V2 framing binds rule, floors and type-projection hash; V1 encoding and arithmetic
  remain in explicit branches. New exclusions and copied projection are hashed in
  the manifest; span lifetime through next-session publication is preserved.

The owner's seven synthetic Python passes are reported evidence, not an independent
runtime rerun here. New C++ checks are still unexecuted. The real 19-session repair,
2013-2015 byte parity, acquired dated type coverage, rebuilt 2018 context and full
D-17/D-18 acceptance remain open. This review makes no alpha or tradeability claim.
