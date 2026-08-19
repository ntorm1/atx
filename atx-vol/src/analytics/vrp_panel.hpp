#pragma once

// vrp_panel.hpp — versioned VRP label/feature panel builder.
// Contracts: `vrp_panel_v1` (FROZEN, 18 columns; 2026-08-15 vrp-ml sprint,
// lane vrp-panel) and `vrp_panel_v2` (round 4, lane vrp-panel-v2).
//
// Header-only core behind the `bev_label_factory --vrp-panel` mode: walks one
// or MORE SurfaceDb roots (the SPY corpus is one root per year, so spot/iv
// history is STITCHED across root boundaries — trailing and forward windows
// straddle a year boundary exactly as they would in one concatenated root)
// and emits one tab-separated row per (symbol, session):
//
//   iv_fair   = sqrt(var_swap_fair_strike(PricedSurface, T).fair_strike_dec)
//               at T = 21/252 (and 63/252 for the term-slope feature only)
//   iv_atmf   = PricedSurface::iv(F(T), T) at T = 21/252 — the ATM-FORWARD
//               implied vol (v2 only; see "The two implied legs" below)
//   rv_fwd    = realized_vol(CloseToClose, 252) over the spot-mirror bars of
//               sessions t+1 .. t+21 (a 21-bar span => 20 close-to-close
//               return terms; the span deliberately starts at t+1, so session
//               t's own close never enters the label — see the off-by-one
//               gate test)
//   label     = (rv_fwd^2 - iv_fair^2) * (21/252)      [variance units, the
//               LONG-VOL sign convention of the vrp-portfolio digest:
//               negative on average = the short side collects the carry]
//
// ## Schema versions — `vrp_panel_v1` vs `vrp_panel_v2`
//
// `VrpPanelConfig::schema` selects the emitted contract; **v2 is the default**
// and v1 is retained behind the flag so every round-1..3 regression anchor
// reproduces BYTE-IDENTICALLY. Consumers detect the version from the file
// itself: line 1 is always `# schema=vrp_panel_v<N>`.
//
// v2 is a strict PREFIX-EXTENSION of v1 in both blocks, which is what makes a
// v1/v2 diff a one-column diff instead of a re-alignment:
//   * columns  — v1's 18 in the frozen order, then `iv_atmf_21d` appended;
//   * meta     — v1's 12 lines in the frozen order, then the v2 counters
//                (`n_atmf21_unavailable`, `n_split_events_applied`,
//                `n_split_symbols_adjusted`, `n_rv_fwd_implausible`).
// The ROW SET is identical between the two versions for the same corpus and
// the same (absent) split reference: the drop policy below is unchanged, so a
// row lives or dies on the 21d strip exactly as it did in v1. Only the split
// adjustment (v2-only, and only when `--splits` is supplied) moves a v1 cell.
//
// ## The two implied legs, and why BOTH ship
//
// `iv_fair_21d` is the variance-swap fair strike `K_var` — an OTM-strip
// quadrature over our own fitted MID surface at a synthetic 21/252 tenor no
// listed contract expires on. `iv_atmf_21d` is the ATM-forward implied vol at
// that same tenor: `iv(F(T), T)`, the single point a `StrikeSelector::Kind::
// AtmForward` straddle (the instrument `VolEdgeStrategy` actually trades) is
// struck and marked at. On any non-flat smile `K_var > sigma_ATMF^2` strictly,
// and the gap scales with each name's skew — `derivatives.hpp`'s forward-
// variance entry states the same fact in the library's own words. A label
// built on `K_var` therefore credits the straddle with skew/convexity premium
// it never receives. v2 emits BOTH columns and changes NEITHER the label nor
// the row policy, so the next round can race the two targets head to head on
// one panel rather than on two runs that differ in more than the target.
//
// ## Split / corporate-action adjustment (v2 only)
//
// A SurfaceDb spot is the raw session spot: it steps discontinuously across a
// split ex-date, and `rv_fwd` reads that step as a genuine return. Supplying
// `VrpPanelConfig::splits` (a `symbol/ex_date/price_factor` TSV — see
// `load_vrp_split_factors`) BACK-ADJUSTS the spot series so those steps
// vanish. The adjustment is pure reference data: this header applies exactly
// the factors it is handed, with no detection threshold and no classification
// rule of its own.
//
// Row policy (frozen):
//   * a session whose 21d strip is unavailable (surface missing / invalid
//     spot / OutOfRange / strip error / non-positive strike) is DROPPED, with
//     per-reason counters printed and echoed into the file's meta header;
//     its SPOT still participates in neighbours' trailing/forward windows;
//   * OutOfRange (or any failure) at the 63d tenor only NaNs iv_fair_63d and
//     f4_term_slope — the row is KEPT;
//   * rows within 21 sessions of the panel tail emit rv_fwd_21d = label =
//     NaN and are KEPT (predict-time rows), counted separately;
//   * features are RAW — per-asset standardization happens in-fold in the
//     trainer (digest "Normalization" + Pitfall 6), never here.
//
// Feature windows (all information available at session t's close; "trailing
// k-session c2c variance" == realized_vol over the (k+1)-bar span ending at
// and including t, i.e. k close-to-close return terms r_{t-k+1..t}):
//   f0_log_rv1     ln(max(252*r_cc(t)^2, 1e-8))
//   f1_log_rv5     ln(trailing 5-session annualized c2c variance)
//   f2_log_rv21    ln(trailing 21-session annualized c2c variance)
//   f3_iv_level    ln(iv_fair_21d^2)
//   f4_term_slope  iv_fair_63d - iv_fair_21d          (NaN when 63d missing)
//   f5_hv_iv_gap   ln(rv_trail_21d / iv_fair_21d)
//   f6_vrp_lag     iv_fair_21d^2 - trailing 21d annualized c2c variance
//   f7_ret_21d     sum of r_cc over the trailing 21 sessions
//                  == ln(spot[t]/spot[t-21])
//   f8_jump_recent 1 if max|r_cc| over the trailing 5 sessions exceeds 4x the
//                  trailing-63-session DAILY c2c sigma, else 0 — the cheap
//                  earnings-proxy mask (round 1 has NO earnings calendar;
//                  documented limitation), NaN inside the 63-session warmup
//   f9_vov_63d     sample stdev (n-1 denominator) of the daily first
//                  difference of iv_fair_21d over the trailing 63 deltas
//                  (sessions t-62..t, so iv is needed back to t-63); NaN when
//                  the window is short or any iv in it is missing
// A window with insufficient trailing history yields NaN for that feature;
// the row is still emitted. A zero-variance trailing window yields -inf
// through the ln() (degenerate fixtures only; never dropped here).
//
// Determinism: output is byte-deterministic — rows sorted (symbol, session),
// %.17g round-trip double formatting, and the meta header carries only the
// schema line, the horizon, and run COUNTERS (no paths, no timestamps), so a
// stitched multi-root run and the same sessions in one concatenated root
// produce byte-identical files (gate-tested).
//
// Placement: private Tier-B header beside realized_vol.hpp — consumed by the
// bev_label_factory example TU (and, through the example's macro-guarded
// textual inclusion, by the gate test TU). Header-only so the example's
// no-CMake-edit contract holds; every function is `inline`.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "analytics/realized_vol.hpp" // OhlcBar, RvEstimator, realized_vol
#include "atx/vol/api/backtest/priced_surface.hpp" // PricedSurface
#include "atx/vol/api/core/types.hpp" // Result, Status, ErrorCode, ATX_TRY
#include "atx/vol/api/pricing/derivatives.hpp" // var_swap_fair_strike, DerivQuote
#include "atx/vol/api/storage/surface_db.hpp"  // SurfaceDb, DbPartitionInfo

namespace atx::vol {

// ── Frozen contract constants (vrp_panel_v1) ─────────────────────────────

inline constexpr std::string_view kVrpPanelSchemaV1 = "vrp_panel_v1";
inline constexpr std::string_view kVrpPanelSchemaV2 = "vrp_panel_v2";
inline constexpr std::string_view kVrpPanelSchemaV3 = "vrp_panel_v3";
inline constexpr std::string_view kVrpPanelSchemaV4 = "vrp_panel_v4";
inline constexpr std::size_t kVrpHorizonSessions = 21;    // forward-RV window
inline constexpr double kVrpTenor21Years = 21.0 / 252.0;  // strip tenor + label scale
inline constexpr double kVrpTenor63Years = 63.0 / 252.0;  // slope tenor
inline constexpr double kVrpLogRv1Floor = 1e-8;           // f0's ln() floor
inline constexpr std::size_t kVrpJumpWindowSessions = 5;  // f8 max|r| window
inline constexpr double kVrpJumpSigmaMultiple = 4.0;      // f8 threshold
inline constexpr std::size_t kVrpSigmaWindowSessions = 63; // f8 sigma window
inline constexpr std::size_t kVrpVovWindowSessions = 63;   // f9 delta count

// PERMANENT DATA-INTEGRITY GATE (v2): the largest forward realized vol this
// panel will accept, annualized decimal. A v2 run whose `rv_fwd_21d` exceeds
// it FAILS — the defect class it exists to catch (an unadjusted corporate
// action read as a genuine return) is silent, cross-sectionally concentrated,
// and dominates a squared-error objective, so it must not be a warning.
//
// Why 3.0 and not a looser number, on measured evidence from the round-1
// SP100 panel (19,042 labeled rows, 102 names, 2025-08..2026-07):
//   * the largest CLEAN value in the panel is 1.222 (ORCL, 2025-08-27) — the
//     gate sits 2.5x above the worst honest observation;
//   * every CORRUPT row sits at 5.79 or above (NOW 5.79, NFLX 8.22, BKNG
//     11.15) — the gate sits 1.9x below the mildest corruption. The two
//     populations are separated by a factor of 4.7 with nothing in between,
//     so the threshold is not fitted to either edge.
// Why 3.0 and not a tighter number, on economics: over a 21-bar (20-return)
// window, 300% annualized needs sum(r^2) >= 0.714 — one session of |r| >=
// 0.845 (a -57% day) or twenty consecutive +-19% sessions. The most extreme
// single-session large-cap collapse on record (AIG, 2008-09-16, -61%, |r| =
// 0.94) lands near 3.35, so a genuine event of that severity trips the gate
// and demands a human confirmation rather than silent ingestion. That is the
// intended behaviour for a data-integrity tier on an S&P-100 panel.
inline constexpr double kVrpMaxPlausibleRvFwd = 3.0;

// The SINGLE-STEP form of the same threshold, DERIVED from it rather than
// chosen: the smallest |log return| that on its own pushes a 21-session
// forward window (20 return terms) over kVrpMaxPlausibleRvFwd. It is exactly
// the arithmetic already spelled out in that constant's comment — "one
// session of |r| >= 0.845 (a -57% day)" — so the two tiers cannot drift apart.
//
// A step this large is the corporate-action signature. What makes it worth
// naming separately is that the forward gate CANNOT see all of the damage: an
// unadjusted split corrupts the 63 sessions of TRAILING windows that follow it
// too, and those rows carry a perfectly plausible rv_fwd. Quarantine (below)
// uses the step, not the row, as the unit of contamination.
inline constexpr double kVrpImplausibleStepReturn = 0.8451542547285166; // sqrt(9*20/252)

// What a run does when it meets a step it has no reference factor for.
//
//   Fail       — the frozen v2 behaviour: refuse the whole panel and name every
//                offender, because the only supported fix is to supply the
//                factor and refusing is what creates that incentive.
//   Quarantine — emit the panel, but NaN every value whose window spans a
//                contaminated step: rv_fwd/label for the 21 sessions before it,
//                the trailing features for the 63 sessions after it. The rows
//                SURVIVE with their spot and their uncontaminated columns.
//
// Quarantine is not a weaker gate, it is a narrower one. Fail is all-or-
// nothing at panel scope: on the 616-name xsec corpus SEVEN names with one
// unadjusted split each — 121 rows of ~150,000 — cost the entire panel. The
// unit of the defect is a step, so the unit of the response should be too.
// Nothing is fabricated either way: a split factor is reference data, and a
// panel that lacks it should say "I don't know", which is what NaN already
// means everywhere else in this file.
enum class VrpImplausiblePolicy : std::uint8_t { Fail = 0, Quarantine = 1 };

// Column names in EXACTLY the emitted order. v1 is FROZEN; v2 appends.
inline constexpr std::array<std::string_view, 18> kVrpPanelColumnsV1{
    "symbol",        "date",         "entry_ts_ns", "spot",
    "iv_fair_21d",   "iv_fair_63d",  "rv_fwd_21d",  "label",
    "f0_log_rv1",    "f1_log_rv5",   "f2_log_rv21", "f3_iv_level",
    "f4_term_slope", "f5_hv_iv_gap", "f6_vrp_lag",  "f7_ret_21d",
    "f8_jump_recent", "f9_vov_63d"};
inline constexpr std::size_t kVrpPanelColumnCount = kVrpPanelColumnsV1.size();

// v2 = v1's 18 columns in the frozen order, then the ATM-forward implied leg.
// Appended (not interleaved) so a v2 row is a strict prefix-extension of the
// v1 row it corresponds to.
inline constexpr std::array<std::string_view, 19> kVrpPanelColumnsV2{
    "symbol",        "date",         "entry_ts_ns", "spot",
    "iv_fair_21d",   "iv_fair_63d",  "rv_fwd_21d",  "label",
    "f0_log_rv1",    "f1_log_rv5",   "f2_log_rv21", "f3_iv_level",
    "f4_term_slope", "f5_hv_iv_gap", "f6_vrp_lag",  "f7_ret_21d",
    "f8_jump_recent", "f9_vov_63d",  "iv_atmf_21d"};
inline constexpr std::size_t kVrpPanelColumnCountV2 = kVrpPanelColumnsV2.size();

// v3 = v2's 19 columns in the frozen order, then the two LIQUIDITY columns.
// Appended, never interleaved, so a v3 row is a strict prefix-extension of the
// v2 row and a v2 reader can be pointed at a v3 file's prefix unchanged.
//
// `liq_hspread_frac` — the MEASURED ATM one-way relative QUOTED half-spread,
//   (ask-bid)/2/mid, for the expiry nearest 21 calendar days, at the ATM strike
//   located by put-call parity. It comes from the reference TSV
//   `atx-vol/scripts/vrp_hive_liquidity.py` emits off the SAME opra-hive
//   snapshot the surface corpus was fit from, so it is a measurement of the
//   corpus's own market, not an outside estimate. NaN where the reference file
//   has no (symbol, date) row.
//   IT IS NOT: an EFFECTIVE spread (a patient order pays less — that discount
//   belongs in the trainer's `crossing` knob); a size/depth measure; an
//   intraday average (one snapshot per session); or a statement about the wings.
//
// `liq_strikes_fit` — the archive-side breadth PROXY: the number of quoted
//   strikes that survived board fitting summed over every expiry of that
//   session's surface (sum of `SliceContext::n_used`). It costs nothing because
//   ATXVSA2 already persists it per slice (`col_nused_off`), and it covers every
//   surface ever written. IT IS A WEAK PROXY AND IS CARRIED AS A DIAGNOSTIC, NOT
//   AS THE COST INPUT: measured against `liq_hspread_frac` over the 614-name
//   corpus its rank correlation is only -0.42 (log-log R^2 0.21), so it explains
//   about a fifth of the cross-sectional spread variation. It PROXIES how broad
//   a strike ladder a vendor quoted well enough to fit; it does NOT proxy the
//   width of the market, the size at the touch, or what a trade costs.
inline constexpr std::array<std::string_view, 21> kVrpPanelColumnsV3{
    "symbol",        "date",         "entry_ts_ns", "spot",
    "iv_fair_21d",   "iv_fair_63d",  "rv_fwd_21d",  "label",
    "f0_log_rv1",    "f1_log_rv5",   "f2_log_rv21", "f3_iv_level",
    "f4_term_slope", "f5_hv_iv_gap", "f6_vrp_lag",  "f7_ret_21d",
    "f8_jump_recent", "f9_vov_63d",  "iv_atmf_21d", "liq_hspread_frac",
    "liq_strikes_fit"};
inline constexpr std::size_t kVrpPanelColumnCountV3 = kVrpPanelColumnsV3.size();

// v4 = v3's 21 columns in the frozen order, then `bar_index`. It is a prefix
// extension like every predecessor, but it is the FIRST schema that also
// changes the ROW POLICY, and that is the whole point of it:
//
//   THE BAR AXIS IS NOT THE EMITTED AXIS. v1/v2/v3 DROP a session whose 21d
//   strip is unavailable while KEEPING that session's spot in its neighbours'
//   trailing/forward windows (see "Row policy (frozen)" above). So those
//   panels compute on the full bar axis and emit a strict SUBSET of it. On
//   the shipped 25-name v2 panel the subset is missing 14.2% of all
//   (symbol, session) pairs — 3544 of 24888 — essentially all of them
//   `var21_out_of_range`, i.e. sessions with a PRESENT surface and a VALID
//   SPOT whose 21d tenor simply fell outside the fitted pillars.
//
//   The consequence is silent and severe for any DOWNSTREAM feature work: a
//   consumer that reads an emitted panel and computes its own trailing
//   21-session window is stepping over holes, so its window spans more
//   calendar than it thinks and its value is WRONG — measured up to 1.261e+01
//   in log-variance units against the panel's own column. Worse, the error
//   is invisible: only windowed features move, row-local ones agree exactly.
//   Gating on emitted-row contiguity restores correctness but destroys
//   coverage: at a 14% hole rate a contiguous 63-session run survives with
//   probability 0.86^63, which is why a 63-session feature measured ZERO
//   usable rows.
//
//   v4 fixes it at the source rather than gating around it. A session is
//   emitted whenever the SURFACE LOADED AND THE SPOT WAS VALID — exactly the
//   bar-axis membership test — so the emitted axis IS the bar axis and a
//   downstream trailing window over emitted rows reproduces the panel's own
//   column. Sessions the 21d strip failed on are emitted with `iv_fair_21d`
//   NaN, which propagates through `label`/`f3`/`f4`/`f5`/`f6` by ordinary
//   NaN arithmetic; the spot-derived columns (`f0`/`f1`/`f2`/`f7`/`f8`,
//   `rv_fwd_21d`) are FULLY VALID on those rows and are the coverage this
//   schema exists to recover. `n_no_surface` and `n_bad_spot` sessions are
//   still absent — there is no bar for them to be on.
//
//   `bar_index` is the row's 0-based position on that symbol's bar axis. It
//   is emitted so the invariant is CHECKABLE rather than merely asserted: a
//   consumer verifies adjacency with `bar_index[i] == bar_index[i-1] + 1`
//   instead of inferring it from dates and a trading calendar it does not
//   have. Under v4 the check must pass for every consecutive pair of a
//   symbol's rows; under v1/v2/v3 the column does not exist precisely
//   because the property does not hold.
inline constexpr std::array<std::string_view, 22> kVrpPanelColumnsV4{
    "symbol",        "date",         "entry_ts_ns", "spot",
    "iv_fair_21d",   "iv_fair_63d",  "rv_fwd_21d",  "label",
    "f0_log_rv1",    "f1_log_rv5",   "f2_log_rv21", "f3_iv_level",
    "f4_term_slope", "f5_hv_iv_gap", "f6_vrp_lag",  "f7_ret_21d",
    "f8_jump_recent", "f9_vov_63d",  "iv_atmf_21d", "liq_hspread_frac",
    "liq_strikes_fit", "bar_index"};
inline constexpr std::size_t kVrpPanelColumnCountV4 = kVrpPanelColumnsV4.size();

enum class VrpPanelSchema : std::uint8_t { V1 = 0, V2 = 1, V3 = 2, V4 = 3 };

[[nodiscard]] inline constexpr std::string_view schema_name(VrpPanelSchema s) noexcept {
  switch (s) {
  case VrpPanelSchema::V1:
    return kVrpPanelSchemaV1;
  case VrpPanelSchema::V2:
    return kVrpPanelSchemaV2;
  case VrpPanelSchema::V3:
    return kVrpPanelSchemaV3;
  case VrpPanelSchema::V4:
    return kVrpPanelSchemaV4;
  }
  return kVrpPanelSchemaV2; // unreachable for a valid enumerator
}

[[nodiscard]] inline constexpr std::size_t schema_column_count(VrpPanelSchema s) noexcept {
  switch (s) {
  case VrpPanelSchema::V1:
    return kVrpPanelColumnCount;
  case VrpPanelSchema::V2:
    return kVrpPanelColumnCountV2;
  case VrpPanelSchema::V3:
    return kVrpPanelColumnCountV3;
  case VrpPanelSchema::V4:
    return kVrpPanelColumnCountV4;
  }
  return kVrpPanelColumnCountV2; // unreachable for a valid enumerator
}

// Column name at index `i` under `s`. Each schema is a prefix extension of its
// predecessor, so one table answers for all of them.
[[nodiscard]] inline constexpr std::string_view schema_column(VrpPanelSchema s,
                                                              std::size_t i) noexcept {
  return s == VrpPanelSchema::V4   ? kVrpPanelColumnsV4[i]
         : s == VrpPanelSchema::V3 ? kVrpPanelColumnsV3[i]
         : s == VrpPanelSchema::V2 ? kVrpPanelColumnsV2[i]
                                   : kVrpPanelColumnsV1[i];
}

// ── Split / corporate-action reference data (v2) ──────────────────────────

// One back-adjustment event. `price_factor` is the multiplier applied to every
// session STRICTLY BEFORE `ex_date`, i.e. the standard back-adjusted-series
// convention: a 10-for-1 split carries 0.1, a 1-for-10 reverse split 10.0.
// This header never derives a factor and never classifies an event; it applies
// exactly what the reference file states.
// One MEASURED (symbol, session) option-market width. `half_spread_frac` is
// (ask-bid)/2/mid at the ATM strike of the expiry nearest 21 calendar days —
// a QUOTED one-way relative half-spread, finite and > 0.
struct VrpLiquidityRef {
  std::string symbol;
  std::string date;
  double half_spread_frac{0.0};
};

struct VrpSplitFactor {
  std::string symbol;
  std::string ex_date; // ISO date, sorts lexicographically == chronologically
  double price_factor{1.0};
};

// ── Config / counters / row / series ─────────────────────────────────────

struct VrpPanelConfig {
  std::vector<std::string> db_roots; // >= 1; stitched in session-date order
  // Symbol filter. Empty => the union of every root's manifest symbol table
  // (an error if that union is empty — pass --uid for roots whose partitions
  // carry symbols the manifest never registered).
  std::vector<std::string> symbols;
  // Optional inclusive ISO-date bounds on the SESSION axis (they bound the
  // loaded history too, so rows near the bounds carry warmup/tail NaNs; pad
  // the window when full features/labels are needed at its edges).
  std::string entry_start;
  std::string entry_end;
  std::string out; // TSV path
  // Emitted contract. V2 by default; V1 reproduces the frozen round-1..3
  // artifact byte-for-byte and therefore REFUSES every v2-only input below.
  VrpPanelSchema schema{VrpPanelSchema::V2};
  // Optional split/corporate-action reference TSV (v2 only; empty = the raw
  // SurfaceDb spot series is used unadjusted, exactly as v1 does).
  std::string splits;
  // What to do with a spot step no supplied split factor explains. See
  // VrpImplausiblePolicy. Fail is the default so the frozen v2 contract and
  // every existing caller are unchanged.
  VrpImplausiblePolicy on_implausible{VrpImplausiblePolicy::Fail};
  // Optional MEASURED liquidity reference TSV (v3 only; empty = every
  // `liq_hspread_frac` is NaN and the row still carries the archive-side
  // `liq_strikes_fit` proxy). Generated by `scripts/vrp_hive_liquidity.py`.
  std::string liquidity;
};

struct VrpPanelCounters {
  std::size_t n_sessions{0};          // merged session axis length
  std::size_t n_symbol_sessions{0};   // (symbol, session) pairs attempted
  std::size_t n_no_surface{0};        // symbol absent from that partition
  std::size_t n_bad_spot{0};          // non-finite/non-positive spot
  std::size_t n_var21_out_of_range{0}; // 21d tenor outside fitted pillars
  std::size_t n_var21_error{0};        // any other 21d strip failure
  std::size_t n_var21_nonfinite{0};    // strip Ok but K_var not finite/positive
  std::size_t n_63d_unavailable{0};    // 63d strip missing (row kept, f4 NaN)
  std::size_t n_rows_tail_nan_label{0}; // kept rows with NaN forward window
  std::size_t n_rows_written{0};
  // ── v2-only (never emitted into a v1 meta header) ──────────────────────
  std::size_t n_atmf21_unavailable{0};   // ATMF leg missing (row kept, col NaN)
  std::size_t n_split_events_applied{0}; // reference events folded into a series
  std::size_t n_split_symbols_adjusted{0}; // symbols with >= 1 folded event
  std::size_t n_rv_fwd_implausible{0};   // rows above kVrpMaxPlausibleRvFwd
  // Quarantine accounting (zero under VrpImplausiblePolicy::Fail).
  std::size_t n_implausible_steps{0};   // bar-axis steps above the step threshold
  std::size_t n_quarantined_forward{0}; // rows whose rv_fwd/label were NaN'd
  std::size_t n_quarantined_trailing{0}; // rows with >= 1 trailing feature NaN'd
  // ── v3-only (never emitted into a v1/v2 meta header) ───────────────────
  std::size_t n_liq_ref_missing{0}; // bars with no (symbol, date) reference row
  std::size_t n_liq_ref_matched{0}; // bars a reference half-spread was found for
};

// One symbol's stitched per-session history (the "bar axis"): parallel
// arrays, one entry per session where the symbol HAD a surface with a valid
// spot. iv21 is NaN where the 21d strip was unavailable (row dropped, bar
// kept); iv63 is NaN where only the 63d strip was unavailable (row kept).
struct VrpSeries {
  std::vector<std::string> dates;  // partition keys, ascending
  std::vector<std::int64_t> ts_ns; // strictly ascending session timestamps
  std::vector<double> spot;        // finite, > 0
  std::vector<double> iv21;
  std::vector<double> iv63;
  // ATM-forward implied vol at kVrpTenor21Years (v2's `iv_atmf_21d`). NaN
  // where the surface could not answer; carried on the bar axis like iv21/
  // iv63 so the same parallel-array invariant covers it.
  std::vector<double> iv_atmf21;
  // v3 liquidity, both on the same bar axis. `liq_strikes_fit` is read out of
  // the archive with the surface (never NaN — a loaded surface always has
  // slices); `liq_hspread` is joined from the reference TSV afterwards and is
  // NaN until/unless that join finds the (symbol, date) row.
  std::vector<double> liq_strikes_fit;
  std::vector<double> liq_hspread;
};

// One emitted panel row (symbol lives beside the row batch, not in it).
struct VrpPanelRow {
  std::string date;
  std::int64_t entry_ts_ns{0};
  double spot{0.0};
  double iv_fair_21d{0.0};
  double iv_fair_63d{0.0};
  double rv_fwd_21d{0.0};
  double label{0.0};
  double f0_log_rv1{0.0};
  double f1_log_rv5{0.0};
  double f2_log_rv21{0.0};
  double f3_iv_level{0.0};
  double f4_term_slope{0.0};
  double f5_hv_iv_gap{0.0};
  double f6_vrp_lag{0.0};
  double f7_ret_21d{0.0};
  double f8_jump_recent{0.0};
  double f9_vov_63d{0.0};
  double iv_atmf_21d{0.0}; // v2 only; NaN when the surface had no ATMF answer
  // v3 only. See kVrpPanelColumnsV3 for what each one does and does not mean.
  double liq_hspread_frac{0.0};
  double liq_strikes_fit{0.0};
  // v4 only: 0-based position on this symbol's BAR AXIS. Under v4 the emitted
  // axis IS the bar axis, so consecutive emitted rows of one symbol always
  // differ by exactly 1 and a downstream window over emitted rows is the
  // panel's own window. Carried as an integer, not a double, because it is an
  // index a consumer compares for exact adjacency, not a measurement.
  std::int64_t bar_index{0};
};

namespace vrp_detail {

[[nodiscard]] inline double nan_d() noexcept {
  return std::numeric_limits<double>::quiet_NaN();
}

// %.17g — max_digits10, the minimum that round-trips any double bit-exactly
// (same convention as bev_label_factory's writer; snprintf is locale-stable
// under "C" numeric formatting). NaN is canonicalized to the single spelling
// "nan" FIRST: NaN-propagating arithmetic (e.g. qNaN * qNaN in the warmup
// features) can carry a set sign bit, which the Windows UCRT prints as
// "-nan(ind)" — an alternate spelling a frozen byte-deterministic contract
// cannot admit (and one Python's float() refuses to parse). A NaN's sign is
// meaningless, so no information is lost.
inline void append_double(std::string &out, double v) {
  if (std::isnan(v)) {
    out += "nan";
    return;
  }
  char buf[64];
  const int len = std::snprintf(buf, sizeof buf, "%.17g", v);
  out.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

inline void append_i64(std::string &out, std::int64_t v) {
  char buf[32];
  const int len = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
  out.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

// Tab/newline would corrupt the TSV framing; mirror bev's sanitizer.
inline void append_sanitized(std::string &out, std::string_view s) {
  for (const char c : s) {
    out += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
  }
}

inline void append_meta_count(std::string &out, std::string_view key, std::size_t value) {
  out += "# ";
  out += key;
  out += '=';
  out += std::to_string(value);
  out += '\n';
}

// Annualized c2c vol over the (k+1)-bar span ending at index `i` — exactly
// `k` close-to-close return terms r_{i-k+1..i}. NaN when the trailing
// history is short. Defensive NaN (never an error) if realized_vol rejects
// the slice — impossible for the validated spot-mirror bars built below.
[[nodiscard]] inline double trailing_c2c_vol(std::span<const OhlcBar> bars, std::size_t i,
                                             std::size_t k) {
  if (k == 0 || i < k) {
    return nan_d();
  }
  const Result<double> v =
      realized_vol(bars.subspan(i - k, k + 1), RvEstimator::CloseToClose, 252.0);
  return v.has_value() ? *v : nan_d();
}

// f9: sample stdev (n-1) of the 63 daily iv21 first differences ending at
// `i` (deltas at sessions i-62..i, so iv21 is read back to i-63). NaN when
// the window is short or any iv21 inside it is missing — a dropped session
// mid-window deliberately NaNs the whole window rather than silently
// bridging the gap.
[[nodiscard]] inline double vov_63d(std::span<const double> iv21, std::size_t i) {
  const std::size_t w = kVrpVovWindowSessions;
  if (i < w) {
    return nan_d();
  }
  double mean = 0.0;
  // Bounded: exactly w iterations.
  for (std::size_t j = i + 1 - w; j <= i; ++j) {
    const double d = iv21[j] - iv21[j - 1];
    if (!std::isfinite(d)) {
      return nan_d();
    }
    mean += d;
  }
  mean /= static_cast<double>(w);
  double acc = 0.0;
  for (std::size_t j = i + 1 - w; j <= i; ++j) {
    const double d = iv21[j] - iv21[j - 1];
    acc += (d - mean) * (d - mean);
  }
  return std::sqrt(acc / static_cast<double>(w - 1));
}

// f8's max|r_cc| over the trailing kVrpJumpWindowSessions sessions ending at
// `i` (returns at bars i-4..i, so closes back to i-5). NaN inside the warmup.
[[nodiscard]] inline double max_abs_r_5d(std::span<const double> spot, std::size_t i) {
  const std::size_t w = kVrpJumpWindowSessions;
  if (i < w) {
    return nan_d();
  }
  double mx = 0.0;
  // Bounded: exactly w iterations.
  for (std::size_t j = i + 1 - w; j <= i; ++j) {
    mx = std::max(mx, std::fabs(std::log(spot[j] / spot[j - 1])));
  }
  return mx;
}

} // namespace vrp_detail

// ── Split / corporate-action back-adjustment (v2) ─────────────────────────

// Parse a split-factor reference TSV. Grammar (see
// `atx-vol/scripts/vrp_split_factors.py`, which generates it):
//
//   `#`-prefixed comment/provenance lines (anywhere), then exactly one header
//   line `symbol<TAB>ex_date<TAB>price_factor`, then data rows. Blank lines
//   are skipped. Extra trailing columns are IGNORED so a generator may carry
//   provenance (source, raw close, implied ratio) beside the three fields this
//   loader contracts on.
//
// Validation is strict at this boundary — the interior applies factors with no
// further checks: `price_factor` must parse fully, be finite and > 0;
// `symbol`/`ex_date` must be non-empty; a (symbol, ex_date) pair must be
// unique. Output is sorted by (symbol, ex_date) so the applier can walk it.
//
// Errors: IoError (unreadable), ParseError (missing/mis-spelled header, short
// row, unparseable or non-positive factor), InvalidArgument (duplicate key).
[[nodiscard]] inline Result<std::vector<VrpSplitFactor>>
load_vrp_split_factors(std::string_view path) {
  std::ifstream is{std::string(path), std::ios::binary};
  if (!is) {
    return atx::core::Err(ErrorCode::IoError, "load_vrp_split_factors: cannot open '" +
                                                  std::string(path) + "'");
  }
  std::vector<VrpSplitFactor> out;
  std::string line;
  bool header_seen = false;
  std::size_t line_no = 0;
  // Bounded by the file's line count.
  while (std::getline(is, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back(); // tolerate CRLF reference files
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::array<std::string_view, 3> field{};
    std::size_t n_field = 0;
    std::size_t start = 0;
    const std::string_view sv{line};
    // Bounded by the line length; keeps only the first three fields.
    while (n_field < field.size()) {
      const std::size_t tab = sv.find('\t', start);
      field[n_field++] = sv.substr(start, tab == std::string_view::npos ? tab : tab - start);
      if (tab == std::string_view::npos) {
        break;
      }
      start = tab + 1;
    }
    if (!header_seen) {
      if (n_field != 3 || field[0] != "symbol" || field[1] != "ex_date" ||
          field[2] != "price_factor") {
        return atx::core::Err(ErrorCode::ParseError,
                              "load_vrp_split_factors: '" + std::string(path) + "' line " +
                                  std::to_string(line_no) +
                                  ": expected header 'symbol<TAB>ex_date<TAB>price_factor'");
      }
      header_seen = true;
      continue;
    }
    if (n_field != 3 || field[0].empty() || field[1].empty()) {
      return atx::core::Err(ErrorCode::ParseError,
                            "load_vrp_split_factors: '" + std::string(path) + "' line " +
                                std::to_string(line_no) + ": need non-empty symbol/ex_date and a "
                                                          "price_factor field");
    }
    const std::string factor_text{field[2]};
    char *end = nullptr;
    const double factor = std::strtod(factor_text.c_str(), &end);
    if (end != factor_text.c_str() + factor_text.size() || !std::isfinite(factor) ||
        !(factor > 0.0)) {
      return atx::core::Err(ErrorCode::ParseError,
                            "load_vrp_split_factors: '" + std::string(path) + "' line " +
                                std::to_string(line_no) + ": price_factor '" + factor_text +
                                "' is not a finite positive number");
    }
    out.push_back(VrpSplitFactor{std::string(field[0]), std::string(field[1]), factor});
  }
  if (!header_seen) {
    return atx::core::Err(ErrorCode::ParseError, "load_vrp_split_factors: '" + std::string(path) +
                                                     "' has no header line");
  }
  std::sort(out.begin(), out.end(), [](const VrpSplitFactor &a, const VrpSplitFactor &b) {
    return a.symbol != b.symbol ? a.symbol < b.symbol : a.ex_date < b.ex_date;
  });
  // Bounded by out.size().
  for (std::size_t i = 1; i < out.size(); ++i) {
    if (out[i].symbol == out[i - 1].symbol && out[i].ex_date == out[i - 1].ex_date) {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "load_vrp_split_factors: duplicate (symbol, ex_date) '" +
                                out[i].symbol + "' / '" + out[i].ex_date + "'");
    }
  }
  return atx::core::Ok(std::move(out));
}

// ── Measured liquidity reference data (v3) ────────────────────────────────

// Parse the liquidity reference TSV `scripts/vrp_hive_liquidity.py` emits.
//
// Grammar: `#`-prefixed comment lines and blank lines anywhere; one header line
// whose FIRST THREE fields are `date`, `underlying`, and whose columns include
// `atm_rel_hspread`; then data rows. The header is located by NAME rather than
// by position for the half-spread column, because the generator carries several
// provenance columns beside it (dte, strike counts, depth) and their order is
// not part of this contract — but `date` and `underlying` are pinned to
// positions 0 and 1 so a wholly unrelated file cannot be mistaken for this one.
//
// Rows whose `atm_rel_hspread` is `nan` are SKIPPED rather than rejected: the
// generator legitimately emits one for a session where no ATM pair was quoted,
// and the panel's own missing-reference path (NaN column, counted) is the right
// handling. Any other unparseable value IS a hard error — a silently-dropped
// malformed width would understate cost.
//
// Errors: IoError (unreadable), ParseError (missing header, absent
// `atm_rel_hspread` column, short row, unparseable or non-positive width),
// InvalidArgument (duplicate (symbol, date)). Output sorted by (symbol, date).
[[nodiscard]] inline Result<std::vector<VrpLiquidityRef>>
load_vrp_liquidity_ref(std::string_view path) {
  std::ifstream is{std::string(path), std::ios::binary};
  if (!is) {
    return atx::core::Err(ErrorCode::IoError,
                          "load_vrp_liquidity_ref: cannot open '" + std::string(path) + "'");
  }
  std::vector<VrpLiquidityRef> out;
  std::string line;
  std::size_t hs_col = 0;
  bool header_seen = false;
  std::size_t line_no = 0;
  // Bounded by the file's line count.
  while (std::getline(is, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back(); // tolerate CRLF reference files
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::vector<std::string_view> field;
    {
      const std::string_view sv{line};
      std::size_t start = 0;
      // Bounded by the line length.
      while (true) {
        const std::size_t tab = sv.find('\t', start);
        field.push_back(sv.substr(start, tab == std::string_view::npos ? tab : tab - start));
        if (tab == std::string_view::npos) {
          break;
        }
        start = tab + 1;
      }
    }
    if (!header_seen) {
      if (field.size() < 3 || field[0] != "date" || field[1] != "underlying") {
        return atx::core::Err(ErrorCode::ParseError,
                              "load_vrp_liquidity_ref: '" + std::string(path) + "' line " +
                                  std::to_string(line_no) +
                                  ": expected a header starting 'date<TAB>underlying'");
      }
      bool found = false;
      // Bounded by the header width.
      for (std::size_t i = 2; i < field.size(); ++i) {
        if (field[i] == "atm_rel_hspread") {
          hs_col = i;
          found = true;
          break;
        }
      }
      if (!found) {
        return atx::core::Err(ErrorCode::ParseError,
                              "load_vrp_liquidity_ref: '" + std::string(path) +
                                  "' header has no 'atm_rel_hspread' column");
      }
      header_seen = true;
      continue;
    }
    if (field.size() <= hs_col || field[0].empty() || field[1].empty()) {
      return atx::core::Err(ErrorCode::ParseError,
                            "load_vrp_liquidity_ref: '" + std::string(path) + "' line " +
                                std::to_string(line_no) +
                                ": need non-empty date/underlying and an atm_rel_hspread field");
    }
    const std::string text{field[hs_col]};
    if (text == "nan" || text == "NaN" || text.empty()) {
      continue; // no ATM pair quoted that session; the panel's NaN path owns it
    }
    char *end = nullptr;
    const double hs = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(hs) || !(hs > 0.0)) {
      return atx::core::Err(ErrorCode::ParseError,
                            "load_vrp_liquidity_ref: '" + std::string(path) + "' line " +
                                std::to_string(line_no) + ": atm_rel_hspread '" + text +
                                "' is not a finite positive number");
    }
    out.push_back(VrpLiquidityRef{std::string(field[1]), std::string(field[0]), hs});
  }
  if (!header_seen) {
    return atx::core::Err(ErrorCode::ParseError,
                          "load_vrp_liquidity_ref: '" + std::string(path) + "' has no header line");
  }
  std::sort(out.begin(), out.end(), [](const VrpLiquidityRef &a, const VrpLiquidityRef &b) {
    return a.symbol != b.symbol ? a.symbol < b.symbol : a.date < b.date;
  });
  // Bounded by out.size().
  for (std::size_t i = 1; i < out.size(); ++i) {
    if (out[i].symbol == out[i - 1].symbol && out[i].date == out[i - 1].date) {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "load_vrp_liquidity_ref: duplicate (symbol, date) '" + out[i].symbol +
                                "' / '" + out[i].date + "'");
    }
  }
  return atx::core::Ok(std::move(out));
}

// Join one symbol's measured widths onto its bar axis, in place. `events` are
// this symbol's reference rows, ascending by date; `s.liq_hspread` is sized to
// the bar axis and left NaN wherever no row matches. Counters record both sides
// so a silently-empty join is visible in the meta header.
//
// @return the number of bars matched.
inline std::size_t apply_vrp_liquidity_ref(VrpSeries &s,
                                           std::span<const VrpLiquidityRef> events) {
  const std::size_t n = s.dates.size();
  s.liq_hspread.assign(n, vrp_detail::nan_d());
  std::size_t matched = 0;
  std::size_t j = 0;
  // Merge walk: both sides are ascending by date, so this is linear and needs
  // no map. Bounded by n + events.size().
  for (std::size_t i = 0; i < n; ++i) {
    while (j < events.size() && events[j].date < s.dates[i]) {
      ++j;
    }
    if (j < events.size() && events[j].date == s.dates[i]) {
      s.liq_hspread[i] = events[j].half_spread_frac;
      ++matched;
    }
  }
  return matched;
}

// Back-adjust one symbol's spot series in place for the supplied events
// (ascending `ex_date`, all belonging to `s`).
//
// Convention: the session ON the ex-date already trades post-event, so every
// session STRICTLY BEFORE it is multiplied by `price_factor`, cumulatively
// across later events. Two consequences worth stating because callers rely on
// them: (a) the MOST RECENT session is never rescaled, so the adjusted series
// stays anchored to the live price rather than drifting with the event count;
// (b) only the spot LEVEL moves — every quantity the panel derives from spot
// is a ratio of two adjusted closes, so a window that does not straddle an
// ex-date is bit-identical to its unadjusted self.
//
// Events outside `(dates.front(), dates.back()]` are ignored: one at or before
// the first session has no earlier session to scale, and one after the last
// would rescale the entire series uniformly — a no-op on every return, but a
// gratuitous change to the emitted `spot` column.
//
// @return the number of events actually folded in.
inline std::size_t apply_vrp_split_adjustment(VrpSeries &s,
                                              std::span<const VrpSplitFactor> events) {
  const std::size_t n = s.spot.size();
  if (n == 0 || events.empty()) {
    return 0;
  }
  const std::string &first = s.dates.front();
  const std::string &last = s.dates.back();
  std::vector<const VrpSplitFactor *> in_range;
  in_range.reserve(events.size());
  // Bounded by events.size().
  for (const VrpSplitFactor &e : events) {
    if (e.ex_date > first && e.ex_date <= last) {
      in_range.push_back(&e);
    }
  }
  if (in_range.empty()) {
    return 0;
  }
  // One descending pass: the qualifying event set grows monotonically as the
  // session index falls, so each event is folded into `cum` exactly once.
  double cum = 1.0;
  std::size_t j = in_range.size();
  for (std::size_t i = n; i-- > 0;) {
    while (j > 0 && in_range[j - 1]->ex_date > s.dates[i]) {
      cum *= in_range[j - 1]->price_factor;
      --j;
    }
    s.spot[i] *= cum;
  }
  return in_range.size();
}

// ── Row building (pure; the gate tests drive this without a SurfaceDb) ───

// Series -> panel rows. Bars with NaN iv21 are the already-counted dropped
// sessions: under v1/v2/v3 they are skipped here, but their spots still feed
// every window (they are in `s`). Under v4 they are EMITTED instead, carrying
// a NaN implied leg and a fully valid spot-derived block — see
// kVrpPanelColumnsV4 for why the emitted-axis/bar-axis gap is a defect worth
// a schema. Counts n_rows_written / n_rows_tail_nan_label into `counters`.
// Errors (InvalidArgument) only on a malformed series: mismatched parallel
// arrays, non-ascending ts, or an invalid spot — the loader guarantees all
// three, so an error here means a caller bug, not data quality.
[[nodiscard]] inline Result<std::vector<VrpPanelRow>>
build_vrp_rows(const VrpSeries &s, VrpPanelCounters &counters, VrpPanelSchema schema,
               VrpImplausiblePolicy on_implausible = VrpImplausiblePolicy::Fail) {
  const std::size_t n = s.spot.size();
  if (s.dates.size() != n || s.ts_ns.size() != n || s.iv21.size() != n || s.iv63.size() != n ||
      s.iv_atmf21.size() != n) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "build_vrp_rows: parallel series arrays disagree in size");
  }
  std::vector<OhlcBar> bars;
  bars.reserve(n);
  // Bounded by n. Validates the series while mirroring spots into bars.
  for (std::size_t i = 0; i < n; ++i) {
    if (!(std::isfinite(s.spot[i]) && s.spot[i] > 0.0)) {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "build_vrp_rows: non-finite/non-positive spot at index " +
                                std::to_string(i));
    }
    if (i > 0 && s.ts_ns[i] <= s.ts_ns[i - 1]) {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "build_vrp_rows: session ts not strictly ascending at index " +
                                std::to_string(i));
    }
    bars.push_back(OhlcBar{
        .ts_ns = s.ts_ns[i], .open = s.spot[i], .high = s.spot[i], .low = s.spot[i],
        .close = s.spot[i]});
  }

  // ── Contaminated-step map (quarantine only) ─────────────────────────────
  //
  // `bad_before[i]` counts the contaminated steps among steps 1..i, where step
  // j is the return from bar j-1 to bar j. A prefix sum answers "does the
  // window covering steps [a, b] contain one?" in O(1), which is what the
  // per-feature masking below asks once per feature per row.
  std::vector<std::uint32_t> bad_before;
  const bool quarantine = on_implausible == VrpImplausiblePolicy::Quarantine;
  if (quarantine) {
    bad_before.assign(n, 0U);
    // Bounded by n.
    for (std::size_t j = 1; j < n; ++j) {
      const double r = std::log(s.spot[j] / s.spot[j - 1]);
      const bool bad = std::isfinite(r) && std::abs(r) >= kVrpImplausibleStepReturn;
      bad_before[j] = bad_before[j - 1] + (bad ? 1U : 0U);
      counters.n_implausible_steps += bad ? 1U : 0U;
    }
  }
  // Any contaminated step in the INCLUSIVE step range [a, b]? Steps outside
  // 1..n-1 do not exist and cannot be contaminated.
  const auto spans_bad = [&bad_before, n, quarantine](std::ptrdiff_t a, std::ptrdiff_t b) {
    if (!quarantine || n == 0) {
      return false;
    }
    const std::ptrdiff_t lo = std::max<std::ptrdiff_t>(a, 1);
    const std::ptrdiff_t hi = std::min<std::ptrdiff_t>(b, static_cast<std::ptrdiff_t>(n) - 1);
    if (lo > hi) {
      return false;
    }
    return bad_before[static_cast<std::size_t>(hi)] >
           bad_before[static_cast<std::size_t>(lo - 1)];
  };

  std::vector<VrpPanelRow> rows;
  rows.reserve(n);
  // Bounded by n — one pass over the bar axis.
  for (std::size_t i = 0; i < n; ++i) {
    const double iv21 = s.iv21[i];
    if (!std::isfinite(iv21) && schema != VrpPanelSchema::V4) {
      continue; // dropped session (reason counted at load time); bar kept above
    }
    VrpPanelRow row;
    row.bar_index = static_cast<std::int64_t>(i);
    row.date = s.dates[i];
    row.entry_ts_ns = s.ts_ns[i];
    row.spot = s.spot[i];
    row.iv_fair_21d = iv21;
    row.iv_fair_63d = s.iv63[i];

    // Forward leg: bars t+1..t+21 only — session t's close never enters
    // (the off-by-one gate test plants spikes at t and t+22 and requires
    // the label at t to hold still under both).
    double rv_fwd = vrp_detail::nan_d();
    if (i + kVrpHorizonSessions < n) {
      const Result<double> v =
          realized_vol(std::span<const OhlcBar>{bars.data() + i + 1, kVrpHorizonSessions},
                       RvEstimator::CloseToClose, 252.0);
      if (v.has_value()) {
        rv_fwd = *v;
      }
    } else {
      ++counters.n_rows_tail_nan_label; // predict-time row: kept, label NaN
    }
    row.rv_fwd_21d = rv_fwd;
    row.label = std::isfinite(rv_fwd)
                    ? (rv_fwd * rv_fwd - iv21 * iv21) * kVrpTenor21Years
                    : vrp_detail::nan_d();
    row.iv_atmf_21d = s.iv_atmf21[i];
    // v3 liquidity. Both are read straight off the bar axis; the vectors are
    // sized defensively because v1/v2 callers never populate `liq_hspread`.
    row.liq_strikes_fit =
        i < s.liq_strikes_fit.size() ? s.liq_strikes_fit[i] : vrp_detail::nan_d();
    row.liq_hspread_frac = i < s.liq_hspread.size() ? s.liq_hspread[i] : vrp_detail::nan_d();
    if (rv_fwd > kVrpMaxPlausibleRvFwd) { // NaN-safe: a NaN compare is false
      ++counters.n_rv_fwd_implausible;
    }

    const double r1 = (i >= 1) ? std::log(s.spot[i] / s.spot[i - 1]) : vrp_detail::nan_d();
    row.f0_log_rv1 = std::isfinite(r1)
                         ? std::log(std::max(252.0 * r1 * r1, kVrpLogRv1Floor))
                         : vrp_detail::nan_d();
    const double vol5 = vrp_detail::trailing_c2c_vol(bars, i, 5);
    row.f1_log_rv5 = std::log(vol5 * vol5); // NaN propagates through log
    const double vol21 = vrp_detail::trailing_c2c_vol(bars, i, 21);
    row.f2_log_rv21 = std::log(vol21 * vol21);
    row.f3_iv_level = std::log(iv21 * iv21);
    row.f4_term_slope = s.iv63[i] - iv21; // NaN when the 63d strip was missing
    row.f5_hv_iv_gap = std::log(vol21 / iv21);
    row.f6_vrp_lag = iv21 * iv21 - vol21 * vol21;
    row.f7_ret_21d = (i >= 21) ? std::log(s.spot[i] / s.spot[i - 21]) : vrp_detail::nan_d();
    const double mx5 = vrp_detail::max_abs_r_5d(s.spot, i);
    const double vol63 = vrp_detail::trailing_c2c_vol(bars, i, kVrpSigmaWindowSessions);
    const double sigma_daily = vol63 / std::sqrt(252.0);
    row.f8_jump_recent = (std::isnan(mx5) || std::isnan(sigma_daily))
                             ? vrp_detail::nan_d()
                             : ((mx5 > kVrpJumpSigmaMultiple * sigma_daily) ? 1.0 : 0.0);
    row.f9_vov_63d = vrp_detail::vov_63d(s.iv21, i);

    // ── Quarantine mask ───────────────────────────────────────────────────
    //
    // Step j is the return from bar j-1 to bar j. Each column is NaN'd exactly
    // when a contaminated step lies inside ITS OWN window, so an unadjusted
    // split costs the 21 rows before it and the 63 after it, not the symbol.
    // The implied-leg columns (f3, f4, f9) are untouched on purpose: implied
    // vol is scale-invariant, so a share-count change does not move them.
    if (quarantine) {
      const std::ptrdiff_t t = static_cast<std::ptrdiff_t>(i);
      // realized_vol over bars i+1..i+21 consumes the returns BETWEEN them.
      if (spans_bad(t + 2, t + 21)) {
        row.rv_fwd_21d = vrp_detail::nan_d();
        row.label = vrp_detail::nan_d();
        ++counters.n_quarantined_forward;
      }
      bool trailing_hit = false;
      if (spans_bad(t, t)) {
        row.f0_log_rv1 = vrp_detail::nan_d();
        trailing_hit = true;
      }
      if (spans_bad(t - 4, t)) {
        row.f1_log_rv5 = vrp_detail::nan_d();
        trailing_hit = true;
      }
      if (spans_bad(t - 20, t)) { // vol21 feeds f2/f5/f6; f7 spans the same steps
        row.f2_log_rv21 = vrp_detail::nan_d();
        row.f5_hv_iv_gap = vrp_detail::nan_d();
        row.f6_vrp_lag = vrp_detail::nan_d();
        row.f7_ret_21d = vrp_detail::nan_d();
        trailing_hit = true;
      }
      if (spans_bad(t - static_cast<std::ptrdiff_t>(kVrpSigmaWindowSessions) + 1, t)) {
        row.f8_jump_recent = vrp_detail::nan_d(); // its sigma is the 63d window
        trailing_hit = true;
      }
      counters.n_quarantined_trailing += trailing_hit ? 1U : 0U;
    }

    rows.push_back(std::move(row));
    ++counters.n_rows_written;
  }
  return atx::core::Ok(std::move(rows));
}

// ── Multi-root session merge ──────────────────────────────────────────────

struct VrpSessionRef {
  std::string date;    // partition key (ISO date)
  std::size_t db_idx{0}; // which root serves it
};

// Union of every root's partition keys, optionally bounded to the inclusive
// ISO window [lo, hi] (empty = unbounded), sorted ascending. ISO dates sort
// lexicographically == chronologically. A date served by MORE than one root
// is an error — the stitch would be ambiguous (which root's surface wins?),
// and the production yearly roots are disjoint by construction.
[[nodiscard]] inline Result<std::vector<VrpSessionRef>>
merge_vrp_sessions(std::span<const SurfaceDb> dbs, std::string_view lo, std::string_view hi) {
  std::vector<VrpSessionRef> out;
  // Bounded by total partition count across roots.
  for (std::size_t d = 0; d < dbs.size(); ++d) {
    for (const DbPartitionInfo &p : dbs[d].partitions()) {
      if (!lo.empty() && std::string_view{p.key} < lo) {
        continue;
      }
      if (!hi.empty() && std::string_view{p.key} > hi) {
        continue;
      }
      out.push_back(VrpSessionRef{p.key, d});
    }
  }
  std::sort(out.begin(), out.end(), [](const VrpSessionRef &a, const VrpSessionRef &b) {
    return a.date != b.date ? a.date < b.date : a.db_idx < b.db_idx;
  });
  for (std::size_t i = 1; i < out.size(); ++i) {
    if (out[i].date == out[i - 1].date) {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "merge_vrp_sessions: session date '" + out[i].date +
                                "' appears in more than one --db root");
    }
  }
  return atx::core::Ok(std::move(out));
}

// ── Series loading (SurfaceDb -> VrpSeries per symbol) ────────────────────

// Date-major walk (one partition mapping serves every symbol on that date;
// the S5 LRU cache then never thrashes). Per (symbol, session): reconstruct
// the owned PricedSurface — the surface-native var_swap_fair_strike overload
// needs the fitted pillars only PricedSurface carries — read its spot and
// price the 21d/63d strips. NotFound = the symbol is simply absent that
// session (counted, skipped); any OTHER load error is a corrupt corpus and
// fails the run loudly.
[[nodiscard]] inline Result<std::vector<VrpSeries>>
load_vrp_series(std::span<const SurfaceDb> dbs, std::span<const VrpSessionRef> sessions,
                std::span<const std::string> symbols, VrpPanelCounters &counters) {
  std::vector<VrpSeries> series(symbols.size());
  // Bounded by sessions.size() * symbols.size().
  for (const VrpSessionRef &sr : sessions) {
    const SurfaceDb &db = dbs[sr.db_idx];
    for (std::size_t k = 0; k < symbols.size(); ++k) {
      ++counters.n_symbol_sessions;
      const Result<PricedSurface> surf = db.load_surface(sr.date, symbols[k]);
      if (!surf.has_value()) {
        if (surf.error().code() == ErrorCode::NotFound) {
          ++counters.n_no_surface;
          continue;
        }
        return atx::core::Err(surf.error().code(),
                              "load_vrp_series: load_surface('" + sr.date + "', '" + symbols[k] +
                                  "'): " + surf.error().to_string());
      }
      const double S = surf->pricing().S;
      if (!(std::isfinite(S) && S > 0.0)) {
        ++counters.n_bad_spot;
        continue;
      }
      const std::int64_t ts = surf->pricing().now_ts_ns;
      VrpSeries &s = series[k];
      if (!s.ts_ns.empty() && ts <= s.ts_ns.back()) {
        return atx::core::Err(ErrorCode::InvalidArgument,
                              "load_vrp_series: session ts not ascending across stitched roots "
                              "for symbol '" + symbols[k] + "' at date " + sr.date);
      }
      double iv21 = vrp_detail::nan_d();
      double iv63 = vrp_detail::nan_d();
      {
        const Result<DerivQuote> q = var_swap_fair_strike(*surf, kVrpTenor21Years);
        if (q.has_value()) {
          const double k_var = q->fair_strike_dec;
          if (std::isfinite(k_var) && k_var > 0.0) {
            iv21 = std::sqrt(k_var);
          } else {
            ++counters.n_var21_nonfinite;
          }
        } else if (q.error().code() == ErrorCode::OutOfRange) {
          ++counters.n_var21_out_of_range;
        } else {
          ++counters.n_var21_error;
        }
      }
      {
        const Result<DerivQuote> q = var_swap_fair_strike(*surf, kVrpTenor63Years);
        if (q.has_value() && std::isfinite(q->fair_strike_dec) && q->fair_strike_dec > 0.0) {
          iv63 = std::sqrt(q->fair_strike_dec);
        } else {
          ++counters.n_63d_unavailable; // row kept; f4 NaN
        }
      }
      // The tradeable implied leg: the surface's OWN ATM-forward point at the
      // strip tenor — the single (K, T) an AtmForward straddle is struck and
      // marked at. `forward_at` returns 0 for an invalid T and `iv` returns
      // NaN off-domain, so both are checked rather than trusted.
      double iv_atmf21 = vrp_detail::nan_d();
      {
        const double F = surf->forward_at(kVrpTenor21Years);
        if (std::isfinite(F) && F > 0.0) {
          const double sigma = surf->iv(F, kVrpTenor21Years);
          if (std::isfinite(sigma) && sigma > 0.0) {
            iv_atmf21 = sigma;
          }
        }
        if (!std::isfinite(iv_atmf21)) {
          ++counters.n_atmf21_unavailable; // row kept; iv_atmf_21d NaN
        }
      }
      // v3 archive-side breadth proxy. ATXVSA2 persists `n_used` per slice
      // (`col_nused_off`) and `reconstruct_v2` restores it into SliceContext,
      // so this is a read of data already on disk for every surface ever
      // written — no refit, no new dependency. Summed as a double because the
      // panel's emitted columns are all doubles; the counts here are O(100)
      // per slice and O(1e3) per surface, far inside exact f64 integers.
      double strikes_fit = 0.0;
      for (const SliceContext &sc : surf->context()) {
        strikes_fit += static_cast<double>(sc.n_used);
      }
      s.dates.push_back(sr.date);
      s.ts_ns.push_back(ts);
      s.spot.push_back(S);
      s.iv21.push_back(iv21);
      s.iv63.push_back(iv63);
      s.iv_atmf21.push_back(iv_atmf21);
      s.liq_strikes_fit.push_back(strikes_fit);
    }
  }
  return atx::core::Ok(std::move(series));
}

// ── TSV writer ────────────────────────────────────────────────────────────

// Meta header (schema + horizon first — the two frozen comment lines — then
// deterministic run counters ONLY: no paths, no timestamps, no root list, so
// a stitched run and its concatenated-root twin are byte-identical), the
// column header, then rows sorted (symbol, session).
//
// v2 appends to BOTH blocks and rewrites neither, so a v1 file is a byte-exact
// prefix-shaped sibling of the v2 file the same corpus produces. The v1 branch
// touches no v2 state at all: that is what keeps the frozen anchor frozen.
[[nodiscard]] inline Status
write_vrp_panel_tsv(std::string_view path, VrpPanelSchema schema,
                    std::span<const std::string> symbols,
                    std::span<const std::vector<VrpPanelRow>> rows_per_symbol,
                    const VrpPanelCounters &c,
                    VrpImplausiblePolicy on_implausible = VrpImplausiblePolicy::Fail) {
  // v2-and-above / v3-and-above gates. Written as >= comparisons on the
  // enumerator ORDER (V1 < V2 < V3) so adding a v4 prefix extension keeps the
  // v2 block emitting rather than silently dropping it.
  const bool v2 = schema != VrpPanelSchema::V1;
  const bool v3 = schema == VrpPanelSchema::V3 || schema == VrpPanelSchema::V4;
  const bool v4 = schema == VrpPanelSchema::V4;
  std::string out;
  out += "# schema=";
  out += schema_name(schema);
  out += '\n';
  vrp_detail::append_meta_count(out, "horizon_days", kVrpHorizonSessions);
  vrp_detail::append_meta_count(out, "n_symbols", symbols.size());
  vrp_detail::append_meta_count(out, "n_sessions", c.n_sessions);
  vrp_detail::append_meta_count(out, "n_symbol_sessions", c.n_symbol_sessions);
  vrp_detail::append_meta_count(out, "n_no_surface", c.n_no_surface);
  vrp_detail::append_meta_count(out, "n_bad_spot", c.n_bad_spot);
  vrp_detail::append_meta_count(out, "n_var21_out_of_range", c.n_var21_out_of_range);
  vrp_detail::append_meta_count(out, "n_var21_error", c.n_var21_error);
  vrp_detail::append_meta_count(out, "n_var21_nonfinite", c.n_var21_nonfinite);
  vrp_detail::append_meta_count(out, "n_63d_unavailable", c.n_63d_unavailable);
  vrp_detail::append_meta_count(out, "n_rows_tail_nan_label", c.n_rows_tail_nan_label);
  vrp_detail::append_meta_count(out, "n_rows", c.n_rows_written);
  if (v2) {
    vrp_detail::append_meta_count(out, "n_atmf21_unavailable", c.n_atmf21_unavailable);
    vrp_detail::append_meta_count(out, "n_split_events_applied", c.n_split_events_applied);
    vrp_detail::append_meta_count(out, "n_split_symbols_adjusted", c.n_split_symbols_adjusted);
    vrp_detail::append_meta_count(out, "n_rv_fwd_implausible", c.n_rv_fwd_implausible);
  }
  // Quarantine accounting appears ONLY on a quarantined run. A default (Fail)
  // run must stay byte-identical to every v2/v3 file already on disk, and
  // "counters that are always zero" is not a good enough reason to move them.
  if (on_implausible == VrpImplausiblePolicy::Quarantine) {
    vrp_detail::append_meta_count(out, "n_implausible_steps", c.n_implausible_steps);
    vrp_detail::append_meta_count(out, "n_quarantined_forward", c.n_quarantined_forward);
    vrp_detail::append_meta_count(out, "n_quarantined_trailing", c.n_quarantined_trailing);
  }
  if (v3) {
    vrp_detail::append_meta_count(out, "n_liq_ref_matched", c.n_liq_ref_matched);
    vrp_detail::append_meta_count(out, "n_liq_ref_missing", c.n_liq_ref_missing);
  }

  const std::size_t n_col = schema_column_count(schema);
  for (std::size_t i = 0; i < n_col; ++i) {
    if (i > 0) {
      out += '\t';
    }
    out += schema_column(schema, i);
  }
  out += '\n';

  // Bounded by total row count.
  for (std::size_t k = 0; k < rows_per_symbol.size(); ++k) {
    for (const VrpPanelRow &r : rows_per_symbol[k]) {
      vrp_detail::append_sanitized(out, symbols[k]);
      out += '\t';
      vrp_detail::append_sanitized(out, r.date);
      out += '\t';
      vrp_detail::append_i64(out, r.entry_ts_ns);
      const double doubles[] = {r.spot,          r.iv_fair_21d,  r.iv_fair_63d,
                                r.rv_fwd_21d,    r.label,        r.f0_log_rv1,
                                r.f1_log_rv5,    r.f2_log_rv21,  r.f3_iv_level,
                                r.f4_term_slope, r.f5_hv_iv_gap, r.f6_vrp_lag,
                                r.f7_ret_21d,    r.f8_jump_recent, r.f9_vov_63d};
      for (const double v : doubles) {
        out += '\t';
        vrp_detail::append_double(out, v);
      }
      if (v2) {
        out += '\t';
        vrp_detail::append_double(out, r.iv_atmf_21d);
      }
      if (v3) {
        out += '\t';
        vrp_detail::append_double(out, r.liq_hspread_frac);
        out += '\t';
        vrp_detail::append_double(out, r.liq_strikes_fit);
      }
      if (v4) {
        out += '\t';
        vrp_detail::append_i64(out, r.bar_index);
      }
      out += '\n';
    }
  }

  std::ofstream os(std::string(path), std::ios::binary | std::ios::trunc);
  if (!os) {
    return atx::core::Err(ErrorCode::IoError,
                          "write_vrp_panel_tsv: cannot open '" + std::string(path) + "'");
  }
  os.write(out.data(), static_cast<std::streamsize>(out.size()));
  if (!os) {
    return atx::core::Err(ErrorCode::IoError, "write_vrp_panel_tsv: write failed");
  }
  return atx::core::Ok();
}

// ── Top-level runner ──────────────────────────────────────────────────────

// Open every root, merge sessions, load per-symbol stitched series, build
// rows, write the TSV, print the per-reason drop accounting. Errors:
// InvalidArgument (bad config, duplicate session date across roots),
// NotFound (no sessions / no symbols / zero rows), or any loud corpus
// failure from the loader.
[[nodiscard]] inline Result<VrpPanelCounters> run_vrp_panel(const VrpPanelConfig &cfg) {
  if (cfg.db_roots.empty() || cfg.out.empty()) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "run_vrp_panel: at least one --db root and --out are required");
  }
  if (!cfg.entry_start.empty() && !cfg.entry_end.empty() && cfg.entry_end < cfg.entry_start) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "run_vrp_panel: --entry-end precedes --entry-start");
  }
  // v1 is the frozen artifact: it must be reproducible from the corpus alone,
  // so it refuses the one input that would silently move a v1 cell.
  if (cfg.schema == VrpPanelSchema::V1 && !cfg.splits.empty()) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "run_vrp_panel: --splits requires --panel-schema v2 (vrp_panel_v1 is a "
                          "frozen unadjusted contract)");
  }
  // Same rule one schema up: v1 and v2 are both frozen regression anchors, so
  // neither may be handed the v3-only reference file. A v2 run with
  // --liquidity would otherwise LOOK adjusted and emit a byte-identical file,
  // which is the worst of both.
  if (cfg.schema != VrpPanelSchema::V3 && cfg.schema != VrpPanelSchema::V4 &&
      !cfg.liquidity.empty()) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "run_vrp_panel: --liquidity requires --panel-schema v3 or v4 "
                          "(vrp_panel_v1/v2 are frozen contracts with no liquidity column)");
  }
  std::vector<VrpSplitFactor> splits;
  if (!cfg.splits.empty()) {
    ATX_TRY(splits, load_vrp_split_factors(cfg.splits));
  }
  std::vector<VrpLiquidityRef> liq;
  if (!cfg.liquidity.empty()) {
    ATX_TRY(liq, load_vrp_liquidity_ref(cfg.liquidity));
  }
  std::vector<SurfaceDb> dbs;
  dbs.reserve(cfg.db_roots.size());
  // Bounded by the root count.
  for (const std::string &root : cfg.db_roots) {
    Result<SurfaceDb> db = SurfaceDb::open(root);
    if (!db.has_value()) {
      return atx::core::Err(db.error().code(),
                            "run_vrp_panel: cannot open --db '" + root +
                                "': " + db.error().to_string());
    }
    dbs.push_back(std::move(*db));
  }

  VrpPanelCounters counters;
  ATX_TRY(const std::vector<VrpSessionRef> sessions,
          merge_vrp_sessions(dbs, cfg.entry_start, cfg.entry_end));
  counters.n_sessions = sessions.size();
  if (sessions.empty()) {
    return atx::core::Err(ErrorCode::NotFound,
                          "run_vrp_panel: no sessions in the requested window");
  }

  std::vector<std::string> symbols = cfg.symbols;
  if (symbols.empty()) {
    for (const SurfaceDb &db : dbs) {
      const std::vector<std::string> names = db.symbols();
      symbols.insert(symbols.end(), names.begin(), names.end());
    }
  }
  std::sort(symbols.begin(), symbols.end());
  symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
  if (symbols.empty()) {
    return atx::core::Err(ErrorCode::NotFound,
                          "run_vrp_panel: no symbols in any root's manifest; pass --uid");
  }

  ATX_TRY(std::vector<VrpSeries> series, load_vrp_series(dbs, sessions, symbols, counters));

  std::vector<std::vector<VrpPanelRow>> rows_per_symbol(symbols.size());
  // Worst offender, carried only to name it in the plausibility gate's message
  // — a gate that says "56 rows" without saying WHICH costs an investigation.
  double worst_rv = 0.0;
  std::string worst_sym;
  std::string worst_date;
  // Every gate offender, "SYM DATE rv_fwd=X", in (symbol, session) order.
  std::vector<std::string> implausible;
  // Bounded by symbol count.
  for (std::size_t k = 0; k < symbols.size(); ++k) {
    // Split adjustment BEFORE row building: every window the builder opens
    // must already read the adjusted series (`splits` is sorted by (symbol,
    // ex_date), so one equal_range yields this symbol's events in order).
    if (!splits.empty()) {
      const auto lo = std::lower_bound(splits.begin(), splits.end(), symbols[k],
                                       [](const VrpSplitFactor &e, const std::string &sym) {
                                         return e.symbol < sym;
                                       });
      const auto hi = std::upper_bound(lo, splits.end(), symbols[k],
                                       [](const std::string &sym, const VrpSplitFactor &e) {
                                         return sym < e.symbol;
                                       });
      const std::size_t applied = apply_vrp_split_adjustment(
          series[k], std::span<const VrpSplitFactor>{splits.data() + (lo - splits.begin()),
                                                     static_cast<std::size_t>(hi - lo)});
      if (applied > 0) {
        counters.n_split_events_applied += applied;
        ++counters.n_split_symbols_adjusted;
      }
    }
    // Liquidity join, likewise before row building. Under v3 this runs even
    // when no --liquidity file was given, so `liq_hspread` is always sized to
    // the bar axis and the missing-reference count is honest rather than zero
    // by omission.
    if (cfg.schema == VrpPanelSchema::V3) {
      const auto lo = std::lower_bound(liq.begin(), liq.end(), symbols[k],
                                       [](const VrpLiquidityRef &e, const std::string &sym) {
                                         return e.symbol < sym;
                                       });
      const auto hi = std::upper_bound(lo, liq.end(), symbols[k],
                                       [](const std::string &sym, const VrpLiquidityRef &e) {
                                         return sym < e.symbol;
                                       });
      const std::size_t matched = apply_vrp_liquidity_ref(
          series[k], std::span<const VrpLiquidityRef>{liq.data() + (lo - liq.begin()),
                                                      static_cast<std::size_t>(hi - lo)});
      counters.n_liq_ref_matched += matched;
      counters.n_liq_ref_missing += series[k].dates.size() - matched;
    }
    Result<std::vector<VrpPanelRow>> rows =
        build_vrp_rows(series[k], counters, cfg.schema, cfg.on_implausible);
    if (!rows.has_value()) {
      return atx::core::Err(rows.error().code(), "run_vrp_panel: symbol '" + symbols[k] +
                                                     "': " + rows.error().to_string());
    }
    rows_per_symbol[k] = std::move(*rows);
    // Bounded by this symbol's row count.
    for (const VrpPanelRow &r : rows_per_symbol[k]) {
      if (r.rv_fwd_21d > worst_rv) { // NaN-safe: a NaN compare is false
        worst_rv = r.rv_fwd_21d;
        worst_sym = symbols[k];
        worst_date = r.date;
      }
      // Every offender, not only the worst. The gate below reports one name
      // per run, which costs one full rebuild per missing split factor; on a
      // 600-name corpus that is the difference between one iteration and
      // dozens. Bounded by n_rv_fwd_implausible, which a passing run leaves
      // at zero.
      if (r.rv_fwd_21d > kVrpMaxPlausibleRvFwd) { // NaN-safe
        char rv_buf[32];
        const int rv_len = std::snprintf(rv_buf, sizeof rv_buf, "%.6g", r.rv_fwd_21d);
        implausible.push_back(symbols[k] + " " + r.date + " rv_fwd=" +
                              std::string(rv_buf, static_cast<std::size_t>(rv_len > 0 ? rv_len : 0)));
      }
    }
  }
  // PERMANENT DATA-INTEGRITY TIER (v2, and v4 which sees strictly more rows).
  // See kVrpMaxPlausibleRvFwd for the threshold's justification. Loud and
  // unconditional: the only supported fix is to supply the missing reference
  // factor, which is exactly the incentive this gate exists to create.
  // `n_rv_fwd_implausible` still counts under Quarantine — the detection is the
  // honest number and stays in the meta header either way; only the RESPONSE
  // is what the policy selects.
  if (cfg.on_implausible == VrpImplausiblePolicy::Fail &&
      (cfg.schema == VrpPanelSchema::V2 || cfg.schema == VrpPanelSchema::V4) &&
      counters.n_rv_fwd_implausible > 0) {
    // Name EVERY offender on stderr before failing, so one run yields the
    // complete list of missing factors instead of one per rebuild.
    for (const std::string &e : implausible) {
      std::fprintf(stderr, "[vrp_panel] IMPLAUSIBLE %s\n", e.c_str());
    }
    char worst_buf[64];
    const int len = std::snprintf(worst_buf, sizeof worst_buf, "%.6g", worst_rv);
    return atx::core::Err(
        ErrorCode::OutOfRange,
        "run_vrp_panel: " + std::to_string(counters.n_rv_fwd_implausible) +
            " row(s) carry rv_fwd_21d above the plausibility gate " +
            std::to_string(kVrpMaxPlausibleRvFwd) + " (worst " +
            std::string(worst_buf, static_cast<std::size_t>(len > 0 ? len : 0)) + " at " +
            worst_sym + " " + worst_date +
            ") — the spot series carries an unadjusted corporate action; supply its factor via "
            "--splits");
  }
  if (counters.n_rows_written == 0) {
    return atx::core::Err(
        ErrorCode::NotFound,
        "run_vrp_panel: produced zero rows (sessions=" + std::to_string(counters.n_sessions) +
            " symbol_sessions=" + std::to_string(counters.n_symbol_sessions) +
            " no_surface=" + std::to_string(counters.n_no_surface) +
            " bad_spot=" + std::to_string(counters.n_bad_spot) +
            " var21_oor=" + std::to_string(counters.n_var21_out_of_range) +
            " var21_err=" + std::to_string(counters.n_var21_error) +
            " var21_nonfinite=" + std::to_string(counters.n_var21_nonfinite) + ")");
  }

  ATX_TRY_VOID(write_vrp_panel_tsv(cfg.out, cfg.schema, symbols, rows_per_symbol, counters,
                                   cfg.on_implausible));

  // The brief's "per-reason counts printed" — one deterministic line.
  std::printf("[vrp_panel] schema=%.*s sessions=%zu symbol_sessions=%zu no_surface=%zu "
              "bad_spot=%zu var21_oor=%zu var21_err=%zu var21_nonfinite=%zu "
              "slope63_unavailable=%zu atmf21_unavailable=%zu split_events=%zu "
              "split_symbols=%zu tail_nan_label=%zu rows=%zu -> %s\n",
              static_cast<int>(schema_name(cfg.schema).size()), schema_name(cfg.schema).data(),
              counters.n_sessions, counters.n_symbol_sessions, counters.n_no_surface,
              counters.n_bad_spot, counters.n_var21_out_of_range, counters.n_var21_error,
              counters.n_var21_nonfinite, counters.n_63d_unavailable,
              counters.n_atmf21_unavailable, counters.n_split_events_applied,
              counters.n_split_symbols_adjusted, counters.n_rows_tail_nan_label,
              counters.n_rows_written, cfg.out.c_str());
  // A quarantined run must never be mistaken for a clean one, so it says so on
  // its own line with the numbers that make the claim checkable.
  if (cfg.on_implausible == VrpImplausiblePolicy::Quarantine) {
    std::printf("[vrp_panel] QUARANTINE steps=%zu rv_fwd_implausible=%zu "
                "forward_nan=%zu trailing_nan=%zu\n",
                counters.n_implausible_steps, counters.n_rv_fwd_implausible,
                counters.n_quarantined_forward, counters.n_quarantined_trailing);
  }
  return atx::core::Ok(counters);
}

} // namespace atx::vol
