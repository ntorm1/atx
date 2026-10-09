// CSV serializers for the analytic bundles, following the house report style
// (`# key=value` meta header + deterministic rows; snprintf, not iostream).
// Depends only on the output struct definitions (analytics.hpp).

// std::fopen: silence MSVC's CRT deprecation-as-error. Must precede every
// include so it wins before <cstdio> is first pulled in (matches the house
// idiom used by the databento examples / atx-impl tests).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "atx/vol/api/analytics/analytics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

// `%.10g` — headline metric scalars (10 significant digits; readable, not meant
// to round-trip bit-exactly). Matches run_report.cpp::fmt10. A non-finite value
// (NaN/Inf) yields an EMPTY field deterministically, so the platform's `nan(ind)`
// token never lands in a cell or meta value.
void put_g10(std::string &s, double v) {
  if (!std::isfinite(v)) {
    return;
  }
  char buf[64];
  const int len = std::snprintf(buf, sizeof buf, "%.10g", v);
  s.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

// `%.17g` — full-precision series (round-trips a double bit-for-bit). Matches
// run_report.cpp's series columns. Non-finite → empty field (see `put_g10`).
void put_g17(std::string &s, double v) {
  if (!std::isfinite(v)) {
    return;
  }
  char buf[64];
  const int len = std::snprintf(buf, sizeof buf, "%.17g", v);
  s.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

void put_i64(std::string &s, std::int64_t v) {
  char buf[64];
  const int len = std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
  s.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

void put_u32(std::string &s, std::uint32_t v) {
  char buf[64];
  const int len = std::snprintf(buf, sizeof buf, "%lu", static_cast<unsigned long>(v));
  s.append(buf, static_cast<std::size_t>(len > 0 ? len : 0));
}

// FIX-E M-5 / AN-P2-6: the delta convention the wings were resolved under,
// written by NAME so the artifact says what "25 delta" meant rather than
// leaving the reader to guess (or to track down which enumerator `1` was).
[[nodiscard]] const char *delta_convention_name(DeltaConvention c) noexcept {
  switch (c) {
  case DeltaConvention::American:
    return "American";
  case DeltaConvention::Forward:
    return "Forward";
  }
  return "Unknown";
}

// A `%.10g` cell if `j` is in range, else an empty field — deterministic, and
// guards against reading past a short/absent aligned vector.
void put_g10_or_empty(std::string &s, const std::vector<double> &v, std::size_t j) {
  if (j < v.size()) {
    put_g10(s, v[j]);
  }
}

// Open in binary mode ("wb") so `\n` line endings are byte-identical on every
// platform, stream the whole assembled buffer with one fwrite, then close.
// Mirrors the house open/write/error sequence via <cstdio> (no iostreams).
[[nodiscard]] Status write_all(const std::string &out, std::string_view path, const char *who) {
  std::FILE *f = std::fopen(std::string(path).c_str(), "wb");
  if (f == nullptr) {
    return Err(ErrorCode::IoError, std::string(who) + ": cannot open file");
  }
  const std::size_t wrote = std::fwrite(out.data(), 1, out.size(), f);
  const int rc = std::fclose(f);
  if (wrote != out.size() || rc != 0) {
    return Err(ErrorCode::IoError, std::string(who) + ": write failed");
  }
  return Ok();
}

} // namespace

Status write_surface_analytics_csv(const SurfaceAnalytics &a, std::string_view path) {
  std::string out;
  out.reserve(a.tenors.size() * 512 + 512);

  // ── Meta header ──
  out += "# uid=";
  put_u32(out, a.uid);
  out += "\n# as_of_ts_ns=";
  put_i64(out, a.as_of_ts_ns);
  out += "\n# spot=";
  put_g10(out, a.spot);
  out += "\n# implied_emove=";
  put_g10(out, a.implied_emove);
  out += "\n# ts_slope_1m_3m=";
  put_g10(out, a.ts_slope_1m_3m);
  out += "\n# ts_slope_3m_1y=";
  put_g10(out, a.ts_slope_3m_1y);
  out += "\n# ts_ratio_1m_3m=";
  put_g10(out, a.ts_ratio_1m_3m);
  out += "\n# forward_vol_segments=";
  for (std::size_t i = 0; i < a.forward_vol_segments.size(); ++i) {
    if (i > 0) {
      out += ';';
    }
    put_g10(out, a.forward_vol_segments[i]);
  }
  out += "\n# backwardation=";
  out += a.backwardation ? '1' : '0';
  out += "\n# delta_convention=";
  out += delta_convention_name(a.delta_convention);
  out += "\n# valid=";
  out += a.valid ? '1' : '0';
  out += '\n';

  // Size the wing / moneyness column groups from the widest per-tenor vector
  // (capped at 8) so a caller with more than the default 4 deltas / 5 moneyness
  // points is not silently truncated (LOW-8).
  std::size_t n_wing = 0;
  std::size_t n_mvol = 0;
  for (const auto &t : a.tenors) {
    n_wing = std::max(n_wing, t.put_delta_vol.size());
    n_wing = std::max(n_wing, t.call_delta_vol.size());
    n_wing = std::max(n_wing, t.risk_reversal.size());
    n_wing = std::max(n_wing, t.butterfly.size());
    n_mvol = std::max(n_mvol, t.moneyness_vol.size());
  }
  n_wing = std::min<std::size_t>(n_wing, 8);
  n_mvol = std::min<std::size_t>(n_mvol, 8);

  // ── Header row: fixed columns, then `n_wing` wing groups, then `n_mvol`
  //    fixed-moneyness columns. ──
  out += "tenor_years,label,forward,df,atm_vol,atm_vol_ex_earn,n_earnings,event_var_share,"
         "skew_slope,skew_slope_sqrt_t,skew_slope_norm,curvature,skew_90_110,var_swap_vol,"
         "convexity_premium,expected_move,rnd_skewness,rnd_kurtosis,prob_below_forward,"
         "extrapolated,valid";
  for (std::size_t j = 0; j < n_wing; ++j) {
    const char d = static_cast<char>('0' + j);
    out += ",put_delta_vol_";
    out += d;
    out += ",call_delta_vol_";
    out += d;
    out += ",rr_";
    out += d;
    out += ",bf_";
    out += d;
  }
  for (std::size_t j = 0; j < n_mvol; ++j) {
    out += ",mvol_";
    out += static_cast<char>('0' + j);
  }
  out += '\n';

  // ── Data rows: one per tenor, same column order as the header. ──
  for (const auto &t : a.tenors) {
    put_g10(out, t.tenor_years);
    out += ',';
    out += t.label;
    out += ',';
    put_g10(out, t.forward);
    out += ',';
    put_g10(out, t.df);
    out += ',';
    put_g10(out, t.atm_vol);
    out += ',';
    put_g10(out, t.atm_vol_ex_earn);
    out += ',';
    put_i64(out, static_cast<std::int64_t>(t.n_earnings));
    out += ',';
    put_g10(out, t.event_var_share);
    out += ',';
    put_g10(out, t.skew_slope);
    out += ',';
    put_g10(out, t.skew_slope_sqrt_t);
    out += ',';
    put_g10(out, t.skew_slope_norm);
    out += ',';
    put_g10(out, t.curvature);
    out += ',';
    put_g10(out, t.skew_90_110);
    out += ',';
    put_g10(out, t.var_swap_vol);
    out += ',';
    put_g10(out, t.convexity_premium);
    out += ',';
    put_g10(out, t.expected_move);
    out += ',';
    put_g10(out, t.rnd_skewness);
    out += ',';
    put_g10(out, t.rnd_kurtosis);
    out += ',';
    put_g10(out, t.prob_below_forward);
    out += ',';
    out += t.extrapolated ? '1' : '0';
    out += ',';
    out += t.valid ? '1' : '0';
    for (std::size_t j = 0; j < n_wing; ++j) {
      out += ',';
      put_g10_or_empty(out, t.put_delta_vol, j);
      out += ',';
      put_g10_or_empty(out, t.call_delta_vol, j);
      out += ',';
      put_g10_or_empty(out, t.risk_reversal, j);
      out += ',';
      put_g10_or_empty(out, t.butterfly, j);
    }
    for (std::size_t j = 0; j < n_mvol; ++j) {
      out += ',';
      put_g10_or_empty(out, t.moneyness_vol, j);
    }
    out += '\n';
  }

  return write_all(out, path, "write_surface_analytics_csv");
}

Status write_surface_diff_csv(const SurfaceDiff &d, std::string_view path) {
  std::string out;
  out.reserve(d.tenors.size() * 128 + 256);

  // ── Meta header ──
  out += "# ts1_ns=";
  put_i64(out, d.ts1_ns);
  out += "\n# ts2_ns=";
  put_i64(out, d.ts2_ns);
  out += "\n# spot1=";
  put_g10(out, d.spot1);
  out += "\n# spot2=";
  put_g10(out, d.spot2);
  out += "\n# d_spot=";
  put_g10(out, d.d_spot);
  out += "\n# log_return=";
  put_g10(out, d.log_return);
  out += "\n# sticky_strike_atm_pred=";
  put_g10(out, d.sticky_strike_atm_pred);
  out += "\n# sticky_delta_atm_pred=";
  put_g10(out, d.sticky_delta_atm_pred);
  out += "\n# residual_atm_move=";
  put_g10(out, d.residual_atm_move);
  out += "\n# delta_convention=";
  out += delta_convention_name(d.delta_convention);
  out += "\n# valid=";
  out += d.valid ? '1' : '0';
  out += '\n';

  // ── Header + one row per TenorDiff. ──
  out += "tenor_years,label,d_forward,d_atm_vol,d_vol_fixed_strike,d_vol_fixed_delta,"
         "d_skew_slope,d_risk_reversal_25,d_butterfly_25,valid\n";

  for (const auto &t : d.tenors) {
    put_g10(out, t.tenor_years);
    out += ',';
    out += t.label;
    out += ',';
    put_g10(out, t.d_forward);
    out += ',';
    put_g10(out, t.d_atm_vol);
    out += ',';
    put_g10(out, t.d_vol_fixed_strike);
    out += ',';
    put_g10(out, t.d_vol_fixed_delta);
    out += ',';
    put_g10(out, t.d_skew_slope);
    out += ',';
    put_g10(out, t.d_risk_reversal_25);
    out += ',';
    put_g10(out, t.d_butterfly_25);
    out += ',';
    out += t.valid ? '1' : '0';
    out += '\n';
  }

  return write_all(out, path, "write_surface_diff_csv");
}

Status write_rnd_csv(const RiskNeutralDensity &r, std::string_view path) {
  std::string out;
  out.reserve(r.strikes.size() * 64 + 512);

  // ── Meta header ──
  out += "# T=";
  put_g10(out, r.T);
  out += "\n# forward=";
  put_g10(out, r.forward);
  out += "\n# df=";
  put_g10(out, r.df);
  out += "\n# mean=";
  put_g10(out, r.mean);
  out += "\n# variance=";
  put_g10(out, r.variance);
  out += "\n# skewness=";
  put_g10(out, r.skewness);
  out += "\n# kurtosis=";
  put_g10(out, r.kurtosis);
  out += "\n# bkm_variance=";
  put_g10(out, r.bkm_variance);
  out += "\n# bkm_skew=";
  put_g10(out, r.bkm_skew);
  out += "\n# bkm_kurt=";
  put_g10(out, r.bkm_kurt);
  out += "\n# skew_index=";
  put_g10(out, r.skew_index);
  out += "\n# var_swap_vol=";
  put_g10(out, r.var_swap_vol);
  out += "\n# mass_before_norm=";
  put_g10(out, r.mass_before_norm);
  out += "\n# prob_below_forward=";
  put_g10(out, r.prob_below_forward);
  out += "\n# extrapolated=";
  out += r.extrapolated ? '1' : '0';
  out += "\n# valid=";
  out += r.valid ? '1' : '0';
  out += '\n';

  // One `# quantile_<p>=<k>` line per aligned (p, k) pair.
  std::size_t nq = r.quantile_p.size();
  if (r.quantile_k.size() < nq) {
    nq = r.quantile_k.size();
  }
  for (std::size_t i = 0; i < nq; ++i) {
    out += "# quantile_";
    put_g10(out, r.quantile_p[i]);
    out += '=';
    put_g10(out, r.quantile_k[i]);
    out += '\n';
  }

  // ── Header + one full-precision row per grid point. ──
  out += "strike,pdf,cdf\n";
  for (std::size_t i = 0; i < r.strikes.size(); ++i) {
    put_g17(out, r.strikes[i]);
    out += ',';
    if (i < r.pdf.size()) {
      put_g17(out, r.pdf[i]);
    }
    out += ',';
    if (i < r.cdf.size()) {
      put_g17(out, r.cdf[i]);
    }
    out += '\n';
  }

  return write_all(out, path, "write_rnd_csv");
}

} // namespace atx::vol
