# Regime / Macro Data Module — Design Spec

**Date:** 2026-06-18
**Status:** Approved design, pre-implementation-plan
**Owner:** atx-engine
**New submodule:** `atx::engine::regime`

---

## 1. Goal

Add a new submodule of `atx-engine` named **`regime`** that:

1. **Collects, stores, and manages** market-wide regime / macro time series (VIX,
   VVIX, MOVE, 2s10s spread, credit spreads, etc.) from publicly available
   sources.
2. **Surfaces** that data into the core simulation layer so alpha signals can
   *interact* with it — e.g. an alpha that runs only when `VIX < 20`.

The driving example: *"only run mean-reversion alpha #36 when VIX < 20."* In this
design that becomes a **regime-masked alpha expression**, not an external per-alpha
on/off switch (see §7).

---

## 2. Context — what already exists (and what does not)

Findings from codebase exploration that constrain the design:

- **No network code in the repo.** All data ingestion reads local files (the ORATS
  loader reads a local zip). `cpp-httplib` + `nlohmann/json` exist in the tree but
  only inside the Databento client. The codebase has a strong byte-reproducibility
  culture (CRC32 segment digests, sorted/deduped inputs, fixed timestamp encoding).
- **The sim panel is instrument-indexed and 2-D** (`date × instrument`, OHLCV).
  There is **no scalar / market-wide time-series concept** today.
  `ISignalSource::evaluate(PanelView) → SignalView`
  ([loop/signal_source.hpp:87](../../../atx-engine/include/atx/engine/loop/signal_source.hpp))
  is the only runtime alpha seam.
- **Gating is admission-time only** (`AlphaGate`: Sharpe / turnover / correlation
  floors, [combine/gate.hpp:94](../../../atx-engine/include/atx/engine/combine/gate.hpp)).
  There is **no runtime predicate gating** and **no numbered-alpha registry** —
  alphas are keyed by `AlphaId` (insertion order) + `canon_hash`. "Alpha #36" is a
  conceptual label, not a stable handle.
- **The DSL already supports everything needed for regime interaction** — no new
  VM opcodes required:
  - Comparison ops `CmpLt / CmpGt / CmpLe / CmpGe / CmpEq / CmpNe` → produce `Mask`
  - Logical ops `And / Or / Not`
  - `Select` (ternary / `where`)
  - all registered in
    [alpha/registry.hpp:74](../../../atx-engine/include/atx/engine/alpha/registry.hpp).
- **Derived panel columns are an established pattern.**
  `datafields::with_datafields`
  ([alpha/datafields.hpp:153](../../../atx-engine/include/atx/engine/alpha/datafields.hpp))
  appends derived columns (`dollar_volume`, `vwap`, `adv{d}`) to the field
  dictionary at panel-build time; an alpha loads them through the same `LoadField`
  path as `close`. **This is the template for surfacing regime data.**
- **The panel-build seam** is `price_to_panel` → `with_datafields`
  ([data/adapt_panel.cpp:47](../../../atx-engine/src/data/adapt_panel.cpp)), called
  inside the `build_real_panel` orchestrator (8-step assembly with a digest pin).

---

## 3. Design decisions (resolved with user)

| # | Decision | Choice |
|---|----------|--------|
| D1 | Data sourcing | **Offline-staged CSV.** A separate fetch script downloads public CSVs into a staging dir; the C++ loader only ingests local files. The staged snapshot is the pinned, reproducible input. No C++ network dependency. |
| D2 | Regime↔signal interaction | **Regime features in the panel.** Macro series are broadcast into the panel as named `regime_*` fields the alpha DSL/VM reads directly. |
| D3 | Storage + join mechanism | **Approach A — reuse the tsdb segment + a panel augmenter** (`with_regime_fields`). Zero VM changes; reuses the segment/digest machinery; mirrors `with_datafields`. |
| D4 | Launch series set | All four groups: **Vol** (vix, vvix, move), **Rates/curve** (dgs2, dgs10, t10y2y, t3m10y), **Credit/liquidity** (hy_oas, ig_oas, nfci), **Breadth/trend** (spx_dist_200dma). Series list is config-driven, not hardcoded. |
| D5 | Conditioning semantics | Regime conditioning **lives inside the alpha expression** (composable, discoverable by search), e.g. `select(regime_vix < 20, <expr>, 0)`. Not an external numbered-alpha switch. |

### Approaches considered for D3

- **A (chosen):** tsdb segment (fields = indicators, one synthetic instrument
  `MACRO`, T = trading dates) + `regime::with_regime_fields` broadcasts into the
  panel. Zero VM change; free CRC32 digest; reusable across universes.
- **B (rejected):** standalone regime store + a new `Shape::Scalar` plane in
  `PanelView` with a dedicated VM load path. More expressive (VM distinguishes
  scalar shape) but touches VM, panel, and typecheck — high cost for no near-term
  benefit, since broadcast columns already give the DSL full access.
- **C (rejected):** bake regime columns into each equity `.seg` at ingest. Simplest
  join but couples the regime digest to the equity digest, bloats segments, and must
  be re-baked per universe.

---

## 4. Module layout

```
atx-engine/
  include/atx/engine/regime/
    fwd.hpp                 # forward decls
    series.hpp             # SeriesId / canonical series-name table + regime_ prefix
    source_csv.hpp         # CSV source adapters (FRED / CBOE / Yahoo) -> (date, value)
    loader.hpp             # offline-CSV -> tsdb segment loader (config + stats)
    store.hpp              # read API over the sealed regime segment (as-of lookup)
    with_regime_fields.hpp # panel augmenter (mirrors datafields::with_datafields)
    README.md              # field dictionary + RUNBOOK pointer
  src/regime/
    source_csv.cpp
    loader.cpp
    store.cpp
    with_regime_fields.cpp   # (or header-only if it stays small like datafields)
  tests/regime/
    regime_loader_test.cpp
    regime_store_test.cpp
    regime_with_fields_test.cpp
    regime_e2e_mask_test.cpp
scripts/regime/
  fetch_regime.py          # offline staging (NOT part of the C++ build)
docs/atx-impl/ or module README
  RUNBOOK section for staging + loading regime data
```

- **Namespace:** `atx::engine::regime`.
- **CMake:** `atx-engine/CMakeLists.txt` uses an explicit source list (not glob).
  Add each `src/regime/*.cpp` line explicitly near the existing per-module blocks.
  Tests register under `atx-engine/tests/` following the existing per-module test
  pattern. No new link dependencies (reuses `atx::core` + `atx::tsdb`, already
  linked).

---

## 5. Phase 1 — Collect / Store / Manage

### 5.1 Offline staging (outside C++)

`scripts/regime/fetch_regime.py` downloads raw CSVs from public sources into a
staging directory. Examples of public, free sources:

- FRED (`https://fred.stlouisfed.org/...`): `VIXCLS`, `DGS2`, `DGS10`, `T10Y2Y`,
  `T10Y3M`, `BAMLH0A0HYM2` (HY OAS), `BAMLC0A0CM` (IG OAS), `NFCI`.
- CBOE / Yahoo: `VVIX`, `MOVE`, index levels for breadth.

The script is documented in the RUNBOOK and is **not part of the build**. The
staged snapshot (a directory of CSVs) is the pinned, reproducible input. Re-running
the loader on the same snapshot yields a byte-identical segment.

### 5.2 Source adapters — `source_csv.hpp`

Each public source has a different CSV schema. A small adapter normalizes each into
a stream of `(date, value)` for one named series:

- `FredCsv` — header `DATE,VALUE`; `.` / empty → NaN.
- `CboeCsv` — header `DATE,OPEN,HIGH,LOW,CLOSE`; read `CLOSE`.
- `YahooCsv` — header `Date,Open,High,Low,Close,Adj Close,Volume`; read `Adj Close`
  (configurable column).

```cpp
namespace atx::engine::regime {

enum class CsvFormat : atx::u8 { Fred, Cboe, Yahoo };

struct SeriesSpec {
  std::string name;        // canonical series name, e.g. "vix", "t10y2y"
  std::string file;        // staged CSV path (relative to staging dir)
  CsvFormat   format{CsvFormat::Fred};
  std::string value_column; // optional override (default per format)
};

// Parse one staged CSV into a sorted, deduped (date_nanos, value) series.
// NaN for missing / unparseable values. Err on schema mismatch (fail-closed,
// mirrors the ORATS header check).
[[nodiscard]] atx::core::Result<std::vector<std::pair<atx::i64, atx::f64>>>
parse_series_csv(const std::string &path, CsvFormat fmt, std::string_view value_column);

} // namespace atx::engine::regime
```

### 5.3 Derived series

Some indicators are computed from staged components rather than fetched directly.
Defined in config as `name = lhs OP rhs` over already-loaded series:

- `t10y2y = dgs10 - dgs2` (if not staged directly from FRED's `T10Y2Y`)
- `t3m10y = dgs10 - dgs3m`

Derived series are computed **after** alignment (§5.4) so both operands share the
date axis. NaN propagates (any operand NaN → NaN). The derived-series mini-language
is intentionally tiny: `series OP series` with `OP ∈ {+, -, *, /}`. Anything richer
is out of scope for v1.

### 5.4 Loader — `loader.{hpp,cpp}`

Mirrors the ORATS loader structure (read → normalize → seal), single-threaded
(the data is small — one value per series per day):

```cpp
struct RegimeLoadConfig {
  std::string        staging_dir;     // dir holding the staged CSVs
  std::string        out_path;        // output .seg path
  std::vector<SeriesSpec> series;     // series to load (raw)
  std::vector<std::string> derived;   // derived defs, e.g. "t10y2y = dgs10 - dgs2"
  atx::i64           min_date_nanos{0};  // inclusive floor
  atx::i64           created_at_nanos{0}; // provenance stamp into segment header
  bool               forward_fill{true};  // carry last obs forward (see §5.5)
};

struct RegimeLoadStats {
  atx::i64 series_count{};
  atx::i64 dates_written{};   // master trading-date axis length
  atx::i64 rows_read{};
  atx::i64 rows_filled{};     // cells produced by forward-fill
  atx::i64 first_date_nanos{};
  atx::i64 last_date_nanos{};
};

[[nodiscard]] atx::core::Result<RegimeLoadStats>
load_regime_history(const RegimeLoadConfig &cfg);
```

Loader flow:

1. Parse each `SeriesSpec` CSV via the matching adapter → sorted `(date, value)`.
2. Build a **master trading-date axis** = union of all series dates intersected
   with the NYSE `Calendar` (`atx-core` datetime), `>= min_date_nanos`, sorted &
   deduped.
3. Reindex each series onto the master axis with **forward-fill** (§5.5).
4. Compute **derived series** (§5.3) over the aligned grid.
5. Pivot into `LongColumns` (fields = series names, one synthetic instrument
   `"MACRO"`, times = master axis) and call
   `atx::tsdb::build_from_long(cols, out_path, created_at_nanos)`
   ([tsdb load_parquet.hpp:31](../../../atx-tsdb/include/atx/tsdb/load_parquet.hpp)).
6. Write a JSON manifest alongside (`<out>.manifest.json`): series list, master
   axis length, date range, per-series source file + format, segment digest.

**Storage shape:** one tsdb segment, `fields = [vix, vvix, move, dgs2, dgs10,
t10y2y, t3m10y, hy_oas, ig_oas, nfci, spx_dist_200dma, ...]`, `instruments =
["MACRO"]`, `times = master trading-date axis`. Read back through the standard
`segment.hpp` accessors.

### 5.5 Forward-fill semantics

Macro prints have gaps (holidays, weekends, late releases). The loader
forward-fills each series on the master axis: cell at date `t` = the most recent
observation at a date `<= t`; NaN before the first observation. This is **PIT-safe**
(only past data) and deterministic. Forward-fill happens once at ingest so the
segment is dense on its own axis; the panel join (§6) does a second as-of step for
panel dates the regime axis still lacks.

### 5.6 Read API — `store.hpp`

```cpp
class RegimeStore {
public:
  [[nodiscard]] static atx::core::Result<RegimeStore> open(const std::string &seg_path);

  [[nodiscard]] std::span<const std::string> series_names() const noexcept;
  [[nodiscard]] std::span<const atx::i64>    date_axis()    const noexcept;

  // As-of (<=) lookup: value of `series` at the latest regime date <= `t`.
  // NaN if `series` unknown or `t` precedes the first observation.
  [[nodiscard]] atx::f64 value(std::string_view series, atx::i64 t_nanos) const noexcept;

private:
  // memory-mapped sealed segment (atx::tsdb)
};
```

### 5.7 Determinism (Phase 1)

- Sorted, deduped master date axis; sorted series order (canonical name order).
- Forward-fill is a pure function of (series, axis).
- Fixed precision via `from_chars` parse; NaN sentinel for missing.
- Output digest = the `SegmentBuilder` CRC32 (free, already byte-reproducible).
- Re-running the loader on the same staged snapshot ⇒ byte-identical `.seg`
  (regression test, §8).

---

## 6. Phase 2 — Surface into the simulation

### 6.1 Panel augmenter — `with_regime_fields.hpp`

Mirrors `datafields::with_datafields`. Given the panel's date axis + instrument
count + the opened `RegimeStore`, it appends one broadcast column per requested
series:

```cpp
namespace atx::engine::regime {

inline constexpr std::string_view kRegimePrefix = "regime_";

// Append broadcast regime columns to a panel's field set.
// For each requested series s and each cell (date d, instrument i):
//   value = store.value(s, panel_dates[d])   if instrument i is in-universe at d
//         = NaN                               otherwise
// Field name = "regime_" + s (e.g. "regime_vix").
[[nodiscard]] atx::core::Result<alpha::Panel>
with_regime_fields(atx::usize dates, atx::usize instruments,
                   std::span<const atx::i64> panel_dates,
                   std::vector<std::string> field_names,
                   std::vector<std::vector<atx::f64>> field_data,
                   std::vector<std::uint8_t> universe,
                   const RegimeStore &store,
                   std::span<const std::string> requested_series);

} // namespace atx::engine::regime
```

- **Broadcast:** the same regime value fills every in-universe instrument cell on a
  given date (macro is market-wide). Out-of-universe cells → NaN, matching the
  `with_datafields` universe policy so a regime column behaves like any other field.
- **As-of join:** `store.value(s, panel_dates[d])` uses `<=` lookup, so panel dates
  missing from the regime axis resolve to the last known regime value (causal, no
  lookahead).
- **Naming:** `regime_<series>` reserves the `regime_` namespace in the field
  dictionary. Collision with an existing field is an error (fail-closed).

### 6.2 Pipeline wiring

Add a regime step to `build_real_panel`, immediately after `price_to_panel`
(`with_datafields`) and before the digest pin, gated by config:

- `--regime-segs <path>` — path to the regime `.seg`. Absent ⇒ step skipped ⇒
  panel digest **unchanged** (no regression to existing runs).
- `--regime-fields vix,t10y2y,hy_oas,...` — which series to broadcast (subset of
  the segment's series).

Config additions follow the existing `RunConfig` + `apply_flag_value` pattern
([atx-impl/src/config.hpp](../../../atx-impl/src/config.hpp),
[config.cpp](../../../atx-impl/src/config.cpp)):

```cpp
// config.hpp (RunConfig)
std::string regime_segs;    // --regime-segs
std::string regime_fields;  // --regime-fields (comma-separated series names)

// config.cpp (apply_flag_value)
if (flag == "regime-segs")   { cfg.regime_segs = value;   return atx::core::Ok(); }
if (flag == "regime-fields") { cfg.regime_fields = value; return atx::core::Ok(); }
```

Also a new top-level subcommand `regime` (alongside `load|panel|discover|...`) that
runs `load_regime_history` from config — the Phase-1 loader entry point.

### 6.3 DSL usage — no new ops

With `regime_vix` in the panel, the driving example is a plain expression:

```
select(regime_vix < 20, ts_zscore(-ts_delta(close, 5), 20), 0)
```

"Run the mean-reversion signal only when VIX < 20, else flat." Because `regime_*`
are ordinary loadable fields, discovery can also **evolve** regime-conditional
alphas natively (e.g. an evolved expression that scales exposure by `regime_move`).
A handful of seed expressions demonstrating regime masking are added to the seed
list used by the discovery stage.

### 6.4 Determinism (Phase 2)

- Regime fields are causal (as-of `<= date`), forward-filled at ingest, NaN before
  first history.
- The panel digest shifts **deterministically** only when `--regime-segs` /
  `--regime-fields` are supplied; the default (no regime) path is byte-identical to
  today.

---

## 7. On "alpha #36 only if VIX < 20"

Alphas have no numbered registry (keyed by `AlphaId` insertion order +
`canon_hash`). Two readings of the request, and how this design serves them:

- **Author a regime-conditional alpha:** wrap the chosen signal in
  `select(regime_vix < 20, <signal>, 0)`. The alpha is flat outside the regime.
  Fully supported, no new machinery.
- **Discover regime-conditional alphas:** because `regime_*` are loadable fields,
  the search can compose them into evolved expressions. Supported for free.

What this design intentionally does **not** add: an external "registry of numbered
alphas with per-alpha regime toggles." That would require a stable alpha-id surface
that does not exist today and is out of scope. If a post-hoc gate on an *already
admitted* signal stream is wanted later, a `RegimeGate` wrapper around
`ISignalSource` is a clean follow-on (it was the alternative "runtime gate" model),
but it is not part of v1.

---

## 8. Testing strategy

| Test | Asserts |
|------|---------|
| `regime_loader_test` | Byte-identical `.seg` across two loader runs on the same staged snapshot; correct master-axis construction; derived series (`t10y2y = dgs10 - dgs2`) correct; date-floor filter; NaN before first obs. |
| `regime_store_test` | `open` round-trips series names + date axis; as-of (`<=`) lookup correct, incl. dates between regime prints and before history (NaN). |
| `regime_with_fields_test` | Broadcast fills every in-universe instrument with the same per-date value; out-of-universe → NaN (parity with `with_datafields` universe policy); `regime_` naming; collision is an error; as-of join for panel dates absent from the regime axis. |
| `regime_e2e_mask_test` | End-to-end: an alpha `select(regime_vix < 20, <expr>, 0)` evaluated over a panel with a known VIX series emits the signal on low-VIX bars and exactly 0 on high-VIX bars. |
| Pipeline regression | With `--regime-segs` absent, `build_real_panel` digest is unchanged vs. the pre-regime baseline. |

Source adapters get focused unit tests (FRED/CBOE/Yahoo header parsing, missing
values → NaN, schema-mismatch → Err).

---

## 9. Phasing

One design doc, two phases (matches the existing sprint convention):

- **Phase 1 (data module):** §4 layout, §5 collect/store/manage + read API + the
  `regime` subcommand. Self-contained and testable without Phase 2.
- **Phase 2 (surface):** §6 `with_regime_fields` + `build_real_panel` wiring +
  config flags + seed exprs + e2e test.

Each phase is independently mergeable. Phase 2 depends on Phase 1's `RegimeStore`.

---

## 10. Out of scope (v1)

- In-process HTTP fetching (offline staging only — D1).
- A scalar-shape plane in the VM / `PanelView` (Approach B).
- A numbered-alpha registry or per-alpha external regime toggles (§7).
- Regime *detection* (HMM / clustering of states) — this module supplies raw +
  derived macro series; learned regime-state inference is a separate future module.
- Intraday regime data (daily series only for v1).
