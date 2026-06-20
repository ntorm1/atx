#pragma once

// atx::engine::data — universe construction: market cap, ADV liquidity, sector
// classification, and the point-in-time membership mask (p3 S1-4).
//
// WHAT THIS IS
//   Given a price Panel (raw close + volume + the derived `dollar_volume`/`adv`
//   datafields) and an AXIS-MATCHED corporate-action `Dataset` (PIT shares +
//   sector, from S1-2), `build_universe` derives the four date×instrument fields
//   the real-data path screens on:
//     * market_cap   — shares_outstanding (PIT as-of) × RAW close
//     * adv_usd      — causal trailing mean of dollar_volume (no look-ahead)
//     * sector_code  — GICS code if present, else SIC fallback, else sentinel -1
//     * in_universe  — PIT membership: present ∧ mktcap-floor ∧ adv-floor ∧ top-N
//   These feed S1-5, which applies `in_universe` as the Panel's universe mask and
//   lands `market_cap`/`adv`/`sector` as Panel fields.
//
// MARKET CAP IS SPLIT-INVARIANT — the load-bearing convention (§0.7 #4)
//   Market cap = shares × price is invariant to a stock split: a 2:1 split halves
//   the price and doubles the share count, leaving the product unchanged. So we
//   MUST use the RAW (unadjusted) close paired with the as-of shares_outstanding,
//   NOT the split-adjusted or total-return close. Using an adjusted close would
//   double-count the split (shares already reflect the post-split basis) and
//   fabricate a market-cap discontinuity across every split date. A cell is NaN
//   when EITHER shares_outstanding or raw_close is NaN — never imputed.
//
// ADV IS CAUSAL — NO LOOK-AHEAD (§0.7 #4)
//   adv_usd is the trailing mean of `dollar_volume` (raw close × volume) over
//   `adv_window` bars, consumed from the Panel's pre-computed `adv{window}` /
//   `dollar_volume` datafields when present (alpha/datafields.hpp computes them
//   with the engine's full-window / any-NaN→NaN / CAUSAL ts_mean policy). The
//   trailing window reads only dates ≤ t, so truncating future dates leaves a
//   past date's ADV byte-identical — the no-look-ahead proof. When the Panel does
//   not already carry the matching adv column, we recompute it with the SAME
//   causal rolling-mean (never a centered or forward window).
//
// SECTOR FALLBACK (never coerce missing → 0)
//   sector_code(t,i) = GICS sector code if the corp-action Dataset carries one at
//   (t,i); else the SEC SIC code; else the sentinel kNoSector (-1). 0 is a valid-
//   looking sector and would silently misclassify, so missing is ALWAYS the
//   sentinel, never 0.
//
// SURVIVORSHIP CAVEAT — documented here, NOT fixed here
//   The screen operates only over the symbols present in the listed-only security
//   master (Nasdaq Trader active symbols; docs/us_split_adjustment_factors.md
//   "Limitations"). A delisted name simply never appears in the corp-action
//   Dataset, so it is silently absent from every universe — a SURVIVORSHIP BIAS
//   that flatters any backtest run over this universe. This unit does not recover
//   delisted equities (that needs a delisting-event source; backlogged to P4);
//   it DOCUMENTS the bias. The canonical statement lives in
//   `p3-impl/data-ingestion-reference.md` §universe (authored in S1-1); until that
//   doc lands, this header doc-comment + the S1-4 ledger row are the record. The
//   unit test asserts the caveat is referenced in that doc IF it exists, else
//   notes the assertion deferred-to-S1-1.
//
// ALIGNMENT CONTRACT — price defines the axis (§0.7 #3)
//   `corp_actions` MUST already be aligned onto the price Panel's axis: same
//   instrument count + order (corp Dataset instrument index i ↔ Panel column i)
//   and same date count + order (corp Dataset date row d ↔ Panel date d). S1-5
//   owns the alignment (it runs `align_onto` to project the corp-action Dataset
//   onto the price-defined canonical axis before calling here), so this unit is a
//   PURE TRANSFORM over matching axes — it does not re-implement as-of resolution.
//   A shape mismatch (date or instrument count differs) fails closed with
//   Err(InvalidArgument); the unit's own tests use tiny matching-axis fixtures.
//
// OWNERSHIP / LIFECYCLE
//   `build_universe` returns owned `UniverseFields` by value (cold path; the
//   four date×instrument vectors are allocated once per backtest window). It
//   borrows neither the Panel nor the Dataset past the call — every input is read
//   and the outputs are materialized before return. Nothing throws; errors travel
//   in Result.

#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/corporate_actions.hpp" // kNoSector, kColShares*, kColGics*, kColSic*
#include "atx/engine/data/dataset.hpp"

namespace atx::engine::data {

// =========================================================================
//  Configuration
// =========================================================================

// Screen parameters for one universe build. Defaults are the S1 convention: a
// 21-bar (≈ one trading month) causal ADV window with a $1M/day liquidity floor
// and no market-cap floor / count cap. A floor of 0 disables that screen; a
// top_n_by_adv of 0 disables the count cap.
struct UniverseConfig {
  atx::usize adv_window = 21;        // trailing bars for ADV (causal); 0 ⇒ ADV all-NaN
  atx::f64 min_adv_usd = 1.0e6;      // liquidity floor (dollars/day); 0 ⇒ no floor
  atx::f64 min_mktcap_usd = 0.0;     // market-cap floor (dollars); 0 ⇒ no floor
  atx::usize top_n_by_adv = 0;       // keep top-N by ADV each date; 0 ⇒ no count cap
  atx::f64 min_price = 0.0;          // raw-close price floor (membership iff raw_close > min_price);
                                     // 0 ⇒ no price screen (preserves legacy behavior)
  bool require_sector = false;       // exclude names with no GICS/SIC sector (a single-stock /
                                     // ETF-fund proxy: ETFs carry no GICS classifier). false ⇒
                                     // no sector requirement (legacy).
};

// =========================================================================
//  Output fields (all date-major, dates×instruments, aligned to the price axis)
// =========================================================================

// The four derived universe fields, each `dates*instruments` in the price
// Panel's date-major layout (cell (t, i) at t*instruments + i). `sector_code`
// holds integer GICS/SIC codes or kNoSectorCode (-1); `in_universe` is the
// {0,1} PIT membership mask S1-5 forwards to the Panel.
struct UniverseFields {
  std::vector<atx::f64> market_cap;  // shares_outstanding × raw_close; NaN if either NaN
  std::vector<atx::f64> adv_usd;     // causal trailing mean of dollar_volume; NaN until full window
  std::vector<atx::i32> sector_code; // GICS else SIC else kNoSectorCode (-1); never 0
  std::vector<atx::u8> in_universe;  // 1 == in-universe at (t, i), else 0
};

// =========================================================================
//  Build
// =========================================================================

// Construct the universe fields from an axis-matched price Panel + corp-action
// Dataset (see the ALIGNMENT CONTRACT above). The Panel must carry `close`
// (raw/unadjusted) and `volume`; ADV is read from the Panel's `adv{adv_window}`
// column when present, else recomputed causally from `dollar_volume` (also read
// from the Panel when present, else derived as close×volume).
//
// Returns Err(InvalidArgument) on: an axis mismatch between Panel and Dataset, a
// missing required Panel field (`close`/`volume`), or a corp-action Dataset that
// is not the canonical 6-column shape (no shares/sector to read).
[[nodiscard]] atx::core::Result<UniverseFields>
build_universe(const alpha::Panel &price_panel, const Dataset &corp_actions,
               const UniverseConfig &cfg);

} // namespace atx::engine::data
