// atx::engine::risk — L7 risk-model validation scorecard (bodies).
//
// See model_validation.hpp for the statistics and the walk-forward protocol.
#include "atx/engine/risk/model_validation.hpp"

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
  return "{\"name\":\"" + b.name + "\",\"n\":" + std::to_string(b.n) + ",\"bias\":" +
         num(b.bias) + ",\"q\":" + num(b.q) + ",\"mean_pred_vol\":" + num(b.mean_pred_vol) +
         ",\"realized_vol\":" + num(b.realized_vol) +
         ",\"in_band\":" + (b.in_band ? "true" : "false") + "}";
}

} // namespace

std::string ValidationScorecard::to_json() const {
  std::string s = "{\"label\":\"" + label + "\",\"n_periods\":" + std::to_string(n_periods);
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
  s += ",\"mean_factors\":" + num(mean_factors) + "}";
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
    for (atx::usize j = 0U; j < m; ++j) {
      const atx::f64 v = ret.r(row, static_cast<Eigen::Index>(snap.assets[j]));
      r[j] = std::isnan(v) ? 0.0 : v;
    }
    auto book_ret = [&](const std::vector<atx::f64> &wt) {
      atx::f64 s = 0.0;
      for (atx::usize j = 0U; j < m; ++j) {
        s += wt[j] * r[j];
      }
      return s;
    };
    // Equal weight.
    w.assign(m, 1.0 / static_cast<atx::f64>(m));
    ew.add(book_ret(w), model.risk(w));
    // Random long/short books (unit gross).
    for (atx::usize b = 0U; b < cfg.n_random; ++b) {
      atx::f64 gross = 0.0;
      for (atx::usize j = 0U; j < m; ++j) {
        w[j] = gw(static_cast<Eigen::Index>(snap.assets[j]), static_cast<Eigen::Index>(b));
        gross += std::abs(w[j]);
      }
      if (!(gross > 0.0)) {
        continue;
      }
      for (atx::f64 &x : w) {
        x /= gross;
      }
      rnd[b].add(book_ret(w), model.risk(w));
    }
    // Unit-gross book from panel-indexed weights `src(asset)`; false when all zero.
    auto unit_gross = [&](auto &&src) {
      atx::f64 gross = 0.0;
      for (atx::usize j = 0U; j < m; ++j) {
        w[j] = src(snap.assets[j]);
        gross += std::abs(w[j]);
      }
      if (!(gross > 0.0)) {
        return false;
      }
      for (atx::f64 &x : w) {
        x /= gross;
      }
      return true;
    };
    for (atx::usize b = 0U; b < cfg.books.size(); ++b) {
      if (unit_gross([&](atx::usize i) { return cfg.books[b].w[i]; })) {
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
      atx::f64 gross = 0.0;
      for (const atx::f64 v : y) {
        gross += std::abs(v);
      }
      if (gross > 0.0) {
        for (atx::usize j = 0U; j < m; ++j) {
          w[j] = y[j] / gross;
        }
        opt[b].add(book_ret(w), model.risk(w));
      }
    }
    // Minimum variance (fully invested): w ∝ V⁻¹1.
    const std::vector<atx::f64> ones(m, 1.0);
    model.apply_inverse(ones, y);
    atx::f64 sy = 0.0;
    for (const atx::f64 v : y) {
      sy += v;
    }
    if (sy > 0.0) {
      for (atx::usize j = 0U; j < m; ++j) {
        w[j] = y[j] / sy;
      }
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
  return atx::core::Ok(std::move(sc));
}

} // namespace atx::engine::risk
