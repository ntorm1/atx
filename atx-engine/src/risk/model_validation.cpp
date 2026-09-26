// atx::engine::risk — L7 risk-model validation scorecard (bodies).
//
// See model_validation.hpp for the statistics and the walk-forward protocol.
#include "atx/engine/risk/model_validation.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include "atx/core/linalg/decompose.hpp"
#include <cmath>   // std::sqrt, std::log, std::isnan, std::abs, std::isfinite
#include <cstdio>  // std::snprintf
#include <utility> // std::move

#include "atx/core/macro.hpp"  // ATX_TRY
#include "atx/core/random.hpp" // Xoshiro256pp

namespace atx::engine::risk {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;

// Streaming moments of z = R/σ̂ (and of R, σ̂) for one book.
struct ZAcc {
  atx::usize n = 0U;
  atx::f64 sz = 0.0;
  atx::f64 szz = 0.0;
  atx::f64 sq = 0.0;
  atx::f64 spred = 0.0;
  atx::f64 sr = 0.0;
  atx::f64 srr = 0.0;

  void add(atx::f64 realized, atx::f64 pred_var) noexcept {
    if (!(pred_var > 0.0) || !std::isfinite(realized)) {
      return;
    }
    const atx::f64 sigma = std::sqrt(pred_var);
    const atx::f64 z = realized / sigma;
    ++n;
    sz += z;
    szz += z * z;
    const atx::f64 z2 = z * z;
    sq += (z2 > 0.0) ? z2 - std::log(z2) : 0.0;
    spred += sigma;
    sr += realized;
    srr += realized * realized;
  }
};

atx::f64 sample_sd(atx::usize n, atx::f64 s, atx::f64 ss) noexcept {
  if (n < 2U) {
    return 0.0;
  }
  const atx::f64 nf = static_cast<atx::f64>(n);
  const atx::f64 v = (ss - s * s / nf) / (nf - 1.0);
  return v > 0.0 ? std::sqrt(v) : 0.0;
}

BiasStat finish(const std::string &name, const ZAcc &a) {
  BiasStat b;
  b.name = name;
  b.n = a.n;
  if (a.n == 0U) {
    return b;
  }
  const atx::f64 nf = static_cast<atx::f64>(a.n);
  b.bias = sample_sd(a.n, a.sz, a.szz);
  b.q = a.sq / nf;
  b.mean_pred_vol = a.spred / nf;
  b.realized_vol = sample_sd(a.n, a.sr, a.srr);
  b.in_band = a.n >= 2U && std::abs(b.bias - 1.0) <= std::sqrt(2.0 / nf);
  return b;
}

// JSON number (%.10g; non-finite ⇒ null).
std::string num(atx::f64 v) {
  if (!std::isfinite(v)) {
    return "null";
  }
  char buf[40];
  const int len = std::snprintf(buf, sizeof(buf), "%.10g", v);
  return std::string(buf, static_cast<atx::usize>(len > 0 ? len : 0));
}

std::string bias_json(const BiasStat &b) {
  return "{\"name\":" + json_quote(b.name) + ",\"n\":" + std::to_string(b.n) + ",\"bias\":" +
         num(b.bias) + ",\"q\":" + num(b.q) + ",\"mean_pred_vol\":" + num(b.mean_pred_vol) +
         ",\"realized_vol\":" + num(b.realized_vol) +
         ",\"in_band\":" + (b.in_band ? "true" : "false") + "}";
}

} // namespace

std::string json_quote(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2U);
  out += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (ch) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20U) {
        char buf[8];
        const int len = std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
        out.append(buf, static_cast<atx::usize>(len > 0 ? len : 0));
      } else {
        out += ch;
      }
    }
  }
  out += '"';
  return out;
}

std::string ValidationScorecard::to_json() const {
  std::string s = "{\"label\":" + json_quote(label) + ",\"n_periods\":" + std::to_string(n_periods);
  s += ",\"band\":[" + num(band_lo) + "," + num(band_hi) + "]";
  s += ",\"equal_weight\":" + bias_json(equal_weight);
  s += ",\"min_variance\":" + bias_json(min_variance);
  s += ",\"random\":{\"bias_mean\":" + num(random_bias_mean) +
       ",\"in_band_frac\":" + num(random_in_band_frac) + ",\"q_mean\":" + num(random_q_mean) +
       "}";
  s += ",\"optimized\":{\"bias_mean\":" + num(optimized_bias_mean) +
       ",\"in_band_frac\":" + num(optimized_in_band_frac) + "}";
  s += ",\"books\":[";
  for (atx::usize i = 0U; i < books.size(); ++i) {
    s += (i == 0U ? "" : ",") + bias_json(books[i]);
  }
  s += "]";
  s += ",\"asset\":{\"n\":" + std::to_string(n_assets_scored) +
       ",\"bias_mean\":" + num(asset_bias_mean) + ",\"mrad\":" + num(asset_mrad) +
       ",\"in_band_frac\":" + num(asset_in_band_frac) + "}";
  s += ",\"mean_factors\":" + num(mean_factors);
  s += ",\"mean_excluded\":" + num(mean_excluded) + "}";
  return s;
}

RiskModelFactory hybrid_model_factory(const ReturnPanel &ret, const ExposureSeries &exp,
                                      const HybridCfg &cfg) {
  // SAFETY (lifetime): ret/exp are captured by reference per the documented contract.
  return [&ret, &exp, cfg](atx::usize as_of) -> atx::core::Result<ModelSnapshot> {
    ATX_TRY(HybridModel hm, HybridFactorModelBuilder::build(ret, exp, cfg, as_of));
    return atx::core::Ok(ModelSnapshot{std::move(hm.model), std::move(hm.assets)});
  };
}

atx::core::Result<ValidationScorecard>
validate_risk_model(const RiskModelFactory &factory, const ReturnPanel &ret,
                    const ValidationCfg &cfg) {
  if (cfg.first_as_of == 0U || cfg.n_periods == 0U || cfg.step == 0U ||
      cfg.first_as_of + (cfg.n_periods - 1U) * cfg.step >= ret.n_dates()) {
    return Err(ErrorCode::InvalidArgument,
               "validate_risk_model: need first_as_of >= 1, n_periods/step > 0, and the last "
               "as_of inside the return panel");
  }
  const atx::usize n = ret.n_assets();
  for (const NamedBook &b : cfg.books) {
    if (b.w.size() != n) {
      return Err(ErrorCode::InvalidArgument,
                 "validate_risk_model: every book must have one weight per panel asset");
    }
  }
  // Random book weights + random alphas: fixed per (asset, book), drawn once in
  // ascending (asset, book) order.
  atx::core::Xoshiro256pp rng{cfg.seed};
  MatX gw(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(cfg.n_random));
  MatX ga(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(cfg.n_optimized));
  for (Eigen::Index i = 0; i < gw.rows(); ++i) {
    for (Eigen::Index b = 0; b < gw.cols(); ++b) {
      gw(i, b) = rng.normal();
    }
    for (Eigen::Index b = 0; b < ga.cols(); ++b) {
      ga(i, b) = rng.normal();
    }
  }
  ZAcc ew;
  ZAcc mv;
  std::vector<ZAcc> rnd(cfg.n_random);
  std::vector<ZAcc> opt(cfg.n_optimized);
  std::vector<ZAcc> custom(cfg.books.size());
  std::vector<ZAcc> asset(n);
  atx::f64 k_sum = 0.0;
  atx::usize scored = 0U;
  std::vector<atx::f64> w;
  std::vector<atx::f64> r;
  std::vector<atx::u8> live; // per model row: 1 ⇔ the return at a−1 is observed
  atx::f64 excluded_sum = 0.0;
  for (atx::usize p = 0U; p < cfg.n_periods; ++p) {
    const atx::usize a = cfg.first_as_of + p * cfg.step;
    ATX_TRY(ModelSnapshot snap, factory(a));
    const FactorModel &model = snap.model;
    const atx::usize m = snap.assets.size();
    if (m == 0U || m != model.n_instruments()) {
      return Err(ErrorCode::InvalidArgument,
                 "validate_risk_model: snapshot asset list must match the model rows");
    }
    ++scored;
    k_sum += static_cast<atx::f64>(model.n_factors());
    const Eigen::Index row = static_cast<Eigen::Index>(a - 1U);
    r.assign(m, 0.0);
    live.assign(m, 0U);
    atx::usize n_live = 0U;
    for (atx::usize j = 0U; j < m; ++j) {
      const atx::f64 v = ret.r(row, static_cast<Eigen::Index>(snap.assets[j]));
      if (!std::isnan(v)) {
        r[j] = v;
        live[j] = 1U;
        ++n_live;
      }
    }
    excluded_sum += static_cast<atx::f64>(m - n_live);
    if (n_live == 0U) {
      continue; // nothing realized this period: no book can be scored
    }
    auto book_ret = [&](const std::vector<atx::f64> &wt) {
      atx::f64 s = 0.0;
      for (atx::usize j = 0U; j < m; ++j) {
        s += wt[j] * r[j];
      }
      return s;
    };
    // Drop the names with no realized return from `w` and renormalize to sum 1
    // (`by_sum`) or unit gross; false when nothing (or a non-positive sum) remains.
    auto restrict_live = [&](bool by_sum) {
      atx::f64 s = 0.0;
      for (atx::usize j = 0U; j < m; ++j) {
        if (live[j] == 0U) {
          w[j] = 0.0;
        }
        s += by_sum ? w[j] : std::abs(w[j]);
      }
      if (!(s > 0.0)) {
        return false;
      }
      for (atx::f64 &x : w) {
        x /= s;
      }
      return true;
    };
    // Equal weight over the live names.
    w.assign(m, 1.0);
    if (restrict_live(true)) {
      ew.add(book_ret(w), model.risk(w));
    }
    // Random long/short books (unit gross over the live names).
    for (atx::usize b = 0U; b < cfg.n_random; ++b) {
      for (atx::usize j = 0U; j < m; ++j) {
        w[j] = gw(static_cast<Eigen::Index>(snap.assets[j]), static_cast<Eigen::Index>(b));
      }
      if (restrict_live(false)) {
        rnd[b].add(book_ret(w), model.risk(w));
      }
    }
    for (atx::usize b = 0U; b < cfg.books.size(); ++b) {
      for (atx::usize j = 0U; j < m; ++j) {
        w[j] = cfg.books[b].w[snap.assets[j]];
      }
      if (restrict_live(false)) {
        custom[b].add(book_ret(w), model.risk(w));
      }
    }
    // Random-alpha optimized books: w ∝ V⁻¹α.
    std::vector<atx::f64> y(m, 0.0);
    std::vector<atx::f64> alpha(m, 0.0);
    for (atx::usize b = 0U; b < cfg.n_optimized; ++b) {
      for (atx::usize j = 0U; j < m; ++j) {
        alpha[j] = ga(static_cast<Eigen::Index>(snap.assets[j]), static_cast<Eigen::Index>(b));
      }
      model.apply_inverse(alpha, y);
      w = y;
      if (restrict_live(false)) {
        opt[b].add(book_ret(w), model.risk(w));
      }
    }
    // Minimum variance (fully invested): w ∝ V⁻¹1, restricted to the live names.
    const std::vector<atx::f64> ones(m, 1.0);
    model.apply_inverse(ones, y);
    w = y;
    if (restrict_live(true)) {
      mv.add(book_ret(w), model.risk(w));
    }
    // Asset level: σ̂_i² = x_i F x_iᵀ + d_i.
    const MatX &x = model.exposures();
    const MatX xf = x * model.factor_cov();
    const VecX &d = model.specific_var();
    for (atx::usize j = 0U; j < m; ++j) {
      const atx::f64 v = ret.r(row, static_cast<Eigen::Index>(snap.assets[j]));
      if (std::isnan(v)) {
        continue;
      }
      const Eigen::Index jj = static_cast<Eigen::Index>(j);
      asset[snap.assets[j]].add(v, xf.row(jj).dot(x.row(jj)) + d[jj]);
    }
  }

  ValidationScorecard sc;
  sc.label = cfg.label;
  sc.n_periods = scored;
  const atx::f64 half = std::sqrt(2.0 / static_cast<atx::f64>(scored));
  sc.band_lo = 1.0 - half;
  sc.band_hi = 1.0 + half;
  sc.equal_weight = finish("equal_weight", ew);
  sc.min_variance = finish("min_variance", mv);
  atx::usize nr = 0U;
  atx::usize nr_in = 0U;
  for (const ZAcc &z : rnd) {
    if (z.n < 2U) {
      continue;
    }
    const BiasStat b = finish("random", z);
    sc.random_bias_mean += b.bias;
    sc.random_q_mean += b.q;
    nr_in += b.in_band ? 1U : 0U;
    ++nr;
  }
  if (nr > 0U) {
    sc.random_bias_mean /= static_cast<atx::f64>(nr);
    sc.random_q_mean /= static_cast<atx::f64>(nr);
    sc.random_in_band_frac = static_cast<atx::f64>(nr_in) / static_cast<atx::f64>(nr);
  }
  atx::usize no = 0U;
  atx::usize no_in = 0U;
  for (const ZAcc &z : opt) {
    if (z.n < 2U) {
      continue;
    }
    const BiasStat b = finish("optimized", z);
    sc.optimized_bias_mean += b.bias;
    no_in += b.in_band ? 1U : 0U;
    ++no;
  }
  if (no > 0U) {
    sc.optimized_bias_mean /= static_cast<atx::f64>(no);
    sc.optimized_in_band_frac = static_cast<atx::f64>(no_in) / static_cast<atx::f64>(no);
  }
  for (atx::usize b = 0U; b < cfg.books.size(); ++b) {
    sc.books.push_back(finish(cfg.books[b].name, custom[b]));
  }
  atx::usize na_in = 0U;
  for (const ZAcc &z : asset) {
    if (z.n < cfg.min_asset_obs || z.n < 2U) {
      continue;
    }
    const BiasStat b = finish("asset", z);
    sc.asset_bias_mean += b.bias;
    sc.asset_mrad += std::abs(b.bias - 1.0);
    na_in += b.in_band ? 1U : 0U;
    ++sc.n_assets_scored;
  }
  if (sc.n_assets_scored > 0U) {
    const atx::f64 na = static_cast<atx::f64>(sc.n_assets_scored);
    sc.asset_bias_mean /= na;
    sc.asset_mrad /= na;
    sc.asset_in_band_frac = static_cast<atx::f64>(na_in) / na;
  }
  sc.mean_factors = k_sum / static_cast<atx::f64>(scored);
  sc.mean_excluded = excluded_sum / static_cast<atx::f64>(scored);
  return atx::core::Ok(std::move(sc));
}


namespace {
struct Acc21 {
  atx::usize n{}, unavailable{}, zero{};
  atx::f64 mz{}, m2z{}, mr{}, m2r{}, pred{}, q{};
  bool add(atx::f64 r, atx::f64 v) {
    if (!std::isfinite(r) || !std::isfinite(v) || v <= 0) return false;
    const auto sigma = std::sqrt(v), z = r / sigma, zz = z * z;
    if (!std::isfinite(zz)) return false;
    ++n;
    const auto dz = z - mz, dr = r - mr;
    mz += dz / static_cast<atx::f64>(n); m2z += dz * (z - mz);
    mr += dr / static_cast<atx::f64>(n); m2r += dr * (r - mr);
    pred += sigma;
    if (zz == 0) ++zero; else q += zz - std::log(zz) - 1.0;
    return std::isfinite(m2z) && std::isfinite(m2r) && std::isfinite(pred) && std::isfinite(q);
  }
  ValidationMetric21 finish(std::string name, std::string cohort) const {
    ValidationMetric21 out{std::move(name), std::move(cohort), n, unavailable, zero};
    out.defined = n >= 2;
    if (n > 0) {
      out.mean_pred_vol = pred / static_cast<atx::f64>(n);
      out.qlike = zero > 0 ? std::numeric_limits<atx::f64>::infinity() : q / static_cast<atx::f64>(n);
    }
    if (n >= 2) {
      out.bias = std::sqrt(std::max(0.0, m2z / static_cast<atx::f64>(n - 1)));
      out.realized_vol = std::sqrt(std::max(0.0, m2r / static_cast<atx::f64>(n - 1)));
      out.mrad = std::abs(out.bias - 1.0);
    }
    return out;
  }
};
}

atx::core::Result<ValidationScorecard21> validate_risk_model_21d(
    const RiskModelFactory& factory, const ReturnPanel& ret, const Validation21Cfg& cfg) {
  constexpr atx::usize horizon = 21;
  const auto n = ret.n_assets(), t = ret.n_dates();
  if (!factory || n == 0 || cfg.first_as_of < horizon || cfg.first_as_of >= t ||
      cfg.n_periods == 0 || cfg.step == 0 ||
      cfg.n_periods - 1 > (t - 1 - cfg.first_as_of) / cfg.step ||
      cfg.n_min_variance > 10'000 || cfg.n_optimized > 10'000 || cfg.books.size() > 10'000 ||
      n > cfg.max_working_bytes / 128 / (cfg.n_min_variance + cfg.n_optimized + 1))
    return Err(ErrorCode::InvalidArgument, "risk validation V2: invalid horizon/shape/budget");
  for (const auto& b : cfg.books) {
    if (b.w.size() != n) return Err(ErrorCode::InvalidArgument, "risk validation V2: book shape");
    for (auto w : b.w) if (!std::isfinite(w))
      return Err(ErrorCode::InvalidArgument, "risk validation V2: nonfinite book");
  }
  atx::core::Xoshiro256pp rng{cfg.seed};
  MatX budgets(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(cfg.n_min_variance));
  MatX alphas(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(cfg.n_optimized));
  for (Eigen::Index i = 0; i < budgets.rows(); ++i) {
    for (Eigen::Index j = 0; j < budgets.cols(); ++j) budgets(i,j) = rng.normal() > 0 ? 1.0 : 2.0;
    for (Eigen::Index j = 0; j < alphas.cols(); ++j) alphas(i,j) = rng.normal();
  }
  Acc21 ew, mv;
  std::vector<Acc21> minvars(cfg.n_min_variance), opt(cfg.n_optimized), custom(cfg.books.size()), eigen;
  std::vector<Acc21> decile(10);
  ValidationScorecard21 out;
  out.label = cfg.label; out.forecast_dates = cfg.n_periods; out.overlapping = cfg.step < horizon;
  atx::usize stable_k = 0;
  for (atx::usize period = 0; period < cfg.n_periods; ++period) {
    const auto a = cfg.first_as_of + period * cfg.step;
    ATX_TRY(auto snap, factory(a));
    const auto& model = snap.model;
    const auto m = snap.assets.size(), k = model.n_factors();
    if (m == 0 || m != model.n_instruments() || model.fit_begin() < a ||
        k == 0 || k > cfg.max_working_bytes / 128 / k || m > cfg.max_working_bytes / 128 / k)
      return Err(ErrorCode::InvalidArgument, "risk validation V2: snapshot shape/fit clock/budget");
    if (period == 0) { stable_k = k; eigen.resize(k); }
    if (k != stable_k)
      return Err(ErrorCode::InvalidArgument, "risk validation V2: eigenportfolio axis changes across forecasts");
    std::vector<atx::u8> seen(n,0), available(m,1);
    std::vector<atx::f64> realized(m,0), w(m), rhs(m), inverse(m);
    for (atx::usize i = 0; i < m; ++i) {
      if (snap.assets[i] >= n || seen[snap.assets[i]] != 0)
        return Err(ErrorCode::InvalidArgument, "risk validation V2: invalid/duplicate asset axis");
      seen[snap.assets[i]] = 1;
      for (atx::usize d = a - horizon; d < a; ++d) {
        const auto r = ret.r(static_cast<Eigen::Index>(d), static_cast<Eigen::Index>(snap.assets[i]));
        if (std::isinf(r)) return Err(ErrorCode::InvalidArgument, "risk validation V2: infinite realized return");
        if (std::isnan(r)) available[i] = 0; else realized[i] += r;
      }
      if (!std::isfinite(realized[i])) return Err(ErrorCode::OutOfRange, "risk validation V2: realized sum overflow");
    }
    const auto& provenance = model.estimator_diagnostics();
    if (provenance.factor_vra != PriorAdjustmentStatus::ObservedPriorV2 ||
        provenance.specific_vra != PriorAdjustmentStatus::ObservedPriorV2) ++out.unverified_vra_dates;
    auto score = [&](Acc21& acc) -> bool {
      atx::f64 gross = 0, r = 0;
      for (atx::usize i = 0; i < m; ++i) {
        if (!std::isfinite(w[i])) return false;
        gross += std::abs(w[i]);
        if (w[i] != 0 && available[i] == 0) { ++acc.unavailable; return true; }
      }
      if (!std::isfinite(gross) || gross <= 0) { ++acc.unavailable; return true; }
      for (atx::usize i = 0; i < m; ++i) { w[i] /= gross; r += w[i] * realized[i]; }
      return acc.add(r, static_cast<atx::f64>(horizon) * model.risk(w));
    };
    w.assign(m,1.0);
    if (!score(ew)) return Err(ErrorCode::OutOfRange, "risk validation V2: invalid equal-weight observation");
    rhs.assign(m,1.0); model.apply_inverse(rhs,inverse); w = inverse;
    if (!score(mv)) return Err(ErrorCode::OutOfRange, "risk validation V2: invalid min-variance observation");
    for (atx::usize j = 0; j < cfg.n_min_variance; ++j) {
      for (atx::usize i = 0; i < m; ++i) rhs[i] = budgets(static_cast<Eigen::Index>(snap.assets[i]),static_cast<Eigen::Index>(j));
      model.apply_inverse(rhs,inverse); w = inverse;
      // Minimum variance subject to the fixed random budget vector's unit
      // exposure; unit-gross rescaling leaves standardized forecast error invariant.
      if (!score(minvars[j])) return Err(ErrorCode::OutOfRange, "risk validation V2: invalid randomized min-variance observation");
    }
    for (atx::usize j = 0; j < cfg.n_optimized; ++j) {
      for (atx::usize i = 0; i < m; ++i) rhs[i] = alphas(static_cast<Eigen::Index>(snap.assets[i]),static_cast<Eigen::Index>(j));
      model.apply_inverse(rhs,inverse); w = inverse;
      if (!score(opt[j])) return Err(ErrorCode::OutOfRange, "risk validation V2: invalid optimized observation");
    }
    for (atx::usize j = 0; j < cfg.books.size(); ++j) {
      bool absent_holding = false;
      for (atx::usize i = 0; i < n; ++i) absent_holding |= seen[i] == 0 && cfg.books[j].w[i] != 0;
      if (absent_holding) { ++custom[j].unavailable; continue; }
      for (atx::usize i = 0; i < m; ++i) w[i] = cfg.books[j].w[snap.assets[i]];
      if (!score(custom[j])) return Err(ErrorCode::OutOfRange, "risk validation V2: invalid custom observation");
    }
    const auto& x = model.exposures(); const auto& d = model.specific_var();
    MatX dx = d.cwiseInverse().asDiagonal() * x;
    MatX gram = x.transpose() * dx;
    Eigen::LDLT<MatX> solve(gram);
    const bool invertible = solve.info() == Eigen::Success && solve.isPositive() && solve.rcond() > 1e-12;
    if (!invertible) {
      for (auto& acc : eigen) ++acc.unavailable;
      for (auto& acc : decile) ++acc.unavailable;
      continue;
    }
    ATX_TRY(auto eig, atx::core::linalg::symmetric_eig(model.factor_cov()));
    for (atx::usize j = 0; j < k; ++j) {
      // Minimum-specific-risk book with X' w = eigenvector(F), using a K x K solve.
      const VecX coeff = solve.solve(eig.vectors.col(static_cast<Eigen::Index>(j)));
      const VecX weights = dx * coeff;
      for (atx::usize i = 0; i < m; ++i) w[i] = weights[static_cast<Eigen::Index>(i)];
      if (!score(eigen[j])) return Err(ErrorCode::OutOfRange, "risk validation V2: invalid eigenportfolio observation");
    }
    if (std::find(available.begin(),available.end(),atx::u8{0}) != available.end()) {
      for (auto& acc : decile) ++acc.unavailable;
      continue;
    }
    const Eigen::Map<const VecX> r(realized.data(),static_cast<Eigen::Index>(m));
    const VecX beta = solve.solve(dx.transpose() * r);
    const VecX residual = r - x * beta;
    const MatX inverse_gram = solve.solve(MatX::Identity(static_cast<Eigen::Index>(k),static_cast<Eigen::Index>(k)));
    std::vector<atx::usize> order(m); std::iota(order.begin(),order.end(),0);
    std::stable_sort(order.begin(),order.end(),[&](auto i,auto j){return d[static_cast<Eigen::Index>(i)] < d[static_cast<Eigen::Index>(j)];});
    for (atx::usize rank = 0; rank < m; ++rank) {
      const auto i = static_cast<Eigen::Index>(order[rank]);
      auto& acc = decile[rank * 10 / m];
      // Frozen-exposure GLS residuals are ex-post diagnostics, not VRA records.
      // Correct residual variance for projection leverage: diag(M D M').
      const auto variance = static_cast<atx::f64>(horizon) *
          (d[i] - (x.row(i) * inverse_gram).dot(x.row(i)));
      if (variance <= 0) { ++acc.unavailable; continue; }
      if (!acc.add(residual[i],variance)) return Err(ErrorCode::OutOfRange, "risk validation V2: invalid residual-decile observation");
    }
  }
  out.metrics.push_back(ew.finish("equal_weight","equal_weight"));
  out.metrics.push_back(mv.finish("min_variance","min_variance"));
  for (atx::usize i=0;i<minvars.size();++i) out.metrics.push_back(minvars[i].finish("random_budget_"+std::to_string(i),"min_variance"));
  for (atx::usize i=0;i<opt.size();++i) out.metrics.push_back(opt[i].finish("optimized_"+std::to_string(i),"optimized"));
  for (atx::usize i=0;i<eigen.size();++i) out.metrics.push_back(eigen[i].finish("factor_eigen_"+std::to_string(i),"eigenportfolio"));
  for (atx::usize i=0;i<decile.size();++i) out.metrics.push_back(decile[i].finish("specific_decile_"+std::to_string(i),"specific_risk_decile"));
  for (atx::usize i=0;i<custom.size();++i) out.metrics.push_back(custom[i].finish(cfg.books[i].name,"custom"));
  return atx::core::Ok(std::move(out));
}

std::string ValidationScorecard21::to_json() const {
  std::string out = "{\"rule\":\"fixed-book-21d-v2\",\"label\":" + json_quote(label) +
      ",\"forecast_dates\":" + std::to_string(forecast_dates) +
      ",\"unverified_vra_dates\":" + std::to_string(unverified_vra_dates) +
      ",\"overlapping\":" + (overlapping ? "true" : "false") +
      ",\"calibrated_confidence_bands\":false,\"qlike\":\"z^2-log(z^2)-1; zero => infinity\",\"metrics\":[";
  for (atx::usize i=0;i<metrics.size();++i) {
    const auto& m=metrics[i];
    if (i>0) out+=",";
    out+="{\"name\":"+json_quote(m.name)+",\"cohort\":"+json_quote(m.cohort)+
        ",\"observations\":"+std::to_string(m.observations)+",\"unavailable\":"+std::to_string(m.unavailable)+
        ",\"zero_realizations\":"+std::to_string(m.zero_realizations)+",\"defined\":"+(m.defined?"true":"false")+
        ",\"bias\":"+(m.defined?num(m.bias):"null")+",\"mrad\":"+(m.defined?num(m.mrad):"null")+
        ",\"qlike\":"+(m.observations?num(m.qlike):"null")+
        ",\"mean_pred_vol\":"+(m.observations?num(m.mean_pred_vol):"null")+
        ",\"realized_vol\":"+(m.defined?num(m.realized_vol):"null")+"}";
  }
  return out+"]}";
}

} // namespace atx::engine::risk
