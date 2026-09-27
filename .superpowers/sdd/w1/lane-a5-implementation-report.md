# W1-A5 implementation receipt ? qualification pending

Source freeze: `ac9d1de4c68615fe6e9850b8139853d54d0b6943`, pool-4, branch
`feat/w0-replay-integration-codex-20260925`. Original contract: production swarm
plan W1-A5 (A-07/A-08/A-19). This is implemented source with postimplementation
fixtures, **not a completed W1-A5 acceptance gate**. No C++ configure/build/test,
large benchmark, market-data read, download, or warehouse access was performed.

## Implemented behavior

- `library/corr_index.hpp`: `SignedHammingV2` uses 256 seeded Gaussian projection
  bits, tests both signed Hamming distances, and exact-scores recalled PnL rows.
  The threshold follows the caller's actual absolute-correlation floor with a
  3.5-sigma binomial margin. Missing/nonfinite observations bypass approximation
  because imputation does not bound pairwise-complete correlation. This is a
  probabilistic O(pool-size) signature scan, not a sublinear or measured-recall claim.
  Explicit `LegacyBandsV1` preserves the original K<=64 banding/signature recipe.
- `factory/pool_view.hpp` (authorized adjacent hunk): continuous redundancy
  exact-scores the top 16 signed-Hamming neighbors plus the .7 shortlist. It does
  not use the raw maximum noisy sketch score as fitness and does not claim a global
  maximum. Hard gates now pass their actual threshold explicitly. Legacy mode
  preserves its old shortlist behavior.
- `library/record.hpp`: V2 records contain expression/provenance, f32 PnL,
  up to 256 int16 sketch values, and theme/family/horizon/tau/residualization plus
  context/position/sketch recipe hashes and sketch scale. Empty sketch means absent;
  none is invented from PnL. No dense positions section exists in V2. The V1 writer
  remains version 1 and retains f64/position bytes. Reader validation checks section
  arithmetic, directory/provenance bounds, CRC and V2 numeric domain before views.
- `library/store.hpp`: V2 has a compact memtable, lazy owning f64 compatibility
  rows, and on-demand positions through an explicit recipe/context resolver.
  `positions_checked` returns owning weights or a real error. The legacy span API
  uses a single V2 resolver buffer, invalidated by the next positions call; its
  inability to return an error retains the existing fail-fast API convention.
  `LibraryStore::open` supplies a checked attach alternative.
- Immutable all-current-alpha time slabs extend T without rewriting base files.
  Later admissions carry full extended history; reopen validates chronological
  slab coverage, base/slab catalog CRCs and geometry. Appended-period holdings
  require that slab's explicit context/recipe metadata; omitted metadata permits
  PnL append but fails holdings retrieval. Admission metadata is never assumed to
  authorize a future context. The facade rebuilds the index after extension.
- Segment/slab publication is exclusive creation (`wbx`, checked write/flush/close).
  A stale handle or orphan collision fails without truncating any existing file.
  Failed writes can leave unreferenced orphans requiring explicit recovery.
- Rule/projection-version/bit-count/seed are durable catalog metadata. Default
  reopen uses the saved rule and seed; metadata-less populated V1 libraries retain
  legacy behavior (manifest seed where available). Explicit changes require
  migration. V2/signed-recipe and appended-slab identities enter manifest record
  CRCs. Old dense/V1 snapshots retain their original segment CRC identity.
- `library/lifecycle.hpp`: adds Admitted->Dead, retains already-existing
  Decaying->Live, and rejects backdated/out-of-i64 transitions. As-of reads clamp
  oversized upper bounds safely. Existing enum numbers/journal rows are unchanged.

## Compatibility and remaining production integration

`LibraryStorageOptions` defaults to existing mode, or dense V1 for a new library
when old callers supply no options. Selecting `CompressedV2` is explicit; reopened
V2 stores recover that policy automatically. This avoids silently breaking existing
holdings consumers. A new V2 call supplies instrument count, actual candidate
metadata/sketch and a resolver bound to its original evaluation recipe/context.
Finite PnL outside f32 range is rejected; V2 intentionally rounds f64 to f32 and
makes no NaN-payload bit-preservation promise. V1 retains old numeric bytes.

Production compression adoption remains outstanding: admission metadata/sketch
producers and resolver installation must be wired through factory/library-opening
consumers. Current relevant opens are `atx-impl/src/stage_discover.cpp`,
`stage_sweep.cpp`, `stage_combine.cpp`, `stage_report.cpp`, `stage_metabook.cpp`, and
`dead_alpha_wire.hpp`. `risk/dead_factor.hpp` immediately copies each positions row,
so its existing span use does not retain two V2 buffers; it still requires a valid
resolver before a compressed library is used. No blanket resolver or fake holdings
fallback was added. The default seed compatibility correction covers existing empty
seed read-only opens and differing per-run search seeds without changing projection
identity. No additional impl caller changes were made in this lane.

## Postimplementation checks authored, not run

Owning target: `atx-engine-library-tests`. Source changes affect existing
`library_corr_index_test.cpp`, `library_lifecycle_test.cpp`, and new
`library_compressed_test.cpp` (12 fixtures). There are **16 new focused cases**.
Suggested bounded filter:

```
LibraryCorrIndex.SignedV2*:LibraryCorrIndex.MissingOverlapBypassesApproximateScreen:LibraryLifecycle.AdmittedCanRetireAndBackdatingCannotRewriteHistory:LibraryCompressed.*
```

Coverage: both signs near |rho|=.700001, V1 signature reproduction, sparse overlap,
lifecycle causality, f32/sketch/metadata roundtrip, one-record T5000 shape/size,
valid-CRC malformed records, checked resolver failure, appended contexts, later
admission and reopen, immutable publication conflicts, catalog CRC replacement,
recipe/seed migration, snapshot binding, index rebuild, invalid first admission,
and exact-score refinement for moderate correlation. New temporary fixtures use
exclusive unique directories and only clean up paths they created. Existing V1
roundtrip/store and PoolView regression targets remain required owning checks.

Independent static review by the G0 agent identified two storage blockers:
truncating publication and missing base-catalog CRC verification. Both were fixed
in `effa293e`, with discriminating fixtures. No final independent approval or
compiled/runtime result is claimed. `git diff --check` passed for the frozen edits.

## Original acceptance still open

| Requirement | Current evidence |
|---|---|
| 10k synthetic alphas, T5000, recall >=.99 at abs(corr)>=.7 including negatives | Unrun; 128-query small fixture authored only |
| Query p99 <=5ms | Unmeasured; no performance claim |
| Disk <=1MB/alpha at target scale | Unmeasured; one-record size fixture authored, no universal record-byte cap |
| Period append works | Production API implemented; focused tests not yet executed |
| Lifecycle graph | Required edges implemented; focused tests not yet executed |
| DAG gate | Pending shared causality adapter and owning qualification |
| Real production V2 adoption | Metadata/sketch producer and context-bound resolver wiring still required |

No old requirement was waived or replaced by the smaller fixtures. Dense V1 input
artifacts remain readable; explicit compression/index migration is an observable
recipe change. Header-only implementation adds no CMake/source registration work.

Ordered commits: `d9925eec` (signed index/lifecycle), `10f0d46e` (V2/append/API),
`1aa13cc9` (identity/failed-stage geometry), `9cbbb963` (fixtures), `e182094e`
(saved identity validation), `effa293e` (publication/CRC/checked attach),
`46e07b1a` (default reopen seed compatibility), `ac9d1de4` (slab context binding).
