# Sprint 6 (p2) — Data / Signal / Feature / Reference Abstraction Layer — Implementation Progress

**Status:** 🟡 IN PROGRESS — S6.0 done (marker + recon + seam map). S6.1–S6.9 pending.
**Worktree:** `C:\Users\natha\atx-wt\p2-s6` (isolated; one branch per worktree)
**Branch:** `feat/p2-s6-data-layer`
**Base:** `main` @ `76f295e70a582f86083dfaf352d44d4ea0fc2712`
**Started:** 2026-06-14
**Source plan:** [`sprint-6-data-signal-feature-reference-abstraction-layer-implementation-plan.md`](sprint-6-data-signal-feature-reference-abstraction-layer-implementation-plan.md)
**Spec (the *what*):** [`sprint-6-data-signal-feature-reference-abstraction-layer.md`](sprint-6-data-signal-feature-reference-abstraction-layer.md)

> **Execution mode:** subagent-driven development (fresh implementer per unit + two-stage review:
> spec-compliance then code-quality). **Cadence (user-set):** gate at S6-a — implement S6.0–S6.4
> (recon + Dataset + Catalog + Align + adapt_panel/feature), then stop and report before S6.5–S6.9.

---

## Per-unit status

| Unit  | Title                                                              | Status      | Commit | Tests | Notes |
|-------|--------------------------------------------------------------------|-------------|--------|-------|-------|
| S6-0  | Marker + ledger + as-built recon-verify + seam map                 | ✅ done     | TBD    | —     | This file. Recon confirms all §0 signatures hold. One drift note: `library::Library::admit` signature is `admit(const AlphaCandidate &c, const AlphaGate &gate)` — takes both args; no source-level drift vs plan §0.4. Full seam map below. |
| S6-1  | `data::Dataset` + `DatasetSchema`                                  | ✅ done     | 729d084 | 10   | dataset.hpp + dataset.cpp + data_dataset_test.cpp |
| S6-2  | `data::DatasetCatalog` + as-of index + lineage                     | ✅ done     | 2a62605 | 9    | catalog.hpp + catalog.cpp + data_catalog_test.cpp; resolve() returns `Result<reference_wrapper<const Dataset>>` (tl::expected cannot hold references) |
| S6-3  | PIT alignment rail (date×inst grid join, NaN/drop/count)           | ✅ done     | a86d148 | 7    | align.hpp + align.cpp + data_align_test.cpp; `as_of_index` axis helper added inline to dataset.hpp (truncation-invariant `upper_bound`-step-back). Match-by-InstKey; missing→NaN (never imputed); delisted final-value carry-forward (no survivorship); future-row drop counted but never read. |
| S6-4  | `adapt_panel` (Price→Panel) + `adapt_feature` (Feature merge)      | ⏳ pending  | —      | —     | — |
| S6-5  | `adapt_signal` (Signal→AlphaCandidate via extract_streams)         | ⏳ pending  | —      | —     | — |
| S6-6  | `FactorModelArtifact` + `adapt_factor` (BYO factor model)          | ⏳ pending  | —      | —     | — |
| S6-7  | Alt-data subsumption test (no new ingestion code needed)           | ⏳ pending  | —      | —     | — |
| S6-8  | `DataContext` + `BookPipeline` DataContext overload                 | ⏳ pending  | —      | —     | — |
| S6-9  | E2E BYO capstone test + ingest bench + ROADMAP close               | ⏳ pending  | —      | —     | — |

---

## §0 As-built recon-verify (S6.0)

Recon run at `76f295e70a582f86083dfaf352d44d4ea0fc2712` against the live tree. Each §0 plan claim is verified below.

### 0.1 `alpha::Panel` — CONFIRMED (no drift)

**`Panel::create`** — `atx-engine/include/atx/engine/alpha/panel.hpp` **L88–90**:
```cpp
[[nodiscard]] static atx::core::Result<Panel>
create(atx::usize dates, atx::usize instruments, std::vector<std::string> field_names,
       std::vector<std::vector<atx::f64>> field_data, std::vector<std::uint8_t> universe);
```
Plan claim: `create(dates, instruments, field_names, field_data, universe)` at ~L88. ✅ EXACT.

**`Panel::create_borrowed`** — L96–99:
```cpp
[[nodiscard]] static atx::core::Result<Panel>
create_borrowed(atx::usize dates, atx::usize instruments, std::vector<std::string> field_names,
                std::vector<std::span<const atx::f64>> columns,
                std::vector<std::uint8_t> universe);
```
Plan claim: ~L96. ✅ EXACT.

**`Panel::field_id`** — L123–132 (inline body):
```cpp
[[nodiscard]] atx::core::Result<FieldId> field_id(std::string_view name) const
```
Plan claim: ~L123. ✅ EXACT.

**`Panel::field_cross_section`** — L144–149 (inline body):
```cpp
[[nodiscard]] std::span<const atx::f64> field_cross_section(FieldId field, DateIdx date) const noexcept
```
Plan claim: ~L144. ✅ EXACT.

**`Panel::in_universe`** — L159–163 (inline body):
```cpp
[[nodiscard]] bool in_universe(DateIdx date, atx::usize inst) const noexcept
```
Plan claim: ~L159. ✅ EXACT.

### 0.2 `datafields::with_datafields` — CONFIRMED (no drift)

**File:** `atx-engine/include/atx/engine/alpha/datafields.hpp` **L153–156**:
```cpp
[[nodiscard]] inline atx::core::Result<Panel>
with_datafields(atx::usize dates, atx::usize instruments, std::vector<std::string> field_names,
                std::vector<std::vector<atx::f64>> field_data, std::vector<std::uint8_t> universe,
                std::span<const atx::u16> adv_windows)
```
Plan claim: ~L153. ✅ EXACT. This is the function the Price→Panel adapter (S6.4) must reproduce bit-for-bit.

### 0.3 `learn::build_features` + `FeatureSpec` — CONFIRMED (no drift)

**File:** `atx-engine/include/atx/engine/learn/feature_matrix.hpp`

**`FeatureSpec`** — L78–83:
```cpp
struct FeatureSpec {
  std::vector<std::string> raw_fields;
  std::vector<combine::AlphaId> pool_alphas;
  std::vector<atx::u16> horizons{1, 5, 21};
  atx::u16 max_lookback{0};
};
```
Plan claim: `FeatureSpec{raw_fields, pool_alphas, horizons, max_lookback}` at ~L78. ✅ EXACT.

**`build_features`** — L200–202:
```cpp
[[nodiscard]] atx::core::Result<FeatureMatrix>
build_features(const alpha::Panel &panel, const combine::AlphaStore &store,
               const FeatureSpec &spec);
```
Plan claim: `build_features(panel, store, spec)` at ~L200. ✅ EXACT.

### 0.4 `risk::FactorModel::create` + `FactorComponents` + `FactorModelBuilder::build_components` — CONFIRMED (no drift)

**File:** `atx-engine/include/atx/engine/risk/factor_model.hpp`

**`FactorModel::create`** — L111–113:
```cpp
[[nodiscard]] static atx::core::Result<FactorModel>
create(atx::core::linalg::MatX x, atx::core::linalg::MatX f, atx::core::linalg::VecX d,
       atx::usize fit_begin, atx::usize fit_end)
```
Plan claim: `FactorModel::create(MatX x, MatX f, VecX d, fit_begin, fit_end)` at ~L111. ✅ EXACT.

**`FactorComponents`** — L461–466:
```cpp
struct FactorComponents {
  atx::core::linalg::MatX X; // M×K exposures (X[0], the current cross-section)
  atx::core::linalg::MatX F; // K×K factor covariance (Ledoit-Wolf shrunk, SPD)
  atx::core::linalg::VecX D; // M specific (idiosyncratic) variances
  atx::usize fit_end;        // the fit window upper bound (== window)
};
```
Plan claim: `FactorComponents{X,F,D,fit_end}` at ~L461. ✅ EXACT.

**`FactorModelBuilder::build_components`** — L509–511:
```cpp
[[nodiscard]] atx::core::Result<FactorComponents>
build_components(const PanelView &panel, atx::usize window, std::span<const atx::f64> market_cap,
                 std::span<const atx::u32> group_id) const
```
Plan claim: `FactorModelBuilder::build_components(panel, window, market_cap, group_id)` at ~L509. ✅ EXACT.

### 0.5 `library::Library::admit` + `AlphaCandidate` — CONFIRMED (no drift)

**File:** `atx-engine/include/atx/engine/library/library.hpp`

**`AlphaCandidate`** — L90–98:
```cpp
struct AlphaCandidate {
  atx::u64 canon_hash;
  std::span<const atx::f64> pnl;      // realized pnl stream (length T)
  std::span<const atx::f64> pos_flat; // positions (T*N), period-major then inst-minor
  combine::AlphaMetrics metrics;
  Provenance prov;
  atx::usize as_of;                   // as-of period for the Candidate->Admitted transition
  ISignalSource *source = nullptr;    // may be null in tests
};
```
Plan claim: `AlphaCandidate{canon_hash, pnl, pos_flat, metrics, prov, as_of, source}` at ~L90. ✅ EXACT.

**`Library::admit`** — L141:
```cpp
[[nodiscard]] AdmitVerdict admit(const AlphaCandidate &c, const AlphaGate &gate)
```
Plan claim: `Library::admit(c, gate)` at ~L141. ✅ EXACT.

### 0.6 `book::BookPipeline` ctor + `run` — CONFIRMED (no drift)

**File:** `atx-engine/include/atx/engine/book/pipeline.hpp`

**`BookPipeline` ctor** — L213–216:
```cpp
BookPipeline(library::Library &lib, const alpha::Library &dsl, const alpha::Panel &panel,
             const exec::ExecutionSimulator &sim, const WeightPolicy &policy,
             const combine::AlphaGate &gate) noexcept
```
Plan claim: `BookPipeline(lib, dsl, panel, sim, policy, gate)` at ~L213. ✅ EXACT.

**`run`** — L220:
```cpp
[[nodiscard]] atx::core::Result<PipelineReport> run(const PipelineConfig &cfg)
```
Plan claim: `run(cfg)` at ~L220. ✅ EXACT.

### 0.7 CMake explicit enumeration — CONFIRMED (no drift)

`atx-engine/CMakeLists.txt` L1–53: `add_library(atx-engine STATIC src/...)` explicitly lists every `.cpp`. Tests/benches are auto-globbed (via `add_subdirectory(tests)` + `add_subdirectory(bench)` — the `tests/` and `bench/` CMakeLists use `file(GLOB_RECURSE … CONFIGURE_DEPENDS …)`).

`src/data/` already present in the static list at **L37–38**:
```
    src/data/data_handler.cpp
    src/data/shm_bar_feed.cpp
```
Plan claim: `src/data/` has `data_handler.cpp` + `shm_bar_feed.cpp`. ✅ EXACT. Every new `src/data/*.cpp` in this sprint **must** be added to the explicit list (S6.2 CMake edit: `dataset.cpp` + `catalog.cpp`; S6.3: `align.cpp`; S6.4: `adapt_panel.cpp` + `adapt_feature.cpp`; S6.5: `adapt_signal.cpp`; S6.6: `adapt_factor.cpp`; S6.8: `context.cpp`).

### 0.8 External-signal path: `alpha::extract_streams` + `WeightPolicy` — CONFIRMED (no drift)

**`alpha::extract_streams`** — `atx-engine/include/atx/engine/alpha/streams.hpp` L174–176:
```cpp
[[nodiscard]] inline atx::core::Result<AlphaStreams>
extract_streams(const SignalSet &signals, const WeightPolicy &policy, const Panel &panel,
                const exec::ExecutionSimulator &sim)
```
`(date×instrument)` scores flow in as `SignalSet::alphas[i].values` (date-major, length `dates*instruments`).
`WeightPolicy::to_target_weights(signal_row, universe)` maps each date's cross-section → gross-normalized target weights.
`pnl[t] = Σ_j w_j[t-1]·ret_j[t] − turnover[t]·cost_rate` (no-look-ahead; pnl[0]=0). Returns `AlphaStreams`.

**`WeightPolicy`** — `atx-engine/include/atx/engine/loop/weight_policy.hpp` L163–184:
```cpp
struct WeightPolicy {
  Transform transform = Transform::Rank;
  bool industry_neutral = false;
  bool dollar_neutral = true;
  atx::f64 gross_leverage = 1.0;
  atx::f64 truncation = 0.0;
  atx::f64 winsorize_limit = 0.025;
};
```
The external-signal adapter (S6.5) must call `extract_streams(signals, policy, panel, sim)` and then `compute_metrics(pnl, pos_flat, n_instruments, book_size)` to populate `AlphaMetrics`. ✅ CONFIRMED.

---

## §0 Recon — DRIFT SUMMARY

**No functional drift found.** All eight §0 plan claims match the live tree at base SHA `76f295e7`. The plan's line-number annotations are accurate to within ±2 lines in every case. No signatures changed.

Minor annotation note (non-drift): the plan cites `FactorComponents` at ~L461 and `build_components` at ~L509; the live file has them at L461 and L509 exactly. The plan cites `BookPipeline` ctor at ~L213 and `run` at ~L220; live file has them at L213 and L220 exactly.

---

## Seam map (S6.8 DataContext wiring + S6.5 signal-adapter shapes)

### A. Panel consumer sites (every place the fixed `alpha::Panel` is consumed — `DataContext` must supply it)

| File | Line | Consumer | What it reads |
|------|------|----------|---------------|
| `include/atx/engine/book/pipeline.hpp` | L213 | `BookPipeline::BookPipeline(…, const alpha::Panel &panel, …)` | Stores `const alpha::Panel &panel_` borrow for lifetime of pipeline. |
| `include/atx/engine/book/pipeline.hpp` | L225 | `ResearchDriver engine{lib_, dsl_, panel_, sim_, policy_, gate_}` | Forwards `panel_` to `ResearchDriver` (the mine engine). |
| `include/atx/engine/book/pipeline.hpp` | L235 | `panel_.instruments()` | Universe size for constituent double sizing. |
| `include/atx/engine/book/pipeline.hpp` | L257 | `accumulate_report(books, panel_, returns_field_id(cfg), …)` | Passed to `accumulate_report` for the `rev` field. |
| `include/atx/engine/book/pipeline.hpp` | L329–330 | `panel_.field_cross_section(rev, t)` | Constituent fit-stream construction (returns CS at date t). |
| `include/atx/engine/book/pipeline.hpp` | L356 | `period < panel_.dates()` / `panel_.field_cross_section(rev, period)` | Constituent eval-schedule construction. |
| `include/atx/engine/book/pipeline.hpp` | L617 | `panel_.dates()` / `panel_.field_cross_section(close, date)` | Standalone PanelView backing (RESIDUAL #2) — fills close field for each date. |
| `include/atx/engine/book/pipeline.hpp` | L686–689 | `panel_.dates()` / `panel_.field_cross_section(rev, t)` | `book_sr_sigma()` — constituent mean stream for size_book. |
| `include/atx/engine/factory/research_driver.hpp` | L172 | `ResearchDriver(…, const alpha::Panel &panel, …)` | Stores `const alpha::Panel &panel_` borrow; passed through to every `run()` call. |
| `include/atx/engine/factory/factory.hpp` | L178 | `Factory(…, const alpha::Panel &panel, …)` | Stores `const alpha::Panel &panel_` borrow; used by `mine()` / `mine_into()`. |

**S6.8 DataContext overload strategy:** add a `BookPipeline(DataContext &ctx, …)` overload that derives `panel` via the price-only adapter (`with_datafields`) and then delegates to the existing `BookPipeline(lib, dsl, panel, sim, policy, gate)` ctor. The existing ctor + all internal `panel_` reads are untouched. Boundary-pin test drives the `DataContext` overload with a price-only context and asserts `book_digest == report_digest` of the fixed-panel path.

### B. Signal-adapter shapes (S6.5 — `adapt_signal` must fill these exactly)

#### `alpha::AlphaStreams` — `include/atx/engine/alpha/streams.hpp` L95–132
```cpp
struct AlphaStreams {
  std::vector<atx::f64> pnl_flat;   // [n_alphas * n_periods], alpha-major
  std::vector<atx::f64> pos_flat;   // [n_alphas * n_periods * n_instruments], alpha→period→inst
  atx::usize n_alphas_{};
  atx::usize n_periods_{};
  atx::usize n_instruments_{};
  // accessors: pnl(alpha), positions(alpha, period), n_alphas(), n_periods(), n_instruments()
};
```
Produced by `alpha::extract_streams(signals, policy, panel, sim)`. pnl[0] = 0 (structural zero; §0-F). pnl[t≥1] = Σ_j w_j[t-1]·ret_j[t] − turnover[t]·cost_rate.

#### `combine::AlphaMetrics` — `include/atx/engine/combine/metrics.hpp` L56–73
```cpp
struct AlphaMetrics {
  atx::f64 sharpe;       // sqrt(252) * mean(pnl[1..T)) / std(pnl[1..T))
  atx::f64 turnover;     // mean(Σ|Δw| per period) over all periods
  atx::f64 returns;      // 252 * mean(pnl[1..T))
  atx::f64 drawdown;     // max peak-to-trough drawdown of cumprod(1+r)
  atx::f64 margin;       // returns / max(turnover, 1e-9)
  atx::f64 fitness;      // sqrt(|returns|/max(turnover, 0.125)) * sharpe  (WQ §4.4)
  atx::f64 holding_days; // 1 / max(turnover, 1e-9)
};
```
Filled by `combine::compute_metrics(pnl, positions_flat, n_instruments, book_size)`. The signal adapter (S6.5) calls this after `extract_streams`.

#### `library::Provenance` — `include/atx/engine/library/record.hpp` L138–143
```cpp
struct Provenance {
  std::string expr_source;              // S3 DSL expression text
  std::vector<atx::u64> parent_hashes; // lineage (parent canonical hashes)
  atx::u16 mutation_op{0};             // factory mutation operator id
  atx::u64 seed{0};                    // RNG seed
};
```
For an externally-sourced signal, S6.5 synthesizes: `expr_source = "<external:dataset_name>"`, `parent_hashes = {}`, `mutation_op = 0`, `seed = 0`. This records the signal as externally-sourced in the PIT journal/manifest.

#### `library::AlphaCandidate` — `include/atx/engine/library/library.hpp` L90–98
```cpp
struct AlphaCandidate {
  atx::u64 canon_hash;                  // stable cross-run dedup key (hash of source + params)
  std::span<const atx::f64> pnl;        // realized pnl stream (length T) — NON-OWNING
  std::span<const atx::f64> pos_flat;   // positions (T*N), period-major then inst-minor — NON-OWNING
  combine::AlphaMetrics metrics;
  Provenance prov;
  atx::usize as_of;                     // as-of period for the Candidate->Admitted lifecycle edge
  ISignalSource *source = nullptr;      // may be null for external signals
};
```
**SAFETY:** `pnl` and `pos_flat` are non-owning spans — the S6.5 adapter MUST keep the backing `AlphaStreams` (or equivalent owned buffers) alive across the `Library::admit` call. The adapter owns the `AlphaStreams` result of `extract_streams`; the spans alias into it.

#### `alpha::extract_streams` full signature — `include/atx/engine/alpha/streams.hpp` L174–176
```cpp
[[nodiscard]] inline atx::core::Result<AlphaStreams>
extract_streams(const SignalSet &signals, const WeightPolicy &policy, const Panel &panel,
                const exec::ExecutionSimulator &sim)
```
**Input:** `SignalSet` — a `std::vector<Alpha>` where `Alpha.values` is `dates*instruments` (date-major, NaN where out-of-universe). The external signal's `(date×instrument)` score matrix is wrapped into a `SignalSet` by the adapter before calling `extract_streams`.

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| TBD | S6-0 | docs(s6-0): open sprint-6 ledger + as-built recon-verify + seam map |

---

## Open cross-platform residuals

None introduced in S6.0. Carried from S7: POSIX SHM/process backends are Linux-CI-pending (not S6 scope).
