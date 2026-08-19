#pragma once
// ── atx::vol::alpha — the long single-name vega strategy framework ──────────
//
// Signal -> admission -> selection -> sizing -> P&L -> significance, with the
// feature set, the target, the horizon, the budget and the cost tier all
// supplied as CONFIG. Nothing here names a feature or a target: swapping the
// signal is a string list, not a recompile.
//
// ── THE FOUR DECISIONS THIS ENCODES ─────────────────────────────────────────
//
// 1. ORIENTATION COMES FROM THE CITATION, NOT FROM THE DATA. Each feature is
//    turned into a within-date percentile rank and then flipped, if needed, so
//    that HIGH means "a long-vega book wants this name" -- using the
//    `SignPrior` the catalogue recorded from the published result BEFORE
//    anything was measured here. A feature with `SignPrior::None` is REFUSED,
//    not fitted: letting the data pick the sign is how an in-sample coin flip
//    becomes a "predicted direction".
//
// 2. THE BLEND HAS NO TUNED PARAMETER. Equal weight on oriented within-date
//    ranks. Not because equal weighting is optimal, but because every
//    alternative spends degrees of freedom on a 250-session sample where the
//    honest hurdle is already t = 2.44 (Goyal-Saretto's 5% FDR bound). A
//    weighting scheme that beats equal-weight has to beat it out of sample.
//
// 3. SELECTION EXCESS IS THE ALPHA; ABSOLUTE RETURN IS THE FLOOR'S BETA.
//    Every date reports the selected book's P&L, the long-EVERYTHING-admitted
//    floor's P&L on the SAME date and the SAME admission set, and the paired
//    difference. A long-vega book that only earns the floor has no selection
//    skill, however good its absolute number looks -- and on a sample where
//    vol happened to rise, the floor alone can look excellent.
//
// 4. THE OVERLAP IS DISCLOSED THREE WAYS. A 21-session hold sampled daily is
//    20-fold overlapping, so a naive t is inflated by roughly sqrt(H). This
//    reports the raw t, the Newey-West t at Bartlett lag H-1, AND the
//    NON-OVERLAPPING phase sweep -- the H disjoint sub-series at lag 0, which
//    are genuinely independent draws and need no HAC correction to be honest.
//    If the phase sweep disagrees with the HAC number, believe the phase sweep.
//
// ── COSTS ───────────────────────────────────────────────────────────────────
//
// A flat two-tier charge in VOL POINTS: `cost_vp_liquid` for a name whose
// measured ATM half-spread is inside `liquid_hspread_cut`, `cost_vp_illiquid`
// otherwise. Defaults 0.10 / 0.25, one crossing. This is a deliberately blunt
// instrument. A per-name microstructure model would spend more modelling
// effort on the cost than on the alpha, and the cost is not where the
// uncertainty in this book lives.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/alpha/compute.hpp"
#include "atx/vol/alpha/frame.hpp"
#include "atx/vol/alpha/registry.hpp"

namespace atx::vol::alpha {

// ── Within-date cross-sectional ranking ─────────────────────────────────────

// Rows sharing one session, in the frame's own order.
struct DateSlice {
  std::string date;
  std::vector<std::size_t> rows;
};

// Groups by the `date` column. The panel sorts (symbol, session), so dates are
// interleaved across symbols and a single pass with a map is required; the
// result is returned in ASCENDING date order because every downstream
// statistic is a time series.
[[nodiscard]] inline Result<std::vector<DateSlice>> group_by_date(const PanelFrame &frame) {
  ATX_TRY(const auto dates, frame.strings("date"));
  std::vector<DateSlice> out;
  std::unordered_map<std::string, std::size_t> where;
  for (std::size_t r = 0; r < frame.rows(); ++r) {
    auto it = where.find(dates[r]);
    if (it == where.end()) {
      it = where.emplace(dates[r], out.size()).first;
      out.push_back(DateSlice{dates[r], {}});
    }
    out[it->second].rows.push_back(r);
  }
  std::sort(out.begin(), out.end(),
            [](const DateSlice &a, const DateSlice &b) { return a.date < b.date; });
  return Ok(std::move(out));
}

// Mid-rank percentile of each finite value within `rows`, in (0, 1). Rows whose
// value is not finite receive NaN and do not participate in the ranking, so a
// name missing one feature is missing from that feature's rank rather than
// being ranked as if it sat at the bottom.
inline void rank_within(std::span<const double> values, std::span<const std::size_t> rows,
                        std::vector<double> &out) {
  std::vector<std::size_t> live;
  live.reserve(rows.size());
  for (const std::size_t r : rows) {
    if (std::isfinite(values[r])) {
      live.push_back(r);
    }
  }
  const double n = static_cast<double>(live.size());
  if (live.empty()) {
    return;
  }
  for (const std::size_t r : live) {
    const double x = values[r];
    std::size_t below = 0;
    std::size_t tie = 0;
    for (const std::size_t q : live) {
      const double y = values[q];
      if (y < x) {
        ++below;
      } else if (y == x) {
        ++tie;
      }
    }
    out[r] = (static_cast<double>(below) + 0.5 * static_cast<double>(tie)) / n;
  }
}

// ── The blend ───────────────────────────────────────────────────────────────

struct BlendConfig {
  // Minimum oriented ranks a row must have before it gets a score. A row whose
  // features are mostly missing would otherwise be scored off whichever one or
  // two happened to be present, which is a different signal per row.
  std::size_t min_features_per_row{2};
  // Features whose published prior is INVERTED for this run. This exists for
  // the one legitimate case: a direction that flips with the AXIS — Vasquez
  // buys the steep-slope FRONT month while Campasano-Linn buy the inverted
  // name's BACK month, and both are right. A flip must cite an axis-specific
  // published direction; flipping a sign because the backtest liked it better
  // is sign-mining, and the loud FLIPPED report downstream exists to make
  // that visible. A name here that matches no selected feature is an error.
  std::vector<std::string> flip;
};

struct BlendResult {
  std::vector<double> score;         // row-aligned; NaN where unscored
  std::vector<std::string> used;     // features that carried a usable prior
  std::vector<std::string> refused;  // features with SignPrior::None
  std::vector<std::string> missing;  // features with no values supplied
  std::vector<std::string> flipped;  // used features whose prior was inverted
  // Rows on which each USED feature produced a finite oriented rank, in `used`
  // order. The blend's coverage is set by its narrowest input, and printing
  // this is how a caller sees WHICH one rather than only that the score is
  // sparse.
  std::vector<std::size_t> used_coverage;
  std::size_t n_scored{0};
  // How many finite features a row actually needed, after clamping the request
  // to the number of usable features. Requiring 2 of 1 scores nothing.
  std::size_t required_per_row{0};
};

// Equal-weight mean of oriented within-date percentile ranks.
//
// `values` supplies each feature's row-aligned column -- from a panel column
// or from `compute.hpp`, indifferently, which is what lets a feature the panel
// never emitted enter the book without a schema change.
[[nodiscard]] inline Result<BlendResult>
blend(const PanelFrame &frame, std::span<const DateSlice> dates,
      std::span<const FeatureSpec *const> features,
      const std::unordered_map<std::string, std::vector<double>> &values, const BlendConfig &cfg) {
  const double nan_v = std::numeric_limits<double>::quiet_NaN();
  BlendResult res;
  res.score.assign(frame.rows(), nan_v);

  std::vector<std::vector<double>> oriented;
  std::vector<bool> flip_hit(cfg.flip.size(), false);
  for (const FeatureSpec *spec : features) {
    if (spec == nullptr) {
      continue;
    }
    if (spec->prior == SignPrior::None) {
      // Refused, not silently dropped: a caller that selected `f11_rr25_21d`
      // must be told the catalogue has no published direction for it rather
      // than discover the book quietly ignored a third of its signal.
      res.refused.push_back(spec->name);
      continue;
    }
    const auto it = values.find(spec->name);
    if (it == values.end()) {
      res.missing.push_back(spec->name);
      continue;
    }
    std::vector<double> ranks(frame.rows(), nan_v);
    for (const DateSlice &d : dates) {
      rank_within(it->second, d.rows, ranks);
    }
    // BuyLow means a long-vega book wants the bottom of the cross-section, so
    // the rank is flipped to keep "high == attractive" true for every column.
    bool buy_low = spec->prior == SignPrior::BuyLow;
    for (std::size_t k = 0; k < cfg.flip.size(); ++k) {
      if (cfg.flip[k] == spec->name) {
        buy_low = !buy_low;
        flip_hit[k] = true;
        res.flipped.push_back(spec->name);
      }
    }
    if (buy_low) {
      for (double &v : ranks) {
        if (std::isfinite(v)) {
          v = 1.0 - v;
        }
      }
    }
    std::size_t cover = 0;
    for (const double v : ranks) {
      if (std::isfinite(v)) {
        ++cover;
      }
    }
    res.used.push_back(spec->name);
    res.used_coverage.push_back(cover);
    oriented.push_back(std::move(ranks));
  }

  if (oriented.empty()) {
    return Err(atx::core::ErrorCode::InvalidArgument,
               "alpha::blend: no selected feature carries a published sign prior; "
               "the blend would have to learn its own directions");
  }
  for (std::size_t k = 0; k < cfg.flip.size(); ++k) {
    // A typo'd flip that silently matched nothing would ship the book with
    // the OPPOSITE of its intended direction on one leg.
    if (!flip_hit[k]) {
      return Err(atx::core::ErrorCode::InvalidArgument,
                 "alpha::blend: --flip '" + cfg.flip[k] + "' matched no blended feature");
    }
  }

  // Clamped: asking for 2 finite features out of a 1-feature set would score
  // no row at all and report a silent empty book.
  res.required_per_row = std::max<std::size_t>(1, std::min(cfg.min_features_per_row, oriented.size()));
  for (std::size_t r = 0; r < frame.rows(); ++r) {
    double sum = 0.0;
    std::size_t n = 0;
    for (const std::vector<double> &col : oriented) {
      if (std::isfinite(col[r])) {
        sum += col[r];
        ++n;
      }
    }
    if (n >= res.required_per_row) {
      res.score[r] = sum / static_cast<double>(n);
      ++res.n_scored;
    }
  }
  return Ok(std::move(res));
}

// ── Admission, selection, sizing, P&L ───────────────────────────────────────

struct StrategyConfig {
  std::size_t horizon_sessions{21};
  // Names held per date. The book is equal-vega across them: under a FIXED
  // vega budget an IC*sigma*z Grinold scalar is a within-date constant that
  // cancels in the normalization, so equal-vega IS the Grinold shape up to a
  // per-name z tilt. Rank-tilted sizing is a separate experiment, not a
  // different answer to the same question.
  std::size_t max_names{20};
  // Liquidity admission. A name whose ATM half-spread was never MEASURED is
  // excluded when `require_measured_liquidity`, never admitted at the cheap
  // tier -- an unmeasured width is unknown, not tight.
  double max_hspread_frac{0.20};
  bool require_measured_liquidity{false};
  // Flat two-tier crossing charge in vol points.
  double liquid_hspread_cut{0.05};
  double cost_vp_liquid{0.10};
  double cost_vp_illiquid{0.25};
  double crossings{1.0};
};

struct DateResult {
  std::string date;
  std::size_t n_rows{0};
  std::size_t n_admitted{0};
  std::size_t n_selected{0};
  double selected_gross{0.0}; // vol points per unit vega
  double selected_net{0.0};
  double floor_gross{0.0}; // long every admitted name, equal vega
  double floor_net{0.0};
  double excess_gross{0.0}; // paired: selected - floor, same date, same set
  double excess_net{0.0};
};

// Newey-West t of a mean, Bartlett kernel at `lag`. `lag = 0` is the plain
// i.i.d. t. Returns NaN on a degenerate series rather than a large number.
[[nodiscard]] inline double newey_west_t(std::span<const double> x, std::size_t lag) noexcept {
  const std::size_t n = x.size();
  if (n < 2) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double dn = static_cast<double>(n);
  const double mean = std::accumulate(x.begin(), x.end(), 0.0) / dn;
  double gamma0 = 0.0;
  for (const double v : x) {
    const double d = v - mean;
    gamma0 += d * d;
  }
  gamma0 /= dn;
  double s = gamma0;
  const std::size_t max_lag = std::min(lag, n - 1);
  for (std::size_t l = 1; l <= max_lag; ++l) {
    double g = 0.0;
    for (std::size_t t = l; t < n; ++t) {
      g += (x[t] - mean) * (x[t - l] - mean);
    }
    g /= dn;
    const double w = 1.0 - static_cast<double>(l) / static_cast<double>(max_lag + 1);
    s += 2.0 * w * g;
  }
  if (!(s > 0.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return mean / std::sqrt(s / dn);
}

struct Scorecard {
  std::vector<DateResult> per_date;
  std::size_t n_dates{0};

  double mean_selected_net{0.0};
  double mean_floor_net{0.0};
  double mean_excess_net{0.0};
  double mean_excess_gross{0.0};

  double t_raw_excess_net{0.0};
  double t_nw_excess_net{0.0};
  double t_nw_selected_net{0.0};

  // The H disjoint, NON-overlapping phase sub-series: mean excess in each, and
  // the fraction of phases with a positive mean. No HAC correction is applied
  // or needed -- consecutive draws inside one phase are H sessions apart.
  std::vector<double> phase_mean_excess_net;
  double phase_positive_fraction{0.0};
  double phase_min_mean{0.0};
  double phase_max_mean{0.0};
};

// Run the book over a scored panel.
//
// `pnl` is the per-row realized P&L of one unit of vega, in vol points, for a
// position ENTERED on that row -- e.g. 100*(rv_fwd_21d - iv_fair_21d) for a
// delta-hedged straddle. Supplied by the caller rather than computed here so
// the strategy layer never hardcodes a target.
//
// `veto`, when non-empty, is row-aligned with the frame and removes a row
// from ADMISSION when its value is finite and nonzero -- from the floor as
// well as the selection, because a veto changes the universe, and a floor
// computed on a different set than the selection would not be a paired
// comparison. NaN does not veto: it means "no information", and an unknown
// hazard is not a present one. The motivating column is imminent-earnings
// exclusion (f28 below a threshold), but the mechanism is any row predicate.
[[nodiscard]] inline Result<Scorecard>
run(const PanelFrame &frame, std::span<const DateSlice> dates, std::span<const double> score,
    std::span<const double> pnl, const StrategyConfig &cfg,
    std::span<const double> veto = {}) {
  if (cfg.max_names == 0) {
    return Err(atx::core::ErrorCode::InvalidArgument, "alpha::run: max_names must be positive");
  }
  if (!veto.empty() && veto.size() != frame.rows()) {
    return Err(atx::core::ErrorCode::InvalidArgument,
               "alpha::run: veto column has " + std::to_string(veto.size()) + " rows, frame has " +
                   std::to_string(frame.rows()));
  }
  const bool has_liq = frame.schema().has("liq_hspread_frac");
  std::span<const double> hspread;
  if (has_liq) {
    ATX_TRY(hspread, frame.numbers("liq_hspread_frac"));
  } else if (cfg.require_measured_liquidity) {
    return Err(atx::core::ErrorCode::NotFound,
               "alpha::run: require_measured_liquidity is set but the panel has no "
               "liq_hspread_frac column");
  }

  const auto cost_of = [&](std::size_t r) {
    const double w = has_liq ? hspread[r] : std::numeric_limits<double>::quiet_NaN();
    const bool liquid = std::isfinite(w) && w <= cfg.liquid_hspread_cut;
    return cfg.crossings * (liquid ? cfg.cost_vp_liquid : cfg.cost_vp_illiquid);
  };

  Scorecard card;
  for (const DateSlice &d : dates) {
    DateResult row;
    row.date = d.date;
    row.n_rows = d.rows.size();

    std::vector<std::size_t> admitted;
    admitted.reserve(d.rows.size());
    for (const std::size_t r : d.rows) {
      if (!std::isfinite(score[r]) || !std::isfinite(pnl[r])) {
        continue;
      }
      if (!veto.empty() && std::isfinite(veto[r]) && veto[r] != 0.0) {
        continue;
      }
      const double w = has_liq ? hspread[r] : std::numeric_limits<double>::quiet_NaN();
      if (!std::isfinite(w)) {
        if (cfg.require_measured_liquidity) {
          continue;
        }
      } else if (w > cfg.max_hspread_frac) {
        continue;
      }
      admitted.push_back(r);
    }
    row.n_admitted = admitted.size();
    // A date that cannot form both books forms neither: a floor computed on a
    // different admission set than the selection would make the paired excess
    // a comparison of two different universes.
    if (admitted.size() < 2) {
      continue;
    }

    std::vector<std::size_t> ranked = admitted;
    std::sort(ranked.begin(), ranked.end(), [&](std::size_t a, std::size_t b) {
      if (score[a] != score[b]) {
        return score[a] > score[b]; // high score == attractive to long vega
      }
      return a < b; // deterministic tie-break, never data-dependent
    });
    const std::size_t take = std::min(cfg.max_names, ranked.size());
    row.n_selected = take;

    double sg = 0.0;
    double sn = 0.0;
    for (std::size_t k = 0; k < take; ++k) {
      const std::size_t r = ranked[k];
      sg += pnl[r];
      sn += pnl[r] - cost_of(r);
    }
    row.selected_gross = sg / static_cast<double>(take);
    row.selected_net = sn / static_cast<double>(take);

    double fg = 0.0;
    double fn = 0.0;
    for (const std::size_t r : admitted) {
      fg += pnl[r];
      fn += pnl[r] - cost_of(r);
    }
    row.floor_gross = fg / static_cast<double>(admitted.size());
    row.floor_net = fn / static_cast<double>(admitted.size());

    row.excess_gross = row.selected_gross - row.floor_gross;
    row.excess_net = row.selected_net - row.floor_net;
    card.per_date.push_back(std::move(row));
  }

  card.n_dates = card.per_date.size();
  if (card.n_dates == 0) {
    return Err(atx::core::ErrorCode::OutOfRange,
               "alpha::run: no date formed both a selected book and a floor");
  }

  std::vector<double> excess_net;
  std::vector<double> excess_gross;
  std::vector<double> sel_net;
  excess_net.reserve(card.n_dates);
  excess_gross.reserve(card.n_dates);
  sel_net.reserve(card.n_dates);
  double floor_sum = 0.0;
  for (const DateResult &r : card.per_date) {
    excess_net.push_back(r.excess_net);
    excess_gross.push_back(r.excess_gross);
    sel_net.push_back(r.selected_net);
    floor_sum += r.floor_net;
  }
  const double dn = static_cast<double>(card.n_dates);
  card.mean_excess_net = std::accumulate(excess_net.begin(), excess_net.end(), 0.0) / dn;
  card.mean_excess_gross = std::accumulate(excess_gross.begin(), excess_gross.end(), 0.0) / dn;
  card.mean_selected_net = std::accumulate(sel_net.begin(), sel_net.end(), 0.0) / dn;
  card.mean_floor_net = floor_sum / dn;

  card.t_raw_excess_net = newey_west_t(excess_net, 0);
  const std::size_t lag = cfg.horizon_sessions > 0 ? cfg.horizon_sessions - 1 : 0;
  card.t_nw_excess_net = newey_west_t(excess_net, lag);
  card.t_nw_selected_net = newey_west_t(sel_net, lag);

  // Phase sweep: H disjoint sub-series, each sampling every H-th date, so
  // consecutive members of one phase share no holding window at all.
  const std::size_t H = cfg.horizon_sessions > 0 ? cfg.horizon_sessions : 1;
  std::size_t positive = 0;
  std::size_t counted = 0;
  double lo = std::numeric_limits<double>::infinity();
  double hi = -std::numeric_limits<double>::infinity();
  for (std::size_t p = 0; p < H; ++p) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = p; i < excess_net.size(); i += H) {
      sum += excess_net[i];
      ++n;
    }
    if (n == 0) {
      card.phase_mean_excess_net.push_back(std::numeric_limits<double>::quiet_NaN());
      continue;
    }
    const double m = sum / static_cast<double>(n);
    card.phase_mean_excess_net.push_back(m);
    ++counted;
    if (m > 0.0) {
      ++positive;
    }
    lo = std::min(lo, m);
    hi = std::max(hi, m);
  }
  card.phase_positive_fraction =
      counted == 0 ? 0.0 : static_cast<double>(positive) / static_cast<double>(counted);
  card.phase_min_mean = counted == 0 ? 0.0 : lo;
  card.phase_max_mean = counted == 0 ? 0.0 : hi;
  return Ok(std::move(card));
}

// ── The money axis, built from a panel's own columns ────────────────────────
//
// Delta-hedged ATM-forward straddle P&L per unit of vega, in vol points:
// 100*(rv_fwd_21d - iv_fair_21d). This is the first-order expression of the
// standard result that a continuously delta-hedged option's P&L is
// vega x (RV - IV); `api/backtest/vol_edge.hpp` prices the real instrument
// with a daily hedge and slippage, and this is the panel-level proxy the
// selection layer is graded on before that runs.
//
// IT CARRIES THE ENTRY MARK BY CONSTRUCTION -- that is what a vol trade IS --
// so `audit.hpp` reports an entry-mark channel for every feature reading
// iv_fair_21d[t] against it, and the cross-read on `rv_fwd_21d` is what
// separates the channel from the skill.
// The DECONTAMINATED cross-read of the axis above: forward realized vol alone,
// in vol points, with no implied leg anywhere in it.
//
// It is NOT a P&L and must never headline — nobody is paid `rv_fwd`. Its job is
// to answer the one question the money axis cannot: does this signal pick names
// that go on to REALIZE more vol, or does it only pick names whose entry mark
// was low? A selection excess that survives here is forecasting skill. One that
// evaporates was the entry-mark channel all along.
// The VOL-CHANGE axis: 100 * (rv_fwd_21d - rv_trail_21d), vol points.
//
// WHY A THIRD AXIS. The raw forward-RV cross-read decontaminates the money
// axis of its implied leg, but it is not itself clean -- it carries the VOL
// LEVEL. Selecting the highest-trailing-variance names beats an
// equal-weight-everything floor on forward RV almost by definition, because
// realized vol is persistent. Measured on the 616-name panel, a top-50 book
// sorted on trailing downside semivariance scored +32.998 vol points of
// forward-RV excess with 100% of phases positive -- and LOST 1.974 vol points
// on the money axis with 0% of phases positive. Volatility persistence is the
// most forecastable thing in the panel and it is already in the mark.
//
// Subtracting the trailing leg removes the level and leaves the CHANGE, which
// is the part a long-vega book is actually paid for. This is not a free lunch
// either: the axis now carries an explicit -rv_trail leg, so every trailing-
// realized-vol feature is mechanically related to it in the same way f3/f4/f5
// are related to the money axis. That is a channel, it is the mirror image of
// the entry-mark channel, and the honest move is to say so and report all
// three axes rather than to pick the flattering one.
[[nodiscard]] inline Result<std::vector<double>> vol_change_vol_points(const PanelFrame &frame) {
  ATX_TRY(const auto rv_fwd, frame.numbers("rv_fwd_21d"));
  ATX_TRY(const auto f2, frame.numbers("f2_log_rv21"));
  std::vector<double> out(frame.rows(), std::numeric_limits<double>::quiet_NaN());
  for (std::size_t r = 0; r < frame.rows(); ++r) {
    // f2 is ln(trailing 21-session annualized variance), so the trailing vol
    // is exp(f2/2). Read from the emitted column rather than recomputed, so
    // the axis is the panel's own trailing leg and not a second opinion.
    if (std::isfinite(rv_fwd[r]) && std::isfinite(f2[r])) {
      out[r] = 100.0 * (rv_fwd[r] - std::exp(0.5 * f2[r]));
    }
  }
  return Ok(std::move(out));
}

[[nodiscard]] inline Result<std::vector<double>> forward_rv_vol_points(const PanelFrame &frame) {
  ATX_TRY(const auto rv_fwd, frame.numbers("rv_fwd_21d"));
  std::vector<double> out(frame.rows(), std::numeric_limits<double>::quiet_NaN());
  for (std::size_t r = 0; r < frame.rows(); ++r) {
    if (std::isfinite(rv_fwd[r])) {
      out[r] = 100.0 * rv_fwd[r];
    }
  }
  return Ok(std::move(out));
}

[[nodiscard]] inline Result<std::vector<double>> dh_straddle_pnl_vol_points(const PanelFrame &frame) {
  ATX_TRY(const auto rv_fwd, frame.numbers("rv_fwd_21d"));
  ATX_TRY(const auto iv, frame.numbers("iv_fair_21d"));
  std::vector<double> out(frame.rows(), std::numeric_limits<double>::quiet_NaN());
  for (std::size_t r = 0; r < frame.rows(); ++r) {
    if (std::isfinite(rv_fwd[r]) && std::isfinite(iv[r])) {
      out[r] = 100.0 * (rv_fwd[r] - iv[r]);
    }
  }
  return Ok(std::move(out));
}

// The 63-session money axis: 100*(rv_fwd_63d - iv_fair_63d), the back-month
// leg Campasano & Linn (SSRN 2871616) show is re-marked LATE — the published
// escape from the front mark absorbing a vol forecast.
//
// The panel carries no rv_fwd_63d column, so the forward leg is computed here
// from the panel's own spot series. Two gates make that safe: the window must
// exist inside the series, and `window_contiguous` must hold over all 63
// forward steps — which folds in BOTH session adjacency and the consumer-side
// corporate-action step check, exactly the fictions a raw unadjusted spot
// series could otherwise smuggle into a 63-session variance.
[[nodiscard]] inline Result<std::vector<double>>
dh63_straddle_pnl_vol_points(const PanelFrame &frame) {
  ATX_TRY(const auto iv63, frame.numbers("iv_fair_63d"));
  ATX_TRY(const auto series, group_by_symbol(frame));
  std::vector<double> out(frame.rows(), std::numeric_limits<double>::quiet_NaN());
  for (const SymbolSeries &s : series) {
    for (std::size_t i = 0; i + 63 < s.size(); ++i) {
      if (!s.window_contiguous(i + 63, 63)) {
        continue;
      }
      const double rv = compute_detail::c2c_vol(s.spot, i + 63, 63);
      const double iv = iv63[s.row[i]];
      if (std::isfinite(rv) && std::isfinite(iv)) {
        out[s.row[i]] = 100.0 * (rv - iv);
      }
    }
  }
  return Ok(std::move(out));
}

} // namespace atx::vol::alpha
