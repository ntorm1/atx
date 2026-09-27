# W2-L2 bounded column-bin GBT implementation

Status: source and postimplementation fixtures frozen; no compilation, runtime, benchmark, market-data, or LightGBM comparison performed in this lane. Full L2 acceptance remains open.

## Frozen packet

- Production `2ca57542fb57a1274ee57893bd5683a53d168dc7`: `learn/gbt.hpp`, `src/learn/gbt.cpp` only.
- Identity correction `71ac85dca835ac500104009e6955bf5e8b5fb4fb`: hash supplied augmentation into the dataset fit recipe following independent review.
- Fixtures `1c6ad62ba29262efde56006ac1287dd36152664b`: new `tests/learn/gbt_v2_test.cpp`, seven cases, filter `GbtColumnV2.*:GbtDatasetV2.*`. Root owns registration and execution.
- Local prerequisite alignment only: reviewed L1 source `f0309cc1`, `d4715797`, `ffc010f4` became `324a235f`, `e8fc4e38`, `30a93925`. Root already owns those prerequisites; do not reimport them or a branch merge.

## Implemented engine behavior

`GbtRule::LegacyV1` remains the default and dispatches to the prior numerical implementation. Its 393-line `gbt_detail` implementation block matches the prior source exactly (SHA-256 `5e025cc0b8d29eea1ac77257e1ca1f4b9a2a3d71b648f26a4559de1190c40123`; local receipt `build-equity/l2-gbt-legacy-body-identity.json`). This is source evidence, not compiled parity proof.

Explicit `ColumnBinsV2` bins once per forest into feature-major u8 storage. Finite bins occupy 0..254; NaN uses 255 and always routes right, matching existing deployed inference. Infinite features and nonfinite used labels refuse. A feature with no useful finite cut stays a leaf; this is not learned missing-direction optimization. The L1 dataset bridge supplies separate missing indicators, retaining members with feature holes.

Independent features build histograms in parallel with ascending row reductions. Split enumeration keeps the first strictly best feature/cut; deterministic seeded sorted subsamples retain fixed traversal. Each split builds the smaller child histogram and subtracts it from the parent for the sibling, with zero-count canonicalization and a cancellation-triggered direct rebuild. Prediction reads the existing matrix directly, removing the old per-row prediction buffer. Gain importance is normalized only when positive deployed gain exists; zero-gain models report zero, not invented importance. Optional date-centered V2 residuals require original ordered date IDs and remove common date-level labels. Worker count changes scheduling, not the numerical recipe.

`fit_gbt_dataset` is a real bounded selected-window consumer. Before materialization it requires V2, DateV2 CPCV, fold-local augmentation, exact persisted holding-plus-delay horizons, nonfuture feature/as-of window and PCA cutoff, checked dimensions, and combined materialization/fit admission. It uses L1 maturity-filtered f64 targets and ranked f32-derived features plus missing indicators. It is not an out-of-core learner: selected rows and fold designs still materialize as f64. Memory figures are conservative owned payload bounds with explicit scratch/slack, not measured RSS or thread-stack bounds. Existing deterministic pool exception propagation is retained; resource admission cannot guarantee host allocations succeed.

The returned wrapper binds dataset manifest, selected-window recipe, versioned algorithm settings, and a canonical SHA-256 of supplied PCA presence/k/fit clock/arrays plus ordered interactions/provenance. Hashing is incremental and locale/storage-order independent. Existing model/node inference format and legacy readers remain unchanged; model-only serializers do not automatically persist this wrapper. Callers publishing V2 models must retain all wrapper identities. Application defaults have not changed.

## Bounded qualification prepared

Fixtures cover an independent direct squared-error stump with NaN/right routing and tied features; exact forest primitive bits at one/four workers; subtraction and normalized importance; date-offset invariance/clock refusal; invalid labels/features/config/budgets; explicit old primitive-path parity; actual dataset fitting with future-close mutation and maturity/PCA/protocol refusals; and augmentation identity mutation versus worker-invariant identity. Exclusive fixture-owned temporary directories avoid shared cleanup. Tests were added after the implementation and have not run.

## Remaining original L2 requirements

- Inner purged-validation early stopping is absent and is explicitly recorded as `early-stop=none` in the recipe.
- Missing routing is fixed-right with separate indicators on the dataset path, not optimized per-node missing direction.
- Full out-of-core fitting and a production model-publication consumer that persists the new wrapper are not supplied by this bounded slice.
- Thread bit identity and bounded conformance await root runtime qualification.
- The original 1M x 200 fit under 60 seconds at eight threads, peak memory, and OOS IC within 10% of LightGBM remain unmeasured. No such pass or empirical alpha claim follows from these synthetic fixtures.
