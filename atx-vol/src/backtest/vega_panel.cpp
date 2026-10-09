// Cross-sectional vega panel: ATMF-strangle resolver, h-day daily-rehedged
// hold-PnL labeler, and the streaming per-symbol feature/label builder.
// See include/atx/vol/vega_panel.hpp for the module contract.

#include "backtest/vega_panel.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <sstream>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/vol/api/analytics/analytics.hpp"        // atmf_vol, skew_curvature, risk_reversal, ...
#include "atx/vol/api/backtest/portfolio_pricer.hpp" // kNsPerYear
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "analytics/realized_vol.hpp" // OhlcBar, realized_vol
#include "atx/vol/api/backtest/strategy.hpp"     // resolve_strike_by_delta

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTenor1m = 30.0 / 365.25;
constexpr double kTenor3m = 91.0 / 365.25;
constexpr double kTenor1y = 1.0;
constexpr double kWingDelta = 0.25; // |delta| for the rr25/bf25 columns

// v.back() − v[n−1−k]; NaN until the lag is observable.
[[nodiscard]] double lag_diff(const std::vector<double> &v, std::size_t k) noexcept {
  if (v.size() < k + 1) {
    return kNaN;
  }
  return v.back() - v[v.size() - 1 - k];
}

// ln(S_t / S_{t−k}); NaN until observable or on a non-positive close.
[[nodiscard]] double lag_log_return(const std::vector<double> &v, std::size_t k) noexcept {
  if (v.size() < k + 1) {
    return kNaN;
  }
  const double a = v[v.size() - 1 - k];
  const double b = v.back();
  if (!(a > 0.0) || !(b > 0.0)) {
    return kNaN;
  }
  return std::log(b / a);
}

// Close-to-close realized vol over the trailing `window` returns (window+1
// closes, entry day inclusive). NaN until the window is observable.
[[nodiscard]] double trailing_rv(const std::vector<std::int64_t> &ts,
                                 const std::vector<double> &closes, std::size_t window) {
  if (closes.size() < window + 1) {
    return kNaN;
  }
  std::vector<OhlcBar> bars;
  bars.reserve(window + 1);
  const std::size_t lo = closes.size() - (window + 1);
  for (std::size_t i = lo; i < closes.size(); ++i) {
    const double c = closes[i];
    bars.push_back(OhlcBar{ts[i], c, c, c, c});
  }
  const auto rv = realized_vol(bars, RvEstimator::CloseToClose, 252.0);
  return rv.has_value() ? *rv : kNaN;
}

// Sample stdev of consecutive diffs over the trailing `n_diffs` diffs (needs
// n_diffs+1 observations, all finite). NaN until observable.
[[nodiscard]] double trailing_diff_stdev(const std::vector<double> &v,
                                         std::size_t n_diffs) noexcept {
  if (v.size() < n_diffs + 1 || n_diffs < 2) {
    return kNaN;
  }
  const std::size_t lo = v.size() - (n_diffs + 1);
  double mean = 0.0;
  for (std::size_t i = lo; i + 1 < v.size(); ++i) {
    const double d = v[i + 1] - v[i];
    if (!std::isfinite(d)) {
      return kNaN;
    }
    mean += d;
  }
  mean /= static_cast<double>(n_diffs);
  double ss = 0.0;
  for (std::size_t i = lo; i + 1 < v.size(); ++i) {
    const double d = (v[i + 1] - v[i]) - mean;
    ss += d * d;
  }
  return std::sqrt(ss / static_cast<double>(n_diffs - 1));
}

// Percentile rank of v.back() within its trailing `window` values (entry day
// inclusive): strictly-below count / (window − 1), in [0, 1]. Ties rank low.
// NaN until observable or when any value in the window is non-finite (a
// partially-measurable rank would silently mix information sets).
[[nodiscard]] double trailing_rank_pct(const std::vector<double> &v, std::size_t window) noexcept {
  if (window < 2 || v.size() < window) {
    return kNaN;
  }
  const double cur = v.back();
  if (!std::isfinite(cur)) {
    return kNaN;
  }
  const std::size_t lo = v.size() - window;
  std::size_t below = 0;
  for (std::size_t i = lo; i + 1 < v.size(); ++i) {
    if (!std::isfinite(v[i])) {
      return kNaN;
    }
    below += (v[i] < cur) ? 1u : 0u;
  }
  return static_cast<double>(below) / static_cast<double>(window - 1);
}

[[nodiscard]] double value_or_nan(const Result<double> &r) noexcept {
  return r.has_value() ? *r : kNaN;
}

// Σ qty·P of `s` on `mark`, each leg's T re-derived from its pinned expiry.
// InvalidArgument on an expired leg — never a fabricated post-expiry mark.
[[nodiscard]] Result<double> structure_value_at(const ResolvedStructure &s,
                                                const PricedSurface &mark) {
  const std::int64_t ts = mark.pricing().now_ts_ns;
  double value = 0.0;
  for (const StructureLeg &leg : s.legs) {
    const double T = static_cast<double>(leg.expiry_ts_ns - ts) / kNsPerYear;
    if (!(T > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "vega_panel: leg expired at mark");
    }
    auto px = mark.fair_value(leg.strike, T, leg.side);
    if (!px.has_value()) {
      return Err(px.error());
    }
    value += leg.qty * *px;
  }
  return Ok(value);
}

// Σ qty·delta of `s` on `mark` — the rehedge ratio for the NEXT session.
[[nodiscard]] Result<double> structure_delta_at(const ResolvedStructure &s,
                                                const PricedSurface &mark) {
  const std::int64_t ts = mark.pricing().now_ts_ns;
  double net = 0.0;
  for (const StructureLeg &leg : s.legs) {
    const double T = static_cast<double>(leg.expiry_ts_ns - ts) / kNsPerYear;
    if (!(T > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "vega_panel: leg expired at mark");
    }
    auto d = mark.delta(leg.strike, T, leg.side);
    if (!d.has_value()) {
      return Err(d.error());
    }
    net += leg.qty * *d;
  }
  return Ok(net);
}

// Numeric column schema — single source of truth for the TSV header and row
// emitters so they cannot drift apart.
struct NumCol {
  const char *name;
  double VegaPanelRow::*member;
};

constexpr NumCol kNumCols[] = {
    {"spot", &VegaPanelRow::spot},
    {"r", &VegaPanelRow::r},
    {"iv_1m", &VegaPanelRow::iv_1m},
    {"iv_3m", &VegaPanelRow::iv_3m},
    {"iv_1y", &VegaPanelRow::iv_1y},
    {"term_slope_1m_1y", &VegaPanelRow::term_slope_1m_1y},
    {"fwd_vol_1m_1y", &VegaPanelRow::fwd_vol_1m_1y},
    {"skew_1m", &VegaPanelRow::skew_1m},
    {"curv_1m", &VegaPanelRow::curv_1m},
    {"skew_1y", &VegaPanelRow::skew_1y},
    {"curv_1y", &VegaPanelRow::curv_1y},
    {"rr25_1y", &VegaPanelRow::rr25_1y},
    {"bf25_1y", &VegaPanelRow::bf25_1y},
    {"rv_21", &VegaPanelRow::rv_21},
    {"rv_63", &VegaPanelRow::rv_63},
    {"rv_252", &VegaPanelRow::rv_252},
    {"ivrv_1y_21", &VegaPanelRow::ivrv_1y_21},
    {"ivrv_1y_63", &VegaPanelRow::ivrv_1y_63},
    {"ret_21d", &VegaPanelRow::ret_21d},
    {"div_1y_21", &VegaPanelRow::div_1y_21},
    {"vol_of_vol_21", &VegaPanelRow::vol_of_vol_21},
    {"iv_1y_rank_252", &VegaPanelRow::iv_1y_rank_252},
    {"entry_vega", &VegaPanelRow::entry_vega},
    {"entry_gamma", &VegaPanelRow::entry_gamma},
    {"entry_theta", &VegaPanelRow::entry_theta},
    {"entry_delta_net", &VegaPanelRow::entry_delta_net},
    {"strike_call", &VegaPanelRow::strike_call},
    {"strike_put", &VegaPanelRow::strike_put},
    {"label_pnl_h", &VegaPanelRow::label_pnl_h},
    {"label_pnl_1d", &VegaPanelRow::label_pnl_1d},
};

} // namespace

Result<ResolvedStructure> resolve_atmf_strangle(const PricedSurface &entry, double target_T,
                                                double target_abs_delta, double vega_target,
                                                int sign) {
  if (!(target_T > 0.0) || !std::isfinite(target_T)) {
    return Err(ErrorCode::InvalidArgument, "resolve_atmf_strangle: target_T must be finite > 0");
  }
  if (!std::isfinite(target_abs_delta) || !(target_abs_delta > 0.0 && target_abs_delta < 1.0)) {
    return Err(ErrorCode::InvalidArgument,
               "resolve_atmf_strangle: target |delta| must lie in (0,1)");
  }
  if (!(vega_target > 0.0) || !std::isfinite(vega_target)) {
    return Err(ErrorCode::InvalidArgument, "resolve_atmf_strangle: vega_target must be finite > 0");
  }
  if (sign != +1 && sign != -1) {
    return Err(ErrorCode::InvalidArgument, "resolve_atmf_strangle: sign must be +1 or -1");
  }
  const double F = entry.forward_at(target_T);
  if (!(F > 0.0) || !std::isfinite(F)) {
    return Err(ErrorCode::InvalidArgument, "resolve_atmf_strangle: degenerate forward");
  }
  // Delta-targeted strikes via the library's American-delta solver
  // (strategy.hpp): call leg at +target, put leg at −target. An unreachable
  // target is its Err — fail-soft at the caller, never a fabricated strike.
  auto k_call = resolve_strike_by_delta(entry, target_T, Side::Call, target_abs_delta);
  if (!k_call.has_value()) {
    return Err(k_call.error());
  }
  auto k_put = resolve_strike_by_delta(entry, target_T, Side::Put, target_abs_delta);
  if (!k_put.has_value()) {
    return Err(k_put.error());
  }
  auto call = entry.greeks(*k_call, target_T, Side::Call);
  if (!call.has_value()) {
    return Err(call.error());
  }
  auto put = entry.greeks(*k_put, target_T, Side::Put);
  if (!put.has_value()) {
    return Err(put.error());
  }
  const double vega_sum = call->vega + put->vega;
  if (!(vega_sum > 0.0) || !std::isfinite(vega_sum)) {
    return Err(ErrorCode::InvalidArgument, "resolve_atmf_strangle: non-positive structure vega");
  }
  const double qty = static_cast<double>(sign) * vega_target / vega_sum;
  const std::int64_t now_ts = entry.pricing().now_ts_ns;
  const auto expiry = now_ts + static_cast<std::int64_t>(std::llround(target_T * kNsPerYear));

  ResolvedStructure rs;
  rs.legs.push_back(StructureLeg{*k_call, expiry, Side::Call, qty});
  rs.legs.push_back(StructureLeg{*k_put, expiry, Side::Put, qty});
  rs.entry_ts_ns = now_ts;
  rs.spot = entry.pricing().S;
  rs.entry_value = qty * (call->price + put->price);
  rs.entry_delta = qty * (call->delta + put->delta);
  rs.entry_gamma = qty * (call->gamma + put->gamma);
  rs.entry_vega = qty * vega_sum;
  rs.entry_theta = qty * (call->theta + put->theta);
  rs.entry_vanna = qty * (call->vanna + put->vanna);
  rs.entry_volga = qty * (call->volga + put->volga);
  return Ok(std::move(rs));
}

Result<std::vector<double>> hedged_daily_pnls(const ResolvedStructure &s,
                                              std::span<const PricedSurface *const> marks) {
  if (s.legs.empty()) {
    return Err(ErrorCode::InvalidArgument, "hedged_daily_pnls: empty structure");
  }
  if (marks.empty()) {
    return Err(ErrorCode::InvalidArgument, "hedged_daily_pnls: no marks");
  }
  std::vector<double> pnls;
  pnls.reserve(marks.size());
  double prev_value = s.entry_value;
  double prev_delta = s.entry_delta;
  double prev_spot = s.spot;
  std::int64_t prev_ts = s.entry_ts_ns;
  // Bounded by marks.size() — the caller's hold horizon.
  for (std::size_t t = 0; t < marks.size(); ++t) {
    const PricedSurface *mark = marks[t];
    if (mark == nullptr) {
      return Err(ErrorCode::InvalidArgument, "hedged_daily_pnls: null mark");
    }
    const std::int64_t ts = mark->pricing().now_ts_ns;
    if (ts <= prev_ts) {
      return Err(ErrorCode::InvalidArgument,
                 "hedged_daily_pnls: marks must strictly ascend past the entry");
    }
    const auto value = structure_value_at(s, *mark);
    if (!value.has_value()) {
      return Err(value.error());
    }
    const double S = mark->pricing().S;
    pnls.push_back((*value - prev_value) - prev_delta * (S - prev_spot));
    if (t + 1 < marks.size()) { // the final session never rehedges
      const auto delta = structure_delta_at(s, *mark);
      if (!delta.has_value()) {
        return Err(delta.error());
      }
      prev_delta = *delta;
    }
    prev_value = *value;
    prev_spot = S;
    prev_ts = ts;
  }
  return Ok(std::move(pnls));
}

Result<double> hedged_hold_pnl(const ResolvedStructure &s,
                               std::span<const PricedSurface *const> marks) {
  const auto pnls = hedged_daily_pnls(s, marks);
  if (!pnls.has_value()) {
    return Err(pnls.error());
  }
  double sum = 0.0;
  for (const double p : *pnls) {
    sum += p;
  }
  return Ok(sum);
}

std::string vega_panel_tsv_header() {
  std::ostringstream os;
  os << "key\tsymbol";
  for (const NumCol &c : kNumCols) {
    os << '\t' << c.name;
  }
  os << "\tlabel_valid";
  return os.str();
}

std::string to_tsv_line(const VegaPanelRow &row) {
  std::ostringstream os;
  os.precision(12);
  os << row.key << '\t' << row.symbol;
  for (const NumCol &c : kNumCols) {
    os << '\t' << row.*(c.member);
  }
  os << '\t' << (row.label_valid ? 1 : 0);
  return os.str();
}

VegaPanelBuilder::VegaPanelBuilder(VegaPanelConfig cfg, std::string symbol)
    : cfg_(cfg), symbol_(std::move(symbol)) {}

std::vector<VegaPanelRow> VegaPanelBuilder::finish() {
  std::vector<VegaPanelRow> rows;
  rows.reserve(pending_.size());
  while (!pending_.empty()) {
    VegaPanelRow row = std::move(pending_.front().row);
    pending_.pop_front();
    row.label_pnl_h = kNaN;
    row.label_pnl_1d = kNaN;
    row.label_valid = false;
    rows.push_back(std::move(row));
  }
  return rows;
}

Result<std::optional<VegaPanelRow>> VegaPanelBuilder::push(const std::string &key,
                                                           const PricedSurface &surf) {
  if (!(cfg_.tenor_T > 0.0) || !std::isfinite(cfg_.tenor_T) ||
      !(cfg_.target_abs_delta > 0.0 && cfg_.target_abs_delta < 1.0) || !(cfg_.vega_target > 0.0) ||
      cfg_.horizon_sessions < 1) {
    return Err(ErrorCode::InvalidArgument, "VegaPanelBuilder: invalid config");
  }
  if (key.empty() || (!last_key_.empty() && key <= last_key_)) {
    return Err(ErrorCode::InvalidArgument,
               "VegaPanelBuilder: keys must be non-empty strictly ascending");
  }
  const std::int64_t ts = surf.pricing().now_ts_ns;
  if (ts <= 0 || ts <= last_ts_) {
    return Err(ErrorCode::InvalidArgument,
               "VegaPanelBuilder: valuation ts must be positive strictly ascending");
  }
  const double S = surf.pricing().S;

  // 1. Mark every pending entry against today's surface (fail-soft). The
  // rehedge is INCREMENTAL: (value, net delta, spot) cached from the previous
  // push stand in for the previous surface, so none is ever retained.
  for (Pending &p : pending_) {
    ++p.marks_seen;
    if (p.failed || !p.strangle.has_value()) {
      continue;
    }
    const auto value = structure_value_at(*p.strangle, surf);
    if (!value.has_value()) {
      p.failed = true;
      ++skipped_;
      continue;
    }
    p.pnl += (*value - p.prev_value) - p.prev_delta * (S - p.prev_spot);
    if (p.marks_seen == 1) {
      p.pnl_1d = p.pnl;
    }
    if (p.marks_seen < cfg_.horizon_sessions) { // the final mark never rehedges
      const auto delta = structure_delta_at(*p.strangle, surf);
      if (!delta.has_value()) {
        p.failed = true;
        ++skipped_;
        continue;
      }
      p.prev_delta = *delta;
    }
    p.prev_value = *value;
    p.prev_spot = S;
  }

  // 2. Complete the oldest entry once its horizon filled (FIFO: only the
  // front can reach the horizon on any push).
  std::optional<VegaPanelRow> completed;
  if (!pending_.empty() && pending_.front().marks_seen >= cfg_.horizon_sessions) {
    Pending done = std::move(pending_.front());
    pending_.pop_front();
    VegaPanelRow row = std::move(done.row);
    if (!done.failed && done.strangle.has_value()) {
      row.label_pnl_h = done.pnl;
      row.label_pnl_1d = done.pnl_1d;
      row.label_valid = true;
    } else {
      row.label_pnl_h = kNaN;
      row.label_pnl_1d = kNaN;
      row.label_valid = false;
    }
    completed = std::move(row);
  }

  // 3. Append today's closes to the trailing history (entry day inclusive).
  const double iv1m = atmf_vol(surf, kTenor1m);
  const double iv3m = atmf_vol(surf, kTenor3m);
  const double iv1y = atmf_vol(surf, kTenor1y);
  ts_hist_.push_back(ts);
  spot_hist_.push_back(S);
  iv1y_hist_.push_back(iv1y);

  // 4. Entry-day features.
  VegaPanelRow row;
  row.key = key;
  row.symbol = symbol_;
  row.spot = S;
  row.r = surf.pricing().r;
  row.iv_1m = iv1m;
  row.iv_3m = iv3m;
  row.iv_1y = iv1y;
  row.term_slope_1m_1y = iv1y - iv1m;
  row.fwd_vol_1m_1y = forward_vol(surf, kTenor1m, kTenor1y);

  const auto skew_at = [&surf](double T, double atm, double &skew_out, double &curv_out) {
    const double k_ref = atm * std::sqrt(T);
    if (k_ref > 0.0 && std::isfinite(k_ref)) {
      const SkewCurvature sc = skew_curvature(surf, T, k_ref);
      skew_out = sc.valid ? sc.skew_slope : kNaN;
      curv_out = sc.valid ? sc.curvature : kNaN;
    } else {
      skew_out = kNaN;
      curv_out = kNaN;
    }
  };
  skew_at(kTenor1m, iv1m, row.skew_1m, row.curv_1m);
  skew_at(kTenor1y, iv1y, row.skew_1y, row.curv_1y);
  row.rr25_1y = value_or_nan(risk_reversal(surf, kTenor1y, kWingDelta));
  row.bf25_1y = value_or_nan(butterfly(surf, kTenor1y, kWingDelta));

  // 5. Trailing-history features.
  row.rv_21 = trailing_rv(ts_hist_, spot_hist_, 21);
  row.rv_63 = trailing_rv(ts_hist_, spot_hist_, 63);
  row.rv_252 = trailing_rv(ts_hist_, spot_hist_, 252);
  row.ivrv_1y_21 = iv1y - row.rv_21;
  row.ivrv_1y_63 = iv1y - row.rv_63;
  row.ret_21d = lag_log_return(spot_hist_, 21);
  row.div_1y_21 = lag_diff(iv1y_hist_, 21);
  row.vol_of_vol_21 = trailing_diff_stdev(iv1y_hist_, 21);
  row.iv_1y_rank_252 = trailing_rank_pct(iv1y_hist_, 252);

  // 6. Resolve today's strangle (fail-soft: label of THIS row).
  Pending pending;
  auto strangle =
      resolve_atmf_strangle(surf, cfg_.tenor_T, cfg_.target_abs_delta, cfg_.vega_target, +1);
  if (strangle.has_value()) {
    row.entry_vega = strangle->entry_vega;
    row.entry_gamma = strangle->entry_gamma;
    row.entry_theta = strangle->entry_theta;
    row.entry_delta_net = strangle->entry_delta;
    row.strike_call = strangle->legs[0].strike;
    row.strike_put = strangle->legs[1].strike;
    pending.prev_value = strangle->entry_value;
    pending.prev_delta = strangle->entry_delta;
    pending.prev_spot = strangle->spot;
    pending.strangle = std::move(*strangle);
  } else {
    row.entry_vega = kNaN;
    row.entry_gamma = kNaN;
    row.entry_theta = kNaN;
    row.entry_delta_net = kNaN;
    row.strike_call = kNaN;
    row.strike_put = kNaN;
    pending.failed = true;
    ++skipped_;
  }
  row.label_pnl_h = kNaN;
  row.label_pnl_1d = kNaN;
  row.label_valid = false;
  pending.row = std::move(row);
  pending_.push_back(std::move(pending));
  last_key_ = key;
  last_ts_ = ts;
  return Ok(std::move(completed));
}

} // namespace atx::vol
