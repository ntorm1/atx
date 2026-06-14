# Sprint S6 (p2) — Data / Signal / Feature / Reference Abstraction Layer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan unit-by-unit. Steps use checkbox (`- [ ]`) syntax for tracking.

**Status:** frozen ahead of kickoff 2026-06-14 (base `main @ d81fa02`). S6 is `⏳ proposed`; this *how* is locked now at
the user's request. The boundary pin (§4, S6.8) is the non-negotiable: a price-only `DataContext` must reduce
**bit-for-bit** to today's fixed-`alpha::Panel` `BookPipeline`.
**Spec (the *what*):** [`sprint-6-data-signal-feature-reference-abstraction-layer.md`](sprint-6-data-signal-feature-reference-abstraction-layer.md) ·
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md) ·
**Quality bar:** [`../docs/implementation-quality.md`](../docs/implementation-quality.md) ·
**C++ authority:** [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md) ·
**Engine deltas:** [`../../../.agents/atx-engine/agent.md`](../../../.agents/atx-engine/agent.md)
**Grounded in:** [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) §8
(the 125k-field PIT datafield catalog, universes, regions, delay) ·
[`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md)
§6.2/§7.1/§9.1 (deeper-cleaner-data edge, PIT as-of versioning, survivorship).

**Goal:** Make atx-engine bring-your-own-data — a typed PIT-versioned dataset catalog + a `DataContext` facade so a user
supplies a required price dataset plus optional feature / external-signal / factor-model / reference plug-ins, and the
existing mine → admit → combine → multi-horizon-book pipeline runs over it unchanged.

**Architecture:** A new `atx::engine::data::` layer (`Dataset`, `DatasetSchema`, `DatasetCatalog`, ingestion/alignment
rail, `DataContext`, `FactorModelArtifact`) sits *under* the as-built pipeline and lowers each registered dataset into
the existing concrete structures (`alpha::Panel`, `learn::FeatureMatrix`, `risk::FactorModel`, `library::Library`) via
differential-pinned adapters. Price is the canonical axis; everything else aligns onto it.

**Tech Stack:** C++20, header-mostly with `src/data/*.cpp` TUs (explicitly registered in CMake), GoogleTest, the
`Result<T>` error channel, `atx-core` L7 `linalg` + L9 `frame`.

---

## §0 — As-built reconciliation (recon fixes)

Recon was run at base SHA against every seam S6 retrofits. Exact signatures below are load-bearing — the adapters lower
*into* these, the boundary pin is *against* them.

### 0.1 The canonical in-memory representation is `alpha::Panel`, built two ways today
`alpha::Panel` ([panel.hpp:77](../../include/atx/engine/alpha/panel.hpp#L77)) is a date-major `f64` column store + a
`{0,1}` universe mask + a field-name dictionary. It is built via:
- `Panel::create(dates, instruments, field_names, field_data, universe)` ([panel.hpp:88](../../include/atx/engine/alpha/panel.hpp#L88)) — **owned**.
- `Panel::create_borrowed(...)` ([panel.hpp:96](../../include/atx/engine/alpha/panel.hpp#L96)) — zero-copy over external storage.
- `datafields::with_datafields(dates, instruments, field_names, field_data, universe, adv_windows)`
  ([datafields.hpp:153](../../include/atx/engine/alpha/datafields.hpp#L153)) — appends `dollar_volume`/`vwap`/`adv{d}`
  derived columns then calls `Panel::create`. **This is the function the PriceDataset adapter must reproduce bit-for-bit.**

Reads: `field_id(name)` ([panel.hpp:123](../../include/atx/engine/alpha/panel.hpp#L123)),
`field_cross_section(field, date)` ([panel.hpp:144](../../include/atx/engine/alpha/panel.hpp#L144)),
`in_universe(date, inst)` ([panel.hpp:159](../../include/atx/engine/alpha/panel.hpp#L159)). There is **no
"aligned-arrays + schema → Panel"** entry beyond `create`; S6.4's adapter is additive over `with_datafields`.

> **Boundary-pin lever:** the price-only adapter is literally `with_datafields(...)` with the same `adv_windows`. If the
> adapter calls it with identical arguments, the produced `Panel` is byte-identical, so the downstream `book_digest`
> cannot move. The pin test asserts exactly this.

### 0.2 `FeatureMatrix` features come only from Panel fields + pool alphas
`build_features(panel, store, spec)` ([feature_matrix.hpp:200](../../include/atx/engine/learn/feature_matrix.hpp#L200))
reads `FeatureSpec{raw_fields, pool_alphas, horizons, max_lookback}`
([feature_matrix.hpp:78](../../include/atx/engine/learn/feature_matrix.hpp#L78)). Raw fields resolve via
`panel.field_id`; pool alphas come from `combine::AlphaStore`. The feature column order is fixed: raw fields then pool
alphas. **There is no external-feature column path** — S6.4 adds one.
- **Lever:** an external `Feature` dataset whose columns are merged into the `Panel` *as extra fields* (via the S6.4
  price+feature adapter) is then referenceable in `FeatureSpec::raw_fields` by name — **zero change to `build_features`**.
  This is the cheapest correct injection: features become panel columns, and the existing PIT/`row_valid` machinery
  (M2/M8, [feature_matrix.hpp:18-37](../../include/atx/engine/learn/feature_matrix.hpp#L18)) applies unchanged. The
  train/eval-parity rule (`learned_source` reconstructs features from a `PanelView`) holds **iff** the feature is a real
  panel column — which it now is. (Features that genuinely cannot live on the panel grid are out of scope; recorded.)

### 0.3 `FactorModel` is price-derived; `create(X, F, D)` is the injection seam
`FactorModel::create(MatX x, MatX f, VecX d, fit_begin, fit_end)`
([factor_model.hpp:111](../../include/atx/engine/risk/factor_model.hpp#L111)) is the low-level assembly; it validates
`F` is K×K with `K == X.cols()`, SPD, etc. The fundamental path is `FactorModelBuilder::build_components(panel, window,
market_cap, group_id)` → `FactorComponents{X, F, D, fit_end}` → `create`
([factor_model.hpp:498](../../include/atx/engine/risk/factor_model.hpp#L498),
[461](../../include/atx/engine/risk/factor_model.hpp#L461),
[509](../../include/atx/engine/risk/factor_model.hpp#L509)). `market_cap` (`span<const f64>`) and `group_id`
(`span<const u32>`) are the **already-existing optional reference spans** — empty ⇒ the column is omitted. So:
- **Full BYO model (S6.6a):** a `data::FactorModelArtifact{X, F, D, fit_begin, fit_end}` lowers *directly* to
  `FactorModel::create` — the adapter is a validate-then-forward.
- **Reference-fed (S6.6b):** a `Reference` dataset materializes the `market_cap` / `group_id` spans (and any custom
  style columns merged as panel fields) feeding `build_components` — formalizing today's ad-hoc spans.
- **FactorModelArtifact home — RESOLVED:** lives in `atx::engine::data::` (it is a data-ingestion artifact, lowered into
  `risk::`), avoiding a `risk → data` include edge.

### 0.4 `library::Library::admit` takes an `AlphaCandidate` — the external-signal seam already fits
`Library::admit(c, gate)` ([library.hpp:141](../../include/atx/engine/library/library.hpp#L141)) consumes
`AlphaCandidate{canon_hash, pnl (span T), pos_flat (span T*N), metrics, prov, as_of, source}`
([library.hpp:90](../../include/atx/engine/library/library.hpp#L90)) and runs dedup → corr-to-pool → the four P4 floors.
So admitting an **external signal** means: convert the `Signal` dataset's `(date × instrument)` scores to positions +
pnl (via `combine::extract_streams` over a `WeightPolicy` — the existing path, RTK-confirmed in the Explore recon),
compute `AlphaMetrics`, synthesize a `canon_hash` + a `Provenance` that records *externally-sourced*, and call `admit`.
**The deflated-Sharpe + S4 robustness battery gate it exactly like a mined alpha** — no new admission logic. (Synthetic
recovery is N/A for a supplied signal; regime/walk-forward + net-of-cost + the deflated gate all apply.)

### 0.5 `BookPipeline` takes a FIXED `alpha::Panel` — that is the S6.8 wiring site
`BookPipeline(lib, dsl, panel, sim, policy, gate)`
([pipeline.hpp:213](../../include/atx/engine/book/pipeline.hpp#L213)); `run(cfg)`
([pipeline.hpp:220](../../include/atx/engine/book/pipeline.hpp#L220)) mines via
`ResearchDriver engine{lib_, dsl_, panel_, sim_, policy_, gate_}` ([pipeline.hpp:225](../../include/atx/engine/book/pipeline.hpp#L225)),
builds the factor model via `build_base_components` / `augment_with_dead`
([pipeline.hpp:240-241](../../include/atx/engine/book/pipeline.hpp#L240)). S6.8 adds a **`DataContext`-taking overload**
that derives `panel` (from the price+feature adapter), an **optional factor-model override** (from a
`FactorModelArtifact`, replacing `build_base_components`), and an **external-signal admit list** (admitted before mining)
— then delegates to the existing flow. The legacy ctor is retained verbatim; the boundary-pin test drives the
`DataContext` overload with a price-only context and asserts the identical `book_digest`/`report_digest`.

### 0.6 Engine sources are EXPLICITLY listed in CMake — new TUs MUST be registered
`add_library(atx-engine STATIC src/...)` enumerates every `.cpp` ([CMakeLists.txt:1](../../CMakeLists.txt#L1)); `src/data/`
already holds `data_handler.cpp` + `shm_bar_feed.cpp` ([CMakeLists.txt:37-38](../../CMakeLists.txt#L37)). **Every new
`src/data/*.cpp` in this sprint MUST be added to that list** (unlike tests/benches, which ARE auto-globbed with
`CONFIGURE_DEPENDS`). This corrects the S3 plan's "no CMake edits" note — that applied to test/bench files only.

### 0.7 Determinism order — RESOLVED
`DatasetCatalog` iterates registered datasets in **ascending dataset-name order** (a `std::map<std::string, …>` or a
sorted key vector), never insertion-hash order. Catalog resolution, lineage emission, and the external-signal admit
sequence all follow this canonical order so the run-to-run digest is byte-identical (carried-forward invariant #1).

### 0.8 Ragged-grid alignment policy — RESOLVED
The **price dataset defines the canonical axis** (the date spine + the instrument universe + the `{0,1}` mask — price is
required precisely so this axis exists). Every other dataset aligns onto it by `(date-key, instrument-key)` join:
- a canonical `(date, instrument)` cell absent from a plugged dataset → **NaN** (never imputed);
- a plugged-dataset cell outside the canonical axis (extra instrument / extra date) → **dropped**, counted, and surfaced
  in the catalog/lineage report (no silent truncation — carried-forward invariant + the spec's "no silent caps" rule);
- as-of/effective-date versioning resolves *which* value is visible at each canonical date (§4, S6.3); the alignment
  reads only `date-key ≤ canonical-date` (truncation-invariant).
Date-key and instrument-key matching are by explicit id (the schema declares the key columns); no positional assumption.

---

## §1 — Design rules (the BYO-data contract)

1. **Price is the only required input; everything else is optional with a price-only fallback.** A `DataContext` with
   only a `Price` dataset must drive the entire pipeline. Each optional plug (feature / signal / factor-model /
   reference) is independently attachable; absent ⇒ today's internal computation.
2. **Additive, never mutative on the proven core.** No existing signature changes. `Panel`/`FeatureMatrix`/`FactorModel`/
   `Library`/`BookPipeline` gain *new* entry points (adapters, a `DataContext` overload); the old ones are untouched and
   remain the boundary-pin anchors.
3. **Ingestion is the PIT/no-survivorship/no-look-ahead enforcement point.** The S6.3 alignment rail enforces these *at
   ingest*, truncation-invariantly, for every plugged dataset (carried-forward invariants #2/#3/#4).
4. **Every adapter's output bit-matches the hand-built structure it replaces.** Price→Panel == `with_datafields`;
   factor-model→`FactorModel` == `create`; signal→`AlphaCandidate` == the `extract_streams` path. The price-only
   `DataContext`→`BookPipeline` digest equality is the top-level differential gate.
5. **Determinism.** Canonical-order catalog iteration (§0.7); no RNG in any adapter; external-signal admission is
   seeded/order-fixed like any library admit. Same inputs → byte-identical digest.
6. **Cold-path only.** Ingestion/alignment/lowering run once at setup; the steady-state eval loop is untouched and
   allocates zero. `std::vector`/`std::map` allocation in the catalog/adapters is fine.
7. **atx-core is the home of general primitives; the engine ships a self-contained fallback.** The PIT as-of/lineage
   versioning is the L9 `as_of_frame` lift (§2.1), shipped engine-local so S6 never blocks on atx-core.
8. **No in-engine file readers.** Construction is bring-your-own-arrays (spans + schema). CSV/Parquet/Arrow parsing is
   the caller's job (anti-roadmap #9).

---

## §2 — File structure

### 2.1 atx-core / Pattern-B request (recorded; engine ships the fallback)
- **L9 `as_of_frame`** (S6.2): PIT as-of / effective-date / restatement versioning + a dataset-lineage map in the
  frame/tsdb. **Engine fallback:** an as-of index (sorted `(effective_date → value)` per cell, resolved by
  `≤ canonical_date`) + a lineage adjacency map, both engine-local in `data/catalog.{hpp,cpp}`. Accelerator, not blocker.

### 2.2 Engine files (this sprint builds/edits these)

| Unit | New | Edited |
|---|---|---|
| S6.1 | `include/atx/engine/data/dataset.hpp`, `data/dataset_schema.hpp`; `src/data/dataset.cpp` | — |
| S6.2 | `include/atx/engine/data/catalog.hpp`; `src/data/catalog.cpp` | `CMakeLists.txt` (register `dataset.cpp`, `catalog.cpp`) |
| S6.3 | `include/atx/engine/data/align.hpp`; `src/data/align.cpp` | `CMakeLists.txt` (register `align.cpp`) |
| S6.4 | `include/atx/engine/data/adapt_panel.hpp`, `data/adapt_feature.hpp`; `src/data/adapt_panel.cpp`, `src/data/adapt_feature.cpp` | `CMakeLists.txt` |
| S6.5 | `include/atx/engine/data/adapt_signal.hpp`; `src/data/adapt_signal.cpp` | `CMakeLists.txt` |
| S6.6 | `include/atx/engine/data/factor_model_artifact.hpp`, `data/adapt_factor.hpp`; `src/data/adapt_factor.cpp` | `CMakeLists.txt` |
| S6.7 | `tests/data_altdata_subsumption_test.cpp` (+ fixtures) | — (proves no new ingestion code is needed) |
| S6.8 | `include/atx/engine/data/context.hpp`; `src/data/context.cpp` | `book/pipeline.hpp` (add a `DataContext` overload — additive), `factory/*` only if the overload needs a seam (prefer routing through the produced `Panel`); `CMakeLists.txt` |
| S6.9 | `tests/data_e2e_byo_capstone_test.cpp`, `bench/data_ingest_bench.cpp`, `tests/fixtures/byo_*.txt` | `ROADMAP.md` (S6 status), `sprint-6-progress.md` |

Tests (`tests/*_test.cpp`) and benches (`bench/*_bench.cpp`) are auto-globbed (`CONFIGURE_DEPENDS`) — **do not hand-edit
CMake for them**. Engine library `.cpp` TUs are **explicitly listed** — every new `src/data/*.cpp` is one CMake edit
(§0.6).

### 2.3 Tests (one per coding unit, `atx-engine/tests/<name>_test.cpp`)
`data_dataset_test.cpp` (S6.1), `data_catalog_test.cpp` (S6.2), `data_align_test.cpp` (S6.3), `data_adapt_panel_test.cpp`
+ `data_adapt_feature_test.cpp` (S6.4), `data_adapt_signal_test.cpp` (S6.5), `data_adapt_factor_test.cpp` (S6.6),
`data_altdata_subsumption_test.cpp` (S6.7), `data_context_test.cpp` + `data_boundary_pin_test.cpp` (S6.8),
`data_e2e_byo_capstone_test.cpp` (S6.9). Each: happy path, boundaries (empty optional plug, single instrument, single
date, all-NaN column, ragged grid), the load-bearing invariant proof, and `EXPECT_DEATH`/`Err` for every precondition.

### 2.4 Ledger
`sprint-6-progress.md` — opened in S6.0 (marker), one row per unit with SHA + test count + measured numbers (alignment
drop counts, ingest throughput, the boundary-pin digest equality) on close.

---

## §3 — Cross-cutting gates (every coding unit) + handoff block

**Gates (a unit is done only when ALL hold):**
- TDD: failing GoogleTest first, for the right reason, then implement the minimum.
- Differential: every adapter's output bit-matches the hand-built structure (price→Panel == `with_datafields`;
  factor→`FactorModel` == `create`; signal→`AlphaCandidate` == `extract_streams`).
- PIT / truncation-invariance test for every dataset role through the S6.3 rail; NaN/no-survivorship test per adapter.
- Determinism: canonical-order iteration (§0.7) replays byte-identically; no adapter RNG.
- Boundary pin (from S6.4 onward, spot-checked; full in S6.8): price-only path digest unchanged.
- `/W4 /permissive- /WX` + `/fp:precise` clean; clang-format clean. **Do not run clang-tidy** (disabled repo-wide).
- Functions ≤ ~60 lines; `const`/`constexpr`/`noexcept`/`[[nodiscard]]`; `Result<T>` at boundaries; zero hot-path alloc.
- Commit `feat(s6-M): …` with the `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` trailer. Do
  not push. Stage explicit pathspecs.

**Handoff block (paste into every coding sub-agent brief):**
> Worktree/branch per kickoff (`feat/p2-s6-data-abstraction`). Build: `cmake --build --preset dev --target
> atx-engine-tests`. Test: `ctest --preset dev -R <Suite> --output-on-failure`. C++ authority: `.agents/cpp/agent.md`
> (no UB, TDD, `Result<T>`, exhaustive switches, ≤60-line fns, zero hot-path alloc). Engine deltas:
> `.agents/atx-engine/agent.md`. **New `src/data/*.cpp` MUST be added to `add_library(atx-engine STATIC …)` in
> CMakeLists.txt (§0.6); tests/benches auto-glob.** Every adapter ships the differential equality against the
> hand-built structure it replaces — that equality is the gate. Price defines the canonical axis; align onto it, NaN
> for missing, drop+count for out-of-axis. NaN out-of-universe, never impute. Price-only path digest must not move.

---

## §4 — Architecture & algorithms (per-unit)

### 4.0 S6.0 — Marker + ledger + as-built recon
- [ ] Open `sprint-6-progress.md`; freeze scope + base SHA; commit the marker.
- [ ] Confirm §0 recon against the live tree (signatures + the CMake source list + the `extract_streams`/`WeightPolicy`
      external-signal path); record any drift as a recon fix in the ledger.
- [ ] Produce the **seam map**: every bare-`alpha::Panel` consumer site `DataContext` will thread through (S6.8) and the
      exact `extract_streams`/`AlphaMetrics`/`Provenance` shapes the signal adapter (S6.5) must fill.

### 4.1 S6.1 — `data::Dataset` + `DatasetSchema`
**Types.** `enum class Role : u8 { Price, Feature, Signal, Reference };`
`struct DatasetSchema { std::vector<std::string> columns; std::vector<ColumnDType> dtypes; Role role; u16 pit_delay; std::string region; std::string universe_tag; AsOfPolicy as_of; };`
`class Dataset` — owns a date axis (`std::vector<DateKey>`), an instrument axis (`std::vector<InstKey>`), date-major
`f64` columns (one per schema column), an optional `{0,1}` mask, and a `Provenance` record. Constructed
bring-your-own-arrays: `Dataset::create(schema, dates, instruments, columns, mask, provenance) -> Result<Dataset>`
(validates `columns.size()==schema.columns.size()`, each column `== dates*instruments`, dtype/role coherence → `Err`).
- [ ] **Step 1 (test):** `data_dataset_test.cpp` — `CreateRejectsRaggedColumns`, `CreateRejectsDtypeRoleMismatch`,
      `RoundTripsColumnsByteIdentical`, `OhlcvDatasetMatchesPanelColumns` (a `Dataset` carrying exactly OHLCV columns
      exposes them in the same order/values a `Panel` would). Run; verify FAIL (types absent).
- [ ] **Step 2 (impl):** `dataset_schema.hpp` (POD + validation helpers), `dataset.hpp` (class decl + inline accessors),
      `dataset.cpp` (`create` validation). Register nothing yet (header-mostly); `dataset.cpp` registered in S6.2's CMake edit.
- [ ] **Step 3:** run the suite → PASS. **Step 4:** commit `feat(s6-1): typed PIT-versioned data::Dataset + schema`.

### 4.2 S6.2 — `data::DatasetCatalog` registry
**Type.** `class DatasetCatalog` — `register_dataset(name, Dataset) -> Status` (schema-enforced; duplicate name →
`Err`), `resolve(name) -> Result<const Dataset&>`, `names() -> std::vector<std::string>` (**ascending**, §0.7),
`role_of(name)`, a lineage graph `derive(child_name, parent_names…)` + `lineage(name) -> provenance chain`, and PIT
as-of version resolution `value_at(name, col, canonical_date, inst) -> f64` (engine-local as-of index, §2.1). Backed by
`std::map<std::string, …>` for canonical order.
- [ ] **Step 1 (test):** `data_catalog_test.cpp` — `RegisterRejectsDuplicate`, `ResolveReturnsRegistered`,
      `NamesAreAscendingDeterministic`, `LineageTracksDerivation`, `AsOfResolvesLatestNotAfterDate`,
      `AsOfNeverLeaksRestatedFuture` (a value with `effective_date > canonical_date` is invisible). FAIL.
- [ ] **Step 2 (impl):** `catalog.hpp`/`catalog.cpp`. As-of index = per-`(col, inst)` sorted `(effective_date, value)`
      resolved by upper-bound `≤ canonical_date`. Lineage = adjacency list keyed by name.
- [ ] **Step 3:** PASS. **Step 4 (CMake):** add `src/data/dataset.cpp` + `src/data/catalog.cpp` to
      `add_library(atx-engine STATIC …)`. **Step 5:** commit `feat(s6-2): DatasetCatalog registry + as-of/lineage (L9 fallback)`.

### 4.3 S6.3 — Ingestion + alignment + PIT-validation rail
**Function.** `align_onto(const Dataset& canonical_price, const Dataset& plug) -> Result<AlignedView>` — reconcile
`plug` onto the price axis (§0.8): for each canonical `(date, inst)`, look up `plug` by `(date-key, inst-key)`; missing →
NaN; resolve as-of value (`≤ canonical_date`); count + record out-of-axis `plug` cells dropped; never impute; carry the
delisted final value (no-survivorship). Returns aligned date-major columns over the canonical axis + a `DropReport`.
- [ ] **Step 1 (test):** `data_align_test.cpp` — `MissingCellsBecomeNaN`, `ExtraInstrumentsDroppedAndCounted`,
      `DelistedFinalValueSurvives`, `AsOfAlignmentIsTruncationInvariant` (appending later dates to `plug` never changes
      an earlier aligned cell — the no-look-ahead pin), `KeyMatchIsByIdNotPosition`. FAIL.
- [ ] **Step 2 (impl):** `align.hpp`/`align.cpp`. **Step 3:** PASS. **Step 4 (CMake):** register `align.cpp`.
      **Step 5:** commit `feat(s6-3): ingestion+alignment+PIT rail (truncation-invariant)`.

### 4.4 S6.4 — PriceDataset → `Panel` + FeatureDataset → `FeatureMatrix`
**Price adapter.** `price_to_panel(const Dataset& price, std::span<const u16> adv_windows) -> Result<alpha::Panel>` —
extract OHLCV columns in canonical field order + the universe mask, then call `datafields::with_datafields(...)` with the
**same** `adv_windows` (§0.1). **Feature adapter.** `merge_features_into_panel(panel_in, const Dataset& feature, catalog)
-> Result<alpha::Panel>` — align the feature dataset (S6.3) onto the price axis and append its columns as extra `Panel`
fields (so they are referenceable by name in `FeatureSpec::raw_fields`, §0.2). Price-only ⇒ no feature merge ⇒ identical
`Panel`.
- [ ] **Step 1 (test):** `data_adapt_panel_test.cpp` — `PriceOnlyPanelEqualsWithDatafields` (build a `Panel` directly via
      `with_datafields` and via the adapter from an OHLCV `Dataset`; assert byte-identical field names + every cell,
      NaN==NaN) — **the boundary-pin seed**; `AdvWindowsMatch`; `MaskPreserved`.
- [ ] **Step 2 (impl):** `adapt_panel.hpp`/`adapt_panel.cpp`. **Step 3:** PASS. **Step 4 (CMake):** register.
      **Step 5:** commit `feat(s6-4a): PriceDataset→Panel adapter (== with_datafields)`.
- [ ] **Step 6 (test):** `data_adapt_feature_test.cpp` — `FeatureColumnsAppendedByName`,
      `AlignedFeatureReferenceableInFeatureSpec` (build a `FeatureMatrix` whose `raw_fields` includes a merged feature
      column; assert the row values equal the aligned dataset), `MissingFeatureCellMakesRowInvalid` (M8 `row_valid==0`).
- [ ] **Step 7 (impl):** `adapt_feature.hpp`/`adapt_feature.cpp`. **Step 8:** PASS. **Step 9 (CMake):** register.
      **Step 10:** commit `feat(s6-4b): FeatureDataset→FeatureMatrix injection`.

### 4.5 S6.5 — SignalDataset (external precomputed signals — both paths)
**As feature:** a `Signal` dataset merges as panel columns exactly like S6.4b (reuse `merge_features_into_panel`). **As
library admission:** `signal_to_candidates(const Dataset& signal, panel, sim, policy, gate, as_of) ->
Result<std::vector<library::AlphaCandidate>>` — for each signal column (one alpha per column): run
`combine::extract_streams` over the `WeightPolicy` to get positions + pnl (the §0.4 path), compute `AlphaMetrics`,
synthesize `canon_hash` (content hash of the column id + provenance — distinct from any mined genome's hash) and a
`Provenance` flagged externally-sourced, fill `AlphaCandidate`. The caller then `lib.admit(candidate, gate)` — **gated by
the deflated-Sharpe + S4 robustness battery, no new admission code.** Provenance is recorded in the catalog lineage.
- [ ] **Step 1 (test):** `data_adapt_signal_test.cpp` — `SignalColumnAdmitsThroughGate` (a strong planted signal clears
      the gate; a noise column is rejected), `AdmitVerdictMatchesExtractStreamsPath` (the candidate built by the adapter
      yields the same `AdmitVerdict` as one built by calling `extract_streams` directly — the differential equality),
      `ProvenanceFlagsExternalSource`, `CanonHashDistinctFromMinedGenome`. FAIL.
- [ ] **Step 2 (impl):** `adapt_signal.hpp`/`adapt_signal.cpp`. **Step 3:** PASS. **Step 4 (CMake):** register.
      **Step 5:** commit `feat(s6-5): external signal → feature + gated library admit`.

### 4.6 S6.6 — BYO factor model + reference ingestion
**FactorModelArtifact (in `data::`, §0.3).** `struct FactorModelArtifact { core::linalg::MatX X, F; core::linalg::VecX
D; usize fit_begin, fit_end; };` **Full-BYO adapter:** `artifact_to_factor_model(const FactorModelArtifact& a) ->
Result<risk::FactorModel>` = validate (shape/SPD via the existing `create` checks, optional `psd_repair`) then
`FactorModel::create(a.X, a.F, a.D, a.fit_begin, a.fit_end)`. **Reference adapter:** `reference_spans(const Dataset&
reference, catalog) -> Result<RefSpans{std::vector<f64> market_cap; std::vector<u32> group_id;}>` — materialize the
aligned `market_cap` / `group_id` (and merge any custom style columns as panel fields) for
`FactorModelBuilder::build_components`. Neither plug ⇒ today's price-derived build.
- [ ] **Step 1 (test):** `data_adapt_factor_test.cpp` — `ArtifactLowersToCreateByteIdentical` (build a `FactorModel` via
      `create` and via the adapter from the same X/F/D; assert identical apply outputs on a probe vector),
      `NonSpdFArtifactErrs`, `ReferenceSpansFeedBuildComponents` (a reference `Dataset` yields the same `market_cap` /
      `group_id` spans a hand-built call uses → identical `FactorComponents`), `NoPlugFallsBackToPriceDerived`. FAIL.
- [ ] **Step 2 (impl):** `factor_model_artifact.hpp`, `adapt_factor.hpp`/`adapt_factor.cpp`. **Step 3:** PASS.
      **Step 4 (CMake):** register. **Step 5:** commit `feat(s6-6): BYO FactorModelArtifact + reference ingestion`.

### 4.7 S6.7 — Alt-data subsumption (proof, no new ingestion code)
A pure test unit: prove fundamental/analyst + news/sentiment ingest through the **existing** S6.1–S6.6 machinery with
*only* `register_dataset` + a schema. `tests/data_altdata_subsumption_test.cpp`:
- [ ] **Step 1 (test):** `FundamentalIngestsAsReferenceDataset` (a fundamental `Dataset` with restatement as-of versioning
      feeds `build_components` as reference, no bespoke code), `NewsSentimentGatesViaTradeWhen` (a sentiment `Feature`
      dataset, merged as a panel column, drives the existing `trade_when` event-gating operator in a DSL formula),
      `AnalystFeatureFeedsFeatureMatrix`. Each asserts the result equals the equivalent hand-built path.
- [ ] **Step 2:** if any alt-data shape needs a column the schema cannot express, record it in the ledger as the residual
      (no code; the abstraction's boundary). **Step 3:** PASS. **Step 4:** commit `test(s6-7): alt-data subsumption proof`.

### 4.8 S6.8 — `DataContext` facade wiring (the boundary pin)
**Type.** `class DataContext` — built from a `DatasetCatalog` + a required price dataset name; lazily lowers:
`price_panel() -> const alpha::Panel&` (S6.4a, with feature merge if a `Feature`/`Signal`-as-feature dataset is
registered), `factor_model_override() -> std::optional<risk::FactorModel>` (S6.6a if a `FactorModelArtifact` is
registered, else nullopt → builder path), `signal_admit_candidates(sim, policy, gate, as_of) ->
std::vector<library::AlphaCandidate>` (S6.5), `reference_spans()` (S6.6b). **Wiring.** Add
`BookPipeline::run_with_context(DataContext&, const PipelineConfig&)` (additive; `book/pipeline.hpp`): admit the
external-signal candidates into `lib_` first, then run the existing flow with the context's `Panel` and — if present —
the factor-model override substituted for `build_base_components`. The legacy `run(cfg)` is unchanged.
- [ ] **Step 1 (test):** `data_context_test.cpp` — `PricePanelLowersFromCatalog`, `FactorOverridePresentWhenArtifact`,
      `SignalCandidatesAdmitBeforeMine`. FAIL → impl `context.hpp`/`context.cpp` → PASS.
- [ ] **Step 2 (CMake):** register `context.cpp`. Commit `feat(s6-8a): DataContext facade`.
- [ ] **Step 3 (THE BOUNDARY PIN test):** `data_boundary_pin_test.cpp` — `PriceOnlyContextDigestEqualsLegacy`: take the
      existing `book_pipeline_test` fixture; run it (a) the legacy way (`BookPipeline::run`) and (b) by wrapping the same
      OHLCV data in a price-only `DataContext` and calling `run_with_context`. Assert `book_digest` **and**
      `report_digest` are byte-identical. This is the non-regression anchor; it MUST pass before S6.9.
- [ ] **Step 4 (impl):** `BookPipeline::run_with_context` (additive overload; reuse every private step). **Step 5:** PASS.
      **Step 6:** commit `feat(s6-8b): BookPipeline DataContext overload + boundary pin`.

### 4.9 S6.9 — E2E BYO capstone + report + bench + close
- [ ] **Step 1 (test):** `data_e2e_byo_capstone_test.cpp` — `ByoPriceSignalFactorRunsDeterministic`: register a price
      dataset + a `Feature` dataset + an external `Signal` dataset + a `FactorModelArtifact`; run `run_with_context`;
      assert (i) the external signal is admitted (lineage records it externally-sourced), (ii) the feature is used,
      (iii) the BYO factor model drives the optimize step, (iv) the run is deterministic (re-run → identical
      `book_digest`/`report_digest`). FAIL → wire → PASS.
- [ ] **Step 2:** catalog/lineage report — a headless artifact (file under `out_dir`) listing registered datasets,
      schemas, PIT versions, drop counts, and the provenance graph (which admitted alphas trace to which external signal
      dataset). Test `CatalogReportIsReproducible`.
- [ ] **Step 3 (bench):** `bench/data_ingest_bench.cpp` — ingest + alignment + lowering throughput; assert the price-only
      lowering adds no steady-state allocation (Debug upper-bound only; never cited as a release figure).
- [ ] **Step 4 (close):** ledger close ceremony; bump `ROADMAP.md` S6 status; write `sprint6.md` user ref. Commit
      `feat(s6-9): E2E BYO capstone + catalog/lineage report + bench + close`.

---

## Exit criteria
Mirror the spec's exit criteria (the spec is the verbatim authority). Concretely green when: `Dataset`/`DatasetSchema`/
`DatasetCatalog`/`DataContext` ship; a price-only `Dataset` lowers to a `Panel` byte-identical to `with_datafields`; the
S6.3 rail enforces PIT/no-survivorship/NaN truncation-invariantly per role; price-required-rest-optional is real with
per-plug price-only fallbacks; external feature datasets inject into `FeatureMatrix` and external signal datasets enter
both as features and as **gated** library admissions with recorded provenance; a `FactorModelArtifact` drives the
optimizer and a `Reference` dataset feeds the builder, both falling back to the price-derived model; alt-data ingests
with no bespoke per-source code; **the price-only `DataContext`→`BookPipeline` digest equals the legacy path byte-for-
byte** (S6.8 pin); the E2E BYO capstone is deterministic and the catalog/lineage report is reproducible; `/W4
/permissive- /WX` + clang-format clean.

## Invariants this sprint must prove
No look-ahead (the S6.3 alignment rail is truncation-invariant; external-signal admit reuses the fit/apply firewall). No
survivorship (delisted final values survive every ingest/align). PIT (NaN out-of-universe/not-yet-knowable across roles;
no restated leak). Determinism (canonical-order catalog; no adapter RNG; byte-identical digest). Differential correctness
(every adapter bit-matches its hand-built twin; the price-only boundary pin is the top-level gate). No hot-path
allocation (ingestion is cold-path; the eval loop is untouched).

## Dependencies
**Upstream:** `alpha::{Panel, datafields}`, `learn::{feature_matrix, learned_source}`, `risk::{factor_model, exposures}`,
`combine::{extract_streams, AlphaStore, AlphaGate, WeightPolicy, AlphaMetrics}`, `library::Library`, `book::BookPipeline`.
**`p2` S4** — S6.5's external-signal admission is gated by S4's robustness battery, so S6-b follows S4's battery landing
(S6-a is independent). **atx-core (Pattern-B, §2.1):** L9 `as_of_frame` — shipped engine-local; lift recorded.

## Explicitly NOT in this sprint
No in-engine file readers (anti-roadmap #9 — bring-your-own-arrays + schema only). No multi-asset/region/intraday
universe (S9 — widening the axis itself). No new signal families or search changes (S5/S4). No new general-purpose
primitives (the as-of/lineage versioning is the L9 request with an engine fallback). No live/streaming data.

## Baton → next
S6 hands the rest of `p2` a bring-your-own-data engine: every consumer reads through `DataContext`, so a user supplies a
price dataset + any of {features, external signals, factor model, reference} and the factory mines, the library admits
(mined *and* externally-supplied, both gated), the combiner blends, and the multi-horizon book builds a tradeable
portfolio over pluggable, PIT-versioned, lineage-tracked data. **S8** runs its top-to-bottom proof over the
`DataContext`. **S9** extends the catalog to multi-asset/region + intraday universes once the daily-bar proof is sealed.

## References
- Spec: [`sprint-6-data-signal-feature-reference-abstraction-layer.md`](sprint-6-data-signal-feature-reference-abstraction-layer.md).
- As-built (base SHA): `alpha::{panel,datafields}`, `learn::{feature_matrix,learned_source}`,
  `risk::{factor_model,exposures}`, `combine::{store,gate,streams,weight_policy}`, `library::library`,
  `book::pipeline`, `CMakeLists.txt`.
- Research: WorldQuant §8/§4.3; RenTech §6.2/§7.1/§9.1.

## §8 — Self-review (pre-implementation)
- [ ] Spec coverage: every spec unit (S6.0–S6.9) maps to a §4 task; the four resolved forks (catalog/registry; subsume
      alt-data + defer S9; dual signal path; full BYO factor model + reference) each have a task.
- [ ] No placeholder/TBD: the two spec "freezes at kickoff" forks are resolved here — `FactorModelArtifact` ∈ `data::`
      (§0.3), ragged-grid alignment policy = price-defines-axis + NaN/drop-count (§0.8).
- [ ] Type consistency: `Dataset::create`, `DatasetCatalog::register_dataset/resolve/names`, `align_onto`,
      `price_to_panel`, `merge_features_into_panel`, `signal_to_candidates`, `artifact_to_factor_model`,
      `reference_spans`, `DataContext::{price_panel,factor_model_override,signal_admit_candidates}`,
      `BookPipeline::run_with_context` — names used identically across §2/§4.
- [ ] The boundary pin has a concrete guarantee (price adapter == `with_datafields` → identical `Panel` → identical
      digest, S6.4a seeds it, S6.8 proves it E2E).
- [ ] CMake: every new `src/data/*.cpp` is registered (§0.6); tests/benches auto-glob.
- [ ] Adapters are additive — no existing signature changes; legacy `BookPipeline::run` is the untouched anchor.
