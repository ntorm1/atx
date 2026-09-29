#include "strategy_risk_model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <locale>
#include <numbers>
#include <numeric>
#include <sstream>
#include <utility>
#include <Eigen/Dense>

namespace atx::impl::strategy::risk {
namespace {
using namespace atx;
namespace co = atx::core;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr usize npos = std::numeric_limits<usize>::max();
constexpr usize style_si_ratio = 5, style_dtc = 6; // descriptor indices of the SI composite

bool finite_positive(f64 x) { return std::isfinite(x) && x > 0; }
Eigen::Index ix(usize k) { return static_cast<Eigen::Index>(k); }
// Same interval guard as the NAV replay and the price exposures.
bool guarded_move(f64 close_a, f64 close_b, f64 raw_a, f64 raw_b) {
  const f64 adjusted = std::log(close_b) - std::log(close_a);
  const f64 raw = std::log(raw_b) - std::log(raw_a);
  return !std::isfinite(adjusted) || std::abs(adjusted) > 1.5 ||
         std::abs(adjusted) > std::abs(raw) + .10;
}
// x_i f over the finite factors of a row (inactive factors contribute 0).
f64 predict(const std::array<f64, factor_count>& f, u8 slot, std::span<const f64> z) {
  f64 value = std::isfinite(f[0]) ? f[0] : 0.0;
  if (slot < industry_slots && std::isfinite(f[industry_factor(slot)])) value += f[industry_factor(slot)];
  for (usize s = 0; s < style_count; ++s)
    if (std::isfinite(f[style_factor(s)])) value += f[style_factor(s)] * z[s];
  return value;
}
u64 splitmix(u64 x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31U);
}
// Median of v (reordered in place): the middle value, or the mean of the two middle values;
// NaN when empty.
f64 median_inplace(std::vector<f64>& v) {
  if (v.empty()) return nan;
  const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
  std::nth_element(v.begin(), mid, v.end());
  const f64 upper = *mid;
  if (v.size() % 2) return upper;
  return 0.5 * (*std::max_element(v.begin(), mid) + upper);
}
// Lower weighted median of (value, weight >= 0) pairs (reordered): the smallest value whose
// cumulative weight, values ascending, reaches half the total; equal weights when the total is
// not positive. NaN when empty.
f64 weighted_median(std::vector<std::pair<f64, f64>>& pairs) {
  if (pairs.empty()) return nan;
  std::sort(pairs.begin(), pairs.end());
  f64 total = 0;
  for (const auto& entry : pairs) total += entry.second;
  const bool equal = !(total > 0) || !std::isfinite(total);
  const f64 half = 0.5 * (equal ? static_cast<f64>(pairs.size()) : total);
  f64 cumulative = 0;
  for (const auto& [value, weight] : pairs) {
    cumulative += equal ? 1.0 : weight;
    if (cumulative >= half) return value;
  }
  return pairs.back().first;
}
// Type-7 sample quantile (Hyndman-Fan 1996; numpy's default) of ascending values, p in [0, 1].
f64 quantile_sorted(std::span<const f64> sorted, f64 p) {
  if (sorted.empty()) return nan;
  const f64 h = std::clamp(p, 0.0, 1.0) * static_cast<f64>(sorted.size() - 1);
  const auto lo = static_cast<usize>(std::floor(h));
  const usize hi = std::min(lo + 1, sorted.size() - 1);
  return sorted[lo] + (h - static_cast<f64>(lo)) * (sorted[hi] - sorted[lo]);
}
std::string format_number(f64 x) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << x;
  return out.str();
}
// UTC calendar date of a session key (ns since the epoch), YYYY-MM-DD.
std::string iso_date(i64 session_ns) {
  namespace ch = std::chrono;
  const ch::sys_time<ch::nanoseconds> at{ch::nanoseconds(session_ns)};
  const ch::year_month_day ymd{ch::floor<ch::days>(at)};
  const auto two = [](unsigned v) { return (v < 10 ? "0" : "") + std::to_string(v); };
  return std::to_string(static_cast<int>(ymd.year())) + '-' +
         two(static_cast<unsigned>(ymd.month())) + '-' + two(static_cast<unsigned>(ymd.day()));
}
} // namespace

StyleScale standardize_style(std::span<const f64> raw, std::span<const u8> universe,
                             std::span<const f64> cap, f64 winsor_k, f64 clip, std::span<f64> z) {
  StyleScale out;
  std::fill(z.begin(), z.end(), 0.0);
  const usize n = raw.size();
  if (universe.size() != n || cap.size() != n || z.size() != n || !finite_positive(winsor_k) ||
      !finite_positive(clip))
    return out;
  std::vector<f64> values;
  values.reserve(n);
  for (usize i = 0; i < n; ++i)
    if (universe[i] && std::isfinite(raw[i])) values.push_back(raw[i]);
  out.names = values.size();
  if (values.size() < 2) return out;
  const f64 med = median_inplace(values);
  for (f64& v : values) v = std::abs(v - med);
  const f64 s = mad_consistency * median_inplace(values);
  out.median = med; out.robust_sd = s;
  if (!finite_positive(s)) return out; // more than half the universe tied at the median
  const f64 lo = med - winsor_k * s, hi = med + winsor_k * s;
  f64 cw = 0, cx = 0;
  for (usize i = 0; i < n; ++i) {
    if (!universe[i] || !std::isfinite(raw[i])) continue;
    const f64 v = std::clamp(raw[i], lo, hi);
    out.fenced += v != raw[i] ? 1U : 0U;
    cw += cap[i]; cx += cap[i] * v;
  }
  if (!finite_positive(cw)) return out;
  const f64 mu = cx / cw;
  f64 ss = 0;
  for (usize i = 0; i < n; ++i) {
    if (!universe[i] || !std::isfinite(raw[i])) continue;
    const f64 d = std::clamp(raw[i], lo, hi) - mu;
    ss += d * d;
  }
  const f64 sd = std::sqrt(ss / static_cast<f64>(out.names - 1));
  out.mean = mu; out.sd = sd;
  if (!finite_positive(sd)) return out;
  for (usize i = 0; i < n; ++i)
    if (std::isfinite(raw[i])) z[i] = std::clamp((std::clamp(raw[i], lo, hi) - mu) / sd, -clip, clip);
  out.valid = true;
  return out;
}

std::string factor_name(usize k) {
  if (k == 0) return "market";
  if (k <= industry_slots)
    return k - 1 == residual_industry ? std::string("ind_residual") : "ind_ff" + std::to_string(k);
  if (k < factor_count) return std::string("style_") + style_names[k - 1 - industry_slots];
  return "unknown";
}

co::Status validate_config(const RiskModelConfig& c) {
  const bool ok = c.vol_halflife > 0 && c.correlation_halflife > 0 && c.nw_halflife > 0 &&
      c.vra_halflife > 0 && c.specific_halflife > 0 && c.variance_nw_lags <= 20 &&
      c.correlation_nw_lags <= 20 && c.specific_nw_lags <= 20 && c.vra_min_dates > 0 &&
      c.min_factor_history >= 2 && c.structural_history > 0 && c.structural_history <= 4096 &&
      std::isfinite(c.bayesian_q) && c.bayesian_q >= 0 && c.min_industry_names > 0 &&
      c.beta_window >= 3 && c.beta_window <= 4096 && c.min_beta_pairs >= 3 &&
      c.min_beta_pairs <= c.beta_window && c.momentum_window <= 4096 &&
      c.momentum_skip < c.momentum_window && c.adv_window >= 1 && c.adv_window <= 4096 &&
      finite_positive(c.winsor_k) && finite_positive(c.clip_z) && c.min_regression_names >= 2 &&
      std::isfinite(c.min_style_effective_names) && c.min_style_effective_names >= 1 &&
      std::isfinite(c.structural_sigma_quantile) && c.structural_sigma_quantile >= 0 &&
      c.structural_sigma_quantile < 0.5 && finite_positive(c.max_specific_variance);
  if (!ok) return co::Err(co::ErrorCode::InvalidArgument, "risk model: invalid configuration");
  return co::Ok();
}

// ---- regression ----------------------------------------------------------------------------
co::Result<CrossSectionFit> regress_cross_section(const CrossSection& x, f64 min_style_names) {
  const usize rows = x.value.size();
  if (x.industry.size() != rows || x.styles.size() != rows * style_count ||
      x.weight.size() != rows || x.cap.size() != rows)
    return co::Err(co::ErrorCode::InvalidArgument, "risk regression: row geometry");
  if (!std::isfinite(min_style_names) || min_style_names < 0)
    return co::Err(co::ErrorCode::InvalidArgument, "risk regression: style floor");
  std::array<usize, industry_slots> count{};
  std::array<f64, industry_slots> cap{};
  std::array<f64, style_count> mass{}, mass_sq{}, z_sum{}, z_sq{};
  f64 wsum = 0;
  for (usize r = 0; r < rows; ++r) {
    const u8 slot = x.industry[r];
    if (slot >= industry_slots || !finite_positive(x.weight[r]) || !std::isfinite(x.cap[r]) ||
        x.cap[r] < 0 || !std::isfinite(x.value[r]))
      return co::Err(co::ErrorCode::InvalidArgument, "risk regression: row values");
    ++count[slot]; cap[slot] += x.cap[r]; wsum += x.weight[r];
    for (usize s = 0; s < style_count; ++s) {
      const f64 z = x.styles[r * style_count + s];
      if (!std::isfinite(z)) return co::Err(co::ErrorCode::InvalidArgument, "risk regression: style");
      const f64 m = x.weight[r] * z * z;
      mass[s] += m; mass_sq[s] += m * m; z_sum[s] += z; z_sq[s] += z * z;
    }
  }
  std::array<usize, factor_count> index{};
  index.fill(npos);
  usize p = 0, active_industries = 0;
  f64 cap_total = 0;
  index[0] = p++;
  for (usize g = 0; g < industry_slots; ++g) {
    if (!count[g]) continue;
    index[industry_factor(g)] = p++; ++active_industries; cap_total += cap[g];
  }
  // Style validity (F3): exposure mass, then breadth (effective names) at least the floor.
  std::array<f64, style_count> effective{}, dispersion{};
  std::array<bool, style_count> dropped{};
  for (usize s = 0; s < style_count; ++s) {
    effective[s] = nan; dispersion[s] = nan;
    if (!(mass[s] > 1e-12 * wsum)) continue;
    effective[s] = mass[s] * mass[s] / mass_sq[s];
    const f64 z_mean = z_sum[s] / static_cast<f64>(rows);
    const f64 z_var = z_sq[s] / static_cast<f64>(rows) - z_mean * z_mean;
    dispersion[s] = std::sqrt(std::max(0.0, z_var));
    if (effective[s] < min_style_names) { dropped[s] = true; continue; }
    index[style_factor(s)] = p++;
  }
  if (rows <= p || !(wsum > 0))
    return co::Err(co::ErrorCode::Unavailable, "risk regression: rows do not exceed parameters");
  Eigen::MatrixXd a = Eigen::MatrixXd::Zero(ix(p + 1), ix(p + 1));
  Eigen::VectorXd b = Eigen::VectorXd::Zero(ix(p + 1));
  std::array<usize, 2 + style_count> cols{};
  std::array<f64, 2 + style_count> vals{};
  for (usize r = 0; r < rows; ++r) {
    usize m = 0;
    cols[m] = index[0]; vals[m++] = 1.0;
    cols[m] = index[industry_factor(x.industry[r])]; vals[m++] = 1.0;
    for (usize s = 0; s < style_count; ++s) {
      if (index[style_factor(s)] == npos) continue;
      cols[m] = index[style_factor(s)]; vals[m++] = x.styles[r * style_count + s];
    }
    const f64 v = x.weight[r] / wsum, y = x.value[r];
    for (usize u = 0; u < m; ++u) {
      b[ix(cols[u])] += v * vals[u] * y;
      for (usize w = 0; w < m; ++w) a(ix(cols[u]), ix(cols[w])) += v * vals[u] * vals[w];
    }
  }
  // Industry constraint row: cap weights (count weights when every cap is zero).
  for (usize g = 0; g < industry_slots; ++g) {
    if (!count[g]) continue;
    const f64 c = cap_total > 0 ? cap[g] / cap_total
                                : static_cast<f64>(count[g]) / static_cast<f64>(rows);
    a(ix(p), ix(index[industry_factor(g)])) = c;
    a(ix(index[industry_factor(g)]), ix(p)) = c;
  }
  const Eigen::FullPivLU<Eigen::MatrixXd> lu(a);
  if (!lu.isInvertible())
    return co::Err(co::ErrorCode::Unavailable, "risk regression: singular (collinear) system");
  const Eigen::VectorXd solution = lu.solve(b);
  if (!solution.allFinite())
    return co::Err(co::ErrorCode::Unavailable, "risk regression: non-finite solution");
  CrossSectionFit fit;
  fit.factor.fill(nan);
  for (usize k = 0; k < factor_count; ++k)
    if (index[k] != npos) fit.factor[k] = solution[ix(index[k])];
  fit.rows = rows; fit.active_industries = active_industries;
  fit.style_effective_names = effective; fit.style_dispersion = dispersion;
  fit.style_dropped = dropped;
  fit.residual.resize(rows);
  f64 mean = 0;
  for (usize r = 0; r < rows; ++r) mean += x.weight[r] / wsum * x.value[r];
  f64 ss_res = 0, ss_tot = 0;
  for (usize r = 0; r < rows; ++r) {
    const f64 u = x.value[r] - predict(fit.factor, x.industry[r],
                                       x.styles.subspan(r * style_count, style_count));
    fit.residual[r] = u;
    const f64 v = x.weight[r] / wsum;
    ss_res += v * u * u; ss_tot += v * (x.value[r] - mean) * (x.value[r] - mean);
  }
  fit.r2 = ss_tot > 0 ? 1.0 - ss_res / ss_tot : nan;
  return co::Ok(std::move(fit));
}

// ---- robustness (F3) -----------------------------------------------------------------------
namespace {
// E0 exp(x b) for every query name with a slot, each style exposure clamped to [lo, hi] of the
// fit rows (flags the names whose clamp touched an active style); false when E0 is unusable.
bool predict_structural(const CrossSectionFit& wls, const std::array<f64, style_count>& lo,
                        const std::array<f64, style_count>& hi, std::span<const u8> slot,
                        std::span<const f64> styles, std::span<f64> sigma, std::span<u8> flags) {
  f64 correction = 0;
  for (const f64 u : wls.residual) correction += std::exp(u);
  correction /= static_cast<f64>(wls.residual.size());
  if (!finite_positive(correction)) return false;
  std::array<f64, style_count> x{};
  for (usize i = 0; i < slot.size(); ++i) {
    if (slot[i] == no_exposure) continue;
    bool clamped = false;
    for (usize s = 0; s < style_count; ++s) {
      const f64 z = styles[i * style_count + s];
      x[s] = std::clamp(z, lo[s], hi[s]);
      clamped = clamped || (x[s] != z && std::isfinite(wls.factor[style_factor(s)]));
    }
    sigma[i] = correction * std::exp(predict(wls.factor, slot[i], x));
    if (clamped) flags[i] = static_cast<u8>(flags[i] | structural_exposure_clamped);
  }
  return true;
}
} // namespace

co::Result<StructuralVol> structural_specific_vol(const CrossSection& fit,
                                                  const RiskModelConfig& cfg,
                                                  std::span<const u8> query_slot,
                                                  std::span<const f64> query_styles,
                                                  std::span<f64> sigma, std::span<u8> flags) {
  const usize rows = fit.value.size(), names = query_slot.size();
  if (!rows || fit.industry.size() != rows || fit.styles.size() != rows * style_count ||
      fit.weight.size() != rows || fit.cap.size() != rows ||
      query_styles.size() != names * style_count || sigma.size() != names || flags.size() != names)
    return co::Err(co::ErrorCode::InvalidArgument, "risk structural vol: geometry");
  std::fill(sigma.begin(), sigma.end(), nan);
  std::fill(flags.begin(), flags.end(), u8{0});
  // The fit rows' sigma_TS (sorted) bound the prediction; their exposures bound x.
  std::vector<f64> fitted(rows);
  std::vector<std::pair<f64, f64>> weighted;
  weighted.reserve(rows);
  std::array<f64, style_count> lo{}, hi{};
  lo.fill(std::numeric_limits<f64>::infinity());
  hi.fill(-std::numeric_limits<f64>::infinity());
  for (usize r = 0; r < rows; ++r) {
    fitted[r] = std::exp(fit.value[r]);
    if (!finite_positive(fitted[r]))
      return co::Err(co::ErrorCode::InvalidArgument, "risk structural vol: fit sigma");
    weighted.emplace_back(fitted[r], std::isfinite(fit.cap[r]) ? std::max(0.0, fit.cap[r]) : 0.0);
    for (usize s = 0; s < style_count; ++s) {
      const f64 z = fit.styles[r * style_count + s];
      if (!std::isfinite(z))
        return co::Err(co::ErrorCode::InvalidArgument, "risk structural vol: style");
      lo[s] = std::min(lo[s], z); hi[s] = std::max(hi[s], z);
    }
  }
  std::sort(fitted.begin(), fitted.end());
  StructuralVol out;
  out.lower = quantile_sorted(fitted, cfg.structural_sigma_quantile);
  out.upper = quantile_sorted(fitted, 1.0 - cfg.structural_sigma_quantile);
  if (rows >= cfg.min_regression_names) {
    const auto wls = regress_cross_section(fit, cfg.min_style_effective_names);
    out.regression =
        wls && predict_structural(*wls, lo, hi, query_slot, query_styles, sigma, flags);
  }
  if (!out.regression) {
    std::fill(flags.begin(), flags.end(), u8{0});
    const f64 fallback = weighted_median(weighted);
    for (usize i = 0; i < names; ++i)
      if (query_slot[i] != no_exposure) sigma[i] = fallback;
  }
  for (usize i = 0; i < names; ++i) {
    if (query_slot[i] == no_exposure || std::isnan(sigma[i])) continue;
    const f64 bounded = std::clamp(sigma[i], out.lower, out.upper);
    if (bounded != sigma[i]) flags[i] = static_cast<u8>(flags[i] | structural_sigma_bounded);
    sigma[i] = bounded;
  }
  return co::Ok(out);
}

co::Result<DecileShrinkage> shrink_to_size_deciles(std::span<const f64> sigma,
                                                   std::span<const f64> cap,
                                                   std::span<const u8> eligible, f64 q) {
  const usize n = sigma.size();
  if (cap.size() != n || eligible.size() != n || !std::isfinite(q) || q < 0)
    return co::Err(co::ErrorCode::InvalidArgument, "risk shrinkage: geometry or q");
  DecileShrinkage out;
  out.shrunk.assign(sigma.begin(), sigma.end());
  out.target.assign(n, nan);
  std::vector<usize> order;
  for (usize i = 0; i < n; ++i)
    if (eligible[i] && std::isfinite(sigma[i]) && finite_positive(cap[i])) order.push_back(i);
  std::stable_sort(order.begin(), order.end(), [&](usize a, usize b) { return cap[a] < cap[b]; });
  const usize count = order.size();
  std::vector<std::pair<f64, f64>> pairs;
  for (usize decile = 0; decile < 10 && count; ++decile) {
    const usize begin = decile * count / 10, end = (decile + 1) * count / 10;
    if (begin == end) continue;
    pairs.clear();
    for (usize r = begin; r < end; ++r) pairs.emplace_back(sigma[order[r]], cap[order[r]]);
    const f64 target = weighted_median(pairs);
    f64 ss = 0;
    for (usize r = begin; r < end; ++r) {
      const f64 d = sigma[order[r]] - target;
      ss += d * d;
    }
    const f64 dispersion = std::sqrt(ss / static_cast<f64>(end - begin));
    for (usize r = begin; r < end; ++r) {
      const usize i = order[r];
      const f64 distance = q * std::abs(sigma[i] - target);
      const f64 intensity = distance > 0 ? distance / (dispersion + distance) : 0.0;
      out.shrunk[i] = intensity * target + (1.0 - intensity) * sigma[i];
      out.target[i] = target;
    }
  }
  return co::Ok(std::move(out));
}

co::Status check_specific_variance(std::span<const f64> variance, std::span<const u64> ids,
                                   i64 session, f64 bound) {
  if (ids.size() != variance.size() || !finite_positive(bound))
    return co::Err(co::ErrorCode::InvalidArgument, "risk invariant: geometry or bound");
  for (usize i = 0; i < variance.size(); ++i) {
    const f64 v = variance[i];
    if (std::isnan(v) || (v > 0 && v < bound)) continue;
    return co::Err(co::ErrorCode::OutOfRange,
                   std::string("risk model: daily specific variance ") + format_number(v) +
                       " of instrument " + std::to_string(ids[i]) + " at session " +
                       std::to_string(session) + " (" + iso_date(session) + ") is outside (0, " +
                       format_number(bound) + "): refused (" + risk_model_id +
                       " invariant; nothing is clamped)");
  }
  return co::Ok();
}

// ---- recursive EWMA moments ----------------------------------------------------------------
LaggedEwma::LaggedEwma(usize columns, usize half_life, usize lags, bool full)
    : k_(columns), lags_(lags), full_(full),
      decay_(half_life == 0 ? 1.0 : std::exp2(-1.0 / static_cast<f64>(half_life))),
      count_(columns), s0_(columns), s1_(columns) {
  const usize pairs = full ? columns * columns : columns;
  w0_.assign(pairs, 0.0); p0_.assign(pairs, 0.0); a0_.assign(pairs, 0.0); b0_.assign(pairs, 0.0);
  wl_.assign(pairs * lags, 0.0); ql_.assign(pairs * lags, 0.0);
  al_.assign(pairs * lags, 0.0); bl_.assign(pairs * lags, 0.0);
  ring_.assign(columns * lags, nan);
}

void LaggedEwma::push(std::span<const f64> row) {
  const f64 d = decay_;
  for (auto* v : {&s0_, &s1_, &w0_, &p0_, &a0_, &b0_, &wl_, &ql_, &al_, &bl_})
    for (f64& x : *v) x *= d;
  const usize width = std::min(k_, row.size());
  const auto ok = [&](usize i) { return i < width && std::isfinite(row[i]); };
  for (usize i = 0; i < k_; ++i) {
    if (!ok(i)) continue;
    s0_[i] += 1.0; s1_[i] += row[i]; ++count_[i];
  }
  const usize pairs = full_ ? k_ * k_ : k_;
  for (usize i = 0; i < k_; ++i) {
    if (!ok(i)) continue;
    const usize first = full_ ? 0 : i, last = full_ ? k_ : i + 1;
    for (usize j = first; j < last; ++j) {
      if (!ok(j)) continue;
      const usize q = pair(i, j);
      w0_[q] += 1.0; p0_[q] += row[i] * row[j]; a0_[q] += row[i]; b0_[q] += row[j];
    }
  }
  // Lag l pairs (older = the row pushed l sessions ago, newer = this row), weighted by the
  // older row's age l: 2^(-l/H), decaying with every later push like any row weight.
  for (usize l = 1; l <= lags_ && l <= pushed_; ++l) {
    const f64* old = ring_.data() + ((pushed_ - l) % lags_) * k_;
    const f64 w = std::pow(d, static_cast<f64>(l));
    const usize base = (l - 1) * pairs;
    for (usize i = 0; i < k_; ++i) {
      if (!std::isfinite(old[i])) continue;
      const usize first = full_ ? 0 : i, last = full_ ? k_ : i + 1;
      for (usize j = first; j < last; ++j) {
        if (!ok(j)) continue;
        const usize q = base + pair(i, j);
        wl_[q] += w; ql_[q] += w * old[i] * row[j]; al_[q] += w * old[i]; bl_[q] += w * row[j];
      }
    }
  }
  if (lags_) {
    f64* slot = ring_.data() + (pushed_ % lags_) * k_;
    for (usize i = 0; i < k_; ++i) slot[i] = ok(i) ? row[i] : nan;
  }
  ++pushed_;
}

f64 LaggedEwma::mean(usize i) const noexcept {
  return i < k_ && s0_[i] > 0 ? s1_[i] / s0_[i] : nan;
}

f64 LaggedEwma::zero_lag(usize i, usize j) const noexcept {
  if (i >= k_ || j >= k_ || (!full_ && i != j)) return nan;
  const usize q = pair(i, j);
  const f64 ma = mean(i), mb = mean(j), mass = w0_[q];
  if (!(mass > 0) || !std::isfinite(ma) || !std::isfinite(mb)) return nan;
  return (p0_[q] - mb * a0_[q] - ma * b0_[q] + ma * mb * mass) / mass;
}

f64 LaggedEwma::covariance(usize i, usize j) const noexcept {
  f64 out = zero_lag(i, j);
  if (!std::isfinite(out)) return nan;
  const f64 ma = mean(i), mb = mean(j), mass = w0_[pair(i, j)];
  const usize pairs = full_ ? k_ * k_ : k_;
  for (usize l = 1; l <= lags_; ++l) {
    const usize qij = (l - 1) * pairs + pair(i, j), qji = (l - 1) * pairs + pair(j, i);
    const f64 ab = ql_[qij] - mb * al_[qij] - ma * bl_[qij] + ma * mb * wl_[qij];
    const f64 ba = ql_[qji] - ma * al_[qji] - mb * bl_[qji] + ma * mb * wl_[qji];
    out += (1.0 - static_cast<f64>(l) / static_cast<f64>(lags_ + 1)) * (ab + ba) / mass;
  }
  return out;
}

f64 serial_adjusted_variance(const LaggedEwma& fast, const LaggedEwma& nw, usize i) noexcept {
  const f64 f = fast.zero_lag(i, i);
  if (!std::isfinite(f)) return nan;
  const f64 base = nw.zero_lag(i, i), hac = nw.covariance(i, i);
  const f64 ratio = !(base > 0) || !std::isfinite(hac) ? 1.0 : std::max(0.0, hac / base);
  return f * ratio;
}

// ---- bias statistics -----------------------------------------------------------------------
f64 bias_statistic(std::span<const f64> z) noexcept {
  f64 sum = 0; usize n = 0;
  for (const f64 v : z)
    if (std::isfinite(v)) { sum += v; ++n; }
  if (n < 2) return nan;
  const f64 mean = sum / static_cast<f64>(n);
  f64 ss = 0;
  for (const f64 v : z)
    if (std::isfinite(v)) ss += (v - mean) * (v - mean);
  return std::sqrt(ss / static_cast<f64>(n - 1));
}
f64 sample_kurtosis(std::span<const f64> z) noexcept {
  f64 sum = 0; usize n = 0;
  for (const f64 v : z)
    if (std::isfinite(v)) { sum += v; ++n; }
  if (n < 4) return nan;
  const f64 mean = sum / static_cast<f64>(n);
  f64 m2 = 0, m4 = 0;
  for (const f64 v : z) {
    if (!std::isfinite(v)) continue;
    const f64 d2 = (v - mean) * (v - mean);
    m2 += d2; m4 += d2 * d2;
  }
  m2 /= static_cast<f64>(n); m4 /= static_cast<f64>(n);
  return m2 > 0 ? m4 / (m2 * m2) : nan;
}
Band bias_band(usize t) noexcept {
  if (t == 0) return {nan, nan};
  const f64 h = std::sqrt(2.0 / static_cast<f64>(t));
  return {1.0 - h, 1.0 + h};
}
Band kurtosis_band(usize t, f64 kurtosis) noexcept {
  if (t == 0 || !(kurtosis >= 1)) return {nan, nan};
  const f64 h = 1.96 * std::sqrt((kurtosis - 1.0) / (4.0 * static_cast<f64>(t)));
  return {1.0 - h, 1.0 + h};
}
std::vector<f64> rolling_bias(std::span<const f64> z, usize window) {
  std::vector<f64> out(z.size(), nan);
  if (window < 2) return out;
  for (usize j = window - 1; j < z.size(); ++j) out[j] = bias_statistic(z.subspan(j + 1 - window, window));
  return out;
}

f64 interval_return(const RiskPanel& p, usize t, usize i) noexcept {
  if (t == 0 || t >= p.dates || i >= p.instruments) return nan;
  const usize a = (t - 1) * p.instruments + i, b = t * p.instruments + i;
  if (!p.present[a] || !p.present[b] || !finite_positive(p.close[a]) || !finite_positive(p.close[b]) ||
      !finite_positive(p.raw_close[a]) || !finite_positive(p.raw_close[b]) ||
      guarded_move(p.close[a], p.close[b], p.raw_close[a], p.raw_close[b]))
    return nan;
  return p.close[b] / p.close[a] - 1.0;
}

// ---- the daily model -----------------------------------------------------------------------
namespace {
// The factors present names (those with an exposure row) load on at t, each industry slot's
// eligible cap at t, and the present names pooled into the residual slot (F2).
struct ExposureAudit {
  std::array<bool, factor_count> exposed{};
  std::array<f64, industry_slots> industry_cap{};
  usize residual_names{};
};
// The correlation estimator's rho of factors a and b (0 without joint history): the expression
// of the fully observed block.
f64 estimator_correlation(const LaggedEwma& corr, usize a, usize b) {
  const f64 ca = corr.covariance(a, a), cb = corr.covariance(b, b), cab = corr.covariance(a, b);
  const f64 denom = std::sqrt(std::max(0.0, ca)) * std::sqrt(std::max(0.0, cb));
  return denom > 0 && std::isfinite(cab) ? std::clamp(cab / denom, -1.0, 1.0) : 0.0;
}
// Structural priors at t: the class mean of the fully observed block's (pre-VRA) variances,
// industries weighted by their eligible cap at t (equal weights when none holds cap), styles
// equal-weighted; NaN for a class without a fully observed factor.
struct ClassPriors {
  f64 industry{nan}, style{nan};
};
ClassPriors class_priors(std::span<const usize> keep, const Eigen::MatrixXd& full,
                         const ExposureAudit& exposure) {
  f64 cap_sum = 0, cap_var = 0, industry_sum = 0, style_sum = 0;
  usize industries = 0, styles = 0;
  for (usize a = 0; a < keep.size(); ++a) {
    const usize k = keep[a];
    const f64 v = full(ix(a), ix(a));
    if (k >= 1 && k <= industry_slots) {
      const f64 c = exposure.industry_cap[k - 1];
      cap_sum += c; cap_var += c * v; industry_sum += v; ++industries;
    } else if (k > industry_slots) {
      style_sum += v; ++styles;
    }
  }
  ClassPriors prior;
  if (industries)
    prior.industry = cap_sum > 0 ? cap_var / cap_sum : industry_sum / static_cast<f64>(industries);
  if (styles) prior.style = style_sum / static_cast<f64>(styles);
  return prior;
}

struct Model {
  Model(const RiskPanel& panel, const RiskModelConfig& config)
      : p(panel), cfg(config), n(panel.instruments), market(panel.dates, nan), sr(n), srr(n),
        srm(n), sm(n), smm(n), pairs(n), adv_sum(n), ret(n, nan), residual(n, nan),
        slot_prev(n, no_exposure), slot_cur(n, no_exposure), elig_prev(n), elig_cur(n),
        z_prev(n * style_count), z_cur(n * style_count),
        factor_fast(factor_count, config.vol_halflife, 0, false),
        factor_nw(factor_count, config.nw_halflife, config.variance_nw_lags, false),
        factor_corr(factor_count, config.correlation_halflife, config.correlation_nw_lags, true),
        spec_fast(n, config.specific_halflife, 0, false),
        spec_nw(n, config.nw_halflife, config.specific_nw_lags, false),
        observed(panel.dates * n), history(n), prior_factor_var(factor_count, nan),
        prior_spec_var(n, nan), factor_return(factor_count, nan),
        cov(factor_count * factor_count, nan), spec(n, nan), raw(n), zbuf(n), zbuf2(n),
        sigma_ts(n, nan), sigma_str(n, nan), blend(n, nan), structural_flag(factor_count, 0),
        structural_vol_flag(n, 0) {}
  const RiskPanel& p;
  const RiskModelConfig& cfg;
  usize n;
  std::vector<f64> market;
  std::vector<f64> sr, srr, srm, sm, smm;
  std::vector<usize> pairs;
  std::vector<f64> adv_sum, ret, residual;
  std::vector<u8> slot_prev, slot_cur, elig_prev, elig_cur;
  std::vector<f64> z_prev, z_cur;
  LaggedEwma factor_fast, factor_nw, factor_corr, spec_fast, spec_nw;
  std::vector<u8> observed;
  std::vector<u32> history;
  f64 vra_f_acc{}, vra_f_mass{}, vra_s_acc{}, vra_s_mass{};
  usize vra_f_n{}, vra_s_n{};
  std::vector<f64> prior_factor_var, prior_spec_var, factor_return, cov, spec;
  std::vector<f64> raw, zbuf, zbuf2, sigma_ts, sigma_str, blend;
  std::vector<u8> structural_flag; // factor_count: 1 = structural forecast at t (F2)
  std::vector<u8> structural_vol_flag; // instruments: structural_sigma_bounded | ..._clamped (F3)

  [[nodiscard]] f64 cap_at(usize t, usize i) const { return p.cap[t * n + i]; }
  void update_prices(usize t);
  void regress(usize t, RiskDay& day);
  void update_vra(usize t, RiskDay& day);
  void exposures(usize t, RiskDay& day);
  [[nodiscard]] ExposureAudit audit_exposures(usize t) const;
  void factor_forecast(RiskDay& day);
  [[nodiscard]] bool fully_observed_forecast(RiskDay& day, std::vector<usize>& keep,
                                             Eigen::MatrixXd& f);
  void structural_forecast(RiskDay& day, const ExposureAudit& exposure,
                           std::span<const usize> keep, const Eigen::MatrixXd& full);
  [[nodiscard]] co::Status specific_forecast(RiskDay& day);
  void blend_specific(RiskDay& day);
  [[nodiscard]] f64 vra(f64 acc, f64 mass, usize count) const {
    return count >= cfg.vra_min_dates && mass > 0 && acc > 0 ? acc / mass : 1.0;
  }
};

// Returns, equal-weight member market, rolling beta sums (window beta_window, pairs ending at
// t) and the rolling dollar-volume sum (adv_window sessions ending at t).
void Model::update_prices(usize t) {
  f64 msum = 0; usize mcount = 0;
  for (usize i = 0; i < n; ++i) {
    ret[i] = interval_return(p, t, i);
    if (t > 0 && p.member[(t - 1) * n + i] && std::isfinite(ret[i])) { msum += ret[i]; ++mcount; }
  }
  market[t] = mcount ? msum / static_cast<f64>(mcount) : nan;
  const usize w = cfg.beta_window;
  for (usize i = 0; i < n; ++i) {
    if (std::isfinite(ret[i]) && std::isfinite(market[t])) {
      const f64 r = ret[i], m = market[t];
      sr[i] += r; srr[i] += r * r; srm[i] += r * m; sm[i] += m; smm[i] += m * m; ++pairs[i];
    }
    if (t >= w) {
      const f64 r = interval_return(p, t - w, i), m = market[t - w];
      if (std::isfinite(r) && std::isfinite(m)) {
        sr[i] -= r; srr[i] -= r * r; srm[i] -= r * m; sm[i] -= m; smm[i] -= m * m; --pairs[i];
      }
    }
    const auto dollars = [&](usize row) {
      const usize k = row * n + i;
      return p.present[k] && std::isfinite(p.raw_close[k]) && std::isfinite(p.volume[k])
          ? p.raw_close[k] * p.volume[k] : 0.0;
    };
    adv_sum[i] += dollars(t);
    if (t >= cfg.adv_window) adv_sum[i] -= dollars(t - cfg.adv_window);
  }
}

// Factor returns of session t on X_{t-1} and every exposed name's specific return.
void Model::regress(usize t, RiskDay& day) {
  std::fill(factor_return.begin(), factor_return.end(), nan);
  std::fill(residual.begin(), residual.end(), nan);
  day.style_effective_names.fill(nan);
  day.style_dispersion.fill(nan);
  day.style_dropped.fill(u8{0});
  if (t == 0) return;
  std::vector<u8> ind; std::vector<f64> sty, wgt, cap, val;
  for (usize i = 0; i < n; ++i) {
    if (!elig_prev[i] || !std::isfinite(ret[i])) continue;
    const f64 c = cap_at(t - 1, i);
    ind.push_back(slot_prev[i]); wgt.push_back(std::sqrt(c)); cap.push_back(c);
    val.push_back(ret[i]);
    sty.insert(sty.end(), z_prev.begin() + static_cast<std::ptrdiff_t>(i * style_count),
               z_prev.begin() + static_cast<std::ptrdiff_t>((i + 1) * style_count));
  }
  day.regression_rows = val.size();
  if (val.size() < cfg.min_regression_names) return;
  const auto fit = regress_cross_section({ind, sty, wgt, cap, val}, cfg.min_style_effective_names);
  if (!fit) return; // an unfittable session is a missing factor-return row
  day.fitted = true; day.r2 = fit->r2; day.active_industries = fit->active_industries;
  day.style_effective_names = fit->style_effective_names;
  day.style_dispersion = fit->style_dispersion;
  for (usize s = 0; s < style_count; ++s)
    day.style_dropped[s] = fit->style_dropped[s] ? u8{1} : u8{0};
  std::copy(fit->factor.begin(), fit->factor.end(), factor_return.begin());
  for (usize i = 0; i < n; ++i) {
    if (slot_prev[i] == no_exposure || !std::isfinite(ret[i])) continue;
    residual[i] = ret[i] - predict(fit->factor, slot_prev[i],
                                   std::span<const f64>(z_prev).subspan(i * style_count, style_count));
  }
}

// Bias statistics of session t on the prior (pre-VRA) forecasts, folded into the VRA EWMAs;
// then the estimators take session t's factor and specific returns.
void Model::update_vra(usize t, RiskDay& day) {
  const f64 decay = std::exp2(-1.0 / static_cast<f64>(cfg.vra_halflife));
  vra_f_acc *= decay; vra_f_mass *= decay; vra_s_acc *= decay; vra_s_mass *= decay;
  day.bias_factor = nan; day.bias_specific = nan;
  f64 bf = 0; usize kf = 0;
  for (usize k = 0; k < factor_count; ++k) {
    if (!std::isfinite(factor_return[k]) || !(prior_factor_var[k] > 0)) continue;
    bf += factor_return[k] * factor_return[k] / prior_factor_var[k]; ++kf;
  }
  if (kf) {
    day.bias_factor = std::sqrt(bf / static_cast<f64>(kf));
    vra_f_acc += bf / static_cast<f64>(kf); vra_f_mass += 1.0; ++vra_f_n;
  }
  f64 bs = 0, ws = 0;
  if (t > 0)
    for (usize i = 0; i < n; ++i) {
      if (!elig_prev[i] || !std::isfinite(residual[i]) || !(prior_spec_var[i] > 0)) continue;
      const f64 c = cap_at(t - 1, i);
      bs += c * residual[i] * residual[i] / prior_spec_var[i]; ws += c;
    }
  if (ws > 0) {
    day.bias_specific = std::sqrt(bs / ws);
    vra_s_acc += bs / ws; vra_s_mass += 1.0; ++vra_s_n;
  }
  if (t > 0) {
    factor_fast.push(factor_return); factor_nw.push(factor_return); factor_corr.push(factor_return);
    spec_fast.push(residual); spec_nw.push(residual);
    for (usize i = 0; i < n; ++i) {
      const u8 seen = std::isfinite(residual[i]) ? u8{1} : u8{0};
      observed[t * n + i] = seen;
      history[i] += seen;
      if (t >= cfg.structural_history) history[i] -= observed[(t - cfg.structural_history) * n + i];
    }
  }
  day.lambda2_factor = vra(vra_f_acc, vra_f_mass, vra_f_n);
  day.lambda2_specific = vra(vra_s_acc, vra_s_mass, vra_s_n);
}

// Exposures X_t: eligibility, industry slots (pooling small industries), 11 styles (robust
// standardisation, F3; day counts the fenced values and the unscaled standardisations).
void Model::exposures(usize t, RiskDay& day) {
  const usize row = t * n;
  std::array<usize, industry_slots> count{};
  for (usize i = 0; i < n; ++i) {
    elig_cur[i] = p.member[row + i] && p.present[row + i] && finite_positive(p.cap[row + i])
        ? u8{1} : u8{0};
    const u8 id = p.industry.empty() ? u8{0} : p.industry[row + i];
    if (elig_cur[i] && id >= 1 && id <= 49) ++count[id - 1U];
  }
  for (usize i = 0; i < n; ++i) {
    if (!p.present[row + i]) { slot_cur[i] = no_exposure; continue; }
    const u8 id = p.industry.empty() ? u8{0} : p.industry[row + i];
    slot_cur[i] = id >= 1 && id <= 49 && count[id - 1U] >= cfg.min_industry_names
        ? static_cast<u8>(id - 1U) : static_cast<u8>(residual_industry);
  }
  const std::span<const f64> caps = p.cap.subspan(row, n);
  const auto standardize = [&](std::span<f64> z) {
    const StyleScale scale = standardize_style(raw, elig_cur, caps, cfg.winsor_k, cfg.clip_z, z);
    day.fenced_values += scale.fenced;
    day.unscaled_descriptors += (scale.names >= 2 && !scale.valid) ? 1U : 0U;
  };
  const auto style = [&](usize s, const auto& descriptor) {
    for (usize i = 0; i < n; ++i) raw[i] = p.present[row + i] ? descriptor(i) : nan;
    standardize(zbuf);
    for (usize i = 0; i < n; ++i) z_cur[i * style_count + s] = zbuf[i];
  };
  const auto cell = [&](usize d, usize i) -> f64 {
    return p.descriptors[d].empty() ? nan : static_cast<f64>(p.descriptors[d][row + i]);
  };
  style(0, [&](usize i) { return finite_positive(caps[i]) ? std::log(caps[i]) : nan; });
  const auto beta_of = [&](usize i, bool residual_vol) -> f64 {
    if (pairs[i] < cfg.min_beta_pairs) return nan;
    const f64 k = static_cast<f64>(pairs[i]);
    const f64 var_m = (smm[i] - sm[i] * sm[i] / k) / (k - 1);
    const f64 cov_rm = (srm[i] - sr[i] * sm[i] / k) / (k - 1);
    const f64 var_r = (srr[i] - sr[i] * sr[i] / k) / (k - 1);
    if (!(var_m > 0)) return nan;
    const f64 beta = cov_rm / var_m;
    return residual_vol ? std::sqrt(std::max(0.0, var_r - beta * cov_rm)) : beta;
  };
  style(1, [&](usize i) { return beta_of(i, false); });
  style(2, [&](usize i) { return beta_of(i, true); });
  style(3, [&](usize i) -> f64 {
    if (t < cfg.momentum_window) return nan;
    const usize a = (t - cfg.momentum_window) * n + i, b = (t - cfg.momentum_skip) * n + i;
    return p.present[a] && p.present[b] && finite_positive(p.close[a]) && finite_positive(p.close[b])
        ? std::log(p.close[b] / p.close[a]) : nan;
  });
  for (usize d = 0; d < 5; ++d) style(4 + d, [&](usize i) { return cell(d, i); });
  style(9, [&](usize i) -> f64 {
    if (t + 1 < cfg.adv_window) return nan;
    const f64 adv = adv_sum[i] / static_cast<f64>(cfg.adv_window);
    return adv > 0 ? std::log(adv) : nan;
  });
  // short_interest: mean of the standardized SI ratio and days to cover, re-standardized.
  for (usize i = 0; i < n; ++i) raw[i] = p.present[row + i] ? cell(style_si_ratio, i) : nan;
  standardize(zbuf);
  for (usize i = 0; i < n; ++i) raw[i] = p.present[row + i] ? cell(style_dtc, i) : nan;
  standardize(zbuf2);
  // The composite reads zbuf/zbuf2 while style() fills raw, before it overwrites zbuf.
  style(10, [&](usize i) -> f64 {
    const bool a = std::isfinite(cell(style_si_ratio, i)), b = std::isfinite(cell(style_dtc, i));
    if (!a && !b) return nan;
    return a && b ? 0.5 * (zbuf[i] + zbuf2[i]) : (a ? zbuf[i] : zbuf2[i]);
  });
}

ExposureAudit Model::audit_exposures(usize t) const {
  ExposureAudit audit;
  for (usize i = 0; i < n; ++i) {
    const u8 slot = slot_cur[i];
    if (slot == no_exposure) continue;
    audit.exposed[0] = true;
    audit.exposed[industry_factor(slot)] = true;
    if (slot == residual_industry) ++audit.residual_names;
    if (elig_cur[i]) audit.industry_cap[slot] += cap_at(t, i);
    for (usize s = 0; s < style_count; ++s)
      if (z_cur[i * style_count + s] != 0.0) audit.exposed[style_factor(s)] = true;
  }
  return audit;
}

// F_t: the fully observed block, then the structural forecasts (F2), then the audit of exposed
// factors left without a forecast.
void Model::factor_forecast(RiskDay& day) {
  std::fill(cov.begin(), cov.end(), nan);
  std::fill(prior_factor_var.begin(), prior_factor_var.end(), nan);
  std::fill(structural_flag.begin(), structural_flag.end(), u8{0});
  const ExposureAudit exposure = audit_exposures(day.date);
  day.residual_names = exposure.residual_names;
  std::vector<usize> keep;
  Eigen::MatrixXd full;
  if (fully_observed_forecast(day, keep, full) && cfg.structural_factor_forecast)
    structural_forecast(day, exposure, keep, full);
  for (usize k = 0; k < factor_count; ++k)
    if (exposure.exposed[k] && !std::isfinite(cov[k * factor_count + k]))
      ++day.unforecast_exposed_factors;
}

// lambda^2 * PSD(rho sigma sigma) over the factors with min_factor_history returns (the pre-F2
// forecast, unchanged). f = the pre-VRA block over `keep`; false when no factor qualifies.
bool Model::fully_observed_forecast(RiskDay& day, std::vector<usize>& keep, Eigen::MatrixXd& f) {
  std::vector<f64> var;
  for (usize k = 0; k < factor_count; ++k) {
    if (factor_fast.observations(k) < cfg.min_factor_history) continue;
    const f64 v = serial_adjusted_variance(factor_fast, factor_nw, k);
    if (!finite_positive(v)) continue;
    keep.push_back(k); var.push_back(v);
  }
  if (keep.empty()) return false;
  const usize m = keep.size();
  f.resize(ix(m), ix(m));
  for (usize a = 0; a < m; ++a) {
    f(ix(a), ix(a)) = var[a];
    const f64 ca = factor_corr.covariance(keep[a], keep[a]);
    for (usize b = a + 1; b < m; ++b) {
      const f64 cb = factor_corr.covariance(keep[b], keep[b]);
      const f64 cab = factor_corr.covariance(keep[a], keep[b]);
      const f64 denom = std::sqrt(std::max(0.0, ca)) * std::sqrt(std::max(0.0, cb));
      const f64 rho = denom > 0 && std::isfinite(cab) ? std::clamp(cab / denom, -1.0, 1.0) : 0.0;
      const f64 value = rho * std::sqrt(var[a]) * std::sqrt(var[b]);
      f(ix(a), ix(b)) = value; f(ix(b), ix(a)) = value;
    }
  }
  const Eigen::LLT<Eigen::MatrixXd> llt(f);
  if (llt.info() != Eigen::Success) {
    const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(f);
    if (eig.info() == Eigen::Success) {
      Eigen::VectorXd values = eig.eigenvalues();
      const f64 floor = std::max(1e-12, 1e-10 * f.trace() / static_cast<f64>(m));
      for (Eigen::Index e = 0; e < values.size(); ++e) values[e] = std::max(values[e], floor);
      Eigen::MatrixXd repaired = eig.eigenvectors() * values.asDiagonal() * eig.eigenvectors().transpose();
      f = 0.5 * (repaired + repaired.transpose());
    }
  }
  for (usize a = 0; a < m; ++a) {
    prior_factor_var[keep[a]] = f(ix(a), ix(a));
    for (usize b = 0; b < m; ++b)
      cov[keep[a] * factor_count + keep[b]] = day.lambda2_factor * f(ix(a), ix(b));
  }
  return true;
}

// Structural forecasts (F2; the rule is in the header): variance w own + (1 - w) prior with
// w = n / min_factor_history, correlations w rho (w w' rho between two structural factors),
// every structural off-diagonal term scaled by the largest s = 2^-j keeping F positive
// definite. Writes only structural rows and columns: `full` and prior_factor_var (the VRA's
// input) stay the fully observed block's.
void Model::structural_forecast(RiskDay& day, const ExposureAudit& exposure,
                                std::span<const usize> keep, const Eigen::MatrixXd& full) {
  const ClassPriors prior = class_priors(keep, full, exposure);
  const f64 horizon = static_cast<f64>(cfg.min_factor_history);
  std::vector<usize> factor;
  std::vector<f64> weight, variance;
  for (usize k = 1; k < factor_count; ++k) { // the market is never structural
    const usize obs = factor_fast.observations(k);
    if (obs >= cfg.min_factor_history || (obs == 0 && !exposure.exposed[k])) continue;
    const f64 class_prior = k <= industry_slots ? prior.industry : prior.style;
    if (!finite_positive(class_prior)) continue;
    const f64 own = obs ? serial_adjusted_variance(factor_fast, factor_nw, k) : nan;
    const bool has_own = std::isfinite(own);
    const f64 w = has_own ? static_cast<f64>(obs) / horizon : 0.0;
    factor.push_back(k); weight.push_back(w);
    variance.push_back(w * (has_own ? std::max(0.0, own) : 0.0) + (1.0 - w) * class_prior);
  }
  if (factor.empty()) return;
  const usize m = keep.size(), s = factor.size(), g = m + s;
  // base = diag blocks (the fully observed block, the structural variances); off = every
  // structural off-diagonal term, scaled below.
  Eigen::MatrixXd base = Eigen::MatrixXd::Zero(ix(g), ix(g));
  Eigen::MatrixXd off = Eigen::MatrixXd::Zero(ix(g), ix(g));
  base.topLeftCorner(ix(m), ix(m)) = full;
  for (usize u = 0; u < s; ++u) {
    const Eigen::Index row = ix(m + u);
    base(row, row) = variance[u];
    for (usize a = 0; a < m; ++a) {
      const f64 c = weight[u] * estimator_correlation(factor_corr, factor[u], keep[a]) *
                    std::sqrt(variance[u]) * std::sqrt(full(ix(a), ix(a)));
      off(row, ix(a)) = c; off(ix(a), row) = c;
    }
    for (usize v = u + 1; v < s; ++v) {
      const f64 rho = estimator_correlation(factor_corr, factor[u], factor[v]);
      const f64 c = weight[u] * weight[v] * rho * std::sqrt(variance[u]) * std::sqrt(variance[v]);
      off(row, ix(m + v)) = c; off(ix(m + v), row) = c;
    }
  }
  // base + s off is positive definite exactly for s in [0, s*) when base is (the eigenvalues of
  // base^-1/2 off base^-1/2 fix s*), so the first power of two that passes is the largest one
  // below s*. Bounded: 31 trials, then 0 (block diagonal).
  f64 scale = 0.0;
  for (int j = 0; j <= 30; ++j) {
    const f64 trial = std::ldexp(1.0, -j);
    const Eigen::LLT<Eigen::MatrixXd> llt(base + trial * off);
    if (llt.info() == Eigen::Success) { scale = trial; break; }
  }
  const f64 lambda2 = day.lambda2_factor;
  for (usize u = 0; u < s; ++u) {
    const usize k = factor[u];
    structural_flag[k] = 1;
    for (usize b = 0; b < g; ++b) {
      const usize other = b < m ? keep[b] : factor[b - m];
      const f64 value = b == m + u ? variance[u] : scale * off(ix(m + u), ix(b));
      cov[k * factor_count + other] = lambda2 * value;
      cov[other * factor_count + k] = lambda2 * value;
    }
  }
  day.structural_factors = s;
  day.structural_scale = scale;
}

// D_t: time-series (EWMA + NW) / structural blend (bounded, F3), Bayesian size-decile shrinkage
// to the cap-weighted median (F3), VRA. Records D_t's maximum; the invariant is checked by the
// driver.
co::Status Model::specific_forecast(RiskDay& day) {
  std::fill(spec.begin(), spec.end(), nan);
  std::fill(prior_spec_var.begin(), prior_spec_var.end(), nan);
  std::fill(sigma_str.begin(), sigma_str.end(), nan);
  std::fill(blend.begin(), blend.end(), nan);
  std::vector<u8> ind; std::vector<f64> sty, wgt, cap, val;
  for (usize i = 0; i < n; ++i) {
    sigma_ts[i] = nan;
    if (slot_cur[i] == no_exposure || spec_fast.observations(i) < 2) continue;
    const f64 v = serial_adjusted_variance(spec_fast, spec_nw, i);
    if (std::isfinite(v) && v >= 0) sigma_ts[i] = std::sqrt(std::max(v, 1e-12));
    if (!elig_cur[i] || history[i] < cfg.structural_history || !finite_positive(sigma_ts[i])) continue;
    const f64 c = cap_at(day.date, i);
    ind.push_back(slot_cur[i]); wgt.push_back(std::sqrt(c)); cap.push_back(c);
    val.push_back(std::log(sigma_ts[i]));
    sty.insert(sty.end(), z_cur.begin() + static_cast<std::ptrdiff_t>(i * style_count),
               z_cur.begin() + static_cast<std::ptrdiff_t>((i + 1) * style_count));
  }
  if (val.empty()) return co::Ok(); // no reliable specific history yet
  ATX_TRY(const StructuralVol structural,
          structural_specific_vol({ind, sty, wgt, cap, val}, cfg, slot_cur, z_cur, sigma_str,
                                  structural_vol_flag));
  day.structural_sigma_lower = structural.lower;
  day.structural_sigma_upper = structural.upper;
  blend_specific(day);
  ATX_TRY(const DecileShrinkage shrinkage,
          shrink_to_size_deciles(blend, p.cap.subspan(day.date * n, n), elig_cur, cfg.bayesian_q));
  for (usize i = 0; i < n; ++i) {
    const f64 shrunk = shrinkage.shrunk[i];
    if (!std::isfinite(shrunk)) continue;
    prior_spec_var[i] = shrunk * shrunk;
    spec[i] = day.lambda2_specific * prior_spec_var[i];
    ++day.specific_names;
    if (std::isnan(day.max_specific_variance) || spec[i] > day.max_specific_variance) {
      day.max_specific_variance = spec[i];
      day.max_specific_instrument = i;
    }
  }
  return co::Ok();
}

// sigma = gamma sigma_TS + (1 - gamma) sigma_STR, gamma = min(1, h / structural_history), and
// the audit of the structural names (gamma < 1) whose sigma_STR sat at a bound or was clamped.
void Model::blend_specific(RiskDay& day) {
  const f64 horizon = static_cast<f64>(cfg.structural_history);
  for (usize i = 0; i < n; ++i) {
    if (slot_cur[i] == no_exposure) continue;
    const f64 gamma = std::min(1.0, static_cast<f64>(history[i]) / horizon);
    const bool ts = std::isfinite(sigma_ts[i]), st = std::isfinite(sigma_str[i]);
    blend[i] = ts && st ? gamma * sigma_ts[i] + (1.0 - gamma) * sigma_str[i]
                        : (ts ? sigma_ts[i] : sigma_str[i]);
    if (!std::isfinite(blend[i]) || !(gamma < 1.0)) continue;
    ++day.structural_names;
    const u8 flag = structural_vol_flag[i];
    if (flag & structural_sigma_bounded) ++day.structural_bounded_names;
    if (flag & structural_exposure_clamped) ++day.structural_clamped_names;
  }
}
} // namespace

co::Status run_risk_model(const RiskPanel& p, const RiskModelConfig& cfg,
                          std::span<RiskSink* const> sinks) {
  ATX_TRY_VOID(validate_config(cfg));
  const usize cells = p.dates * p.instruments;
  if (!p.dates || !p.instruments || p.sessions.size() != p.dates || p.ids.size() != p.instruments ||
      p.close.size() != cells || p.raw_close.size() != cells || p.volume.size() != cells ||
      p.present.size() != cells || p.member.size() != cells || p.cap.size() != cells ||
      (!p.industry.empty() && p.industry.size() != cells))
    return co::Err(co::ErrorCode::InvalidArgument, "risk model: panel geometry");
  for (const auto& d : p.descriptors)
    if (!d.empty() && d.size() != cells)
      return co::Err(co::ErrorCode::InvalidArgument, "risk model: descriptor geometry");
  try {
    Model model(p, cfg);
    for (usize t = 0; t < p.dates; ++t) {
      RiskDay day;
      day.date = t; day.session = p.sessions[t];
      model.update_prices(t);
      model.regress(t, day);
      model.update_vra(t, day);
      model.exposures(t, day);
      model.factor_forecast(day);
      ATX_TRY_VOID(model.specific_forecast(day));
      // The v1.1 invariant (F3): refuse before any sink sees an out-of-range D_t.
      ATX_TRY_VOID(
          check_specific_variance(model.spec, p.ids, day.session, cfg.max_specific_variance));
      day.forecast = day.specific_names > 0 &&
          std::any_of(model.cov.begin(), model.cov.end(), [](f64 v) { return std::isfinite(v); });
      day.factor_return = model.factor_return; day.covariance = model.cov;
      day.specific_variance = model.spec; day.industry_slot = model.slot_cur;
      day.eligible = model.elig_cur; day.styles = model.z_cur;
      day.factor_structural = model.structural_flag;
      for (auto* sink : sinks) ATX_TRY_VOID(sink->on_day(day));
      std::swap(model.slot_prev, model.slot_cur);
      std::swap(model.elig_prev, model.elig_cur);
      std::swap(model.z_prev, model.z_cur);
    }
    return co::Ok();
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "risk model: allocation failed");
  }
}

// ---- bias harness --------------------------------------------------------------------------
f64 random_score(u64 seed, usize portfolio, u64 id) noexcept {
  const u64 h1 = splitmix(seed ^ splitmix(static_cast<u64>(portfolio) * 0x100000001b3ULL ^ splitmix(id)));
  const u64 h2 = splitmix(h1);
  const f64 u1 = (static_cast<f64>(h1 >> 11U) + 1.0) * 0x1.0p-53; // (0, 1]
  const f64 u2 = static_cast<f64>(h2 >> 11U) * 0x1.0p-53;         // [0, 1)
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi * u2);
}

BiasHarness::BiasHarness(const RiskPanel& panel, usize random_portfolios, u64 seed,
                         std::span<const BookWeight> book)
    : panel_(panel), random_(random_portfolios), seed_(seed), book_by_date_(panel.dates),
      has_book_(!book.empty()), prior_factor_var_(factor_count, nan) {
  for (const auto& w : book) {
    const auto s = std::lower_bound(panel.sessions.begin(), panel.sessions.end(), w.session);
    const auto id = std::lower_bound(panel.ids.begin(), panel.ids.end(), w.instrument_id);
    if (s == panel.sessions.end() || *s != w.session || id == panel.ids.end() ||
        *id != w.instrument_id || !std::isfinite(w.weight)) {
      ++unmatched_book_rows_;
      continue;
    }
    if (w.weight == 0) continue; // flat rows (L3 holdings.csv lists every name with state)
    book_by_date_[static_cast<usize>(s - panel.sessions.begin())].emplace_back(
        static_cast<usize>(id - panel.ids.begin()), w.weight);
  }
  held_.resize(random_ + (has_book_ ? 1U : 0U));
  scores_.resize(random_ * panel.instruments);
  for (usize q = 0; q < random_; ++q)
    for (usize i = 0; i < panel.instruments; ++i)
      scores_[q * panel.instruments + i] = random_score(seed_, q, panel.ids[i]);
  for (usize q = 0; q < random_; ++q) {
    std::string name = std::to_string(q);
    const std::string pad(3 - std::min<usize>(3, name.size()), '0');
    series_.push_back(BiasSeries{"random", "random-" + pad + name, {}, {}, {}, {}, 0, 0, 0, 0, 0});
  }
  for (usize k = 0; k < factor_count; ++k)
    series_.push_back(BiasSeries{"factor", factor_name(k), {}, {}, {}, {}, 0, 0, 0, 0, 0});
  if (has_book_) series_.push_back(BiasSeries{"book", "book", {}, {}, {}, {}, 0, 0, 0, 0, 0});
}

f64 dropped_share(const BiasSeries& s) noexcept {
  const usize dropped = s.dropped_factor_exposures + s.uncovered_name_returns;
  const usize total = dropped + s.z.size();
  return total ? static_cast<f64>(dropped) / static_cast<f64>(total) : 0.0;
}
SeriesStatus series_status(const BiasSeries& s) noexcept {
  if (s.z.empty()) return SeriesStatus::Empty;
  return dropped_share(s) > max_dropped_share ? SeriesStatus::Refused : SeriesStatus::Ok;
}
const char* series_status_name(SeriesStatus s) noexcept {
  switch (s) {
  case SeriesStatus::Ok: return "ok";
  case SeriesStatus::Refused: return "refused";
  case SeriesStatus::Empty: return "empty";
  }
  return "unknown";
}

co::Status BiasHarness::on_day(const RiskDay& day) {
  if (unmatched_book_rows_)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "bias harness: book weights with a session or instrument outside the role");
  realize(day);
  form(day);
  return co::Ok();
}

void BiasHarness::realize(const RiskDay& day) {
  const usize t = day.date;
  if (t == 0) return;
  const auto settle = [&](Held& held, BiasSeries& s) {
    if (held.excluded != Excluded::None) exclude(s, held.excluded);
    if (!held.live) return;
    f64 r = 0;
    for (const auto& [i, w] : held.weights) {
      const f64 x = interval_return(panel_, t, i);
      if (std::isfinite(x)) r += w * x; else ++s.missing_returns;
    }
    s.sessions.push_back(day.session); s.forecast_vol.push_back(held.vol);
    s.realized.push_back(r); s.z.push_back(r / held.vol);
  };
  for (usize q = 0; q < random_; ++q) settle(held_[q], series_[q]);
  if (has_book_) settle(held_.back(), series_.back());
  for (usize k = 0; k < factor_count; ++k) {
    const f64 f = day.factor_return[k], v = prior_factor_var_[k];
    if (!std::isfinite(f)) continue;
    auto& s = series_[random_ + k];
    if (!(v > 0)) { exclude(s, Excluded::FactorExposure); continue; } // the factor is unforecast
    s.sessions.push_back(day.session); s.forecast_vol.push_back(std::sqrt(v));
    s.realized.push_back(f); s.z.push_back(f / std::sqrt(v));
  }
}

void BiasHarness::apply_forecast(Held& held, Forecast forecast) noexcept {
  held.vol = forecast.vol;
  held.excluded = forecast.excluded;
  held.live = forecast.excluded == Excluded::None && std::isfinite(forecast.vol);
}
void BiasHarness::exclude(BiasSeries& s, Excluded why) noexcept {
  if (s.z.empty()) ++s.warmup_excluded;
  else if (why == Excluded::FactorExposure) ++s.dropped_factor_exposures;
  else ++s.uncovered_name_returns;
}

// x'Fx + sum w^2 D over every held name, or the reason there is no complete forecast: a held
// name without an exposure row or specific variance, or a nonzero exposure to a factor without
// a forecast. Nothing is dropped from the sums (R1 M-5).
BiasHarness::Forecast BiasHarness::forecast_vol(const RiskDay& day, const Held& held) const {
  std::array<f64, factor_count> x{};
  f64 specific = 0;
  for (const auto& [i, w] : held.weights) {
    const u8 slot = day.industry_slot[i];
    const f64 d = day.specific_variance[i];
    if (slot == no_exposure || !std::isfinite(d)) return {nan, Excluded::UncoveredName};
    x[0] += w; x[industry_factor(slot)] += w;
    for (usize s = 0; s < style_count; ++s) x[style_factor(s)] += w * day.styles[i * style_count + s];
    specific += w * w * d;
  }
  for (usize a = 0; a < factor_count; ++a)
    if (x[a] != 0 && !std::isfinite(day.covariance[a * factor_count + a]))
      return {nan, Excluded::FactorExposure};
  f64 systematic = 0;
  for (usize a = 0; a < factor_count; ++a) {
    if (x[a] == 0) continue;
    for (usize b = 0; b < factor_count; ++b) {
      const f64 c = day.covariance[a * factor_count + b];
      if (x[b] != 0 && std::isfinite(c)) systematic += x[a] * c * x[b];
    }
  }
  const f64 var = systematic + specific;
  return {var > 0 ? std::sqrt(var) : nan, Excluded::None};
}

void BiasHarness::form(const RiskDay& day) {
  for (auto& h : held_) { h.live = false; h.excluded = Excluded::None; h.weights.clear(); }
  std::fill(prior_factor_var_.begin(), prior_factor_var_.end(), nan);
  if (!day.forecast) return;
  for (usize k = 0; k < factor_count; ++k) prior_factor_var_[k] = day.covariance[k * factor_count + k];
  const usize n = panel_.instruments;
  std::vector<usize> names;
  for (usize i = 0; i < n; ++i)
    if (day.eligible[i] && day.industry_slot[i] != no_exposure && std::isfinite(day.specific_variance[i]))
      names.push_back(i);
  if (names.size() >= 2)
    for (usize q = 0; q < random_; ++q) {
      const f64* s = scores_.data() + q * n;
      f64 mean = 0;
      for (const usize i : names) mean += s[i];
      mean /= static_cast<f64>(names.size());
      f64 gross = 0;
      for (const usize i : names) gross += std::abs(s[i] - mean);
      if (!(gross > 0)) continue;
      auto& h = held_[q];
      for (const usize i : names) h.weights.emplace_back(i, (s[i] - mean) / gross);
      apply_forecast(h, forecast_vol(day, h));
    }
  if (has_book_) {
    auto& h = held_.back();
    auto& s = series_.back();
    // Every held name stays in the book: an uncovered one excludes the observation whole.
    for (const auto& [i, w] : book_by_date_[day.date]) {
      if (day.industry_slot[i] == no_exposure || !std::isfinite(day.specific_variance[i]))
        ++s.uncovered_names;
      h.weights.emplace_back(i, w);
    }
    if (!h.weights.empty()) apply_forecast(h, forecast_vol(day, h));
  }
}
} // namespace atx::impl::strategy::risk
