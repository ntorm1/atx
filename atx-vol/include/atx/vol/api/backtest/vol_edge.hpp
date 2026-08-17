#pragma once

// Vol-edge portfolio backtest: ranked long/short vega-normalized straddle book
// driven by an upstream cross-sectional VRP signal (2026-08-15 vrp-ml sprint,
// lane vrp-book; digest: .superpowers/sdd/2026-08-15-vrp-ml/research-vrp-portfolio.md).
//
// TIER-B, HEADER-ONLY, AND DELIBERATELY OUTSIDE THE `vol.hpp` UMBRELLA. This
// header is public direct-include (the derivatives/lakehouse precedent): it
// adds no TU, no CMake edit, and no umbrella-manifest change. Everything it
// needs is already reachable from the Tier-A backtest headers it includes.
//
// THREE PIECES, EACH TESTABLE ON ITS OWN:
//
//   1. The FROZEN `vrp_signal_v1` TSV contract + fail-closed loader. The model
//      lane (vrp-model) produces the file; this lane only consumes it. Any
//      deviation from the frozen schema is an error, never a best-effort read.
//   2. `VolEdgeConfig` + `build_vol_edge_book`: the PURE cross-sectional
//      ranking/sizing rule (deciles, vega-normalization by vol-of-vol, caps,
//      no-trade band, optional net-short tilt). No surface, no engine — a
//      deterministic function of one day's signal rows.
//   3. `VolEdgeStrategy` (an `IStrategy`): expresses the target book in
//      delta-hedged ATM-forward straddles (~21d tenor) against the EXISTING
//      backtest engine (`run_backtest`'s strategy overload), rolling the whole
//      book each rebalance tick. Costs ride the engine's own `FrictionModel`
//      (`vol_edge_frictions` maps the config's half-spreads onto it), so the
//      charge lands in `BacktestResult::cost` exactly like every other run.
//
// ROUND-1 LIMITATIONS (out of scope by the lane brief, stated rather than
// silent): straddles only (no strangle ladders / strips), per-name caps + an
// optional net tilt only (no sector/beta vega-neutrality solver), no earnings
// masking beyond what the upstream signal already encodes, no margin/Kelly
// sizing. A rebalance date MISSING from the signal file keeps the previous
// book unchanged (fail-soft hold, counted in `n_steps_entry_skipped` and
// `held_steps`).
//
// EXPIRY SAFETY (round-2 hardening, experiments F3/F4): a lot carried past
// its expiry without an exact-expiry snapshot observation is a run-ending
// engine error by the engine's own contract, and round 1 could reach it two
// ways — the 21/21 default pair left < 1.1 calendar days of margin (a
// two-holiday December window crossed it), and the fail-soft hold above
// could carry a book through a mid-run signal gap until it expired. The
// strategy now roll-closes the WHOLE book at marks on any step where a lot
// sits within `expiry_guard_days` calendar days of its expiry (counted in
// `guard_roll_closes`), and the shipped defaults keep a worst-case margin
// above the guard so the guard never fires on a normal US-shaped calendar.
//
// Thread-safety: the free functions are pure. `VolEdgeStrategy` follows the
// `IStrategy` rule verbatim — one instance, one engine loop, one thread.

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/api/backtest/backtest.hpp" // MarketSnapshot, Lot, PortfolioState
#include "atx/vol/api/backtest/strategy.hpp" // IStrategy, LegSpec, resolve_spec
#include "atx/vol/api/core/types.hpp"        // Result, Status, ErrorCode

namespace atx::vol {

// ── 1. The FROZEN vrp_signal_v1 contract ────────────────────────────────────
//
// Line 1: exactly `# schema=vrp_signal_v1`.
// Line 2: exactly the tab-separated header below (this order, nothing else).
// Rows:   exactly five tab-separated fields per row. `symbol` and `date`
//         non-empty; the three numeric fields finite doubles; `vov_63d >= 0`.
// Fully-empty lines are ignored (a trailing newline is not a schema breach);
// everything else fails closed with InvalidArgument naming the first bad line.

inline constexpr std::string_view kVrpSignalSchemaLineV1 = "# schema=vrp_signal_v1";
inline constexpr std::string_view kVrpSignalHeaderV1 =
    "symbol\tdate\tpred_label\tpred_edge_norm\tvov_63d";

struct VrpSignalRow {
  std::string symbol;
  std::string date; // "YYYY-MM-DD" (UTC session date; see vol_edge_session_date)
  double pred_label{0.0};
  double pred_edge_norm{0.0};
  double vov_63d{0.0};
};

namespace vol_edge_detail {

[[nodiscard]] inline bool parse_finite_double(std::string_view text, double &out) noexcept {
  if (text.empty()) {
    return false;
  }
  const char *first = text.data();
  const char *last = text.data() + text.size();
  const std::from_chars_result r = std::from_chars(first, last, out);
  return r.ec == std::errc{} && r.ptr == last && std::isfinite(out);
}

// Split one row into exactly `n` tab-separated fields. False on any other count.
template <std::size_t N>
[[nodiscard]] inline bool split_fields(std::string_view line,
                                       std::array<std::string_view, N> &out) noexcept {
  std::size_t field = 0;
  std::size_t first = 0;
  while (true) {
    const std::size_t tab = line.find('\t', first);
    const std::string_view piece =
        tab == std::string_view::npos ? line.substr(first) : line.substr(first, tab - first);
    if (field >= N) {
      return false;
    }
    out[field++] = piece;
    if (tab == std::string_view::npos) {
      break;
    }
    first = tab + 1;
  }
  return field == N;
}

} // namespace vol_edge_detail

// Parse the frozen vrp_signal_v1 TSV from an in-memory buffer. Fail-closed:
// the schema comment line, the exact header, the field count, numeric
// finiteness, and `vov_63d >= 0` are all hard requirements; the error message
// carries the 1-based line number of the first violation.
[[nodiscard]] inline Result<std::vector<VrpSignalRow>> parse_vrp_signal_v1(std::string_view text) {
  std::vector<VrpSignalRow> rows;
  std::size_t line_no = 0;
  bool saw_schema = false;
  bool saw_header = false;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t nl = text.find('\n', pos);
    std::string_view line =
        nl == std::string_view::npos ? text.substr(pos) : text.substr(pos, nl - pos);
    pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
    ++line_no;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1); // tolerate CRLF; the contract is about content
    }
    if (line.empty()) {
      continue; // blank line carries no data either way
    }
    if (!saw_schema) {
      if (line != kVrpSignalSchemaLineV1) {
        return atx::core::Err(ErrorCode::InvalidArgument,
                              "vrp_signal_v1: line " + std::to_string(line_no) +
                                  " is not the frozen schema line '" +
                                  std::string(kVrpSignalSchemaLineV1) + "'");
      }
      saw_schema = true;
      continue;
    }
    if (!saw_header) {
      if (line != kVrpSignalHeaderV1) {
        return atx::core::Err(ErrorCode::InvalidArgument,
                              "vrp_signal_v1: line " + std::to_string(line_no) +
                                  " is not the frozen v1 header (columns must be exactly "
                                  "symbol, date, pred_label, pred_edge_norm, vov_63d)");
      }
      saw_header = true;
      continue;
    }
    std::array<std::string_view, 5> f{};
    if (!vol_edge_detail::split_fields(line, f)) {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "vrp_signal_v1: line " + std::to_string(line_no) +
                                " does not have exactly 5 tab-separated fields");
    }
    VrpSignalRow row;
    row.symbol.assign(f[0]);
    row.date.assign(f[1]);
    const bool numeric_ok = vol_edge_detail::parse_finite_double(f[2], row.pred_label) &&
                            vol_edge_detail::parse_finite_double(f[3], row.pred_edge_norm) &&
                            vol_edge_detail::parse_finite_double(f[4], row.vov_63d);
    if (row.symbol.empty() || row.date.empty() || !numeric_ok || row.vov_63d < 0.0) {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "vrp_signal_v1: line " + std::to_string(line_no) +
                                " has an empty key or a non-finite/negative numeric field");
    }
    rows.push_back(std::move(row));
  }
  if (!saw_schema || !saw_header) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "vrp_signal_v1: file is missing the schema line and/or header");
  }
  return atx::core::Ok(std::move(rows));
}

// Load + parse a vrp_signal_v1 file. NotFound when the file cannot be opened;
// parse failures propagate from `parse_vrp_signal_v1` verbatim.
[[nodiscard]] inline Result<std::vector<VrpSignalRow>> load_vrp_signal_v1(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return atx::core::Err(ErrorCode::NotFound, "vrp_signal_v1: cannot open '" + path + "'");
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  if (in.bad()) {
    return atx::core::Err(ErrorCode::Unavailable, "vrp_signal_v1: read failed for '" + path + "'");
  }
  const std::string text = buffer.str();
  return parse_vrp_signal_v1(text);
}

// ── 2. Config + the pure ranking/sizing rule ────────────────────────────────

// Designated-initializer config. Defaults express the digest's phase-1 book:
// ~21 trading-day horizon straddles, 10% deciles, vega budget normalized by
// vol-of-vol with a floor, everything optional off.
struct VolEdgeConfig {
  // Straddle tenor in TRADING days; the leg tenor is horizon_days / 252 years.
  double horizon_days{21.0};
  // Roll cadence in clock STEPS (sessions), kept BELOW the tenor's calendar
  // span by a real margin: 15 sessions span at most ceil(15 * 7/5) + 2 = 23
  // calendar days on a weekend calendar with a two-holiday cluster, vs the
  // default tenor's 21/252 * 365.25 = 30.4375 calendar days — a worst-case
  // margin of ~7.4 days, above `expiry_guard_days`, so the fail-safe below
  // never fires on a normal calendar. (The round-1 default of 21 left < 1.1
  // calendar days and a December two-holiday window crossed the synthetic
  // expiry between sessions: experiment F4.) 1 = re-rank every session.
  unsigned rebalance_every_n_steps{15};
  // Decile fractions of the day's usable cross-section (long = top, short =
  // bottom by pred_edge_norm). At least one name per side once >= 2 usable
  // names exist; a day where the two selections would overlap trades nothing.
  double long_fraction{0.10};
  double short_fraction{0.10};
  // Per-name risk budget in the engine's TargetVega dollars: the target book
  // vega magnitude for a name with vov_63d == 1. Sized per name as
  //   vega_target = sign * risk_budget_vega / max(vov_63d, vov_floor).
  double risk_budget_vega{1.0e4};
  double vov_floor{0.05};
  // Hard per-name |vega| cap AFTER the tilt below. 0 = uncapped.
  double per_name_vega_cap{0.0};
  // Optional net-short tilt: every SHORT target is scaled by (1 + tilt), so a
  // book whose untilted sides are vega-equal carries net vega of exactly
  // -tilt * gross_long_vega. 0 = symmetric book.
  double net_short_tilt{0.0};
  // No-trade band on the SIGNAL: a selected name with |pred_edge_norm| <= band
  // is suppressed (no position) rather than traded at noise-level edge.
  double no_trade_band{0.0};
  // QUOTED option half-spread in VOL POINTS (1.0 = one vol point of IV).
  // Mapped onto FrictionModel::vol_tick by `vol_edge_frictions`, so the engine
  // charges half-spread = vega * (vol_pts * crossing / 100) per share, per leg,
  // per fill.
  //
  // ROUND-6 SEMANTIC CORRECTION. This used to be documented as an EFFECTIVE
  // half-spread and was charged with no crossing factor -- the run crossed
  // 100% of it while the repo's own ORATS calibration
  // (`FrictionModel::crossing_fraction_complex = 0.53`) sat unreachable under
  // `SpreadKind::VolTicks` (audit-cost-model.md defect #2). The field is now a
  // QUOTED width and `cost_crossing_fraction` below is the discount, so both
  // halves of the correction are visible together. THEY DO NOT CANCEL AND THEY
  // DO NOT POINT THE SAME WAY: a crossing below 1.00 makes the charge cheaper,
  // but the 0.5 vol-point width this book ran with was already ~1.9x too cheap
  // as an EFFECTIVE charge (Christoffersen et al., RFS 2018, measure ~0.96
  // one-way vol points on an S&P 500 ATM name at sigma = 30%). Applying 0.53 to
  // 0.5 without restating the width is the flattering half of the correction,
  // and is what this comment exists to prevent.
  //
  // KNOWN LIMITATION, stated rather than buried: this charge is a FLAT number
  // of vol points, but option cost in vol points is (fraction of premium) x
  // sigma, so it scales with the name's own IV. A flat width undercharges
  // high-vol names and overcharges low-vol ones. The vega book in
  // tools/vrp_train.hpp charges the premium-fraction form directly.
  double cost_half_spread_vol_pts{0.0};
  // Fraction of the quoted half-spread above that a fill actually crosses.
  // 1.00 reproduces the pre-round-6 charge exactly for a given width. The
  // default is Zhan-Han-Cao-Tong (RFS 2022) realized effective/quoted, measured
  // on actual OPRA prints 2003-2016; the ORATS complex-order constant 0.53 for
  // a two-leg order is within 4% of it. Must lie in (0, 1] -- "no option cost"
  // is expressed by leaving `cost_half_spread_vol_pts` at 0, never by a zero
  // crossing fraction, which would silently make every fill free.
  double cost_crossing_fraction{0.55};
  // Stock half-spread for the delta-hedge overlay, in bps of spot per share
  // traded (FrictionModel::hedge_slippage_bps verbatim).
  double stock_half_spread_bps{0.0};
  // Engine-owned daily delta hedge (HedgeSpec::DeltaToZero) and its band.
  bool delta_hedge{true};
  double delta_hedge_band{0.0};
  // FAIL-SAFE ROLL-CLOSE MARGIN in CALENDAR days (F3/F4 hardening). On EVERY
  // step — held, off-tick, or rebalancing — a lot within this margin of its
  // synthetic expiry roll-closes the WHOLE book at marks, so neither a
  // missing-signal hold nor a holiday-stretched cadence can carry a lot past
  // an expiry the clock never observes exactly (a run-ending engine error).
  // Must be at least the LONGEST inter-session calendar gap the corpus can
  // produce (5.0 covers US calendars, whose worst gap — a holiday abutting a
  // weekend — is 4 days) and strictly below the tenor's calendar days, or no
  // book could ever be held at all.
  double expiry_guard_days{5.0};
  // ── Round-3 cost-aware selective construction (lane vrp-selective-book) ──
  // ALL defaults keep every knob OFF: the shipped default book is bit-for-bit
  // the round-2 book. Research grounding: research-vrp-costs.md (Garleanu-
  // Pedersen no-trade region [1], Novy-Marx–Velikov sS buy/hold band [4],
  // Goyal-Saretto hold-to-expiration cost convention [7]).
  //
  // HOLD-TO-HORIZON: a rebalance tick trades the DIFF against the held book —
  // names still selected are KEPT (no close/reopen churn, so the entry spread
  // is paid once per holding period), departed names close, fresh names open.
  // The expiry guard becomes a PER-NAME ROLL: an expiring name closes and
  // immediately re-enters at its held vega target (fresh tenor, no off-cadence
  // re-rank) instead of flattening the whole book. false = round-2 semantics
  // (full close + reopen every tick, whole-book guard close).
  bool hold_to_horizon{false};
  // sS ADMISSION HYSTERESIS (buy/hold band): enter a side only inside its
  // long/short_fraction entry band; EXIT a held name only when it leaves this
  // WIDER band (suggested 0.10 entry / 0.20 exit — the 10%/20% rule of [4]).
  // 0 = that side's exit band equals its entry band. > 0 requires
  // hold_to_horizon (the band adjudicates a persistent book) and must lie in
  // [entry fraction, 0.5]. Held names are exempt from no_trade_band and the
  // cost gate below: admission rules gate ENTRIES, the band alone gates exits.
  double exit_long_fraction{0.0};
  double exit_short_fraction{0.0};
  // COST-GATED ADMISSION, per unit of position vega (the vega cancels, so the
  // gate is a pure per-name predicate on pred_label): admit an entry only if
  //   |pred_label| * (252 / horizon_days) / (2 * cost_gate_ref_vol)
  //     > cost_gate_k * (2 * cost_half_spread_vol_pts * cost_crossing_fraction
  //                        / 100
  //                      + cost_gate_hedge_per_vega).
  // ROUND 6: the crossing fraction enters the ADMISSION predicate too, so the
  // gate and the CHARGE cannot drift apart -- a gate that admits at a cost the
  // engine does not charge is worse than no gate at all.
  // The left side maps the signal's total-variance-unit label to an expected
  // vol-point move through a REFERENCE vol level — a crude pre-recalibration
  // conversion, deliberately: currency-correct labels await the recalibration
  // lane; the MECHANISM ships now. The right side decomposes the fixture's
  // round-trip cost into option entry+exit spread and a configurable hedge
  // component. 0 = gate off (the default; round-2 semantics).
  double cost_gate_k{0.0};
  double cost_gate_ref_vol{0.25};
  double cost_gate_hedge_per_vega{0.0};
};

// Field-count drift pin: a new knob must be appended AND documented above.
static_assert(detail::aggregate_arity_is_v<VolEdgeConfig, 21>,
              "VolEdgeConfig field count changed: update this pin and the doc block.");

[[nodiscard]] inline Status validate_vol_edge_config(const VolEdgeConfig &cfg) {
  const auto bad = [](const char *what) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          std::string("VolEdgeConfig: ") + what);
  };
  if (!(std::isfinite(cfg.horizon_days) && cfg.horizon_days > 0.0)) {
    return bad("horizon_days must be finite and > 0");
  }
  if (cfg.rebalance_every_n_steps == 0u) {
    return bad("rebalance_every_n_steps must be >= 1");
  }
  if (!(std::isfinite(cfg.long_fraction) && cfg.long_fraction > 0.0 &&
        cfg.long_fraction <= 0.5) ||
      !(std::isfinite(cfg.short_fraction) && cfg.short_fraction > 0.0 &&
        cfg.short_fraction <= 0.5)) {
    return bad("long/short fractions must be in (0, 0.5]");
  }
  if (!(std::isfinite(cfg.risk_budget_vega) && cfg.risk_budget_vega > 0.0)) {
    return bad("risk_budget_vega must be finite and > 0");
  }
  if (!(std::isfinite(cfg.vov_floor) && cfg.vov_floor > 0.0)) {
    return bad("vov_floor must be finite and > 0");
  }
  if (!(std::isfinite(cfg.per_name_vega_cap) && cfg.per_name_vega_cap >= 0.0)) {
    return bad("per_name_vega_cap must be finite and >= 0 (0 = uncapped)");
  }
  if (!(std::isfinite(cfg.net_short_tilt) && cfg.net_short_tilt >= 0.0)) {
    return bad("net_short_tilt must be finite and >= 0");
  }
  if (!(std::isfinite(cfg.no_trade_band) && cfg.no_trade_band >= 0.0)) {
    return bad("no_trade_band must be finite and >= 0");
  }
  if (!(std::isfinite(cfg.cost_half_spread_vol_pts) && cfg.cost_half_spread_vol_pts >= 0.0) ||
      !(std::isfinite(cfg.stock_half_spread_bps) && cfg.stock_half_spread_bps >= 0.0)) {
    return bad("cost half-spreads must be finite and >= 0");
  }
  // (0, 1] and not [0, 1]: a zero crossing fraction reads as a calibration and
  // behaves as "every option fill is free", which is the single most flattering
  // silent error this cost model can make.
  if (!(std::isfinite(cfg.cost_crossing_fraction) && cfg.cost_crossing_fraction > 0.0 &&
        cfg.cost_crossing_fraction <= 1.0)) {
    return bad("cost_crossing_fraction must be finite and in (0, 1]");
  }
  if (!(std::isfinite(cfg.delta_hedge_band) && cfg.delta_hedge_band >= 0.0)) {
    return bad("delta_hedge_band must be finite and >= 0");
  }
  // Negative-margin guard/tenor pairs are rejected outright: a guard at or
  // above the tenor's calendar span would roll-close every entry on the very
  // step it opened, i.e. the config can never hold a book.
  const double tenor_calendar_days = cfg.horizon_days * (365.25 / 252.0);
  if (!(std::isfinite(cfg.expiry_guard_days) && cfg.expiry_guard_days > 0.0 &&
        cfg.expiry_guard_days < tenor_calendar_days)) {
    return bad("expiry_guard_days must be finite, > 0, and below the tenor's calendar days");
  }
  // Round-3 selective knobs. An exit band NARROWER than its entry band is not
  // a hysteresis band; an exit band without a persistent book is meaningless,
  // so it fails closed here rather than being silently ignored.
  const auto exit_band_ok = [](double exit_fraction, double entry_fraction) noexcept {
    return std::isfinite(exit_fraction) &&
           (exit_fraction == 0.0 ||
            (exit_fraction >= entry_fraction && exit_fraction <= 0.5));
  };
  if (!exit_band_ok(cfg.exit_long_fraction, cfg.long_fraction) ||
      !exit_band_ok(cfg.exit_short_fraction, cfg.short_fraction)) {
    return bad("exit_*_fraction must be 0 (off) or in [entry fraction, 0.5]");
  }
  if ((cfg.exit_long_fraction > 0.0 || cfg.exit_short_fraction > 0.0) &&
      !cfg.hold_to_horizon) {
    return bad("exit bands (sS hysteresis) require hold_to_horizon");
  }
  if (!(std::isfinite(cfg.cost_gate_k) && cfg.cost_gate_k >= 0.0)) {
    return bad("cost_gate_k must be finite and >= 0 (0 = gate off)");
  }
  if (!(std::isfinite(cfg.cost_gate_ref_vol) && cfg.cost_gate_ref_vol > 0.0)) {
    return bad("cost_gate_ref_vol must be finite and > 0");
  }
  if (!(std::isfinite(cfg.cost_gate_hedge_per_vega) && cfg.cost_gate_hedge_per_vega >= 0.0)) {
    return bad("cost_gate_hedge_per_vega must be finite and >= 0");
  }
  return atx::core::Ok();
}

// COST-GATED ADMISSION predicate (round 3). Both sides of the comparison are
// per unit of position vega — the position size cancels — so the gate reduces
// to a deterministic per-name test on the signal's pred_label. See the config
// doc block for the exact formula and the pre-recalibration caveat. Pure;
// callers reach it only with a validated config.
[[nodiscard]] inline bool vol_edge_cost_gate_admits(const VolEdgeConfig &cfg,
                                                    double pred_label) noexcept {
  if (cfg.cost_gate_k <= 0.0) {
    return true; // gate off (the default)
  }
  const double edge_vol_per_vega =
      std::fabs(pred_label) * (252.0 / cfg.horizon_days) / (2.0 * cfg.cost_gate_ref_vol);
  const double round_trip_per_vega =
      2.0 * (cfg.cost_half_spread_vol_pts * cfg.cost_crossing_fraction / 100.0) +
      cfg.cost_gate_hedge_per_vega;
  return edge_vol_per_vega > cfg.cost_gate_k * round_trip_per_vega;
}

// The per-name vega-target magnitude rule, stated once: budget over floored
// vol-of-vol, sign applied by the caller. Pure; NaN-safe via the validation
// above (callers reach this only with a validated config and finite vov).
[[nodiscard]] inline double vol_edge_vega_target(const VolEdgeConfig &cfg, double sign,
                                                 double vov_63d) noexcept {
  return sign * cfg.risk_budget_vega / std::max(vov_63d, cfg.vov_floor);
}

// One name of the target book: signed vega target in TargetVega dollars.
struct VolEdgeTarget {
  std::string symbol;
  double pred_edge_norm{0.0};
  double vega_target{0.0}; // > 0 long vol, < 0 short vol
};

// Rank ONE day's cross-section and size the long/short straddle book.
//
// Deterministic: rows are ordered by (pred_edge_norm DESC, symbol ASC), longs
// are the top `long_fraction` (at least one name), shorts the bottom
// `short_fraction` (at least one name). Emitted longs-first then shorts, each
// side in rank order. Rows with a non-finite edge or vov are dropped before
// ranking. Fewer than two usable rows, or overlapping selections, yield an
// EMPTY book (a no-trade day), never an error. Per name:
//
//   target = vol_edge_vega_target(cfg, sign, vov);
//   shorts *= (1 + net_short_tilt);
//   |target| capped at per_name_vega_cap (cap > 0), tilt included — the cap
//   is a hard risk limit, so it binds LAST;
//   |pred_edge_norm| <= no_trade_band suppresses the name entirely.
[[nodiscard]] inline Result<std::vector<VolEdgeTarget>>
build_vol_edge_book(std::span<const VrpSignalRow> day, const VolEdgeConfig &cfg) {
  ATX_TRY_VOID(validate_vol_edge_config(cfg));
  std::vector<const VrpSignalRow *> usable;
  usable.reserve(day.size());
  for (const VrpSignalRow &row : day) {
    if (std::isfinite(row.pred_edge_norm) && std::isfinite(row.vov_63d) && row.vov_63d >= 0.0 &&
        !row.symbol.empty()) {
      usable.push_back(&row);
    }
  }
  std::vector<VolEdgeTarget> book;
  if (usable.size() < 2u) {
    return atx::core::Ok(std::move(book)); // nothing to rank against
  }
  std::sort(usable.begin(), usable.end(),
            [](const VrpSignalRow *a, const VrpSignalRow *b) noexcept {
              if (a->pred_edge_norm != b->pred_edge_norm) {
                return a->pred_edge_norm > b->pred_edge_norm;
              }
              return a->symbol < b->symbol; // deterministic tie-break
            });
  const std::size_t n = usable.size();
  const auto side_count = [n](double fraction) noexcept {
    const double raw = std::floor(static_cast<double>(n) * fraction);
    return std::max<std::size_t>(1u, static_cast<std::size_t>(raw));
  };
  const std::size_t n_long = side_count(cfg.long_fraction);
  const std::size_t n_short = side_count(cfg.short_fraction);
  if (n_long + n_short > n) {
    return atx::core::Ok(std::move(book)); // sides would overlap: no-trade day
  }
  const auto emit = [&](const VrpSignalRow &row, double sign) {
    if (std::fabs(row.pred_edge_norm) <= cfg.no_trade_band) {
      return; // inside the no-trade band: suppressed, not resized
    }
    if (!vol_edge_cost_gate_admits(cfg, row.pred_label)) {
      return; // round 3: edge does not clear k x round-trip cost (k=0 never lands here)
    }
    double target = vol_edge_vega_target(cfg, sign, row.vov_63d);
    if (sign < 0.0) {
      target *= (1.0 + cfg.net_short_tilt);
    }
    if (cfg.per_name_vega_cap > 0.0) {
      target = std::clamp(target, -cfg.per_name_vega_cap, cfg.per_name_vega_cap);
    }
    book.push_back(VolEdgeTarget{row.symbol, row.pred_edge_norm, target});
  };
  book.reserve(n_long + n_short);
  for (std::size_t i = 0; i < n_long; ++i) {
    emit(*usable[i], +1.0);
  }
  for (std::size_t i = n - n_short; i < n; ++i) {
    emit(*usable[i], -1.0);
  }
  return atx::core::Ok(std::move(book));
}

// ── 2b. Round-3 selective plan: entries / keeps / exits over a held book ────

// One currently-held name, as the planner needs it: the symbol and which side
// it is held on. Symbols must be unique across the span (the strategy's held
// book is keyed by symbol).
struct VolEdgeHeldName {
  std::string symbol;
  double sign{0.0}; // > 0 held long vol, < 0 held short vol
};

// The tick-level trading plan for a PERSISTENT (hold-to-horizon) book.
// `entries` are sized exactly as `build_vol_edge_book` sizes a fresh name;
// `keeps` are held as-is (no resize — resizing would trade; 1/N-at-entry per
// the digest's estimation-error argument); `exits` close at marks.
struct VolEdgeBookPlan {
  std::vector<VolEdgeTarget> entries;
  std::vector<VolEdgeHeldName> keeps;
  std::vector<std::string> exits;
  // False when the day cannot be ranked (< 2 usable rows, or the exit bands
  // would overlap): the sS rule has no evidence a held name left its band, so
  // EVERYTHING held is kept and nothing enters or exits.
  bool rankable{true};
  // Entry candidates suppressed by the cost gate this tick (attribution).
  std::size_t n_cost_gated{0};
};

// Rank one day's cross-section against the currently-held book and emit the
// sS-banded, cost-gated trading plan (round 3; research-vrp-costs.md [4]).
//
// Deterministic: usable rows are ordered by (pred_edge_norm DESC, symbol ASC)
// exactly as `build_vol_edge_book`; entries are emitted longs-first then
// shorts in rank order; keeps/exits follow the input order of `held`. Rules:
//   * ENTRY band per side = long/short_fraction (>= 1 name); a held name in
//     its own side's entry band is a KEEP, never a re-entry.
//   * HOLD band per side = exit_*_fraction when > 0, else the entry band. A
//     held name is kept while it ranks inside its side's hold band; leaving
//     the band, vanishing from the day's usable rows, or flipping sides is an
//     exit (a flipped name may simultaneously re-enter the other side).
//   * Entries (only) pass no_trade_band and the cost gate; sizing, tilt and
//     cap match `build_vol_edge_book` exactly.
//   * Duplicate symbols in `day` rank by their best row (first in sort order).
[[nodiscard]] inline Result<VolEdgeBookPlan>
plan_vol_edge_book(std::span<const VrpSignalRow> day, const VolEdgeConfig &cfg,
                   std::span<const VolEdgeHeldName> held) {
  ATX_TRY_VOID(validate_vol_edge_config(cfg));
  std::vector<const VrpSignalRow *> usable;
  usable.reserve(day.size());
  for (const VrpSignalRow &row : day) {
    if (std::isfinite(row.pred_edge_norm) && std::isfinite(row.vov_63d) && row.vov_63d >= 0.0 &&
        !row.symbol.empty()) {
      usable.push_back(&row);
    }
  }
  VolEdgeBookPlan plan;
  const auto hold_everything = [&plan, held]() {
    plan.rankable = false;
    plan.keeps.assign(held.begin(), held.end());
  };
  if (usable.size() < 2u) {
    hold_everything();
    return atx::core::Ok(std::move(plan));
  }
  std::sort(usable.begin(), usable.end(),
            [](const VrpSignalRow *a, const VrpSignalRow *b) noexcept {
              if (a->pred_edge_norm != b->pred_edge_norm) {
                return a->pred_edge_norm > b->pred_edge_norm;
              }
              return a->symbol < b->symbol; // deterministic tie-break
            });
  const std::size_t n = usable.size();
  const auto side_count = [n](double fraction) noexcept {
    const double raw = std::floor(static_cast<double>(n) * fraction);
    return std::max<std::size_t>(1u, static_cast<std::size_t>(raw));
  };
  const std::size_t n_entry_long = side_count(cfg.long_fraction);
  const std::size_t n_entry_short = side_count(cfg.short_fraction);
  const std::size_t n_hold_long =
      side_count(cfg.exit_long_fraction > 0.0 ? cfg.exit_long_fraction : cfg.long_fraction);
  const std::size_t n_hold_short =
      side_count(cfg.exit_short_fraction > 0.0 ? cfg.exit_short_fraction : cfg.short_fraction);
  // Hold bands contain their entry bands (validated), so a hold-band overlap
  // check covers the entry bands too. Overlapping bands cannot adjudicate.
  if (n_hold_long + n_hold_short > n) {
    hold_everything();
    return atx::core::Ok(std::move(plan));
  }
  // Best (first-in-sort-order) rank per symbol.
  std::map<std::string_view, std::size_t> rank_of;
  for (std::size_t i = 0; i < n; ++i) {
    rank_of.try_emplace(usable[i]->symbol, i);
  }
  // Held adjudication: keep inside the side's hold band, exit otherwise.
  std::vector<std::string_view> kept; // suppresses same-name re-entry below
  kept.reserve(held.size());
  for (const VolEdgeHeldName &name : held) {
    const auto rank = rank_of.find(name.symbol);
    const bool keep =
        rank != rank_of.end() && (name.sign > 0.0 ? rank->second < n_hold_long
                                                  : rank->second >= n - n_hold_short);
    if (keep) {
      plan.keeps.push_back(name);
      kept.push_back(name.symbol);
    } else {
      plan.exits.push_back(name.symbol);
    }
  }
  const auto emit_entry = [&](const VrpSignalRow &row, double sign) {
    if (std::find(kept.begin(), kept.end(), std::string_view{row.symbol}) != kept.end()) {
      return; // already held on this side (bands are disjoint): a keep, not an entry
    }
    if (std::fabs(row.pred_edge_norm) <= cfg.no_trade_band) {
      return; // inside the no-trade band: suppressed, not resized
    }
    if (!vol_edge_cost_gate_admits(cfg, row.pred_label)) {
      ++plan.n_cost_gated;
      return;
    }
    double target = vol_edge_vega_target(cfg, sign, row.vov_63d);
    if (sign < 0.0) {
      target *= (1.0 + cfg.net_short_tilt);
    }
    if (cfg.per_name_vega_cap > 0.0) {
      target = std::clamp(target, -cfg.per_name_vega_cap, cfg.per_name_vega_cap);
    }
    plan.entries.push_back(VolEdgeTarget{row.symbol, row.pred_edge_norm, target});
  };
  plan.entries.reserve(n_entry_long + n_entry_short);
  for (std::size_t i = 0; i < n_entry_long; ++i) {
    emit_entry(*usable[i], +1.0);
  }
  for (std::size_t i = n - n_entry_short; i < n; ++i) {
    emit_entry(*usable[i], -1.0);
  }
  return atx::core::Ok(std::move(plan));
}

// ── 3. Engine adapters ──────────────────────────────────────────────────────

// UTC calendar date ("YYYY-MM-DD") of a snapshot timestamp — the join key
// between `MarketSnapshot::ts_ns()` and `VrpSignalRow::date`. The stored
// corpora snapshot at 15:55 ET, which is the same UTC calendar date, so a UTC
// day is the correct session key for every archive this engine replays.
// (Howard Hinnant's civil-from-days; exact over the whole int64 ns range.)
[[nodiscard]] inline std::string vol_edge_session_date(std::int64_t ts_ns) {
  constexpr std::int64_t kDay = 86'400'000'000'000LL;
  std::int64_t z = ts_ns / kDay - ((ts_ns % kDay) < 0 ? 1 : 0);
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::uint32_t doe = static_cast<std::uint32_t>(z - era * 146097);
  const std::uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const std::uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::uint32_t mp = (5 * doy + 2) / 153;
  const std::uint32_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u",
                static_cast<long long>(y + (m <= 2 ? 1 : 0)), m, d);
  return std::string{buf};
}

// Map the config's cost knobs onto the engine's OWN friction model, so the
// charge flows through `BacktestResult::cost` like every other run:
//   * option legs: SpreadKind::VolTicks with
//     vol_tick = vol_pts * crossing / 100 — the engine charges
//     |qty| * multiplier * (vega * vol_tick) per leg per fill, which IS
//     "crossed half-spread in vol points x vega traded";
//   * delta rebalances: hedge_slippage_bps verbatim — the overlay charges
//     |shares| * spot * bps/1e4 per rebalance into the same cost column.
//
// ROUND 6: the crossing fraction is applied HERE rather than left to
// `SpreadKind::QuoteSide`, because QuoteSide needs per-fill recorded quotes
// this book does not have, and leaving the constant reachable only from a code
// path nothing selects is how it came to be dead in the first place. A
// non-finite or out-of-range crossing fraction is clamped OUT of the friction
// model entirely (no option spread charged) rather than silently trusted --
// `validate_vol_edge_config` rejects it first, so reaching that branch means a
// caller bypassed validation, and charging nothing is the loud outcome.
[[nodiscard]] inline FrictionModel vol_edge_frictions(const VolEdgeConfig &cfg) noexcept {
  FrictionModel f{};
  const bool crossing_ok = std::isfinite(cfg.cost_crossing_fraction) &&
                           cfg.cost_crossing_fraction > 0.0 &&
                           cfg.cost_crossing_fraction <= 1.0;
  if (cfg.cost_half_spread_vol_pts > 0.0 && crossing_ok) {
    f.spread_kind = FrictionModel::SpreadKind::VolTicks;
    f.vol_tick = cfg.cost_half_spread_vol_pts * cfg.cost_crossing_fraction / 100.0;
  }
  f.hedge_slippage_bps = cfg.stock_half_spread_bps;
  return f;
}

// The strategy. Holds the whole signal panel (indexed by date at
// construction) and, on each rebalance tick, closes the previous cohort at
// marks (an engine roll-close) and opens the ranked target book as ATM-forward
// straddles sized TargetVega to each name's signed target.
//
// FAIL-SOFT PER NAME, FAIL-CLOSED PER CONFIG: a symbol absent from the day's
// snapshot, or a name whose straddle cannot be resolved/sized on the board
// (NotFound/Unavailable), is skipped and counted; a configuration error
// (InvalidArgument from the DSL or an invalid VolEdgeConfig) aborts the run.
class VolEdgeStrategy final : public IStrategy {
public:
  VolEdgeStrategy(std::vector<VrpSignalRow> signal, VolEdgeConfig cfg) : cfg_{cfg} {
    for (VrpSignalRow &row : signal) {
      by_date_[row.date].push_back(std::move(row));
    }
  }

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    return on_step(base, step_index, book, next_lot_id, PriceOptions{});
  }

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id, const PriceOptions &price_options) override {
    last_entry_seeds_.clear();
    ATX_TRY_VOID(validate_vol_edge_config(cfg_));
    // FAIL-SAFE ROLL-CLOSE (F3/F4 hardening) — checked on EVERY step, before
    // any hold/tick early-out: a lot within `expiry_guard_days` of expiry is
    // roll-closed at this step's marks, because the NEXT session may lie past
    // the expiry and a crossing without an exact observation is a run-ending
    // engine error. The guard is sufficient whenever `expiry_guard_days` is at
    // least the corpus's longest inter-session calendar gap: the last session
    // strictly before an expiry is then always within the guard of it.
    //
    // Round 3: under `hold_to_horizon` the same safety property is delivered
    // PER NAME — an expiring name rolls (close + immediate re-entry at the
    // held target) instead of the whole book flattening — so staggered
    // cohorts never churn each other and the book stays invested through the
    // horizon boundary. Either way no lot ever crosses an unobserved expiry.
    if (!cfg_.hold_to_horizon) {
      if (!book.lots.empty()) {
        const auto guard_ns =
            static_cast<std::int64_t>(std::llround(cfg_.expiry_guard_days * 86'400.0e9));
        bool expiring = false;
        for (const Lot &lot : book.lots) {
          if (lot.expiry_ts_ns - base.ts_ns() <= guard_ns) {
            expiring = true;
            break;
          }
        }
        if (expiring) {
          book.lots.clear(); // the engine books each close at the current mark
          ++n_guard_roll_closes_;
        }
      }
    } else {
      reconcile_held(book);
      ATX_TRY_VOID(guard_roll_expiring_names(base, book, next_lot_id, price_options));
    }
    if (step_index % cfg_.rebalance_every_n_steps != 0u) {
      return atx::core::Ok(); // hold between rebalance ticks
    }
    const std::string date = vol_edge_session_date(base.ts_ns());
    const auto day = by_date_.find(date);
    if (day == by_date_.end()) {
      ++n_steps_entry_skipped_; // no signal for this session: fail-soft hold
      ++n_steps_held_missing_signal_;
      return atx::core::Ok();
    }
    if (cfg_.hold_to_horizon) {
      return rebalance_hold_to_horizon(base, day->second, book, next_lot_id, price_options);
    }
    auto targets = build_vol_edge_book(day->second, cfg_);
    if (!targets) {
      return atx::core::Err(targets.error());
    }
    // Roll: erase the previous cohort's option lots — the engine books each
    // close at the current mark (roll-close), never settlement. The swap lane
    // is untouched (this strategy never opens one).
    book.lots.clear();
    ++cohort_counter_;
    std::size_t n_long = 0;
    std::size_t n_short = 0;
    double net_target = 0.0;
    for (const VolEdgeTarget &target : *targets) {
      auto ids =
          open_straddle(base, target.symbol, target.vega_target, book, next_lot_id, price_options);
      if (!ids) {
        return atx::core::Err(ids.error());
      }
      if (ids->empty()) {
        ++skipped_names_; // name not in this snapshot / a data hole: fail-soft
        continue;
      }
      (target.vega_target >= 0.0 ? n_long : n_short) += 1u;
      net_target += target.vega_target;
      ++n_name_entries_;
    }
    if (n_long + n_short == 0u) {
      ++n_steps_entry_skipped_; // ranked day, nothing tradeable
    }
    last_n_long_ = static_cast<double>(n_long);
    last_n_short_ = static_cast<double>(n_short);
    last_net_target_vega_ = net_target;
    return atx::core::Ok();
  }

  [[nodiscard]] std::span<const FullGreekSeed> entry_risk_seeds() const noexcept override {
    return last_entry_seeds_;
  }

  [[nodiscard]] HedgeSpec hedge_spec() const override {
    HedgeSpec hedge;
    if (cfg_.delta_hedge) {
      hedge.kind = HedgeSpec::Kind::DeltaToZero;
      hedge.cadence = HedgeSpec::Cadence::Daily;
      hedge.band = cfg_.delta_hedge_band;
    }
    return hedge;
  }

  // Always the same six columns so every recorded row stays column-parallel.
  // The last two are CUMULATIVE counters (the DeclarativeStrategy convention:
  // a renderer differences consecutive rows to find the session an event
  // landed on) — they surface the held / roll-closed attribution per row.
  [[nodiscard]] std::vector<std::pair<std::string, double>>
  signals(const MarketSnapshot & /*base*/) const override {
    return {{"vol_edge_n_long", last_n_long_},
            {"vol_edge_n_short", last_n_short_},
            {"vol_edge_net_target_vega", last_net_target_vega_},
            {"vol_edge_skipped_names", static_cast<double>(skipped_names_)},
            {"vol_edge_held_steps", static_cast<double>(n_steps_held_missing_signal_)},
            {"vol_edge_roll_closed", static_cast<double>(n_guard_roll_closes_)}};
  }

  [[nodiscard]] std::uint64_t n_steps_entry_skipped() const noexcept override {
    return n_steps_entry_skipped_;
  }

  // Cumulative per-name fail-soft skips (symbol absent / unresolvable board).
  [[nodiscard]] std::uint64_t skipped_names() const noexcept { return skipped_names_; }

  // Cumulative rebalance ticks held on a MISSING signal date (the fail-soft
  // hold path only — the "ranked day, nothing tradeable" case counts in
  // `n_steps_entry_skipped` but not here).
  [[nodiscard]] std::uint64_t held_steps() const noexcept {
    return n_steps_held_missing_signal_;
  }

  // Cumulative fail-safe roll-closes: steps on which the expiry guard closed
  // (legacy mode) or per-name rolled (hold-to-horizon mode) expiring lots at
  // marks before an unobservable expiry crossing.
  [[nodiscard]] std::uint64_t guard_roll_closes() const noexcept {
    return n_guard_roll_closes_;
  }

  // Cumulative per-name turnover attribution (round 3). `name_entries` counts
  // every cost-bearing option entry by name: fresh admissions in either mode
  // (the legacy full-churn mode re-enters its whole book each tick, and
  // counts so) plus hold-to-horizon guard-roll re-entries. `name_exits`
  // counts per-name closes the strategy attributes by name — hold-to-horizon
  // band exits and roll closes; the legacy whole-book clear is implicit and
  // stays 0 here.
  [[nodiscard]] std::uint64_t name_entries() const noexcept { return n_name_entries_; }
  [[nodiscard]] std::uint64_t name_exits() const noexcept { return n_name_exits_; }

  [[nodiscard]] const VolEdgeConfig &config() const noexcept { return cfg_; }

private:
  // One held name of the persistent (hold-to-horizon) book: which side, the
  // signed vega target it was ENTERED at (guard rolls re-enter at this same
  // target — re-ranking and re-sizing happen only on the cadence), and the
  // engine lots it owns.
  struct HeldPosition {
    double sign{0.0};
    double vega_target{0.0};
    std::vector<std::uint64_t> lot_ids;
  };

  // The engine owns `book.lots` (it settles/erases on its own authority), so
  // the held view is reconciled against it every step: ids the engine dropped
  // leave the record, and a name with no surviving lots leaves the book.
  void reconcile_held(const PortfolioState &book) {
    for (auto it = held_.begin(); it != held_.end();) {
      std::vector<std::uint64_t> &ids = it->second.lot_ids;
      std::erase_if(ids, [&book](std::uint64_t id) {
        return std::none_of(book.lots.begin(), book.lots.end(),
                            [id](const Lot &lot) noexcept { return lot.id == id; });
      });
      it = ids.empty() ? held_.erase(it) : std::next(it);
    }
  }

  static void erase_lots(PortfolioState &book, std::span<const std::uint64_t> ids) {
    std::erase_if(book.lots, [ids](const Lot &lot) noexcept {
      return std::find(ids.begin(), ids.end(), lot.id) != ids.end();
    });
  }

  // Open one ATM-forward straddle sized TargetVega to `vega_target` (the one
  // entry shape both modes share). Returns the new lot ids; an EMPTY vector
  // is the per-name fail-soft skip (symbol absent from the snapshot or an
  // unresolvable board) — the caller counts it. Config/DSL errors propagate.
  [[nodiscard]] Result<std::vector<std::uint64_t>>
  open_straddle(const MarketSnapshot &base, const std::string &symbol, double vega_target,
                PortfolioState &book, std::uint64_t &next_lot_id,
                const PriceOptions &price_options) {
    const auto uid = base.uid_of(symbol);
    if (!uid.has_value()) {
      return atx::core::Ok(std::vector<std::uint64_t>{});
    }
    StrategySpec spec;
    spec.name = "vol-edge";
    LegSpec leg;
    leg.uid = *uid;
    leg.symbol = symbol;
    leg.tenor.target_T = cfg_.horizon_days / 252.0;
    leg.structure.kind = StructureSpec::Kind::Straddle;
    leg.strike = StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0};
    leg.size = SizeSpec{SizeSpec::Kind::TargetVega, std::fabs(vega_target),
                        vega_target < 0.0 ? -1.0 : +1.0};
    spec.legs.push_back(std::move(leg));
    auto sized = resolve_spec(base, spec, price_options);
    if (!sized) {
      const ErrorCode code = sized.error().code();
      if (code == ErrorCode::NotFound || code == ErrorCode::Unavailable) {
        return atx::core::Ok(std::vector<std::uint64_t>{}); // a data hole, not a config error
      }
      return atx::core::Err(sized.error());
    }
    std::vector<std::uint64_t> ids;
    ids.reserve(sized->size());
    for (SizedLeg &sl : *sized) {
      if (!(std::isfinite(sl.leg.model_price) && sl.leg.model_price >= 0.0) ||
          sl.leg.expiry_ts_ns <= base.ts_ns() || !sl.leg.full_greek_seed.has_value()) {
        return atx::core::Err(ErrorCode::Unavailable,
                              "VolEdgeStrategy: sized leg lacks a usable mark/expiry/seed");
      }
      Lot lot;
      lot.id = next_lot_id++;
      lot.contract = OptionContract{sl.leg.uid, sl.leg.K, sl.leg.T, sl.leg.side};
      lot.qty = sl.qty;
      lot.multiplier = sl.multiplier;
      lot.expiry_ts_ns = sl.leg.expiry_ts_ns;
      lot.cohort = cohort_counter_;
      lot.entry_price = sl.leg.model_price; // fill at model mid; frictions ride RunConfig
      book.lots.push_back(lot);
      last_entry_seeds_.push_back(std::move(*sl.leg.full_greek_seed));
      ids.push_back(lot.id);
    }
    return atx::core::Ok(std::move(ids));
  }

  // Hold-to-horizon expiry guard: roll each expiring NAME at marks — close its
  // lots and immediately re-enter at the held signed target with a fresh
  // tenor. A blind roll by design: re-ranking happens only on the cadence, so
  // an off-tick roll must not smuggle in a re-sort (research-vrp-costs.md
  // [7]: hold to the horizon, pay one entry per holding period).
  [[nodiscard]] Status guard_roll_expiring_names(const MarketSnapshot &base, PortfolioState &book,
                                                 std::uint64_t &next_lot_id,
                                                 const PriceOptions &price_options) {
    if (held_.empty()) {
      return atx::core::Ok();
    }
    const auto guard_ns =
        static_cast<std::int64_t>(std::llround(cfg_.expiry_guard_days * 86'400.0e9));
    std::vector<std::string> expiring;
    for (const auto &[symbol, position] : held_) {
      for (const Lot &lot : book.lots) {
        const bool owned = std::find(position.lot_ids.begin(), position.lot_ids.end(),
                                     lot.id) != position.lot_ids.end();
        if (owned && lot.expiry_ts_ns - base.ts_ns() <= guard_ns) {
          expiring.push_back(symbol);
          break;
        }
      }
    }
    if (expiring.empty()) {
      return atx::core::Ok();
    }
    ++n_guard_roll_closes_; // step-level counter, exactly like the legacy guard
    ++cohort_counter_;      // the rolled lots form a fresh cohort
    for (const std::string &symbol : expiring) {
      const auto it = held_.find(symbol);
      if (it == held_.end()) {
        continue; // unreachable: expiring names were drawn from held_ just above
      }
      HeldPosition &position = it->second;
      erase_lots(book, position.lot_ids); // the engine books each close at marks
      ++n_name_exits_;
      auto ids =
          open_straddle(base, symbol, position.vega_target, book, next_lot_id, price_options);
      if (!ids) {
        return atx::core::Err(ids.error());
      }
      if (ids->empty()) {
        held_.erase(symbol); // data hole on the roll: fail-soft, a tick may re-admit
        ++skipped_names_;
        continue;
      }
      position.lot_ids = std::move(*ids);
      ++n_name_entries_;
    }
    publish_book_signals();
    return atx::core::Ok();
  }

  // Hold-to-horizon rebalance tick: adjudicate the held book against today's
  // ranking (sS band + admission gates inside `plan_vol_edge_book`), close the
  // exits, keep the keeps untouched, open the entries.
  [[nodiscard]] Status rebalance_hold_to_horizon(const MarketSnapshot &base,
                                                 std::span<const VrpSignalRow> day,
                                                 PortfolioState &book,
                                                 std::uint64_t &next_lot_id,
                                                 const PriceOptions &price_options) {
    std::vector<VolEdgeHeldName> held;
    held.reserve(held_.size());
    for (const auto &[symbol, position] : held_) {
      held.push_back(VolEdgeHeldName{symbol, position.sign});
    }
    auto plan = plan_vol_edge_book(day, cfg_, held);
    if (!plan) {
      return atx::core::Err(plan.error());
    }
    if (!plan->rankable) {
      ++n_steps_entry_skipped_; // unrankable day: the book holds as-is
      publish_book_signals();
      return atx::core::Ok();
    }
    ++cohort_counter_;
    for (const std::string &symbol : plan->exits) {
      const auto it = held_.find(symbol);
      if (it == held_.end()) {
        continue; // unreachable: exits are drawn from the held view built above
      }
      erase_lots(book, it->second.lot_ids); // the engine books each close at marks
      held_.erase(it);
      ++n_name_exits_;
    }
    for (const VolEdgeTarget &target : plan->entries) {
      auto ids =
          open_straddle(base, target.symbol, target.vega_target, book, next_lot_id, price_options);
      if (!ids) {
        return atx::core::Err(ids.error());
      }
      if (ids->empty()) {
        ++skipped_names_; // name not in this snapshot / a data hole: fail-soft
        continue;
      }
      held_.insert_or_assign(target.symbol,
                             HeldPosition{target.vega_target >= 0.0 ? +1.0 : -1.0,
                                          target.vega_target, std::move(*ids)});
      ++n_name_entries_;
    }
    if (held_.empty()) {
      ++n_steps_entry_skipped_; // ranked day, nothing tradeable and nothing held
    }
    publish_book_signals();
    return atx::core::Ok();
  }

  // Recompute the recorded book-shape signals from the held view (the plan
  // path's analogue of the legacy tick's n_long/n_short/net assignment).
  void publish_book_signals() noexcept {
    std::size_t n_long = 0;
    std::size_t n_short = 0;
    double net_target = 0.0;
    for (const auto &[symbol, position] : held_) {
      (void)symbol;
      (position.sign > 0.0 ? n_long : n_short) += 1u;
      net_target += position.vega_target;
    }
    last_n_long_ = static_cast<double>(n_long);
    last_n_short_ = static_cast<double>(n_short);
    last_net_target_vega_ = net_target;
  }

  VolEdgeConfig cfg_;
  std::map<std::string, std::vector<VrpSignalRow>, std::less<>> by_date_;
  std::map<std::string, HeldPosition, std::less<>> held_; // hold-to-horizon book, by symbol
  std::uint32_t cohort_counter_{0};
  std::vector<FullGreekSeed> last_entry_seeds_;
  double last_n_long_{0.0};
  double last_n_short_{0.0};
  double last_net_target_vega_{0.0};
  std::uint64_t skipped_names_{0};
  std::uint64_t n_steps_entry_skipped_{0};
  std::uint64_t n_steps_held_missing_signal_{0};
  std::uint64_t n_guard_roll_closes_{0};
  std::uint64_t n_name_entries_{0};
  std::uint64_t n_name_exits_{0};
};

} // namespace atx::vol
