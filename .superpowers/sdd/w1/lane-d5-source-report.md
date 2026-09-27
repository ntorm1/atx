# W1-D5 source implementation: QA-v2 and common-stock universe V2

Source implementation only; D-17/D-18 historical acceptance remains open. No
market payload, warehouse, download, migration, 2018 rebuild, or C++ compilation
was performed. No coverage, byte-parity, speed, or tradeable-alpha claim is made.

## QA-v2

`prepare_tickerhistory.py` now has explicit `--qa-rule qa-v2|legacy-v1`; the CLI
defaults to QA-v2, while the existing Python API with no rule/dates retains V1.
The recovered 19-date allowlist is embedded with canonical SHA-256
`0589dc9ae5c96e68d183820f4733ade7df94245d805e285e43e1bdf29c0fef60` (provenance in
`d1-d5-case-provenance.md`). V2 derives the exact in-window subset, rejects any
different explicit list, and refuses a window reaching 2020. This is an input
policy, not evidence that the partial historical rebuild finished.

Only a sole `ohlc_order_violation` on those dates is rescued. Original raw close,
volume, factor, ID and all other fields must already pass QA; duplicate IDs,
invalid closes/factors/volume and additional rejection reasons remain quarantined.
Only O/H/L cells become empty (native NaN), preserving all other row bytes and line
endings. Unaffected accepted ZIP bytes remain deterministic; the tiny synthetic
2013 fixture matches V1 exactly. Real 2013-2015 segment parity is unmeasured.

The preparation policy is truthfully `tickerhistory-qa-v2`, with separate changed
and unchanged counts and byte-preservation declarations. Manifest publication
remains last. The authorized adjacent `stage_data_provenance.cpp` dispatch validates
V2 allowlist/hash, blanked fields, exact rescue reason, date/count arithmetic and
accepted archive binding; the V1 path retains its prior interpretation. Ingestion
and downstream panel receipts preserve the actual QA policy version.

## Dated instrument-type projection

The actual stage defaults to `--universe-rule common-stock-v2` and requires
`--instrument-types <JSON>`. Explicit `legacy-v1` refuses a type input and retains
the old numerical recipe. The engine library still defaults to LegacyV1 for
existing direct callers. These two narrow `RunConfig`/parser changes were
authorized by root, as was the provenance consumer seam.

Input schema `atx-instrument-types-v1` requires `status=complete`,
`sealed_end_exclusive=2020-01-01`,
`clock_rule=verified-publication-strict-before-session`, and
`validity_rule=endpoints-known-at-source-clock`. `sources[]` contains unique
`id`, original `sha256`, and `locator`. `rows[]` contains:

- `security_id`, `valid_from_ns`, exclusive `valid_to_ns`,
  `source_published_at_ns`, `available_at_ns` as exact signed integers;
- `instrument_type`: unknown/common-stock/etf/adr/preferred/fund/reit/
  limited-partnership/other; `source_kind`: vendor/sec;
- `source_id`, `evidence_locator`, and separate `evidence_status`,
  `availability_status`, `vintage_status` (verified/unverified);
- `endpoints_known_at_source_clock`: true for qualified interval evidence.

The source hash binds the retained original proof, not proof authenticity. A current
vendor category, SIC, FSDS issuer row, or today's company identity cannot establish
historical security-line type by itself. Producers must supply actual original
dated evidence; this lane adds no inference or claim of acquired type coverage.
Both finite endpoints must have been known in the source payload under its own
clock. A later discovered expiry must not silently truncate an old type interval.
Contradictory public overlapping evidence fails closed; no last-row-wins override
or revision backfill is introduced.

Reads are bounded to 16 MiB, 65,536 source/row entries; the builder copies and sorts
the immutable projection once. Strict `available_at < rank_session_key` applies,
with publication no later than availability and all clocks/intervals pre-2020.
An unavailable row never qualifies membership. No verified evidence at all fails
construction. Unknown and unverified types cannot be silently promoted to common.
At least one verified known type is needed per eligible line; all available
qualified types must agree, and unknown/unverified classification records block
trading only when their publication/vintage/validity clock is independently
verified and strictly earlier. Unknown-clock records remain unavailable and cannot
suppress an existing qualified common-stock record. If no qualified record exists,
the line remains excluded; no unknown-clock record is promoted into common stock.
Type categories REIT and LP are explicitly excluded in this narrow common-stock
policy; they are not silently treated as ordinary common-stock evidence.

V2 applies inclusive raw close >=$5 and 63-session median dollar ADV >=$5,000,000,
retaining 57 valid observations, rank ties, bands and next-session effectiveness.
The engine observe/rebalance paths retain their preallocated storage contract;
type lookup is per-ID binary search plus that ID's bounded evidence records.

## Artifacts and consumer compatibility

V2 publishes the exact validated `instrument_types.json` bytes,
`excluded_instruments.csv`, and `exclusion_counts_by_year.csv`, in addition to
existing membership/churn/coverage/union outputs. Exclusions carry joint reason
bits, the observed type, source-row reference where qualified, clocks, price/ADV,
and the complete projection hash. All source records for that ID are recoverable
from the copied projection. Unknown/unverified trading exclusions must not be used
to shrink D1's fixed identity-coverage denominator.

`ATXPITU2`/version 2 adds rule, inclusive price, ADV floor and type-projection
SHA-256 to membership framing. `ATXPITU1` bytes/decoding remain explicit V1;
existing membership consumers use the same dispatched decoder. V2 decoder checks
recipe, bounded cuts, dates, IDs/ranks, framing and checksum. The new
`atx-equity-universe-v2` manifest/domain, request config and non-trial ledger recipe
bind the actual rule and evidence; manifest publication remains last. No ledger
schema/global ledger file was changed or executed. The cp15 design-note parent is
retained as historical provenance, and the V2 ledger note explicitly supersedes
its eligibility recipe.

## Checks and remaining gates

Implementation preceded the fixture additions. Pure synthetic Python QA checks:
**7/7 passed, 0.370 s**, log `pool-3/build-equity/d5-qa-synthetic-tests.log`.
`git diff --check` passed. Owning C++ fixtures added (uncompiled/unrun): four
builder cases for floors/types/strict clocks/expiry/future evidence/codec; one
configuration case; one actual-stage type-to-membership/excluded-manifest case;
one native QA-v2 provenance/load case including false-claim rejection. Existing
stage fixtures explicitly select legacy-v1.

Root owns independent review and the next focused compilation. Historical type
projection acquisition/verification, the actual 19-session repair, 2013-2015
segment byte comparison, full membership identity where filters do not bind,
historical churn/coverage and the rebuilt 2018 context for 252-session families
all remain unrun. No CMake or original DAG/global ledger edit is included here.

## Independent-review corrections after a5250e25

The auditor identified two source blockers; this follow-up implements both for
independent re-review, without compilation or new numeric runs:

- QA-v2 daily accepted and unchanged totals must fit their global counts. Each
  addition is bounded by the remaining global count before addition, preventing
  overflow. Fixtures reject a one-row manifest claiming 100 daily accepted rows,
  a maximum-u64 claim, and individually small daily claims whose sum is too large.
- `PitInstrumentTypeEvidence::clock_verified` separates publication/vintage/known
  validity endpoints from classification quality. A qualified classification
  requires a verified positive clock; an unverified clock cannot acquire an
  epoch-zero exclusion effect. Builder fixtures cover mixed qualified/undated
  rows, a lone undated line, and dated ambiguous evidence at/beyond the strict
  publication boundary. The actual-stage fixture appends contradictory rows with
  unknown availability, vintage or endpoint timing and preserves qualified
  membership. The artifact recipe now states this distinction explicitly.

All seven owning C++ cases remain uncompiled/unrun here; V1 source paths and the
prior seven Python QA checks are unchanged. Historical acceptance remains open.
