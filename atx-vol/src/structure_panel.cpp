// Daily one-day-hold structure PnL + streaming feature/label panel builder.
// See include/atx/vol/structure_panel.hpp for the module contract.

#include "atx/vol/structure_panel.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <sstream>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/vol/analytics.hpp"        // atmf_vol, skew_curvature, risk_reversal, ...
#include "atx/vol/portfolio_pricer.hpp" // kNsPerYear
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/realized_vol.hpp" // OhlcBar, realized_vol

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTenor1w = 7.0 / 365.25;
constexpr double kTenor1m = 30.0 / 365.25;
constexpr double kTenor3m = 91.0 / 365.25;
constexpr double kTenor1y = 1.0;

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

// Mean of the trailing `window` values; NaN until observable or if any entry
// in the window is non-finite (a partially-measurable mean would silently mix
// regimes with different information sets).
[[nodiscard]] double trailing_mean(const std::vector<double> &v, std::size_t window) noexcept {
  if (v.size() < window) {
    return kNaN;
  }
  const std::size_t lo = v.size() - window;
  double sum = 0.0;
  for (std::size_t i = lo; i < v.size(); ++i) {
    if (!std::isfinite(v[i])) {
      return kNaN;
    }
    sum += v[i];
  }
  return sum / static_cast<double>(window);
}

[[nodiscard]] double value_or_nan(const Result<double> &r) noexcept {
  return r.has_value() ? *r : kNaN;
}

// Numeric column schema — single source of truth for the TSV header and row
// emitters so they cannot drift apart.
struct NumCol {
  const char *name;
  double PanelRow::*member;
};

constexpr NumCol kNumCols[] = {
    {"spot", &PanelRow::spot},
    {"r", &PanelRow::r},
    {"iv_1w", &PanelRow::iv_1w},
    {"iv_1m", &PanelRow::iv_1m},
    {"iv_3m", &PanelRow::iv_3m},
    {"iv_1y", &PanelRow::iv_1y},
    {"short_slope", &PanelRow::short_slope},
    {"vsw_1m", &PanelRow::vsw_1m},
    {"vsw_1y", &PanelRow::vsw_1y},
    {"vsw_conv_1m", &PanelRow::vsw_conv_1m},
    {"term_slope", &PanelRow::term_slope},
    {"fwd_vol_front_back", &PanelRow::fwd_vol_front_back},
    {"fwd_minus_front", &PanelRow::fwd_minus_front},
    {"skew_1m", &PanelRow::skew_1m},
    {"curv_1m", &PanelRow::curv_1m},
    {"skew_1y", &PanelRow::skew_1y},
    {"curv_1y", &PanelRow::curv_1y},
    {"rr25_1m", &PanelRow::rr25_1m},
    {"bf25_1m", &PanelRow::bf25_1m},
    {"rr25_1y", &PanelRow::rr25_1y},
    {"bf25_1y", &PanelRow::bf25_1y},
    {"rv5", &PanelRow::rv5},
    {"rv21", &PanelRow::rv21},
    {"rv63", &PanelRow::rv63},
    {"ivrv_1m_21", &PanelRow::ivrv_1m_21},
    {"ivrv_1y_63", &PanelRow::ivrv_1y_63},
    {"ret_1d", &PanelRow::ret_1d},
    {"ret_5d", &PanelRow::ret_5d},
    {"ret_21d", &PanelRow::ret_21d},
    {"div_1m_1d", &PanelRow::div_1m_1d},
    {"div_1m_5d", &PanelRow::div_1m_5d},
    {"div_1m_21d", &PanelRow::div_1m_21d},
    {"dslope_1d", &PanelRow::dslope_1d},
    {"dslope_5d", &PanelRow::dslope_5d},
    {"vol_of_vol_21", &PanelRow::vol_of_vol_21},
    {"vrp_mean_63", &PanelRow::vrp_mean_63},
    {"front_gamma", &PanelRow::front_gamma},
    {"front_theta", &PanelRow::front_theta},
    {"front_delta", &PanelRow::front_delta},
    {"front_vanna", &PanelRow::front_vanna},
    {"front_volga", &PanelRow::front_volga},
    {"back_gamma", &PanelRow::back_gamma},
    {"back_theta", &PanelRow::back_theta},
    {"back_delta", &PanelRow::back_delta},
    {"back_vanna", &PanelRow::back_vanna},
    {"back_volga", &PanelRow::back_volga},
    {"pnl_front", &PanelRow::pnl_front},
    {"pnl_back", &PanelRow::pnl_back},
};

} // namespace

Result<ResolvedStructure> resolve_atmf_straddle(const PricedSurface &entry, double target_T,
                                                double vega_target, double sign) {
  if (!(target_T > 0.0) || !std::isfinite(target_T)) {
    return Err(ErrorCode::InvalidArgument, "resolve_atmf_straddle: target_T must be finite > 0");
  }
  if (!(vega_target > 0.0) || !std::isfinite(vega_target)) {
    return Err(ErrorCode::InvalidArgument, "resolve_atmf_straddle: vega_target must be finite > 0");
  }
  if (!(sign == +1.0 || sign == -1.0)) {
    return Err(ErrorCode::InvalidArgument, "resolve_atmf_straddle: sign must be +1 or -1");
  }
  const double F = entry.forward_at(target_T);
  if (!(F > 0.0) || !std::isfinite(F)) {
    return Err(ErrorCode::InvalidArgument, "resolve_atmf_straddle: degenerate forward");
  }
  auto call = entry.greeks(F, target_T, Side::Call);
  if (!call.has_value()) {
    return Err(call.error());
  }
  auto put = entry.greeks(F, target_T, Side::Put);
  if (!put.has_value()) {
    return Err(put.error());
  }
  const double vega_sum = call->vega + put->vega;
  if (!(vega_sum > 0.0) || !std::isfinite(vega_sum)) {
    return Err(ErrorCode::InvalidArgument, "resolve_atmf_straddle: non-positive structure vega");
  }
  const double qty = sign * vega_target / vega_sum;
  const std::int64_t now_ts = entry.pricing().now_ts_ns;
  const auto expiry = now_ts + static_cast<std::int64_t>(std::llround(target_T * kNsPerYear));

  ResolvedStructure rs;
  rs.legs.push_back(StructureLeg{F, expiry, Side::Call, qty});
  rs.legs.push_back(StructureLeg{F, expiry, Side::Put, qty});
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

Result<double> delta_neutral_pnl(const ResolvedStructure &s, const PricedSurface &mark) {
  if (s.legs.empty()) {
    return Err(ErrorCode::InvalidArgument, "delta_neutral_pnl: empty structure");
  }
  const std::int64_t mark_ts = mark.pricing().now_ts_ns;
  if (mark_ts <= s.entry_ts_ns) {
    return Err(ErrorCode::InvalidArgument, "delta_neutral_pnl: mark must postdate entry");
  }
  double value = 0.0;
  for (const StructureLeg &leg : s.legs) {
    const double T = static_cast<double>(leg.expiry_ts_ns - mark_ts) / kNsPerYear;
    if (!(T > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "delta_neutral_pnl: leg expired at mark");
    }
    auto px = mark.fair_value(leg.strike, T, leg.side);
    if (!px.has_value()) {
      return Err(px.error());
    }
    value += leg.qty * *px;
  }
  const double d_spot = mark.pricing().S - s.spot;
  return Ok(value - s.entry_value - s.entry_delta * d_spot);
}

std::string panel_tsv_header() {
  std::ostringstream os;
  os << "key";
  for (const NumCol &c : kNumCols) {
    os << '\t' << c.name;
  }
  os << "\tpnl_valid";
  return os.str();
}

std::string to_tsv_line(const PanelRow &row) {
  std::ostringstream os;
  os.precision(12);
  os << row.key;
  for (const NumCol &c : kNumCols) {
    os << '\t' << row.*(c.member);
  }
  os << '\t' << (row.pnl_valid ? 1 : 0);
  return os.str();
}

StructurePanelBuilder::StructurePanelBuilder(StructurePanelConfig cfg) : cfg_(cfg) {}

std::optional<PanelRow> StructurePanelBuilder::finish() {
  if (!prev_.has_value()) {
    return std::nullopt;
  }
  PanelRow row = std::move(prev_->row);
  row.pnl_front = kNaN;
  row.pnl_back = kNaN;
  row.pnl_valid = false;
  prev_.reset();
  return row;
}

Result<std::optional<PanelRow>> StructurePanelBuilder::push(const std::string &key,
                                                            const PricedSurface &surf) {
  if (!(cfg_.front_T > 0.0) || !(cfg_.back_T > cfg_.front_T) || !(cfg_.vega_target > 0.0) ||
      !(cfg_.rr_delta > 0.0 && cfg_.rr_delta < 1.0)) {
    return Err(ErrorCode::InvalidArgument, "StructurePanelBuilder: invalid config");
  }
  if (key.empty() || (!last_key_.empty() && key <= last_key_)) {
    return Err(ErrorCode::InvalidArgument,
               "StructurePanelBuilder: keys must be non-empty strictly ascending");
  }
  const std::int64_t ts = surf.pricing().now_ts_ns;
  if (ts <= 0 || (!ts_hist_.empty() && ts <= ts_hist_.back())) {
    return Err(ErrorCode::InvalidArgument,
               "StructurePanelBuilder: valuation ts must be positive strictly ascending");
  }

  // 1. Complete the previous day's row against today's marks (fail-soft).
  std::optional<PanelRow> completed;
  if (prev_.has_value()) {
    PanelRow row = std::move(prev_->row);
    row.pnl_front = kNaN;
    row.pnl_back = kNaN;
    row.pnl_valid = false;
    if (prev_->front.has_value() && prev_->back.has_value()) {
      const auto pf = delta_neutral_pnl(*prev_->front, surf);
      const auto pb = delta_neutral_pnl(*prev_->back, surf);
      if (pf.has_value() && pb.has_value()) {
        row.pnl_front = *pf;
        row.pnl_back = *pb;
        row.pnl_valid = true;
      }
    }
    if (!row.pnl_valid) {
      ++skipped_;
    }
    completed = std::move(row);
    prev_.reset();
  }

  // 2. Append today's closes to the trailing history (entry day inclusive).
  const double S = surf.pricing().S;
  const double iv1m = atmf_vol(surf, kTenor1m);
  const double iv3m = atmf_vol(surf, kTenor3m);
  const double iv1y = atmf_vol(surf, kTenor1y);
  ts_hist_.push_back(ts);
  spot_hist_.push_back(S);
  iv1m_hist_.push_back(iv1m);
  slope_hist_.push_back(iv1y - iv1m);

  // 3. Entry-day features.
  PanelRow row;
  row.key = key;
  row.spot = S;
  row.r = surf.pricing().r;
  row.iv_1w = atmf_vol(surf, kTenor1w);
  row.iv_1m = iv1m;
  row.iv_3m = iv3m;
  row.iv_1y = iv1y;
  row.short_slope = iv1m - row.iv_1w;
  row.vsw_1m = value_or_nan(var_swap_vol(surf, kTenor1m));
  row.vsw_1y = value_or_nan(var_swap_vol(surf, kTenor1y));
  row.vsw_conv_1m = row.vsw_1m - iv1m;
  row.term_slope = iv1y - iv1m;
  row.fwd_vol_front_back = forward_vol(surf, cfg_.front_T, cfg_.back_T);
  row.fwd_minus_front = row.fwd_vol_front_back - atmf_vol(surf, cfg_.front_T);

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
  row.rr25_1m = value_or_nan(risk_reversal(surf, kTenor1m, cfg_.rr_delta));
  row.bf25_1m = value_or_nan(butterfly(surf, kTenor1m, cfg_.rr_delta));
  row.rr25_1y = value_or_nan(risk_reversal(surf, kTenor1y, cfg_.rr_delta));
  row.bf25_1y = value_or_nan(butterfly(surf, kTenor1y, cfg_.rr_delta));

  // 4. Trailing-history features.
  row.rv5 = trailing_rv(ts_hist_, spot_hist_, 5);
  row.rv21 = trailing_rv(ts_hist_, spot_hist_, 21);
  row.rv63 = trailing_rv(ts_hist_, spot_hist_, 63);
  row.ivrv_1m_21 = iv1m - row.rv21;
  row.ivrv_1y_63 = iv1y - row.rv63;
  row.ret_1d = lag_log_return(spot_hist_, 1);
  row.ret_5d = lag_log_return(spot_hist_, 5);
  row.ret_21d = lag_log_return(spot_hist_, 21);
  row.div_1m_1d = lag_diff(iv1m_hist_, 1);
  row.div_1m_5d = lag_diff(iv1m_hist_, 5);
  row.div_1m_21d = lag_diff(iv1m_hist_, 21);
  row.dslope_1d = lag_diff(slope_hist_, 1);
  row.dslope_5d = lag_diff(slope_hist_, 5);
  row.vol_of_vol_21 = trailing_diff_stdev(iv1m_hist_, 21);
  ivrv_hist_.push_back(row.ivrv_1m_21);
  row.vrp_mean_63 = trailing_mean(ivrv_hist_, 63);

  // 5. Resolve today's structures (fail-soft: label of THIS row).
  Pending pending;
  auto front = resolve_atmf_straddle(surf, cfg_.front_T, cfg_.vega_target, +1.0);
  auto back = resolve_atmf_straddle(surf, cfg_.back_T, cfg_.vega_target, +1.0);
  if (front.has_value()) {
    row.front_gamma = front->entry_gamma;
    row.front_theta = front->entry_theta;
    row.front_delta = front->entry_delta;
    row.front_vanna = front->entry_vanna;
    row.front_volga = front->entry_volga;
    pending.front = std::move(*front);
  } else {
    row.front_gamma = kNaN;
    row.front_theta = kNaN;
    row.front_delta = kNaN;
    row.front_vanna = kNaN;
    row.front_volga = kNaN;
  }
  if (back.has_value()) {
    row.back_gamma = back->entry_gamma;
    row.back_theta = back->entry_theta;
    row.back_delta = back->entry_delta;
    row.back_vanna = back->entry_vanna;
    row.back_volga = back->entry_volga;
    pending.back = std::move(*back);
  } else {
    row.back_gamma = kNaN;
    row.back_theta = kNaN;
    row.back_delta = kNaN;
    row.back_vanna = kNaN;
    row.back_volga = kNaN;
  }
  pending.row = std::move(row);
  prev_ = std::move(pending);
  last_key_ = key;
  return Ok(std::move(completed));
}

} // namespace atx::vol
