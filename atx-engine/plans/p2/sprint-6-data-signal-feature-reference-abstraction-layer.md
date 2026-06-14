# Sprint S6 — Data / Signal / Feature / Reference Abstraction Layer — the BYO-Data Engine (sprint spec)

**Status:** ⏳ proposed (not open). A **foundational retrofit** over the as-built `p0`/`p1` seams
(`alpha::Panel`/`datafields`, `learn::FeatureMatrix`, `risk::{FactorModel, exposures}`, `library::Library`,
`book::BookPipeline`, `fund::` meta-book). Independent of the S3/S4 alpha-depth track; opens concurrently. **Split
`S6-a` / `S6-b`** (10 units > the 7-unit threshold, per the `p0` 4a/4b/4c precedent).
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md) ·
[`../docs/module.md`](../docs/module.md) · [`../docs/implementation-quality.md`](../docs/implementation-quality.md) ·
[`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md)
**Grounded in:** [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) §8
(the 125,000+ PIT datafield catalog, universes, regions, delay), §4.3 (datafield families) ·
[`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md)
§6.2/§7.1/§9.1 (the deeper-cleaner-data edge, PIT as-of versioning, survivorship discipline) ·
[`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md) §1.2 (the
datafield inputs alphas consume) · `p1` S1 (the bias-audit battery the ingestion rail extends), `p1` S5 (the feature
matrix), `p1` S8 (the factor model).

---

## Why this sprint

`p1` built a working alpha factory; `p2` S3/S4 deepen what it can express and how smartly it searches. But the factory
is still wired to **one fixed in-memory `alpha::Panel` per run**, and every exogenous input is computed *internally from
price*. The as-built reality (confirmed by recon):

1. **Price is hard-wired, not plugged.** The canonical in-memory representation — `alpha::Panel` (date-major `f64`
   columns + a `{0,1}` universe mask + a field-name dictionary) — is produced either by the backtest loop's
   `RollingPanel` (fed by an `IDataHandler`) or by `attach_segment_panel()` over an `.atxseg` segment. There is **no
   generic "aligned arrays + schema → Panel" builder**, and `BookPipeline`/`Factory` take a single fixed `Panel`
   reference. The only abstraction seam (`IDataHandler`) sits at the *event* level, not the dataset level.

2. **Features come only from price + the pool.** `learn::FeatureMatrix` is built from `Panel` raw fields plus
   `AlphaStore` pool-alpha positions (`build_features(panel, store, spec)`). There is **no way to inject an external
   feature dataset** — "here are my 50 fundamental features aligned by `(date, instrument)`" has no home.

3. **The factor model is always price-derived.** `risk::FactorModelBuilder` regresses style/sector exposures out of the
   price panel (`build_exposures`), estimates `F` from WLS factor returns and `D` from residuals. `market_cap` and
   `group_id` enter as *ad-hoc optional spans*; there is **no clean "bring your own factor model"** — `FactorModel::
   create(X, F, D)` exists as a low-level constructor but has no ingestion adapter or reference-dataset feed.

4. **External signals have no first-class path.** `ISignalSource` is pluggable, and `extract_streams` accepts a
   `SignalSet`, but a *precomputed external signal library* cannot be admitted into `library::Library` (which is
   mining/learning-output only) nor cleanly fed as ML features — today it must masquerade as a `ScriptedSignalSource`
   double or a compiled VM program.

The cost of this is the engine's identity: it is **not** bring-your-own-data. You cannot say *"here is my price data,
here is my signal dataset, here is my factor model — run it through the engine and build me a WorldQuant alpha library
and an optimal allocation framework on top of it."* S6 builds exactly that.

S6 introduces a **first-class data abstraction layer** as a retrofit *under* the existing pipeline: a typed,
PIT-versioned `data::Dataset` (roles *Price / Feature / Signal / Reference*) + a `DatasetCatalog` registry
(schema/lineage/as-of versioning) + an ingestion/alignment/PIT-validation rail + a `DataContext` facade that replaces
the bare-`Panel` argument and lowers each registered dataset into the existing concrete structures via
differential-pinned adapters. **The only required input is a price dataset.** Features, external signals, a factor
model, and reference data are all optional plug-ins, each with a price-only fallback that computes internally exactly as
today.

Two re-scopings fall out, both resolved at re-theme (see ROADMAP "Strategic decisions"):
- **Alt-data stops being special-cased.** The deferred fundamental/analyst/news/sentiment frontier (old S6) becomes
  *ordinary `Feature`/`Reference` dataset instances* — no bespoke per-source ingestion code; news/sentiment stays
  event-gated through the existing `trade_when` operator. The abstraction *is* the alt-data story.
- **Structural-universe breadth defers to S9.** Multi-asset/region and intraday-bar universes widen the *axis itself*
  (cross-asset risk, sub-daily horizon); they ride S6's catalog but land **after** the S8 capstone so the proof runs
  first over the trustworthy US-equity daily-bar spine.

The non-negotiable: this is **additive**. With *only* a price dataset registered and no optional plug-ins, the
`DataContext` path must reduce **bit-for-bit** to today's fixed-`Panel` `BookPipeline` — the regression anchor that
proves the abstraction breaks none of S1–S5/S7's proven layers.

---

## Architecture (the seam, top reads down)

```
user code ── data::Dataset{role, schema, columns, provenance} ──► DatasetCatalog.register(name, dataset)
                                                                        │  (schema enforcement · lineage graph ·
                                                                        │   deterministic order · as-of versioning)
                                                                        ▼
                                                                   DataContext  (facade: required Price + optional plugs)
                                                                        │  adapters (lowering, differential-pinned)
        ┌───────────────────┬───────────────────┬──────────────────────┼──────────────────────┐
        ▼                   ▼                   ▼                      ▼                      ▼
   alpha::Panel      learn::FeatureMatrix   risk::FactorModel    library::Library        exposures inputs
   (Price+Feature      (Feature columns       (FactorModelArtifact  (Signal admit,         (Reference:
    columns merged;     as ML inputs)          X/F/D  OR  Reference- gated by deflation     sector/group_id,
    subsumes with_                              fed builder)         + S4 robustness)        market_cap, styles)
    datafields)
        └───────────────────┴────── factory::{Factory, ResearchDriver} · book::BookPipeline · fund:: meta-book ──────┘
```

**New `data::` types**
- `Dataset` — a typed, PIT-versioned `(date × instrument)` named-column store with a `role ∈ {Price, Feature, Signal,
  Reference}` and a provenance record. Columns are `f64` date-major over a declared `(date, instrument)` axis, NaN for
  out-of-universe / not-yet-knowable cells (the existing Panel discipline).
- `FactorModelArtifact` — a typed *external factor model* (exposures `X`, factor covariance `F`, specific variance `D`,
  fit window). Distinct from the column datasets because a factor model is not a column store. (Home — `data::` vs
  `risk::` — freezes in the impl-plan.)
- `DatasetSchema` — declared column names, dtype, role, **PIT delay**, universe/region tags, and the
  as-of/effective-date/restatement policy.
- `DatasetCatalog` — register/resolve named datasets; enforce schema on register; track a lineage/provenance graph;
  resolve PIT as-of versions deterministically; iterate in canonical order.
- `DataContext` — the facade the pipeline consumes: a required price view + resolved optional plugs, each with a
  price-only fallback.
- The **ingestion/alignment/PIT-validation rail** — reconcile heterogeneous `(date × instrument)` grids onto the
  canonical axis; enforce no-look-ahead / no-survivorship / NaN policy / as-of versioning **at ingestion**.

---

## Scope — units

### S6-a — abstraction core + price + feature

#### S6.0 — Marker + ledger + as-built recon
Open `sprint-6-progress.md`, freeze scope, base SHA. As-built recon against the seams S6 retrofits: `alpha::Panel` +
`alpha::datafields` (`with_datafields` derived columns), `loop::{RollingPanel, panel_types}`, `data::{data_handler,
shm_bar_feed, segment_panel}` (`IDataHandler`, `attach_segment_panel`), `learn::{feature_matrix, learned_source}`
(`build_features`, `FeatureSpec`), `risk::{factor_model, exposures}` (`FactorModelBuilder::build_components`,
`build_exposures`, `FactorModel::create`, the `market_cap`/`group_id` optional spans), `library::Library` (`admit`,
`AlphaCandidate`, the gate order), `book::BookPipeline` + `fund::` meta-book (the bare-`Panel` argument sites).
Produce the **seam map**: every place a `Panel`/feature/factor/signal is consumed, and the exact bare-`Panel` call
sites `DataContext` must thread through (S6.8).

#### S6.1 — `data::Dataset` + `DatasetSchema`
The typed, PIT-versioned `(date × instrument)` named-column value type, with `role`, dtype, universe mask, and a
provenance record; plus `DatasetSchema` (column names/dtype/role, PIT delay, universe/region tags, as-of policy).
Construction is **bring-your-own-arrays**: aligned column spans + a schema in, validated `Dataset` out (no file
parsing — anti-roadmap #9). **Differential:** round-trip + schema-conformance tests; a `Dataset` carrying exactly the
OHLCV columns must be byte-identical, on lowering, to the `Panel` the current path builds.

#### S6.2 — `data::DatasetCatalog` registry
Register/resolve datasets by name; enforce schema on register (reject dtype/axis/role mismatch); maintain a
lineage/provenance graph (which dataset derived from which); resolve PIT as-of versions deterministically; iterate
registered datasets in **canonical order** (determinism). *atx-core Pattern-B edge: **L9 `as_of_frame`** — PIT
as-of/effective-date/restatement versioning + lineage in the frame/tsdb; engine-side fallback = an as-of index +
lineage map over the existing PIT panel + tsdb.*

#### S6.3 — Ingestion + alignment + PIT-validation rail
The correctness heart of the sprint. Reconcile heterogeneous BYO `(date × instrument)` grids onto the canonical
date/universe axis (a user feature set may have different dates, a sparser universe, a different instrument ordering);
enforce, **at ingestion**: no-look-ahead (an as-of/effective-date dataset transitions only on PIT boundaries — no
restated value leaks into a past decision), no-survivorship (a delisted instrument's final value passes through; never
retroactively erased), and the NaN/out-of-universe policy. **The alignment is truncation-invariant by test** — the new
PIT enforcement point for every plugged dataset (carried-forward invariant #4). Ragged-grid alignment policy (how
missing-date / extra-instrument reconciliation resolves) freezes here.

#### S6.4 — PriceDataset → `Panel` adapter + FeatureDataset → `FeatureMatrix` injection
- **Price (required):** lower a `Price` dataset into `alpha::Panel`, **subsuming `with_datafields`** (the
  `vwap`/`adv{d}`/`dollar_volume` derived columns become a deterministic post-ingest transform over the price columns).
  The **price-only fallback** path must produce a `Panel` bit-identical to today's construction.
- **Feature (optional):** lower a `Feature` dataset into `learn::FeatureMatrix` as first-class external inputs
  (extending `FeatureSpec` so raw-field + pool-alpha + **external-feature** columns coexist, PIT-aligned). Respect the
  existing train/eval-parity constraint (the `learned_source` eval path reads only what a `PanelView` can reconstruct
  — external features that are not panel-reconstructable are train-only, flagged at spec construction).

### S6-b — signal + factor-model + reference + subsumption + wiring + capstone

#### S6.5 — SignalDataset (external precomputed signals — both paths)
A `Signal` dataset (precomputed per-`(date, instrument)` scores) enters **both**:
- **As ML features** — usable in a `FeatureMatrix` like any feature column (subject to the S6.4 parity rule).
- **As direct library admissions** — a `Signal` dataset can be admitted into `library::Library` *bypassing GA mining*,
  converted to the `AlphaCandidate` shape (PnL + positions via the existing `extract_streams` over a `WeightPolicy`),
  and **gated by the same deflated-Sharpe + S4 robustness battery** as a mined alpha (synthetic-recovery is N/A for a
  given signal, but regime/walk-forward survival + net-of-cost + the deflated gate all apply). Provenance records the
  signal as externally-sourced (not engine-discovered) in the lineage graph. This makes *"here is my signal dataset"*
  first-class end-to-end.

#### S6.6 — BYO factor model + reference-dataset ingestion
Two optional factor-model plugs, both falling back to today's internal price-derived build:
- **Full BYO model:** a `FactorModelArtifact` (external `X`, `F`, `D` + fit window) ingested through a clean adapter
  onto `risk::FactorModel::create`, with PSD/shape validation (reuse the existing `psd_repair`/shape checks). The
  optimizer (S1) then trades the user's `V` instead of a computed one.
- **Reference-fed builder:** a `Reference` dataset (sector/industry `group_id`, `market_cap`, custom style columns)
  feeds the *internal* `FactorModelBuilder` — formalizing the today-ad-hoc `market_cap`/`group_id` spans into a typed,
  PIT-versioned reference plug and adding user-supplied style factors to `build_exposures`.
- **Fallback:** with neither plug, the builder computes `X`/`F`/`D` from price exactly as today.

#### S6.7 — Alt-data subsumption
Demonstrate the abstraction generalizes the deferred alt-data frontier with **no bespoke per-source code**: express a
fundamental/analyst dataset as a `Reference`/`Feature` dataset (with as-of/restatement versioning via the S6.3 rail),
and a news/sentiment/event dataset as a `Feature` dataset surfaced through the **existing** `trade_when` event-gating
operator. The proof is that ingesting these requires *only* `DatasetCatalog.register` + a schema — no new ingestion
path. Records, as a residual, any alt-data shape the column model cannot express (e.g. genuinely event-stream data with
no natural `(date, instrument)` grid).

#### S6.8 — `DataContext` facade wiring (the boundary pin)
Thread `DataContext` through every bare-`Panel` call site found in S6.0: `factory::{Factory, ResearchDriver}`,
`book::BookPipeline`, and the `fund::` meta-book — each consuming a `DataContext` and pulling its `Panel`/feature/
factor/signal inputs through the adapters. **Boundary pin:** a `DataContext` built from *only* a price dataset, no
optional plugs, must drive `BookPipeline` to a **byte-identical** `book_digest`/`report_digest` versus today's
fixed-`Panel` path — the regression anchor proving the abstraction is purely additive.

#### S6.9 — E2E BYO capstone + report + bench + close
- **E2E BYO test:** register a price dataset + a plugged feature dataset + a plugged external signal dataset + a plugged
  `FactorModelArtifact`, run the full pipeline (mine/admit + external-signal admit → combine → multi-horizon book), and
  assert a deterministic digest — the literal *"price + signals + factor model in → library + tradeable portfolio
  out"* proof.
- **Catalog/lineage report:** a reproducible headless artifact listing registered datasets, their schemas, PIT
  versions, and the provenance graph (which library alphas trace to which external signal datasets).
- **Bench:** ingestion + alignment throughput; confirm the lowering adds no hot-path allocation and the price-only path
  matches the baseline.
- **Close** ceremony (residuals lifted, ROADMAP S6 status bumped, `sprint6.md` user ref).

---

## Exit criteria

- `data::Dataset` + `DatasetSchema` + `DatasetCatalog` + `DataContext` ship; a price-only `Dataset` lowers to a `Panel`
  **bit-identical** to today's construction.
- The ingestion/alignment/PIT rail reconciles heterogeneous BYO grids onto the canonical axis and enforces
  no-look-ahead / no-survivorship / NaN at ingest, proven by a **truncation-invariance** test per dataset role.
- **Price-required, rest-optional** is real: a price-only `DataContext` runs the full pipeline; each optional plug
  (feature / signal / factor-model / reference) is independently attachable with a price-only fallback that computes
  internally as today.
- External feature datasets inject into `FeatureMatrix`; external signal datasets enter **both** as ML features **and**
  as **gated** `library::Library` admissions (deflated-Sharpe + S4 robustness battery), with provenance recorded.
- A BYO `FactorModelArtifact` (external `X`/`F`/`D`) drives the optimizer; a `Reference` dataset feeds the internal
  builder; neither-plug falls back to the price-derived model.
- Alt-data (fundamental/analyst/news/sentiment) ingests as `Feature`/`Reference` datasets with **no bespoke per-source
  code**; news/sentiment event-gates through existing `trade_when`.
- **Boundary pin:** price-only `DataContext` → `BookPipeline` yields byte-identical `book_digest`/`report_digest`
  versus today's fixed-`Panel` path.
- The E2E BYO capstone (price + feature + signal + factor model → library → multi-horizon book) is deterministic and
  replayable; the catalog/lineage report is a reproducible headless artifact.
- `/W4 /permissive- /WX`, clang-tidy + clang-format clean; test file per unit.

## Invariants this sprint must prove

- **No look-ahead.** The S6.3 ingestion rail is the new PIT enforcement point: every plugged dataset's
  as-of/effective-date/restatement versioning transitions on PIT boundaries only; the heterogeneous-grid alignment is
  truncation-invariant by test. External-signal admission reuses the existing fit/apply firewall.
- **No survivorship.** Delisted instruments' final values pass through every dataset ingest and every alignment; never
  retroactively erased.
- **Point-in-time.** Out-of-universe / not-yet-knowable cells read NaN across every dataset role; restated values never
  leak into past decisions.
- **Determinism.** Catalog resolution + dataset iteration are canonical-order; the lowering adapters introduce no RNG;
  external-signal admission is seeded/order-fixed like any library admit. Same inputs → byte-identical digest.
- **Differential correctness.** Every adapter's output **bit-matches** the hand-built structure it replaces (price→
  Panel, feature→FeatureMatrix, factor-model→FactorModel, signal→AlphaCandidate); the price-only boundary pin is the
  top-level differential gate.
- **No hot-path allocation.** Ingestion/alignment/lowering are cold-path (run once at setup); the steady-state eval
  loop allocates zero, unchanged.

## Dependencies

- **Upstream:** the `p0`/`p1` seams S6 retrofits — `alpha::{Panel, datafields}`, `loop::{RollingPanel, panel_types}`,
  `data::{data_handler, segment_panel}`, `learn::{feature_matrix, learned_source}`, `risk::{factor_model, exposures}`,
  `library::Library`, `book::BookPipeline`, `fund::` meta-book. **`p2` S4** (the robustness battery external-signal
  admission must pass) — S6-b's S6.5 is gated by it, so S6-b follows S4's battery landing.
- **atx-core (Pattern B edge):** **L9 `as_of_frame`** (S6.2 — PIT as-of/effective-date/restatement versioning + dataset
  lineage); engine-side fallback = an as-of index + lineage map over the existing PIT panel + tsdb. Ships the fallback;
  the L9 kernel is an accelerator, not a blocker.

## Explicitly NOT in this sprint

- **No in-engine data-format readers.** S6 is a typed in-memory dataset *catalog* + *contract*. CSV/Parquet/Arrow/JSON
  parsing stays the user's (or atx-tsdb's) job, lowered into a `data::Dataset` at the boundary (anti-roadmap #9). The
  engine ingests aligned arrays/spans + a schema, never a file format.
- **No multi-asset / multi-region / intraday universe.** Widening the axis itself (cross-asset risk, sub-daily horizon)
  is **S9**, post-capstone. S6 makes *new datasets* pluggable on the existing daily-bar US-equity axis.
- **No new signal families or search changes.** Deep-learning alphas are S5; multi-objective search + the robustness
  battery are S4. S6 *plugs data in*; it does not change how signals are discovered (it adds one path: admitting
  externally-supplied signals through the existing gate).
- **No new general-purpose primitives in the engine.** The as-of/lineage versioning is the atx-core L9 request with an
  engine-side fallback; the engine adds no general-purpose data primitive.
- **No live data / streaming.** S6 is a research-time dataset abstraction; live order routing and streaming adapters
  remain delegated/out-of-scope (anti-roadmap #1).

## Baton → next

S6 hands the rest of `p2` a **bring-your-own-data engine**: every consumer reads its inputs through `DataContext`, so a
user supplies a price dataset (required) plus any of {features, external signals, factor model, reference data} and the
factory mines, the library admits (mined *and* externally-supplied, both gated), the combiner blends, and the
multi-horizon book builds a tradeable portfolio — all over pluggable, PIT-versioned, lineage-tracked data. **S8** (the
capstone) runs its top-to-bottom proof over the `DataContext`. **S9** extends the catalog to multi-asset/region +
intraday universes once the daily-bar proof is sealed.
