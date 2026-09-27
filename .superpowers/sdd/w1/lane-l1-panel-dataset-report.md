# W1-L1: streamed rank/residual dataset and bounded learner consumer

Original lane: L-04/L-05 in the 2026-09-24 DAG. Implementation first, followed
by five synthetic owning cases. Production `603e7e9c`, presence/identity repair
`e672bcd8`, checked recipe/PCA admission `6cf937f8`; initial fixtures `342563f6`.
This report's commit adds the small future-PCA assertion. Root alignment merge
`3d73b90c` preserves frozen DateV2 CPCV and D6 source corrections. **No C++ build,
runtime, real-data evaluation or scale run was performed by this lane.**

## Actual implementation and consumers

New `learn/panel_dataset.hpp` and `src/learn/panel_dataset.cpp` implement the
explicit `rank-residual-v2` artifact. Immutable date blocks hold feature-major
f32 columns; each column contains date-major instrument cells. Columns 0..F-1
are ranks and F..2F-1 are missing indicators. Labels remain f64. Independent
source-present and dated-member masks retain their distinct meanings, including
members whose current source row is absent. Complete source session ordinals
and canonical sorted numeric IDs persist without per-window compaction.

Within each date, finite present features among decision members receive an
average-tie rank divided by `finite_count-1`, minus 0.5. A singleton maps to zero.
Missing/nonfinite source values map to zero plus indicator one. All member rows
remain eligible feature rows, including rows with every feature missing; label
availability is a separate property. f32 rounding occurs only after the bounded
rank is computed in f64, with absolute rank rounding at most about 3e-8. Source
features already stored as f32 can have collapsed near ties; this implementation
does not claim equivalence to their unavailable original f64 ordering.

The default holding horizons are 21/63/126 sessions with execution delay one.
Labels use original-f64 adjusted-close endpoint/entry-1, divided by strictly
prior daily sample volatility times sqrt(holding horizon), then demeaned across
finite member labels within that date. Default volatility uses at most 63 prior
one-day returns, at least 20 finite observations, and a 1e-4 daily floor. A
return ending on date d enters normalization only for d+1. Missing history,
invalid price endpoints and cohorts with fewer than two usable labels remain
NaN. There is no invented zero target, terminal-return imputation or exposure
residualization; the latter awaits I2's dated exposure contract.

`build_panel_dataset_from_panel` consumes existing raw Panel fields and/or
AlphaStore streams synchronously. Its supplied numeric axes/source hash/recipe
are **caller assertions**: those in-memory objects have no durable artifact
identity to verify. The output hashes bind the actual produced values, not an
independent authentication of those assertions. Its explicit presence contract
maps absent closes to NaN even when backing storage contains a finite placeholder.
`build_panel_dataset_from_store` verifies D6 manifest identity, axes and namespace,
uses its original-f64 close channel and independent masks, and caches at most
four bounded source chunks. Other raw D6 feature fields retain their declared
stored precision; their parent manifest supplies that provenance.

`read_dataset_features` is an explicit bounded selected-window bridge. It emits
member rows, widens f32 features before arithmetic, preserves source ordinals,
records presence and manifest/window identity, and carries endpoint horizons as
`execution_delay + holding_horizon`. L0's same-close inclusive as-of rule masks
immature labels; feature rows beyond as-of are refused. Legacy `build_features`
retains raw values and drop-invalid rows; it now supplies its previously missing
`label_horizons` metadata without changing numerical output.

`fit_linear_dataset` requires DateV2, matching endpoint horizons and a nonfuture
PCA basis. It calls the existing checked learner and returns the fitted model
alongside immutable dataset/window identity. Existing fold-local standardization,
purge/embargo, row-expansion and CPCV budget checks remain active. This is **not a
whole-dataset out-of-core fit**. Publishing a model must retain the returned
identity; a model-only legacy serialization does not automatically preserve it.

## Storage, budgets and availability contract

ATXMLD2 manifests bind exact source/feature names, complete axes, transformation
rule, lookback, holding horizons, delay, volatility recipe, maturity end and
budgets. Each block has its own SHA256 and checked geometry. Files are exclusively
created; a hard-link publishes `manifest.bin` last after all block writes/flushes.
Reader metadata is bounded to 8 MiB, single blocks to 256 MiB, total payload to
1 TiB, axes to 100,000 entries each and features to 4,096. Default blocks have
eight dates. Captured file extent is admitted before mmap; SHA/geometry and all
numeric/mask/clock invariants are checked before spans are exposed. Shared live
mapping-byte/handle budgets survive reader copies and borrowed block lifetimes.
Pages are released before budget credit returns.

Preflight includes owned block storage, F*N f64 date scratch, bounded volatility
history, ranking/index scratch and metadata slack. The D6 adapter additionally
admits its four source mappings and source metadata. Caller-owned full Panel or
AlphaStore objects are outside this streamed storage bound. Legacy matrix
materialization has a separate explicit budget including row lookup overhead;
no entire large dataset is implicitly materialized.

For T=3000,N=1750,F=500,H=3 and eight-date blocks, stored payload is
21,136,542,000 bytes before the small manifest: 1,000 f32 feature/indicator
columns, three f64 targets, two masks and clocks. This is a byte-layout formula,
not a constructed artifact or measured process-RSS result. The metadata-only
synthetic admission check does not satisfy the original <=4 GB scale gate.

Session keys are sealed pre-2020 labels, not evidence of historical data
publication. Positive membership decision clocks must be strictly before their
session; masks remain independent. The generic source interface requires causal
features and original-f64 observed closes; it cannot authenticate a caller's
availability assertions. D6's vintage qualifications remain bound through its
manifest. No actual market payloads were opened for this implementation.

## Prepared qualification and remaining gates

Five owning cases in `tests/learn/learn_panel_dataset_test.cpp` cover:

- 40% missing features with full member-row retention, average ties, absent-member
  indicators, prior-vol warmup, f64 per-date demeaning and an independent scalar
  label oracle.
- Future mutation and truncation invariance on the protected prefix, plus exact
  endpoint equality and refusal to include future feature rows in a fit.
- Shared mapping lifetime/handle budgets, changed captured extent, hash mismatch,
  failed-source publication and strict membership-clock equality.
- Actual Panel and D6 producers, original f64 label parity on a deliberately
  well-separated synthetic feature fixture, finite-but-absent backing prices,
  and explicit legacy missing-row behavior.
- Formula-only large-shape admission, a tiny actual checked DateV2 linear fit,
  endpoint mismatch, future PCA refusal and retained CPCV workspace refusal.

All five cases remain **uncompiled/unrun**. Root owns CMake registration and the
next combined focused build. Large RSS, empirical row-retention/label quality,
feature-precision tolerance and tradeable-alpha outcomes remain unqualified.
Full streaming learner training, learned-model serializer migration, exposure
residualization and live feature recipe deployment are separate followups.
