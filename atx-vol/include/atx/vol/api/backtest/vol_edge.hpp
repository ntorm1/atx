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
// book unchanged (fail-soft hold, counted in `n_steps_entry_skipped`); the
// caller must therefore choose `horizon_days` comfortably above the rebalance
// cadence, because a lot carried past its expiry without an exact-expiry
// snapshot observation is a run-ending engine error by the engine's own
// contract.
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
  // Roll cadence in clock STEPS (sessions). Must stay below the tenor in
  // sessions or a held lot crosses its expiry between rebalances (see the
  // header doc). 1 = re-rank and re-open every session.
  unsigned rebalance_every_n_steps{21};
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
  // Effective option half-spread in VOL POINTS (1.0 = one vol point of IV).
  // Mapped onto FrictionModel::vol_tick by `vol_edge_frictions`, so the engine
  // charges half-spread = vega * (vol_pts / 100) per share, per leg, per fill.
  double cost_half_spread_vol_pts{0.0};
  // Stock half-spread for the delta-hedge overlay, in bps of spot per share
  // traded (FrictionModel::hedge_slippage_bps verbatim).
  double stock_half_spread_bps{0.0};
  // Engine-owned daily delta hedge (HedgeSpec::DeltaToZero) and its band.
  bool delta_hedge{true};
  double delta_hedge_band{0.0};
};

// Field-count drift pin: a new knob must be appended AND documented above.
static_assert(detail::aggregate_arity_is_v<VolEdgeConfig, 13>,
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
  if (!(std::isfinite(cfg.delta_hedge_band) && cfg.delta_hedge_band >= 0.0)) {
    return bad("delta_hedge_band must be finite and >= 0");
  }
  return atx::core::Ok();
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
//   * option legs: SpreadKind::VolTicks with vol_tick = vol_pts / 100 — the
//     engine charges |qty| * multiplier * (vega * vol_tick) per leg per fill,
//     which IS "effective half-spread in vol points x vega traded";
//   * delta rebalances: hedge_slippage_bps verbatim — the overlay charges
//     |shares| * spot * bps/1e4 per rebalance into the same cost column.
[[nodiscard]] inline FrictionModel vol_edge_frictions(const VolEdgeConfig &cfg) noexcept {
  FrictionModel f{};
  if (cfg.cost_half_spread_vol_pts > 0.0) {
    f.spread_kind = FrictionModel::SpreadKind::VolTicks;
    f.vol_tick = cfg.cost_half_spread_vol_pts / 100.0;
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
    if (step_index % cfg_.rebalance_every_n_steps != 0u) {
      return atx::core::Ok(); // hold between rebalance ticks
    }
    const std::string date = vol_edge_session_date(base.ts_ns());
    const auto day = by_date_.find(date);
    if (day == by_date_.end()) {
      ++n_steps_entry_skipped_; // no signal for this session: fail-soft hold
      return atx::core::Ok();
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
      const auto uid = base.uid_of(target.symbol);
      if (!uid.has_value()) {
        ++skipped_names_; // name not in this snapshot: fail-soft
        continue;
      }
      StrategySpec spec;
      spec.name = "vol-edge";
      LegSpec leg;
      leg.uid = *uid;
      leg.symbol = target.symbol;
      leg.tenor.target_T = cfg_.horizon_days / 252.0;
      leg.structure.kind = StructureSpec::Kind::Straddle;
      leg.strike = StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0};
      leg.size = SizeSpec{SizeSpec::Kind::TargetVega, std::fabs(target.vega_target),
                          target.vega_target < 0.0 ? -1.0 : +1.0};
      spec.legs.push_back(std::move(leg));
      auto sized = resolve_spec(base, spec, price_options);
      if (!sized) {
        const ErrorCode code = sized.error().code();
        if (code == ErrorCode::NotFound || code == ErrorCode::Unavailable) {
          ++skipped_names_; // a data hole on this name, not a config error
          continue;
        }
        return atx::core::Err(sized.error());
      }
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
      }
      (target.vega_target >= 0.0 ? n_long : n_short) += 1u;
      net_target += target.vega_target;
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

  // Always the same four columns so every recorded row stays column-parallel.
  [[nodiscard]] std::vector<std::pair<std::string, double>>
  signals(const MarketSnapshot & /*base*/) const override {
    return {{"vol_edge_n_long", last_n_long_},
            {"vol_edge_n_short", last_n_short_},
            {"vol_edge_net_target_vega", last_net_target_vega_},
            {"vol_edge_skipped_names", static_cast<double>(skipped_names_)}};
  }

  [[nodiscard]] std::uint64_t n_steps_entry_skipped() const noexcept override {
    return n_steps_entry_skipped_;
  }

  // Cumulative per-name fail-soft skips (symbol absent / unresolvable board).
  [[nodiscard]] std::uint64_t skipped_names() const noexcept { return skipped_names_; }

  [[nodiscard]] const VolEdgeConfig &config() const noexcept { return cfg_; }

private:
  VolEdgeConfig cfg_;
  std::map<std::string, std::vector<VrpSignalRow>, std::less<>> by_date_;
  std::uint32_t cohort_counter_{0};
  std::vector<FullGreekSeed> last_entry_seeds_;
  double last_n_long_{0.0};
  double last_n_short_{0.0};
  double last_net_target_vega_{0.0};
  std::uint64_t skipped_names_{0};
  std::uint64_t n_steps_entry_skipped_{0};
};

} // namespace atx::vol
