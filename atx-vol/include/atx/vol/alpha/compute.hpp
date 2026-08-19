#pragma once
// ── atx::vol::alpha — registry-driven feature computation ───────────────────
//
// Turns a `PanelFrame` back into per-symbol series and evaluates NAMED features
// over them. Adding a feature is one `FeatureSpec` in the catalogue plus one
// evaluator here — no struct field, no schema version, no column-count bump.
//
// WHY THIS RECOMPUTES WHAT THE PANEL ALREADY EMITS. The evaluators for
// `f0_log_rv1 .. f9_vov_63d` are an INDEPENDENT reimplementation of
// `src/analytics/vrp_panel.hpp`'s window definitions, working from the panel's
// own `spot` / `iv_fair_21d` / `iv_fair_63d` columns rather than from a
// surface. That redundancy is the point: running them against a real panel and
// comparing to the emitted columns is a cross-check of the emitter that no
// amount of reading either implementation provides. `alpha_registry_test.cpp`
// pins the agreement, and the new features below are computed by the same
// machinery that just reproduced the old ones.
//
// WHAT IS AND IS NOT COMPUTABLE FROM A PANEL. Everything here reads only
// columns a v2/v3 panel already carries. Features needing a fitted surface at
// a tenor the panel does not publish -- the 25-delta wings (`f11`, `f12`,
// `f21`), the 10d strip (`f13`), the 126/189/252d long anchors (`f17`-`f19`)
// -- have catalogue entries but NO evaluator here, and `evaluate()` says so by
// name instead of silently emitting NaN. They need the surface DB, not this
// header.
//
// ── THE AXIS HAZARD, AND THE GATE THAT CLOSES IT ────────────────────────────
//
// A PANEL IS NOT A BAR AXIS. `vrp_panel.hpp`'s row policy drops any session
// whose 21d strip is unavailable, and states that the dropped session's SPOT
// "still participates in neighbours' trailing/forward windows" -- the emitter
// computes on the FULL bar axis and emits a SUBSET of it. On a real 25-name
// panel that subset is missing 14% of symbol-sessions.
//
// So `spot[i-1]` on the emitted rows is NOT the previous session's close. A
// trailing-21 window over emitted rows can span 25 calendar sessions, and the
// resulting number is not the feature it is named after. Measured, on
// vrp_panel_clean25_v2: recomputing f0/f1/f2/f5/f6/f7/f8 off the emitted rows
// disagrees with the emitted columns by up to 12.6 in log-variance units,
// while the row-local f3/f4 and the gap-NaNing f9 agree to 0.0e+00 exactly.
// That contrast IS the diagnosis -- only the window features move.
//
// The gate: `SymbolSeries::contiguous[i]` is true iff this row's date is the
// GLOBAL session immediately after the previous row's, where the global
// session axis is the union of dates across every symbol in the panel. A
// window containing any non-contiguous step yields NaN. The result is a
// strict subset of what the emitter can compute, and every value in it agrees
// with the emitter exactly. Fewer answers, no wrong ones.
//
// THE THREE PUBLISHED LONG-VEGA PREDICTORS THIS MAKES OPERATIONAL, none of
// which the shipped panel carries:
//
//   f15_idio_share   Cao & Han (JFE 108(1) 2013): delta-hedged returns fall
//                    monotonically in idiosyncratic vol, and the IVOL
//                    coefficient MORE THAN DOUBLES once log(HV/IV) -- this
//                    panel's own f5 -- is controlled for. Long vega wants LOW.
//   f16_iv_vov_21d   Cao, Vasquez, Xiao & Zhan (QJF 2023): stdev of daily LOG
//                    changes in implied vol over the previous month. Both the
//                    log transform and the 21-session window are load-bearing;
//                    `f9_vov_63d` has neither. Long vega wants LOW.
//   liq_hspread_frac Christoffersen, Goyenko, Jacobs & Karoui (RFS 31(2)
//                    2018): delta-hedged returns are significantly more
//                    negative when the option is less liquid. The panel has
//                    measured this column since v3 and used it ONLY as a cost.
//                    It is also a signal. Long vega wants the tight end.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/alpha/frame.hpp"
#include "atx/vol/alpha/registry.hpp"

namespace atx::vol::alpha {

// Annualization used everywhere in this file, matching
// `analytics/realized_vol.cpp`: var_per_bar = sum(r^2)/n, vol =
// sqrt(var_per_bar * 252). No mean subtraction — that is the repo's convention
// and a mean-subtracted variant would silently disagree with every emitted
// panel by a few basis points.
inline constexpr double kTradingDays = 252.0;
inline constexpr double kLogRv1Floor = 1.0e-8;
inline constexpr double kJumpSigmaMultiple = 4.0;
// sqrt(3.0^2 * 20 / 252) — the smallest |log return| that on its own pushes a
// 21-session realized-vol window past 300% annualized. It is `vrp_panel`'s
// kVrpImplausibleStepReturn, restated because this layer links only
// atx::core; AlphaComputeStepThresholdMatchesTheEmitter pins the two equal.
// A move this large in a RAW spot series is a corporate action, not a return.
inline constexpr double kAlphaImplausibleStepReturn = 0.8451542547285166;

// One symbol's rows, in date order, with a back-pointer to the frame row each
// came from so a computed column can be written back row-aligned.
struct SymbolSeries {
  std::string symbol;
  std::vector<std::size_t> row;  // index into the frame
  std::vector<std::string> date; // ascending
  std::vector<double> spot;
  std::vector<double> iv21;
  std::vector<double> iv63;
  std::vector<double> atmf21;
  std::vector<double> liq_hspread;
  std::vector<double> liq_strikes;
  // The emitter's own bar-axis position (vrp_panel_v4's `bar_index`), or NaN
  // on a panel that predates it. Carried as a double because the frame stores
  // every numeric column as one; the values are small exact integers.
  std::vector<double> bar_index;
  // contiguous[i]: the step from row i-1 to row i is a USABLE session step.
  // Two things can make it not one:
  //   * the sessions are not adjacent -- read from `bar_index` when the panel
  //     has one, inferred on the global date axis when it does not; or
  //   * the step is not a return. A spot series is RAW: across the ex-date of
  //     a corporate action no supplied factor covers, spot[i]/spot[i-1] is a
  //     share-count ratio. `vrp_panel` quarantines its OWN columns against
  //     that (VrpImplausiblePolicy) but cannot repair the `spot` column, which
  //     is contractually the unadjusted series -- so a consumer recomputing
  //     from spot would read a -222% "return" and every window over it would
  //     be fiction. Folding it in here means one flag protects every windowed
  //     feature, present and future, with no new machinery: the step simply is
  //     not a session-to-session move, which is what this flag already means.
  // Always true at i == 0 (nothing precedes it to be adjacent to).
  // `unsigned char` rather than `bool` so a `std::span` over it is valid --
  // `std::vector<bool>` is a bit proxy with no contiguous storage.
  std::vector<unsigned char> contiguous;

  [[nodiscard]] std::size_t size() const noexcept { return date.size(); }

  // Is every step inside the k-step window ending at `i` a real adjacency?
  // A window of k differences ending at i consumes steps i-k+1 .. i.
  [[nodiscard]] bool window_contiguous(std::size_t i, std::size_t k) const noexcept {
    if (i < k) {
      return false;
    }
    for (std::size_t j = i + 1 - k; j <= i; ++j) {
      if (contiguous[j] == 0U) {
        return false;
      }
    }
    return true;
  }
};

// The market proxy f15 regresses against: a date-indexed close series.
struct MarketSeries {
  std::string symbol;
  std::vector<std::string> date; // ascending, unique
  std::vector<double> spot;
  std::unordered_map<std::string, std::size_t> index;
  // Sessions on the panel's global axis this proxy actually covers.
  std::size_t global_sessions{0};

  [[nodiscard]] bool empty() const noexcept { return date.empty(); }
  [[nodiscard]] double coverage_fraction() const noexcept {
    return global_sessions == 0
               ? 0.0
               : static_cast<double>(date.size()) / static_cast<double>(global_sessions);
  }
};

namespace compute_detail {

[[nodiscard]] inline double nan_d() noexcept { return std::numeric_limits<double>::quiet_NaN(); }

// ln(spot[i]/spot[i-1]). NaN at i == 0 or on a non-positive close.
[[nodiscard]] inline double logret(std::span<const double> spot, std::size_t i) noexcept {
  if (i == 0 || i >= spot.size()) {
    return nan_d();
  }
  const double a = spot[i - 1];
  const double b = spot[i];
  if (!(a > 0.0) || !(b > 0.0)) {
    return nan_d();
  }
  return std::log(b / a);
}

// Annualized close-to-close vol over exactly `k` return terms r_{i-k+1..i}.
// NaN inside the warmup or if any term is non-finite.
[[nodiscard]] inline double c2c_vol(std::span<const double> spot, std::size_t i,
                                    std::size_t k) noexcept {
  if (k == 0 || i < k) {
    return nan_d();
  }
  double sum = 0.0;
  for (std::size_t j = i + 1 - k; j <= i; ++j) {
    const double r = logret(spot, j);
    if (!std::isfinite(r)) {
      return nan_d();
    }
    sum += r * r;
  }
  return std::sqrt(sum / static_cast<double>(k) * kTradingDays);
}

// Sample stdev (n-1) of the `w` daily first differences of `v` ending at `i`.
// `log_scale` takes the difference of ln(v) instead of v. A single missing
// value inside the window NaNs the whole window rather than bridging the gap.
[[nodiscard]] inline double diff_stdev(std::span<const double> v, std::size_t i, std::size_t w,
                                       bool log_scale) noexcept {
  if (w < 2 || i < w) {
    return nan_d();
  }
  const auto delta = [&](std::size_t j) -> double {
    const double a = v[j - 1];
    const double b = v[j];
    if (!log_scale) {
      return b - a;
    }
    if (!(a > 0.0) || !(b > 0.0)) {
      return nan_d();
    }
    return std::log(b / a);
  };
  double mean = 0.0;
  for (std::size_t j = i + 1 - w; j <= i; ++j) {
    const double d = delta(j);
    if (!std::isfinite(d)) {
      return nan_d();
    }
    mean += d;
  }
  mean /= static_cast<double>(w);
  double acc = 0.0;
  for (std::size_t j = i + 1 - w; j <= i; ++j) {
    const double d = delta(j) - mean;
    acc += d * d;
  }
  return std::sqrt(acc / static_cast<double>(w - 1));
}

// Mid-rank percentile of v[i] within the `w` values ending at and including i,
// mapped to (0, 1) by (n_below + n_tie/2)/w. NaN if any value in the window is
// missing. Mid-ranking matches the trainer's within-date percentile convention,
// so a within-name rank and a within-date rank mean the same thing.
[[nodiscard]] inline double own_rank(std::span<const double> v, std::size_t i,
                                     std::size_t w) noexcept {
  if (w == 0 || i + 1 < w) {
    return nan_d();
  }
  const double x = v[i];
  if (!std::isfinite(x)) {
    return nan_d();
  }
  std::size_t below = 0;
  std::size_t tie = 0;
  for (std::size_t j = i + 1 - w; j <= i; ++j) {
    const double y = v[j];
    if (!std::isfinite(y)) {
      return nan_d();
    }
    if (y < x) {
      ++below;
    } else if (y == x) {
      ++tie;
    }
  }
  return (static_cast<double>(below) + 0.5 * static_cast<double>(tie)) / static_cast<double>(w);
}

// 1 - R^2 of the symbol's daily log returns on the market's, over `w` return
// PAIRS ending at i.
//
// A pair enters only when the two legs span the SAME session interval: the
// market must carry both this bar's date and the previous bar's date, and they
// must be ADJACENT in the market series. That rule is what stops a symbol that
// missed a session from regressing its 2-day return on the market's 1-day one
// — the defect a naive positional zip produces exactly on the days that matter.
// Realized SEMIvariance over the k returns ending at and including i:
// `want_down` selects sum(r^2 | r < 0), otherwise sum(r^2 | r > 0). Annualized
// on the SAME k denominator as `c2c_vol`, not on the count of qualifying
// sessions -- RS+ + RS- must equal RV, which is the identity the signed jump
// variation is built on, and normalizing each side by its own count breaks it.
// A zero return contributes to neither side, matching Patton & Sheppard's
// indicator convention.
[[nodiscard]] inline double semivar(std::span<const double> spot, std::size_t i, std::size_t k,
                                    bool want_down) noexcept {
  if (k == 0 || i < k) {
    return nan_d();
  }
  double sum = 0.0;
  for (std::size_t j = i + 1 - k; j <= i; ++j) {
    const double r = logret(spot, j);
    if (!std::isfinite(r)) {
      return nan_d();
    }
    if (want_down ? (r < 0.0) : (r > 0.0)) {
      sum += r * r;
    }
  }
  return sum / static_cast<double>(k) * kTradingDays;
}

[[nodiscard]] inline double idio_share(const SymbolSeries &sym, const MarketSeries &mkt,
                                       std::size_t i, std::size_t w) noexcept {
  if (mkt.empty() || w < 3 || i < w) {
    return nan_d();
  }
  double sx = 0.0;
  double sy = 0.0;
  double sxx = 0.0;
  double syy = 0.0;
  double sxy = 0.0;
  std::size_t n = 0;
  for (std::size_t j = i + 1 - w; j <= i; ++j) {
    const auto cur = mkt.index.find(sym.date[j]);
    const auto prev = mkt.index.find(sym.date[j - 1]);
    if (cur == mkt.index.end() || prev == mkt.index.end()) {
      return nan_d();
    }
    // Global contiguity (checked by the caller via `window_contiguous`) already
    // guarantees no session sits between these two dates, so if the market
    // carries BOTH of them its own indices are necessarily adjacent. The check
    // is kept as an assertion of that reasoning rather than as a filter.
    if (cur->second != prev->second + 1) {
      return nan_d(); // the legs do not span the same session interval
    }
    const double ry = logret(sym.spot, j);
    const double rx = logret(mkt.spot, cur->second);
    if (!std::isfinite(rx) || !std::isfinite(ry)) {
      return nan_d();
    }
    sx += rx;
    sy += ry;
    sxx += rx * rx;
    syy += ry * ry;
    sxy += rx * ry;
    ++n;
  }
  if (n < 3) {
    return nan_d();
  }
  const double dn = static_cast<double>(n);
  const double cov = sxy - sx * sy / dn;
  const double vx = sxx - sx * sx / dn;
  const double vy = syy - sy * sy / dn;
  if (!(vx > 0.0) || !(vy > 0.0)) {
    return nan_d(); // a degenerate leg has no R^2 to report
  }
  const double r2 = (cov * cov) / (vx * vy);
  return 1.0 - r2;
}

} // namespace compute_detail

// ── Evaluators ──────────────────────────────────────────────────────────────

struct EvalInputs {
  const SymbolSeries *sym{nullptr};
  const MarketSeries *market{nullptr}; // may be null; f15 then yields NaN
};

using FeatureFn = double (*)(const EvalInputs &, std::size_t);

// Name -> evaluator, for exactly the catalogue features a panel's own columns
// can support. A catalogue name ABSENT from this map is not an error; it means
// "needs the surface DB", and `evaluate()` reports it by name.
[[nodiscard]] inline const std::unordered_map<std::string, FeatureFn> &panel_evaluators() {
  using compute_detail::c2c_vol;
  using compute_detail::diff_stdev;
  using compute_detail::idio_share;
  using compute_detail::logret;
  using compute_detail::nan_d;
  using compute_detail::own_rank;
  using compute_detail::semivar;

  static const std::unordered_map<std::string, FeatureFn> kMap = {
      {"f0_log_rv1",
       [](const EvalInputs &in, std::size_t i) {
         if (!in.sym->window_contiguous(i, 1)) {
           return nan_d();
         }
         const double r = logret(in.sym->spot, i);
         return std::isfinite(r) ? std::log(std::max(kTradingDays * r * r, kLogRv1Floor)) : nan_d();
       }},
      {"f1_log_rv5",
       [](const EvalInputs &in, std::size_t i) {
         if (!in.sym->window_contiguous(i, 5)) {
           return nan_d();
         }
         const double v = c2c_vol(in.sym->spot, i, 5);
         return std::log(v * v);
       }},
      {"f2_log_rv21",
       [](const EvalInputs &in, std::size_t i) {
         if (!in.sym->window_contiguous(i, 21)) {
           return nan_d();
         }
         const double v = c2c_vol(in.sym->spot, i, 21);
         return std::log(v * v);
       }},
      {"f3_iv_level",
       [](const EvalInputs &in, std::size_t i) {
         const double iv = in.sym->iv21[i];
         return std::log(iv * iv);
       }},
      {"f4_term_slope",
       [](const EvalInputs &in, std::size_t i) { return in.sym->iv63[i] - in.sym->iv21[i]; }},
      {"f5_hv_iv_gap",
       [](const EvalInputs &in, std::size_t i) {
         if (!in.sym->window_contiguous(i, 21)) {
           return nan_d();
         }
         return std::log(c2c_vol(in.sym->spot, i, 21) / in.sym->iv21[i]);
       }},
      {"f6_vrp_lag",
       [](const EvalInputs &in, std::size_t i) {
         if (!in.sym->window_contiguous(i, 21)) {
           return nan_d();
         }
         const double iv = in.sym->iv21[i];
         const double v = c2c_vol(in.sym->spot, i, 21);
         return iv * iv - v * v;
       }},
      {"f7_ret_21d",
       [](const EvalInputs &in, std::size_t i) {
         if (i < 21 || !in.sym->window_contiguous(i, 21)) {
           return nan_d();
         }
         const double a = in.sym->spot[i - 21];
         const double b = in.sym->spot[i];
         return (a > 0.0 && b > 0.0) ? std::log(b / a) : nan_d();
       }},
      {"f8_jump_recent",
       [](const EvalInputs &in, std::size_t i) {
         if (i < 5 || !in.sym->window_contiguous(i, 63)) {
           return nan_d();
         }
         double mx = 0.0;
         for (std::size_t j = i - 4; j <= i; ++j) {
           const double r = logret(in.sym->spot, j);
           if (!std::isfinite(r)) {
             return nan_d();
           }
           mx = std::max(mx, std::fabs(r));
         }
         const double sigma_daily = c2c_vol(in.sym->spot, i, 63) / std::sqrt(kTradingDays);
         if (!std::isfinite(sigma_daily)) {
           return nan_d();
         }
         return mx > kJumpSigmaMultiple * sigma_daily ? 1.0 : 0.0;
       }},
      {"f9_vov_63d",
       [](const EvalInputs &in, std::size_t i) {
         if (!in.sym->window_contiguous(i, 63)) {
           return nan_d();
         }
         return diff_stdev(in.sym->iv21, i, 63, /*log_scale=*/false);
       }},
      {"f10_iv_rank_252",
       [](const EvalInputs &in, std::size_t i) {
         // 252 VALUES ending at i span 251 steps.
         return in.sym->window_contiguous(i, 251) ? own_rank(in.sym->atmf21, i, 252) : nan_d();
       }},
      {"f14_iv_chg_5d",
       [](const EvalInputs &in, std::size_t i) {
         return in.sym->window_contiguous(i, 5) ? in.sym->atmf21[i] - in.sym->atmf21[i - 5]
                                                : nan_d();
       }},
      {"f15_idio_share",
       [](const EvalInputs &in, std::size_t i) {
         if (in.market == nullptr || !in.sym->window_contiguous(i, 63)) {
           return nan_d();
         }
         return idio_share(*in.sym, *in.market, i, 63);
       }},
      {"f16_iv_vov_21d",
       [](const EvalInputs &in, std::size_t i) {
         if (!in.sym->window_contiguous(i, 21)) {
           return nan_d();
         }
         return diff_stdev(in.sym->atmf21, i, 21, /*log_scale=*/true);
       }},
      {"f20_iv_vov_63d",
       [](const EvalInputs &in, std::size_t i) {
         if (!in.sym->window_contiguous(i, 63)) {
           return nan_d();
         }
         return diff_stdev(in.sym->atmf21, i, 63, /*log_scale=*/false);
       }},
      // ── Round 11: realized-vol predictors ───────────────────────────────
      {"f22_semivar_dn_21d",
       [](const EvalInputs &in, std::size_t i) {
         if (!in.sym->window_contiguous(i, 21)) {
           return nan_d();
         }
         // Floored, not guarded: a 21-session window with no down close is
         // rare but real, and ln(0) would emit -inf into a rank. The floor is
         // f0's, so the two log-variance features share one convention.
         return std::log(std::max(semivar(in.sym->spot, i, 21, /*want_down=*/true), kLogRv1Floor));
       }},
      {"f23_semivar_up_21d",
       [](const EvalInputs &in, std::size_t i) {
         if (!in.sym->window_contiguous(i, 21)) {
           return nan_d();
         }
         return std::log(std::max(semivar(in.sym->spot, i, 21, /*want_down=*/false), kLogRv1Floor));
       }},
      {"f24_signed_jump_21d",
       [](const EvalInputs &in, std::size_t i) {
         if (!in.sym->window_contiguous(i, 21)) {
           return nan_d();
         }
         const double up = semivar(in.sym->spot, i, 21, /*want_down=*/false);
         const double dn = semivar(in.sym->spot, i, 21, /*want_down=*/true);
         const double rv = up + dn; // the identity: RS+ + RS- == RV
         if (!(std::isfinite(rv) && rv > 0.0)) {
           return nan_d();
         }
         // Scaled to a share so it is a JUMP ASYMMETRY and not a restatement
         // of the variance level, which f2 already carries.
         return (up - dn) / rv;
       }},
      {"f25_leverage_21d",
       [](const EvalInputs &in, std::size_t i) {
         if (!in.sym->window_contiguous(i, 21)) {
           return nan_d();
         }
         const double r = logret(in.sym->spot, i);
         if (!std::isfinite(r)) {
           return nan_d();
         }
         const double v = c2c_vol(in.sym->spot, i, 21);
         // The source's RV_t * 1{r_t < 0}: the variance on down closes, the
         // floor otherwise. Emitting 0 instead of the floor would put the
         // no-leverage rows ABOVE the quietest down-close rows in a rank.
         return r < 0.0 ? std::log(std::max(v * v, kLogRv1Floor)) : std::log(kLogRv1Floor);
       }},
      {"f26_gs_hviv_252d",
       [](const EvalInputs &in, std::size_t i) {
         if (!in.sym->window_contiguous(i, 252)) {
           return nan_d();
         }
         const double v = c2c_vol(in.sym->spot, i, 252);
         const double iv = in.sym->iv21[i];
         return (v > 0.0 && iv > 0.0) ? std::log(v / iv) : nan_d();
       }},
      {"f27_sysvol_share_63d",
       [](const EvalInputs &in, std::size_t i) {
         if (in.market == nullptr || !in.sym->window_contiguous(i, 63)) {
           return nan_d();
         }
         const double idio = idio_share(*in.sym, *in.market, i, 63);
         // 1 - idio, not a second regression: computing it separately would
         // let the two features disagree about the same fit.
         return std::isfinite(idio) ? 1.0 - idio : nan_d();
       }},
      {"liq_hspread_frac",
       [](const EvalInputs &in, std::size_t i) { return in.sym->liq_hspread[i]; }},
      {"liq_strikes_fit",
       [](const EvalInputs &in, std::size_t i) { return in.sym->liq_strikes[i]; }},
  };
  return kMap;
}

// ── Frame -> per-symbol series ──────────────────────────────────────────────
//
// Rows are grouped by `symbol` and kept in the file's own order within a
// group. The panel writes rows sorted (symbol, session) and that sort is a
// gate-tested contract, so re-sorting here would only mask a violation of it;
// instead the date order is CHECKED and a non-ascending group is an error.
[[nodiscard]] inline Result<std::vector<SymbolSeries>> group_by_symbol(const PanelFrame &frame) {
  ATX_TRY(const auto symbols, frame.strings("symbol"));
  ATX_TRY(const auto dates, frame.strings("date"));
  ATX_TRY(const auto spot, frame.numbers("spot"));
  ATX_TRY(const auto iv21, frame.numbers("iv_fair_21d"));
  ATX_TRY(const auto iv63, frame.numbers("iv_fair_63d"));

  const bool has_atmf = frame.schema().has("iv_atmf_21d");
  const bool has_hspread = frame.schema().has("liq_hspread_frac");
  const bool has_strikes = frame.schema().has("liq_strikes_fit");
  std::span<const double> atmf;
  std::span<const double> hspread;
  std::span<const double> strikes;
  if (has_atmf) {
    ATX_TRY(atmf, frame.numbers("iv_atmf_21d"));
  }
  if (has_hspread) {
    ATX_TRY(hspread, frame.numbers("liq_hspread_frac"));
  }
  if (has_strikes) {
    ATX_TRY(strikes, frame.numbers("liq_strikes_fit"));
  }

  // `bar_index` (vrp_panel_v4) is the emitter's OWN bar-axis position for the
  // row. When the panel carries it, adjacency is EXACT and per-symbol: no
  // trading calendar, no union heuristic, no degradation on a one-name panel.
  // It also means the emitted axis is the bar axis, so the gate below stops
  // costing coverage -- which is the entire reason v4 exists. Prefer it
  // whenever present and fall back to the date-union axis otherwise.
  const bool has_bar_index = frame.schema().has("bar_index");
  std::span<const double> bar_index;
  if (has_bar_index) {
    ATX_TRY(bar_index, frame.numbers("bar_index"));
  }

  // The GLOBAL session axis: the union of dates across every symbol, sorted.
  // A symbol missing one of these dates had that session DROPPED by the
  // emitter's row policy, and every window spanning the hole is unknowable
  // from the panel. This union is the closest thing to the emitter's bar axis
  // that a panel file contains; it is exact whenever at least one symbol
  // survived each session, which is the normal case on a multi-name panel and
  // is DEGRADED, never wrong, when it is not -- a session no symbol survived
  // simply does not appear, so a window over it is treated as contiguous.
  // On a single-symbol panel the union collapses to that symbol's own rows and
  // the gate cannot fire at all; `group_by_symbol` reports that below.
  std::vector<std::string> axis;
  {
    std::vector<std::string> all(dates.begin(), dates.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    axis = std::move(all);
  }
  std::unordered_map<std::string, std::size_t> axis_at;
  axis_at.reserve(axis.size());
  for (std::size_t i = 0; i < axis.size(); ++i) {
    axis_at.emplace(axis[i], i);
  }

  std::vector<SymbolSeries> out;
  std::unordered_map<std::string, std::size_t> where;
  const double nan_v = compute_detail::nan_d();
  for (std::size_t r = 0; r < frame.rows(); ++r) {
    const std::string &sym = symbols[r];
    auto it = where.find(sym);
    if (it == where.end()) {
      it = where.emplace(sym, out.size()).first;
      out.push_back(SymbolSeries{});
      out.back().symbol = sym;
    }
    SymbolSeries &s = out[it->second];
    if (!s.date.empty() && dates[r] <= s.date.back()) {
      return Err(atx::core::ErrorCode::InvalidArgument,
                 "alpha::group_by_symbol: rows for '" + sym + "' are not date-ascending at '" +
                     dates[r] + "' (the panel's (symbol, session) sort is a contract)");
    }
    // Adjacency: exact from the emitter's own bar index when the panel carries
    // one, otherwise inferred on the GLOBAL date axis rather than on this
    // symbol's own rows.
    unsigned char contig = 1U;
    if (!s.date.empty()) {
      if (has_bar_index) {
        const double cur = bar_index[r];
        const double prev = s.bar_index.back();
        contig = (std::isfinite(cur) && std::isfinite(prev) && cur == prev + 1.0) ? 1U : 0U;
      } else {
        const auto cur = axis_at.find(dates[r]);
        const auto prev = axis_at.find(s.date.back());
        contig = (cur != axis_at.end() && prev != axis_at.end() && cur->second == prev->second + 1)
                     ? 1U
                     : 0U;
      }
    }
    // A step that is not a return is not a step. Threshold DERIVED, not
    // chosen: it is `vrp_panel`'s own kVrpImplausibleStepReturn, the smallest
    // |log return| that alone pushes a 21-session window past that header's
    // rv plausibility gate ("a -57% day"). Duplicated as a literal rather than
    // included, because this layer links only atx::core -- the test pins the
    // two together so they cannot drift.
    if (contig == 1U && !s.spot.empty()) {
      const double step = std::log(spot[r] / s.spot.back());
      if (std::isfinite(step) && std::abs(step) >= kAlphaImplausibleStepReturn) {
        contig = 0U;
      }
    }
    s.bar_index.push_back(has_bar_index ? bar_index[r] : nan_v);
    s.contiguous.push_back(contig);
    s.row.push_back(r);
    s.date.push_back(dates[r]);
    s.spot.push_back(spot[r]);
    s.iv21.push_back(iv21[r]);
    s.iv63.push_back(iv63[r]);
    s.atmf21.push_back(has_atmf ? atmf[r] : nan_v);
    s.liq_hspread.push_back(has_hspread ? hspread[r] : nan_v);
    s.liq_strikes.push_back(has_strikes ? strikes[r] : nan_v);
  }
  return Ok(std::move(out));
}

// A market proxy assembled from one symbol already present in the panel. Using
// a panel symbol rather than an outside file guarantees the two legs share a
// session calendar by construction.
//
// COVERAGE IS THE WHOLE STORY FOR f15. The proxy is subject to the SAME row
// policy as every other symbol: a session where the proxy's own 21d strip was
// unavailable produces no row, and `idio_share` needs the proxy present on
// EVERY date of a 63-session window. At a 14% drop rate that is 0.86^63, i.e.
// never. `coverage_fraction` against the panel's global session count is what
// tells a caller that up front instead of leaving them with an all-NaN column.
[[nodiscard]] inline Result<MarketSeries> market_from(std::span<const SymbolSeries> series,
                                                      std::string_view symbol,
                                                      std::size_t global_sessions = 0) {
  for (const SymbolSeries &s : series) {
    if (s.symbol != symbol) {
      continue;
    }
    MarketSeries m;
    m.symbol = s.symbol;
    m.date = s.date;
    m.spot = s.spot;
    m.index.reserve(m.date.size());
    for (std::size_t i = 0; i < m.date.size(); ++i) {
      m.index.emplace(m.date[i], i);
    }
    m.global_sessions = global_sessions;
    return Ok(std::move(m));
  }
  return Err(atx::core::ErrorCode::NotFound,
             "alpha::market_from: symbol '" + std::string(symbol) + "' is not in the panel");
}

// ── Evaluation ──────────────────────────────────────────────────────────────

struct ComputedFeatures {
  // Row-aligned with the frame: values[name][row].
  std::unordered_map<std::string, std::vector<double>> values;
  // Catalogue names that were requested but have no panel evaluator. Reported
  // rather than silently emitted as an all-NaN column, which is exactly the
  // failure `ColumnStats::all_missing()` exists to catch downstream.
  std::vector<std::string> needs_surface;
};

[[nodiscard]] inline Result<ComputedFeatures>
evaluate(const PanelFrame &frame, std::span<const FeatureSpec *const> features,
         const MarketSeries *market = nullptr) {
  ATX_TRY(auto series, group_by_symbol(frame));
  const auto &evals = panel_evaluators();

  ComputedFeatures out;
  std::vector<std::pair<std::string, FeatureFn>> todo;
  for (const FeatureSpec *spec : features) {
    if (spec == nullptr) {
      continue;
    }
    const auto it = evals.find(spec->name);
    if (it == evals.end()) {
      out.needs_surface.push_back(spec->name);
      continue;
    }
    todo.emplace_back(spec->name, it->second);
  }

  const double nan_v = compute_detail::nan_d();
  for (const auto &[name, fn] : todo) {
    out.values.emplace(name, std::vector<double>(frame.rows(), nan_v));
  }
  for (const SymbolSeries &s : series) {
    EvalInputs in{&s, market};
    for (const auto &[name, fn] : todo) {
      std::vector<double> &col = out.values[name];
      for (std::size_t i = 0; i < s.size(); ++i) {
        col[s.row[i]] = fn(in, i);
      }
    }
  }
  return Ok(std::move(out));
}

} // namespace atx::vol::alpha
