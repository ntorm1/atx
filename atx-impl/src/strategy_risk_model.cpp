#include "strategy_risk_model.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <numeric>
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
// Cross-sectional z-score (Barra): cap-weighted mean, equal-weighted SD over the universe,
// winsorized at +-winsor SD (second pass on the winsorized values), clipped at +-clip; names
// outside the universe get the same transform; missing -> 0. False (z all 0) when fewer than
// two universe values or no dispersion.
bool standardize(std::span<const f64> raw, std::span<const u8> universe, std::span<const f64> cap,
                 f64 winsor, f64 clip, std::span<f64> z) {
  const usize n = raw.size();
  std::fill(z.begin(), z.end(), 0.0);
  f64 lo = -std::numeric_limits<f64>::infinity(), hi = std::numeric_limits<f64>::infinity();
  f64 mu = 0, sd = 0;
  for (int pass = 0; pass < 2; ++pass) {
    f64 cw = 0, cx = 0; usize count = 0;
    for (usize i = 0; i < n; ++i) {
      if (!universe[i] || !std::isfinite(raw[i])) continue;
      const f64 v = std::clamp(raw[i], lo, hi);
      cw += cap[i]; cx += cap[i] * v; ++count;
    }
    if (count < 2 || !(cw > 0)) return false;
    mu = cx / cw;
    f64 ss = 0;
    for (usize i = 0; i < n; ++i) {
      if (!universe[i] || !std::isfinite(raw[i])) continue;
      const f64 d = std::clamp(raw[i], lo, hi) - mu;
      ss += d * d;
    }
    sd = std::sqrt(ss / static_cast<f64>(count - 1));
    if (!(sd > 0) || !std::isfinite(sd)) return false;
    if (pass == 0) { lo = mu - winsor * sd; hi = mu + winsor * sd; }
  }
  for (usize i = 0; i < n; ++i)
    if (std::isfinite(raw[i])) z[i] = std::clamp((std::clamp(raw[i], lo, hi) - mu) / sd, -clip, clip);
  return true;
}
} // namespace

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
      std::isfinite(c.winsor_sd) && c.winsor_sd > 0 && std::isfinite(c.clip_z) && c.clip_z > 0 &&
      c.min_regression_names >= 2;
  if (!ok) return co::Err(co::ErrorCode::InvalidArgument, "risk model: invalid configuration");
  return co::Ok();
}

// ---- regression ----------------------------------------------------------------------------
co::Result<CrossSectionFit> regress_cross_section(const CrossSection& x) {
  const usize rows = x.value.size();
  if (x.industry.size() != rows || x.styles.size() != rows * style_count ||
      x.weight.size() != rows || x.cap.size() != rows)
    return co::Err(co::ErrorCode::InvalidArgument, "risk regression: row geometry");
  std::array<usize, industry_slots> count{};
  std::array<f64, industry_slots> cap{};
  std::array<f64, style_count> mass{};
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
      mass[s] += x.weight[r] * z * z;
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
  for (usize s = 0; s < style_count; ++s)
    if (mass[s] > 1e-12 * wsum) index[style_factor(s)] = p++;
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
        sigma_ts(n, nan), sigma_str(n, nan), blend(n, nan) {}
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

  [[nodiscard]] f64 cap_at(usize t, usize i) const { return p.cap[t * n + i]; }
  void update_prices(usize t);
  void regress(usize t, RiskDay& day);
  void update_vra(usize t, RiskDay& day);
  void exposures(usize t);
  void factor_forecast(RiskDay& day);
  void specific_forecast(RiskDay& day);
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
  const auto fit = regress_cross_section({ind, sty, wgt, cap, val});
  if (!fit) return; // an unfittable session is a missing factor-return row
  day.fitted = true; day.r2 = fit->r2; day.active_industries = fit->active_industries;
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

// Exposures X_t: eligibility, industry slots (pooling small industries), 11 styles.
void Model::exposures(usize t) {
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
  const auto style = [&](usize s, const auto& descriptor) {
    for (usize i = 0; i < n; ++i) raw[i] = p.present[row + i] ? descriptor(i) : nan;
    standardize(raw, elig_cur, caps, cfg.winsor_sd, cfg.clip_z, zbuf);
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
  standardize(raw, elig_cur, caps, cfg.winsor_sd, cfg.clip_z, zbuf);
  for (usize i = 0; i < n; ++i) raw[i] = p.present[row + i] ? cell(style_dtc, i) : nan;
  standardize(raw, elig_cur, caps, cfg.winsor_sd, cfg.clip_z, zbuf2);
  // The composite reads zbuf/zbuf2 while style() fills raw, before it overwrites zbuf.
  style(10, [&](usize i) -> f64 {
    const bool a = std::isfinite(cell(style_si_ratio, i)), b = std::isfinite(cell(style_dtc, i));
    if (!a && !b) return nan;
    return a && b ? 0.5 * (zbuf[i] + zbuf2[i]) : (a ? zbuf[i] : zbuf2[i]);
  });
}

// F_t = lambda^2 * PSD(rho sigma sigma) over the factors with min_factor_history returns.
void Model::factor_forecast(RiskDay& day) {
  std::fill(cov.begin(), cov.end(), nan);
  std::fill(prior_factor_var.begin(), prior_factor_var.end(), nan);
  std::vector<usize> keep;
  std::vector<f64> var;
  for (usize k = 0; k < factor_count; ++k) {
    if (factor_fast.observations(k) < cfg.min_factor_history) continue;
    const f64 v = serial_adjusted_variance(factor_fast, factor_nw, k);
    if (!finite_positive(v)) continue;
    keep.push_back(k); var.push_back(v);
  }
  if (keep.empty()) return;
  const usize m = keep.size();
  Eigen::MatrixXd f(ix(m), ix(m));
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
}

// D_t: time-series (EWMA + NW) / structural blend, Bayesian size-decile shrinkage, VRA.
void Model::specific_forecast(RiskDay& day) {
  std::fill(spec.begin(), spec.end(), nan);
  std::fill(prior_spec_var.begin(), prior_spec_var.end(), nan);
  std::fill(sigma_str.begin(), sigma_str.end(), nan);
  std::vector<u8> ind; std::vector<f64> sty, wgt, cap, val; std::vector<usize> fit_names;
  f64 pop_sum = 0, pop_mass = 0;
  for (usize i = 0; i < n; ++i) {
    sigma_ts[i] = nan;
    if (slot_cur[i] == no_exposure || spec_fast.observations(i) < 2) continue;
    const f64 v = serial_adjusted_variance(spec_fast, spec_nw, i);
    if (std::isfinite(v) && v >= 0) sigma_ts[i] = std::sqrt(std::max(v, 1e-12));
    if (!elig_cur[i] || history[i] < cfg.structural_history || !finite_positive(sigma_ts[i])) continue;
    const f64 c = cap_at(day.date, i);
    ind.push_back(slot_cur[i]); wgt.push_back(std::sqrt(c)); cap.push_back(c);
    val.push_back(std::log(sigma_ts[i])); fit_names.push_back(i);
    sty.insert(sty.end(), z_cur.begin() + static_cast<std::ptrdiff_t>(i * style_count),
               z_cur.begin() + static_cast<std::ptrdiff_t>((i + 1) * style_count));
    pop_sum += c * sigma_ts[i]; pop_mass += c;
  }
  if (fit_names.empty()) return; // no reliable specific history yet
  bool structural = false;
  if (val.size() >= cfg.min_regression_names) {
    const auto fit = regress_cross_section({ind, sty, wgt, cap, val});
    if (fit) {
      f64 correction = 0;
      for (const f64 u : fit->residual) correction += std::exp(u);
      correction /= static_cast<f64>(fit->residual.size());
      structural = finite_positive(correction);
      for (usize i = 0; structural && i < n; ++i) {
        if (slot_cur[i] == no_exposure) continue;
        sigma_str[i] = correction * std::exp(predict(fit->factor, slot_cur[i],
            std::span<const f64>(z_cur).subspan(i * style_count, style_count)));
      }
    }
  }
  if (!structural)
    for (usize i = 0; i < n; ++i)
      if (slot_cur[i] != no_exposure) sigma_str[i] = pop_sum / pop_mass;
  const f64 horizon = static_cast<f64>(cfg.structural_history);
  for (usize i = 0; i < n; ++i) {
    blend[i] = nan;
    if (slot_cur[i] == no_exposure) continue;
    const f64 gamma = std::min(1.0, static_cast<f64>(history[i]) / horizon);
    const bool ts = std::isfinite(sigma_ts[i]), st = std::isfinite(sigma_str[i]);
    blend[i] = ts && st ? gamma * sigma_ts[i] + (1.0 - gamma) * sigma_str[i] : (ts ? sigma_ts[i] : sigma_str[i]);
    if (std::isfinite(blend[i]) && gamma < 1.0) ++day.structural_names;
  }
  // Bayesian shrinkage toward the cap-weighted size-decile mean (eligible names).
  std::vector<usize> order;
  for (usize i = 0; i < n; ++i)
    if (elig_cur[i] && std::isfinite(blend[i])) order.push_back(i);
  std::stable_sort(order.begin(), order.end(),
                   [&](usize a, usize b) { return cap_at(day.date, a) < cap_at(day.date, b); });
  std::vector<f64> shrunk(blend);
  const usize count = order.size();
  for (usize decile = 0; decile < 10 && count; ++decile) {
    const usize begin = decile * count / 10, end = (decile + 1) * count / 10;
    if (begin == end) continue;
    f64 sum = 0, mass = 0;
    for (usize r = begin; r < end; ++r) {
      const f64 c = cap_at(day.date, order[r]);
      sum += c * blend[order[r]]; mass += c;
    }
    const f64 target = sum / mass;
    f64 ss = 0;
    for (usize r = begin; r < end; ++r) ss += (blend[order[r]] - target) * (blend[order[r]] - target);
    const f64 dispersion = std::sqrt(ss / static_cast<f64>(end - begin));
    for (usize r = begin; r < end; ++r) {
      const usize i = order[r];
      const f64 distance = cfg.bayesian_q * std::abs(blend[i] - target);
      const f64 intensity = distance > 0 ? distance / (dispersion + distance) : 0.0;
      shrunk[i] = intensity * target + (1.0 - intensity) * blend[i];
    }
  }
  for (usize i = 0; i < n; ++i) {
    if (!std::isfinite(shrunk[i])) continue;
    prior_spec_var[i] = shrunk[i] * shrunk[i];
    spec[i] = day.lambda2_specific * prior_spec_var[i];
    ++day.specific_names;
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
      model.exposures(t);
      model.factor_forecast(day);
      model.specific_forecast(day);
      day.forecast = day.specific_names > 0 &&
          std::any_of(model.cov.begin(), model.cov.end(), [](f64 v) { return std::isfinite(v); });
      day.factor_return = model.factor_return; day.covariance = model.cov;
      day.specific_variance = model.spec; day.industry_slot = model.slot_cur;
      day.eligible = model.elig_cur; day.styles = model.z_cur;
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
    series_.push_back(BiasSeries{"random", "random-" + std::string(3 - std::min<usize>(3, name.size()), '0') + name, {}, {}, {}, {}, 0, 0});
  }
  for (usize k = 0; k < factor_count; ++k)
    series_.push_back(BiasSeries{"factor", factor_name(k), {}, {}, {}, {}, 0, 0});
  if (has_book_) series_.push_back(BiasSeries{"book", "book", {}, {}, {}, {}, 0, 0});
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
    if (!std::isfinite(f) || !(v > 0)) continue;
    auto& s = series_[random_ + k];
    s.sessions.push_back(day.session); s.forecast_vol.push_back(std::sqrt(v));
    s.realized.push_back(f); s.z.push_back(f / std::sqrt(v));
  }
}

f64 BiasHarness::forecast_vol(const RiskDay& day, const Held& held, usize& uncovered) const {
  std::array<f64, factor_count> x{};
  f64 specific = 0;
  for (const auto& [i, w] : held.weights) {
    const u8 slot = day.industry_slot[i];
    const f64 d = day.specific_variance[i];
    if (slot == no_exposure || !std::isfinite(d)) { ++uncovered; continue; }
    x[0] += w; x[industry_factor(slot)] += w;
    for (usize s = 0; s < style_count; ++s) x[style_factor(s)] += w * day.styles[i * style_count + s];
    specific += w * w * d;
  }
  f64 systematic = 0;
  for (usize a = 0; a < factor_count; ++a) {
    if (x[a] == 0 || !std::isfinite(day.covariance[a * factor_count + a])) continue;
    for (usize b = 0; b < factor_count; ++b) {
      const f64 c = day.covariance[a * factor_count + b];
      if (x[b] != 0 && std::isfinite(c)) systematic += x[a] * c * x[b];
    }
  }
  const f64 var = systematic + specific;
  return var > 0 ? std::sqrt(var) : nan;
}

void BiasHarness::form(const RiskDay& day) {
  for (auto& h : held_) { h.live = false; h.weights.clear(); }
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
      usize uncovered = 0;
      h.vol = forecast_vol(day, h, uncovered);
      h.live = std::isfinite(h.vol);
    }
  if (has_book_) {
    auto& h = held_.back();
    auto& s = series_.back();
    for (const auto& [i, w] : book_by_date_[day.date]) {
      if (day.industry_slot[i] == no_exposure || !std::isfinite(day.specific_variance[i])) {
        ++s.uncovered_names; continue;
      }
      h.weights.emplace_back(i, w);
    }
    usize ignored = 0;
    h.vol = h.weights.empty() ? nan : forecast_vol(day, h, ignored);
    h.live = std::isfinite(h.vol);
  }
}
} // namespace atx::impl::strategy::risk
