#pragma once

// atx::engine::data — real_panel: the real-data end-to-end assembly (p3 S1-5).
//
// WHAT THIS OWNS (and what it does NOT)
//   real_panel OWNS the ORCHESTRATION of the real-data path: it composes the
//   already-shipped seams — the databento price parquet, the S1-2 corporate-action
//   Dataset, the S1-3 total-return adjustment, the S1-4 universe screen, and the
//   S6 Dataset/Catalog/adapt layer — into ONE deterministic alpha::Panel plus a
//   golden digest pin. It does NOT re-implement any of those seams: it borrows
//   them. It owns the assembled Panel's columns, the DatasetCatalog that records
//   lineage, and the seg cache it (optionally) writes; it borrows the on-disk
//   parquet only for the duration of each read (read → materialize → close).
//
// THE ASSEMBLY (deterministic, fixed order — §S1-5 / §0.7)
//   1. load_security_master(security_master_path)  -> corp-action Dataset. Its
//      instruments are interned FIRST-SEEN in master-file row order; that intern
//      assignment defines the canonical symbol -> InstKey map every other axis
//      reuses (so align_onto joins by matching InstKey, not by position).
//   2. Read the databento price parquet(s) over the window, RESTRICTED to the
//      corp-action symbol set, into a Role::Price Dataset on the SAME InstKeys.
//      Price defines the canonical (date x instrument) axis (§0.7 #3): a
//      (symbol,date) absent from price is absent from the Panel.
//   3. price_to_panel (adapt_panel) -> augmented Panel carrying dollar_volume,
//      vwap, and adv{adv_window} (the universe ADV window) derived from raw OHLCV.
//   4. align_onto(price, corp) -> the corp columns re-expressed on the price axis;
//      per symbol adjust_total_return(raw_close, cum_adj_factor, cash_dividend)
//      (S1-3) -> total_return_index (the canonical `close`) with raw_close kept.
//   5. build_universe (S1-4) over the Panel + the axis-matched corp Dataset ->
//      market_cap / adv / sector / in_universe; in_universe becomes the Panel mask.
//   6. Register price / corp_actions / universe in a DatasetCatalog, record the
//      derivation lineage, assemble the final Panel (close=TRI, raw_close, volume,
//      dollar_volume, adv, market_cap, sector; mask applied), and digest it.
//
// THE DIGEST PIN (§0.7 #5 — the whole point is determinism)
//   `digest` is signal_set_digest over the Panel's fields in CANONICAL field order
//   (the order build_real_panel lands them; see kFieldClose..kFieldSector). Two
//   builds of the same inputs produce a BYTE-IDENTICAL digest — symbol interning
//   order, the date axis, NaN placement, and field order are all fixed. A change
//   to any adjustment / universe step that moves the digest is a deliberate,
//   reviewed change (re-pin the golden + note why). The E2E test asserts the
//   literal and re-asserts on a second build.
//
// ERROR CONTRACT — build_real_panel returns Err when:
//   * a required input path is empty or the data cannot be read
//     (Err IoError / propagated from the parquet / segment loaders);
//   * the window selects NO trading dates over the price data (Err InvalidArgument
//     — an empty Panel is a caller mistake, not a silent success);
//   * the corp-action Dataset is malformed / non-USD (propagated from S1-2);
//   * a price↔corp axis or shape mismatch survives assembly (propagated from
//     align_onto / build_universe — Err InvalidArgument).
//   Every error fails CLOSED: no partially-built Panel escapes, owned scratch is
//   released by RAII.
//
// OWNERSHIP / LIFECYCLE
//   RealPanel owns its Panel (columns by value) and the lineage strings. The
//   DatasetCatalog used to record lineage is internal to build_real_panel and is
//   destroyed before return — only the extracted lineage names survive in
//   RealPanel. Cold path (once per backtest window); std::vector allocation is
//   intentional and explicit.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"          // alpha::Panel
#include "atx/engine/alpha/segment_panel.hpp"  // alpha::TimeWindow
#include "atx/engine/data/universe.hpp"        // UniverseConfig

namespace atx::engine::data {

// =========================================================================
//  Canonical field order of the assembled real-data Panel.
//
//  Load-bearing: the digest hashes the Panel's fields in THIS order. Changing the
//  order (or adding/removing a field) is a deliberate digest-moving change. The
//  derived datafields (dollar_volume / vwap / adv{w}) are appended by
//  price_to_panel after the base fields and BEFORE the merged feature fields, so
//  the final order is exactly the sequence build_real_panel materializes.
// =========================================================================
inline constexpr std::string_view kFieldClose = "close";       // == total_return_index (canonical)
inline constexpr std::string_view kFieldRawClose = "raw_close"; // unadjusted close (retained)
inline constexpr std::string_view kFieldVolume = "volume";
inline constexpr std::string_view kFieldHigh = "high";
inline constexpr std::string_view kFieldLow = "low";
inline constexpr std::string_view kFieldOpen = "open";
inline constexpr std::string_view kFieldMarketCap = "market_cap";
inline constexpr std::string_view kFieldSector = "sector";

// Catalog registration names (the lineage records these three composing datasets).
inline constexpr std::string_view kDatasetPrice = "price";
inline constexpr std::string_view kDatasetCorpActions = "corp_actions";
inline constexpr std::string_view kDatasetUniverse = "universe";

// =========================================================================
//  Configuration
// =========================================================================

// Inputs for one real-data Panel build. All paths are on-disk locations; the
// window is a half-open [start, end) in unix-nanos (alpha::TimeWindow), matched
// against each trading date's start-of-day timestamp. `universe` carries the
// S1-4 screen parameters (its adv_window also fixes which adv{w} column the
// Panel derives, so build_universe reads a matching causal ADV).
struct RealDataConfig {
  std::string databento_hive_root;  // data/databento/equs_ohlcv_1d_by_date
  std::string security_master_path; // .../security_master/security_master.parquet
  std::string seg_cache_dir;        // reserved for the .seg cache (see real_panel.cpp note)
  alpha::TimeWindow window{};       // [start,end) trading dates (unix-nanos)
  UniverseConfig universe{};        // market-cap / ADV / sector / top-N screen

  // Optional regime/macro overlay (off when regime_seg_path is empty): broadcast
  // each requested series as a `regime_<series>` field, as-of the panel date axis.
  // When empty, build_real_panel's panel + digest are byte-identical to the
  // pre-regime path (no-regression).
  std::string regime_seg_path;             // a sealed regime .seg (empty = off)
  std::vector<std::string> regime_fields;  // requested series, e.g. {"vix","t10y2y"}
};

// =========================================================================
//  Result
// =========================================================================

// The assembled real-data Panel plus its determinism pin and composing lineage.
//
// `panel` fields (in canonical order, see kField*): close (= total_return_index),
// raw_close, volume, high, low, open, dollar_volume, vwap, adv{w}, market_cap,
// sector — with the S1-4 in_universe mask applied. `digest` is signal_set_digest
// over those fields in that order (the golden pin). `lineage` lists the catalog
// dataset names that composed the Panel (price + corp_actions + universe), in
// ascending order.
struct RealPanel {
  alpha::Panel panel;
  atx::u64 digest{};
  std::vector<std::string> lineage;
};

// =========================================================================
//  Build
// =========================================================================

// Assemble the real-data Panel deterministically from the on-disk databento price
// data ⋈ the corporate-action master ⋈ the derived universe. See the file header
// for the fixed assembly order, the digest-pin contract, and the named error
// cases. Two calls with identical inputs return identical `digest`.
[[nodiscard]] atx::core::Result<RealPanel> build_real_panel(const RealDataConfig &cfg);

// Finalize the assembled field set into a Panel, optionally overlaying regime
// columns. When regime_seg_path is empty (or no series requested), this is exactly
// alpha::Panel::create(dates, instruments, field_names, field_data, universe) — the
// no-regression path (identical digest). Otherwise it opens the RegimeStore at
// regime_seg_path and appends one `regime_<series>` column per requested series,
// broadcast as-of `panel_dates_nanos` across in-universe instruments (out-of-universe
// -> NaN). Err propagated from RegimeStore::open / with_regime_fields.
[[nodiscard]] atx::core::Result<alpha::Panel> finalize_panel_with_regime(
    atx::usize dates, atx::usize instruments,
    std::span<const atx::i64> panel_dates_nanos,
    std::vector<std::string> field_names,
    std::vector<std::vector<atx::f64>> field_data,
    std::vector<std::uint8_t> universe,
    const std::string &regime_seg_path,
    const std::vector<std::string> &requested_series);

} // namespace atx::engine::data
