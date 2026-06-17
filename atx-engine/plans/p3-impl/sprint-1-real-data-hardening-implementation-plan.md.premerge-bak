# p3 S1 — Real-Data Hardening — Implementation Plan (frozen *how*)

**Module:** [`ROADMAP.md`](ROADMAP.md) — atx-engine v4: Real-Data Validation & Robustness Verification.
**Sprint theme:** Tie the as-built ingestion stack, the `p2` S6 BYO-data layer, and the on-disk real US equity data into **one coherent, PIT-correct, digest-pinned real-data path** — corporate actions applied, a real universe constructed, coverage expanded — so the existing `p2` mine→prove pipeline can be pointed at real data without fabricating returns or leaking the future.
**Base:** master @ `<SHA recorded at S1-0>` — **with `p2` S5 (`feat/p2-s5-dl-alphas`) + S6 (`feat/p2-s6-data-layer`) treated as merged** (see S1-0).
**Status:** ⏳ frozen, not opened. This plan is a fossil of intent; mid-sprint scope changes go in `sprint-1-progress.md` "Plan adjustments", never here (`sprint.md` §"The plan moved").
**Discipline:** [`../docs/sprint.md`](../docs/sprint.md) · [`../docs/module.md`](../docs/module.md) · [`../docs/implementation-quality.md`](../docs/implementation-quality.md). Marker + per-unit + close commits; `--no-ff` merge; **no push unless the user asks**.

---

## §0 — As-built reconnaissance (the seams S1 builds on)

**S1-0 must verify every signature in this section against the actual code on the worktree base and correct any drift in the ledger before any other unit starts.** The signatures below are the *expected* as-built surface from the p2 S6 layer + the atx-core/atx-tsdb ingestion stack; treat them as the contract, but **trust the code over this doc** if they diverge.

### 0.1 Ingestion stack (`atx-core` + `atx-tsdb`) — DONE, consume only

| Seam | Signature (expected) | Role |
|---|---|---|
| `atx::core::io::ParquetTable` | `open(path) -> Result<ParquetTable>`; `column_view(name)`, `to_column<T>(name)`, `strings(name)`, predicate/projection | Lazy Arrow parquet scan. Reads the security-master + databento parquet. |
| `atx::tsdb::load_parquet` | `load_parquet(path, time_col, symbol_col, field_cols) -> Result<Segment>` | Long parquet → sealed segment (pivot). |
| `atx::tsdb::load_parquet_scaled` | `..._scaled(path, …, scale_per_field) -> Result<Segment>` | int64 fixed-point fields (OHLCV at 1e-9, volume at 1.0). |
| `atx::tsdb::build_dated_segments` | `build_dated_segments(hive_root, seg_dir) -> Result<vector<path>>` | `date=YYYY-MM-DD/part-*.parquet` → per-date `.seg`. |
| `atx::tsdb::SegmentReader` | `attach(path)`, `value(field,t,inst)`, `present(t,inst)`, `field_block_view(field)`, `times()`, `cutoff_index(now_nanos)` | Zero-copy mmap read of a sealed segment. |

### 0.2 Segment→Panel bridge (`atx-engine`) — DONE, consume only

| Seam | Signature (expected) | Role |
|---|---|---|
| `alpha::attach_segment_panel` | `attach_segment_panel(seg_path, TimeWindow, UniversePolicy) -> Result<MappedPanel>` (`alpha/segment_panel.hpp`) | Map segment, slice window, derive PIT universe → borrowed `Panel`. |
| `alpha::build_augmented_panel` | appends `dollar_volume = close·volume`, `vwap`, `adv{d}` (causal trailing mean) (`alpha/datafields.hpp`) | Derived price fields; NaN-propagating. |
| `alpha::Panel` | `create(dates, instruments, field_names, columns, universe)`, `create_borrowed(...)`, `field_id(name)`, `field_name(id)`, `field_cross_section(f,date)`, `field_all(f)`, `in_universe(date,inst)` (`alpha/panel.hpp`) | Date-major f64 grid + universe mask. Layout: `columns_[field][date*instruments + inst]`. |

### 0.3 S6 BYO-data layer (`feat/p2-s6-data-layer`) — treated DONE, build *between* it and the parquet

| Seam | Signature (expected) | Role |
|---|---|---|
| `data::Dataset` + `DatasetSchema` | typed, PIT-versioned; roles Price/Feature/Signal/Reference; column-by-name accessors (`data/dataset.hpp`, `dataset_schema.hpp`) | The typed container the corporate-action data lands in (S1-2). |
| `data::DatasetCatalog` | register/lookup by name; as-of + lineage (`data/catalog.hpp`) | Named registry; S1-5 registers `price`, `corp_actions`, `universe`. |
| `data::align` | ingestion + alignment + truncation-invariant PIT rail (`data/align.hpp`) | Aligns a Feature/Reference dataset onto the price-defined canonical axis. **Price defines the axis** (resolved S6 fork). |
| `data::adapt_panel` | `PriceDataset` → `Panel`, byte-`==` `with_datafields` (`data/adapt_panel.hpp`) | Price → Panel adapter. |
| `data::adapt_feature` | `merge_features_into_panel(panel, FeatureDataset)` (`data/adapt_feature.hpp`) | Merge extra fields (adjusted close, market cap, sector) into the Panel. |

### 0.4 The `p2` mine→prove pipeline (S2 consumes; S1 only must not break) — DONE

`factory::{SearchDriver, Factory, ResearchDriver, RobustResearchDriver, fitness, pareto}`; `eval::{deflated_sharpe, cpcv, pbo, regime_slice, lockbox}`; `combine::*`; `risk::{MultiPeriodOptimizer, FactorModel}`; `book::BookPipeline`; `parallel::DetPool`. Boundary-pinned to golden digests. **S1 adds no code here; the full suite must stay green per unit.**

### 0.5 On-disk real data (consume)

```
data/databento/equs_ohlcv_1d_by_date/date=YYYY-MM-DD/part-00000.parquet
  schema: date(date32), symbol(str), instrument_id(u32), ts_event(ts[ns,UTC]),
          open/high/low/close(f64), volume(u64), publisher_id, rtype, source_file
  coverage: from 2024-07-01; broad US-equity universe; UNADJUSTED prices.

data/us_split_adjustment_factors/                 (and .../us_security_master_smoke/)
  security_master/security_master.parquet
    schema: date, symbol, cumulative_adjustment_factor, cash_dividend, dividend_currency,
            shares_outstanding, shares_as_of_date, shares_filed_date, sec_cik, sec_sic,
            sec_sic_description, gics_sector_code, gics_sector, gics_sub_industry, gics_source
    coverage: 3 symbols (AAPL + 2), 1962→2026  [EXPANDED in S1-6]
  factors_by_symbol/symbol=SYM/data_0.parquet     schema: symbol,date,return_factor   (canonical, split-only)
  dividends_by_symbol/symbol=SYM/data_0.parquet   schema: symbol,date,cash_dividend,currency,source
  split_events_by_symbol/, shares_outstanding_by_symbol/, sectors_by_symbol/
  manifest/  _cache/
```

Semantics (from [`../../../docs/us_split_adjustment_factors.md`](../../../docs/us_split_adjustment_factors.md)):
- `cumulative_adjustment_factor` = cumulative **future split** factor. `raw_close × factor = split-adjusted close on the latest share basis`. **Split-only** by default (`--mode split`). `--mode adjclose` instead yields the factor mapping raw close → Yahoo adjusted close **including dividends** — used as the S1-3 cross-check **oracle**.
- `cash_dividend` = per-share cash dividend on the **ex-date**.
- Universe is **listed-only** (Nasdaq Trader active symbols) → **survivorship bias**; does not recover delisted equities.
- `shares_outstanding` = SEC fact values **forward-filled by fact `end` date**, with `shares_filed_date` for provenance (the PIT leak guard).
- `gics_*` is **sparse** (licensed); SEC `sic`/`sic_description` is the open fallback.

### 0.6 CMake source registration (do NOT hand-author blindly)

New `src/data/*.cpp` must be registered into the `atx-engine` STATIC library exactly as the S6 units registered `catalog.cpp`/`dataset.cpp`/`adapt_*.cpp` (the S6 impl-plan recorded "the CMake source-registration fix"). **Test `.cpp` under `atx-engine/tests/` are auto-globbed (`CONFIGURE_DEPENDS`) — never hand-edit the test glob.** Build with the `dev` preset via the PowerShell tool and the env wrapper; **do not run clang-tidy**; `/W4 /permissive- /WX` + `/fp:precise` must stay clean.

---

## §0.7 — Resolved conventions (frozen at kickoff)

1. **Adjustment = total-return.** The canonical price field consumed downstream is a **total-return index** (split + reinvested-dividend adjusted). Raw close and split-adjusted close are retained as additional fields for signals that explicitly want them. *(ROADMAP §"Strategic decisions" #1.)*
2. **Back-adjustment is return-invariant ⇒ non-leaking.** Full-history back-adjustment rescales price *levels* when a future split/dividend arrives, but **returns are invariant to a constant multiplicative rescale**. Alphas consume returns/ranks, so back-adjustment does not leak signal. **The PIT leak guard is on the as-of join of fundamentals** (`shares_outstanding`, sector) using `shares_filed_date`/knowledge-date — **not** on price adjustment. This must be stated in `data-ingestion-reference.md`.
3. **Price defines the canonical axis** (inherited S6 fork). Corporate-action / universe datasets align *onto* the price dates×symbols; a symbol/date absent from price is absent from the Panel.
4. **Market cap is split-invariant** (`shares × price`); compute from **raw** close × as-of shares. Liquidity (ADV) uses `dollar_volume` from raw close × volume.
5. **Determinism.** The assembled real-data Panel is **digest-pinned** (`signal_set_digest` over its fields in canonical field order) exactly like every synthetic fixture. Symbol interning order, date axis, and NaN handling are fixed and deterministic.

---

## §1 — Per-unit plan

Each unit: **one sub-agent, one commit (or a tight series), tests green before stop.** Dispatch **sequentially** (S1-2→S1-3→S1-4→S1-5 have a hard data dependency; S1-1 and S1-6 are independent and may run in parallel with the chain but are briefed separately). Every brief ends with the verbatim marker-commit reminder (§3).

---

### S1-0 — Marker (orchestrator, not a sub-agent)

**Scope:** Open the sprint; freeze scope; **record the S5/S6 merge-base reality**.

**Do:**
1. Create worktree + branch per `sprint.md` (`worktree-phase-1-real-data-hardening` / matching branch) off the chosen base.
2. **Resolve the merge-base** (the module's load-bearing precondition): either (a) merge `feat/p2-s5-dl-alphas` + `feat/p2-s6-data-layer` into the worktree base and record the resulting SHA, or (b) branch off an existing integration commit that already contains them. **Record which, with SHAs, in the ledger header.** If S6's `data/` layer is absent from the base, **stop and surface to the user** — every other unit depends on it.
3. Verify the §0 signatures against the actual code; correct drift in a ledger "as-built recon" subsection.
4. Open `sprint-1-progress.md` (skeleton from `sprint.md` §"Progress ledger"). Point the ROADMAP S1 row at it; bump `Last reviewed`.

**Commit:** `docs(s1-0): open phase-1 real-data-hardening ledger + as-built recon + S5/S6 merge-base record`
**Acceptance:** ledger exists; base SHA + S5/S6 merge reality recorded; §0 drift reconciled. No code yet.

---

### S1-1 — Documentation (independent; dispatch in parallel with the chain)

**Scope (from ROADMAP 1.1):** Author the two canonical reference docs — the user's "document the alpha pipeline + data ingestion better, to start" ask.

**Files (create):**
- `atx-engine/plans/p3-impl/alpha-pipeline-reference.md`
- `atx-engine/plans/p3-impl/data-ingestion-reference.md`

**`alpha-pipeline-reference.md` — required content:** the end-to-end map *as a reference, not a tutorial*. Sections: (1) Panel representation + the DSL/expression substrate (parse→analyze→Genome); (2) backtest/sim kernel (signal→weights→fills→PnL, the cost model); (3) fitness + the objective vector slots (wq/diversify/robust/behavioral-novelty/−cost); (4) search/mining (`SearchDriver` loop, NSGA-II, behavioral archive, dedup, `SearchResult.digest`); (5) robustness battery (CPCV, regime/walk-forward `RobustnessVerdict`, lockbox); (6) deflation/overfitting guards (deflated Sharpe at running trial count, PBO); (7) determinism (`signal_set_digest`, `seed_for`, golden pins); (8) **the public entry points** a user calls (`Factory::mine`, `ResearchDriver::run`, `RobustResearchDriver::run`) with the minimal call sequence. **Every claim carries a `file.hpp:line` ref.** This is a *map*, not new prose theory — it distills what already exists. Cross-link the research deep-dives rather than restating them.

**`data-ingestion-reference.md` — required content:** (1) the on-disk datasets (§0.5 schemas + paths); (2) the ingestion stack layers (parquet→segment→`SegmentReader`→Panel, and the S6 `Dataset`/`Catalog`/`adapt_*` layer) with the layer table; (3) **corporate-action semantics** — the adjustment-factor + dividend conventions (§0.5), the total-return model (§0.7 #1–2), and the **return-invariance ⇒ non-leak** argument verbatim; (4) **universe construction** — market cap, ADV, sector, PIT membership, and the **survivorship caveat**; (5) the **end-to-end real-data path** S1-5 builds, as the canonical "how to get a real-data Panel" recipe; (6) the **coverage / rebuild** instructions (point at `build_us_split_adjustments.py`, the polite-crawl flags, `--mode split` vs `--mode adjclose`). Forward-reference S1-2..S1-6 file names (they may land after this doc; mark them "lands in S1-N").

**Test plan:** docs unit — no test binary. The gate is **accuracy**: every signature/line-ref must match the code at the worktree base (the author opens the referenced files to confirm). A wrong line-ref is a defect.

**Commit:** `docs(s1-1): alpha-pipeline + data-ingestion reference docs`
**Acceptance:** both docs exist under `p3-impl/`; signature/line-refs verified against the base; the return-invariance non-leak argument + survivorship caveat present. Listed in ROADMAP companion index (move from Pending).
**Depends:** S1-0 (recon line-refs). Independent of S1-2..S1-6 code.

---

### S1-2 — Security-master ingestion → typed PIT `Dataset`

**Scope (from ROADMAP 1.2):** Load `security_master.parquet` + the by-symbol partitions into a typed PIT `data::Dataset` (Reference / corporate-action role), with as-of + lineage and a knowledge-date leak guard.

**Files (create):**
- `atx-engine/include/atx/engine/data/corporate_actions.hpp`
- `atx-engine/src/data/corporate_actions.cpp` (register in the atx-engine lib per §0.6)
- `atx-engine/tests/data_corporate_actions_test.cpp` (auto-globbed)

**API (design to the as-built `data::Dataset`/`ParquetTable`; adjust names to match S6):**
```cpp
namespace atx::engine::data {

// One symbol's corporate-action / fundamental history, knowledge-date stamped.
struct CorpActionColumns {
  std::vector<i64>    dates;                 // trading-date axis (ns or date32 — match price axis units)
  std::vector<f64>    cum_adj_factor;        // cumulative_adjustment_factor (split-only)
  std::vector<f64>    cash_dividend;         // per-share, ex-date (0.0 when none)
  std::vector<f64>    shares_outstanding;    // SEC fact, fwd-filled
  std::vector<i64>    shares_filed_date;     // knowledge-date for the PIT leak guard
  std::vector<i32>    gics_sector_code;      // sparse; sentinel when absent
  std::vector<i32>    sic_code;              // open fallback
};

// Load the security master into a Reference-role Dataset keyed (symbol,date).
// PIT: a (symbol,date) fundamental is visible only when knowledge_date <= date.
Result<Dataset> load_security_master(std::string_view master_parquet_path,
                                     const DatasetSchema& schema);

// Optional: load the canonical by-symbol partitions (factors/dividends/shares/sectors)
// when the single master file is unavailable; same Dataset shape.
Result<Dataset> load_security_master_partitioned(std::string_view root_dir,
                                                 std::span<const std::string> symbols,
                                                 const DatasetSchema& schema);
} // namespace atx::engine::data
```

**Semantics + edge cases:**
- **PIT leak guard:** when materializing a fundamental at date `d`, use only rows with `shares_filed_date <= d` (knowledge-date), forward-filled. Corporate-action `cum_adj_factor`/`cash_dividend` are mechanical/ex-date facts (not forecasts) and are joined on their event date.
- **Missing data:** absent dividend ⇒ `0.0`; absent sector ⇒ sentinel (`-1`), not silently `0`. NaN-propagate per `datafields` discipline.
- **Currency:** assert `dividend_currency == "USD"` for the US-equity scope; non-USD ⇒ `Result::Err` (out of scope).
- **Determinism:** symbol order = first-seen interning matching the price segment (so the corp-action Dataset aligns onto the same instrument ids in S1-5).

**Test plan (`data_corporate_actions_test.cpp`, `ATX_TEST` framework):**
1. `LoadsSmokeMasterRowShapeMatchesManifest` — load the on-disk smoke `security_master.parquet`; assert symbol count (3), row count, column presence vs the manifest schema.
2. `DividendZeroFilledOffExDates` — non-ex-dates read `cash_dividend == 0.0`, ex-dates read the known AAPL dividend.
3. `SharesOutstandingPitForwardFill` — a fundamental is invisible before `shares_filed_date` and forward-filled after; assert no row leaks before its filing knowledge-date.
4. `NonUsdCurrencyRejected` — a synthetic non-USD row ⇒ `Err`.
5. `SymbolInterningDeterministic` — two loads produce identical symbol→id order.

**Commit:** `feat(s1-2): security-master ingestion into typed PIT corporate-action Dataset`
**Acceptance:** all 5 tests pass; full suite green; `/W4 /WX` clean; lib registration correct. Ledger row written (test counts).
**Depends:** S1-0.

---

### S1-3 — Total-return adjustment *(the load-bearing correctness unit — do not rush)*

**Scope (from ROADMAP 1.3):** Fold split (`cum_adj_factor`) + reinvested `cash_dividend` into a **total-return index** field; PIT/return-invariant; validated against known AAPL split/dividend events and cross-checked against the `--mode adjclose` builder output.

**Files (create):**
- `atx-engine/include/atx/engine/data/adjust.hpp`
- `atx-engine/src/data/adjust.cpp`
- `atx-engine/tests/data_adjust_test.cpp`

**API:**
```cpp
namespace atx::engine::data {

struct AdjustedSeries {
  std::vector<f64> split_adj_close;   // raw_close * cum_adj_factor  (split-only)
  std::vector<f64> total_return_index;// split + reinvested-dividend; level basis = first valid close
  std::vector<f64> total_return;      // daily total return r_t (for backtest/fitness)
};

// Per symbol, ascending dates. raw_close UNADJUSTED; cum_adj_factor split-only;
// cash_dividend per-share on ex-date (0.0 otherwise).
AdjustedSeries adjust_total_return(std::span<const f64> raw_close,
                                   std::span<const f64> cum_adj_factor,
                                   std::span<const f64> cash_dividend);
} // namespace atx::engine::data
```

**Math (specify exactly; the sub-agent implements + validates):**
- **Split-adjusted close:** `S_t = raw_close_t * cum_adj_factor_t`. Invariant: no discontinuity across split dates (AAPL 4:1 2020-08-31, 7:1 2014-06-09).
- **Daily total return** on the split-adjusted series with dividends reinvested on the ex-date:
  `r_t = (S_t + D_t_adj) / S_{t-1} - 1`, where `D_t_adj` is the cash dividend expressed on the **same split basis** as `S_t` (i.e. `D_t * cum_adj_factor_t`). `r_0 = 0`.
- **Total-return index:** `TRI_0 = S_0` (first valid close); `TRI_t = TRI_{t-1} * (1 + r_t)`. Robust to large dividends (no `1 - D/C` negative-factor pathology).
- **NaN handling:** a NaN raw_close yields NaN `r_t` and carries `TRI` forward as NaN for that cell; the series resumes at the next valid close (no silent zero-fill).

**Validation (the gate):**
- **Event validation:** on the smoke AAPL series, the split-adjusted close across each known split date has `|S_after/S_before − raw_ratio_corrected| ≤ ε` (no fabricated jump); the **total-return index daily returns** match Yahoo total-return within a documented tolerance on dividend ex-dates.
- **Oracle cross-check:** rebuild the smoke factors with `--mode adjclose` (yields the dividend-inclusive factor) and assert the engine's `total_return_index` matches `raw_close * adjclose_factor` (up to level normalization) within tolerance `ε_oracle` (record the chosen ε in the ledger). *If the adjclose rebuild is impractical in-sprint, fall back to a hand-checked AAPL fixture of 3–5 known ex-dates with published adjusted closes, and record that substitution in the ledger.*
- **Return invariance:** assert that scaling the whole `raw_close` history by an arbitrary constant leaves `total_return` byte-identical (the non-leak property, §0.7 #2).

**Test plan (`data_adjust_test.cpp`):**
1. `SplitAdjustedNoDiscontinuityAtKnownAaplSplits`
2. `TotalReturnReinvestsDividendOnExDate` (hand fixture: known div, known prior close)
3. `TotalReturnIndexMatchesAdjcloseOracleWithinTolerance` (or the documented hand-fixture fallback)
4. `ReturnInvariantUnderConstantPriceRescale` (non-leak)
5. `NanRawCloseDoesNotZeroFill`
6. `ZeroDividendSeriesEqualsSplitAdjustedReturns` (degenerate: no dividends ⇒ TRI returns == split-adj returns)

**Commit:** `feat(s1-3): split+dividend total-return adjustment (validated vs known AAPL events)`
**Acceptance:** all 6 tests pass; tolerances recorded in the ledger; full suite green; `/W4 /WX` clean.
**Depends:** S1-2 (consumes the corp-action columns).

---

### S1-4 — Universe / market-cap / liquidity / sector

**Scope (from ROADMAP 1.4):** `shares_outstanding`→market cap; trailing-ADV liquidity screen; GICS sector field; PIT membership mask; the listed-only survivorship caveat documented.

**Files (create):**
- `atx-engine/include/atx/engine/data/universe.hpp`
- `atx-engine/src/data/universe.cpp`
- `atx-engine/tests/data_universe_test.cpp`

**API:**
```cpp
namespace atx::engine::data {

struct UniverseConfig {
  usize adv_window      = 21;       // trailing days for ADV (causal)
  f64   min_adv_usd     = 1.0e6;    // liquidity floor (dollars/day)
  f64   min_mktcap_usd  = 0.0;      // 0 = no cap floor
  usize top_n_by_adv    = 0;        // 0 = no count cap; else keep top-N each date
};

struct UniverseFields {
  std::vector<f64> market_cap;      // shares_outstanding * raw_close (split-invariant)
  std::vector<f64> adv_usd;         // trailing causal mean of dollar_volume
  std::vector<i32> sector_code;     // GICS if present else SIC fallback; sentinel -1
  std::vector<u8>  in_universe;     // dates*instruments PIT membership mask
};

// All inputs date-major, aligned to the price axis (price defines the axis).
UniverseFields build_universe(const Panel& price_panel,           // raw close + volume + dollar_volume
                              const Dataset& corp_actions,        // shares, sector (PIT as-of)
                              const UniverseConfig& cfg);
} // namespace atx::engine::data
```

**Semantics + edge cases:**
- **Market cap** = `shares_outstanding (as-of, PIT) * raw_close`. Split-invariant; use raw close. NaN if either input NaN.
- **ADV** reuse `datafields` `adv{d}` / `dollar_volume` (causal, trailing only — **no look-ahead**). If `adv` already exists on the augmented panel, consume it; do not recompute differently.
- **Membership** `in_universe(t,i)` = `present(t,i)` ∧ `mktcap ≥ min_mktcap` ∧ `adv ≥ min_adv` ∧ (top-N rank by ADV if `top_n_by_adv>0`). Top-N tie-break is **canonical-id**, deterministic.
- **Sector** GICS code if present else SIC fallback; sentinel `-1` if neither. Never coerce missing→0.
- **Survivorship:** the screen operates only on symbols that exist in the listed-only master ⇒ a delisted name simply never appears. **The unit does not fix this; it documents it** — the test asserts the caveat is referenced in `data-ingestion-reference.md`, and the ledger row states the bias.

**Test plan (`data_universe_test.cpp`):**
1. `MarketCapIsSplitInvariant` (apply a split, cap unchanged within ε)
2. `AdvIsCausalNoLookAhead` (truncating future dates does not change a past date's ADV)
3. `LiquidityFloorDropsBelowThreshold`
4. `TopNByAdvDeterministicTieBreak`
5. `SectorGicsThenSicFallbackThenSentinel`
6. `MembershipExcludesNanPriceCells`

**Commit:** `feat(s1-4): universe — market-cap/ADV/sector PIT membership (survivorship-caveated)`
**Acceptance:** 6 tests pass; suite green; `/W4 /WX` clean; survivorship caveat present in the data doc + ledger.
**Depends:** S1-2. (Independent of S1-3; may run after S1-3 in the sequential chain.)

---

### S1-5 — Real-data E2E path *(the integration unit)*

**Scope (from ROADMAP 1.5):** databento segment ⋈ corp-actions ⋈ universe → `DatasetCatalog` → `adapt_panel` → `Panel`; end-to-end test over the **actual on-disk smoke data**; **golden digest pin** on the assembled Panel.

**Files (create):**
- `atx-engine/include/atx/engine/data/real_panel.hpp`
- `atx-engine/src/data/real_panel.cpp`
- `atx-engine/tests/data_real_panel_e2e_test.cpp`

**API:**
```cpp
namespace atx::engine::data {

struct RealDataConfig {
  std::string databento_hive_root;    // data/databento/equs_ohlcv_1d_by_date
  std::string security_master_path;   // data/.../security_master/security_master.parquet
  std::string seg_cache_dir;          // where build_dated_segments writes .seg
  TimeWindow  window;                 // [start,end) trading dates
  UniverseConfig universe;
};

struct RealPanel {
  Panel  panel;                       // fields: close(=total_return_index), raw_close, volume,
                                      //         dollar_volume, adv, market_cap, sector; universe mask applied
  u64    digest;                      // signal_set_digest over fields in canonical order — the pin
  // lineage: which datasets/versions composed this panel (from the Catalog)
};

// One documented call: assemble the real-data Panel deterministically.
Result<RealPanel> build_real_panel(const RealDataConfig& cfg);
} // namespace atx::engine::data
```

**Assembly steps (deterministic, in order):**
1. `build_dated_segments(databento_hive_root, seg_cache_dir)` → per-date `.seg` (or reuse if present + content-hash matches).
2. `attach_segment_panel` over the window → raw price `Panel`; `build_augmented_panel` → `dollar_volume`, `vwap`, `adv`.
3. `load_security_master` (S1-2) → corp-action `Dataset`; `data::align` onto the price axis (price defines axis; absent (symbol,date) ⇒ absent).
4. Per symbol: `adjust_total_return` (S1-3) → `total_return_index` (becomes the canonical `close` field) + `raw_close` retained.
5. `build_universe` (S1-4) → `market_cap`, `adv`, `sector`, `in_universe`; apply the mask to the Panel.
6. Register `price` / `corp_actions` / `universe` in a `DatasetCatalog`; `adapt_panel` + `adapt_feature` to land all fields; compute `signal_set_digest` → `digest`.

**The pin:** the assembled smoke Panel has a **golden digest** asserted in the E2E test (record the literal in the test + the ledger), exactly as the synthetic fixtures are pinned. Two builds ⇒ identical digest. A change to any adjustment/universe step that moves the digest is a deliberate, reviewed change (update the golden + note why).

**Test plan (`data_real_panel_e2e_test.cpp` — reads the ACTUAL on-disk smoke data):**
1. `BuildsSmokeRealPanelDeterministicDigest` — `build_real_panel` over the 3-symbol smoke; assert `digest == 0x<golden>` (pinned); assert again on a second build (stability).
2. `AaplAdjustedCloseSpotCheck` — known AAPL date(s): `close` (total-return index) and `raw_close` match expected within tolerance (ties S1-3 into the real path).
3. `UniverseMaskNonEmptyAndCausal` — the mask has ≥1 in-universe cell; truncating the window does not change a retained past date's values (no look-ahead).
4. `MissingSymbolDateAbsentFromPanel` — a (symbol,date) in price but absent in corp-actions falls back per policy (raw close, sector sentinel) — assert no fabricated dividend/sector.
5. `LineageRecordsComposingDatasets` — the catalog lineage names price + corp_actions + universe versions.

**Commit:** `feat(s1-5): end-to-end real-data Panel (databento ⋈ corp-actions ⋈ universe), digest-pinned`
**Acceptance:** 5 tests pass against the real on-disk smoke data; golden digest recorded; full suite green; `/W4 /WX` clean. **This unit's green E2E test is the S1 exit-criteria evidence.**
**Depends:** S1-2, S1-3, S1-4.

---

### S1-6 — Coverage expansion (data-build; independent)

**Scope (from ROADMAP 1.6):** Re-run `build_us_split_adjustments.py` over the databento universe → expanded security master for a **liquid subset**; documented data-build. *(Per the ROADMAP §"Strategic decisions" #2 "expand coverage first" call.)*

**Do:**
1. Derive the target symbol set: the databento `equs_ohlcv_1d` universe, filtered to a **liquid subset** (e.g. top-N by trailing dollar-volume over the available window). Record the selection rule + N in the ledger. (Keep N tractable for a polite crawl — hundreds, not thousands.)
2. Run the builder politely:
   ```powershell
   python python/scripts/build_us_split_adjustments.py \
     --symbols <liquid-subset> \
     --datasets yahoo sec security-master \
     --incremental \
     --sec-user-agent "atx-research <nathan.tormaschy@gmail.com>"
   ```
   Use the `--yahoo-min-interval` / `--sec-min-interval` / `--request-delay` / `--backoff-max-seconds` polite-crawl flags; stay below the SEC EDGAR automated-access ceiling. This is an **offline, rate-limited crawl** — it runs and is *recorded*; it does **not** block engine compilation.
3. Verify the expanded master: row/symbol counts, date coverage, schema parity with the smoke master, manifest written. Spot-check 2–3 expanded symbols against Yahoo adjusted closes.
4. Update `data-ingestion-reference.md` §coverage with the produced universe (symbol count, date range, selection rule, rebuild command) and the known gaps (GICS sparsity, listed-only survivorship).

**Test plan:** no C++ test binary. The gate is the **manifest + a recorded verification** (counts + spot-check) in the ledger, and the doc update. If the crawl cannot complete in-sprint (rate limits / source outage), land what completed, record the partial coverage + the resume command in the ledger, and mark the unit `⚠️ partial` — **do not block S1 close on a flaky external crawl**; S2 can run on whatever liquid subset S1-6 produced (down to the smoke 3 if the crawl wholly fails, with a recorded caveat).

**Commit:** `chore(s1-6): expand security-master coverage over databento liquid subset + doc`
**Acceptance:** expanded master on disk with a recorded manifest + spot-check, or a recorded partial with a resume command; data doc §coverage updated.
**Depends:** S1-0. Independent of the engine chain (may run in parallel / last).

---

### S1-close — Sprint close ceremony

Run in one close commit (or tight series), per `sprint.md` §"Sprint close ceremony":
1. Lift residuals → ROADMAP "Phase 1d / future-work backlog" (already seeded; add any new ones).
2. Update the ROADMAP S1 status table `⏳ → ✅ <sha>` / `⚠️`; record commit SHAs; bump `Last reviewed`.
3. Write `sprint1.md` user reference — the "how to build a real-data Panel" recipe + the public surface (`build_real_panel`) + known limitations (links into the ledger numbers, no duplication).
4. Write the ledger "What S1 proves / Next sprint priorities" baton — hand S2 the real-data Panel + the liquid universe.
5. Resolve any ROADMAP §"Strategic decisions" forks that S1 settled (e.g. final ADV threshold).
6. **`--no-ff` merge** into the base branch with `merge: phase-1 — real-data hardening`. **No push unless the user asks.**

**Commit:** `docs(s1-close): close phase-1 real-data hardening — N units, M tests` then the merge.

---

## §2 — Dependency graph + dispatch order

```
S1-0 (marker, orchestrator)
  ├── S1-1 docs ............................. parallel (independent)
  ├── S1-6 coverage crawl .................. parallel (independent, long-running)
  └── S1-2 sec-master ingest
        ├── S1-3 total-return adjust  ← load-bearing
        └── S1-4 universe/mktcap/liq
              └── S1-5 E2E real-data path (needs S1-2,3,4)  ← integration + pin
S1-close (after S1-2..S1-6 land)
```

**Dispatch:** sequential for the chain **S1-2 → S1-3 → S1-4 → S1-5** (hard data dependency; a sub-agent can't see an uncommitted predecessor). **S1-1** and **S1-6** are dispatched in parallel with the chain (disjoint files: docs + python/data vs `data/*.{hpp,cpp}`), per `sprint.md` §"Parallel dispatch" — send both in a single message at kickoff. Verify each unit's commit + green suite before dispatching its dependent.

---

## §3 — Sub-agent brief template (every code unit)

Each brief MUST include (per `sprint.md` §"What each sub-agent brief must include"):

1. **Worktree path + branch** — `cd` target.
2. **Scope** — quoted verbatim from this plan's unit section (not paraphrased).
3. **Acceptance criteria** — the named tests that must exist + pass, full suite stays green, `/W4 /permissive- /WX` + `/fp:precise` clean.
4. **Predecessor SHAs** — what they build on (e.g. S1-3 gets S1-2's commit SHA).
5. **Expected commit message** — the `feat(s1-N): …` / `chore(s1-N): …` from the unit.
6. **Ledger row to write** — the `sprint.md` row shape (status, SHA, one-paragraph notes **with test counts**), committed as part of the unit.
7. **Build instructions** — PowerShell tool + env wrapper, `dev` preset; **`atx-shm-worker` must be built** if any Parallel test is touched; **do NOT run clang-tidy**; **never hand-edit the test CMake glob**; register new `src/data/*.cpp` per the S6 precedent (§0.6); **never reformat third-party/submodule code**.
8. **Implementation-quality standard** — paste the handoff block from [`../docs/implementation-quality.md`](../docs/implementation-quality.md): module-level intent comment, grouped APIs, explicit ownership/lifecycle, smart comments (invariants not noise), **no partial stubs** (every unit ships complete + tested), functions ≤ ~60 lines, exhaustive enum switches with **no `default`**.
9. **The verbatim marker-commit reminder:**

   > **Marker-commit pattern is mandatory: commit before stopping or the work is lost.** Your unit is not done until its code + tests + ledger row are committed and the full test suite is green. Do not return uncommitted.

A brief that omits any of these produces work that doesn't fit the seam — reject and re-open.

---

## §4 — Risks + how this plan de-risks them

| Risk | Mitigation in-plan |
|---|---|
| **S5/S6 not actually present at base** | S1-0 makes the merge-base explicit + stops if `data/` is missing. The whole module's #1 precondition. |
| **Wrong total-return adjustment silently fabricates returns** | S1-3 is isolated, validated against known AAPL events + an adjclose oracle (or hand fixture), with the return-invariance non-leak test. The most-scrutinized unit. |
| **Look-ahead via fundamentals** | PIT leak guard on `shares_filed_date` (knowledge-date), causal ADV, truncation-invariance tests in S1-2/S1-4/S1-5. |
| **Non-deterministic real-data Panel** | S1-5 digest pin + fixed symbol interning / date axis / NaN policy. |
| **External crawl flakiness (S1-6)** | S1-6 is independent, non-blocking, `⚠️ partial`-tolerant; S2 runs on whatever subset lands (down to smoke-3 with a caveat). |
| **Survivorship bias mistaken for alpha** | S1-4 documents listed-only bias; S2 scorecard reports it as a first-class caveat (ROADMAP §positioning). |
| **Breaking the green p2 pipeline** | S1 adds no `factory::`/`eval::` code; full suite green is a per-unit gate. |

---

*This plan is frozen. Scope changes during execution go into `sprint-1-progress.md` "Plan adjustments", not here.*
