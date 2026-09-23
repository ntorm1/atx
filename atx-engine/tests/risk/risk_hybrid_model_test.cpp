// risk_hybrid_model_test.cpp — L7: hybrid fundamental + APCA factor model.
//
// Covers risk/hybrid_factor_model.hpp + the K_s selection kernels in
// risk/stat_factor_model.hpp:
//   * the structured per-date WLS equals a dense WLS reference;
//   * the market + industry design imposes the cap-weighted sum-to-zero constraint
//     exactly without changing the fit;
//   * a planted latent factor is recovered (count via Bai-Ng and Marchenko-Pastur,
//     loadings correlated with the truth);
//   * determinism and PIT (rows newer than as_of never read);
//   * the PanelView adapter runs end to end.

#include <bit>     // std::bit_cast
#include <cmath>   // std::abs, std::sqrt, std::exp, std::isnan
#include <cstdint> // std::uint64_t
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/linalg/regression.hpp" // wls (dense reference)
#include "atx/core/random.hpp"            // Xoshiro256pp
#include "atx/core/types.hpp"

#include "atx/engine/loop/panel_types.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/risk/hybrid_factor_model.hpp"
#include "atx/engine/risk/stat_factor_model.hpp"

namespace atx_test_l7_riskmodel_hybrid {

using atx::f64;
using atx::i64;
using atx::u32;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using namespace atx::engine::risk; // NOLINT(google-build-using-namespace) test-local

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// A simulated factor world: static styles, industries, caps, planted latent factor.
struct Sim {
  ReturnPanel ret;
  ExposureSeries exp;
  VecX latent_loading; // N
  VecX latent_ret;     // T (newest first)
};

Sim simulate(usize n, usize t, usize g, usize ks, f64 latent_vol, std::uint64_t seed,
             bool market = true) {
  atx::core::Xoshiro256pp rng{seed};
  Sim s;
  s.exp.market = market;
  s.exp.n_industries = static_cast<u32>(g);
  MatX style(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(ks));
  s.latent_loading.resize(static_cast<Eigen::Index>(n));
  for (usize i = 0; i < n; ++i) {
    if (g > 0U) {
      s.exp.industry.push_back(static_cast<u32>(i % g));
    }
    s.exp.cap.push_back(1e9 * std::exp(rng.normal()));
    for (usize l = 0; l < ks; ++l) {
      style(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(l)) = rng.normal();
    }
    s.latent_loading[static_cast<Eigen::Index>(i)] = rng.normal();
  }
  s.exp.style.push_back(style);
  const usize k = s.exp.n_factors();
  s.ret.r.resize(static_cast<Eigen::Index>(t), static_cast<Eigen::Index>(n));
  s.latent_ret.resize(static_cast<Eigen::Index>(t));
  for (usize d = 0; d < t; ++d) {
    VecX f(static_cast<Eigen::Index>(k));
    for (usize c = 0; c < k; ++c) {
      f[static_cast<Eigen::Index>(c)] = 0.01 * rng.normal();
    }
    const f64 h = latent_vol * rng.normal();
    s.latent_ret[static_cast<Eigen::Index>(d)] = h;
    for (usize i = 0; i < n; ++i) {
      const Eigen::Index ii = static_cast<Eigen::Index>(i);
      f64 r = market ? f[0] : 0.0;
      if (g > 0U) {
        r += f[static_cast<Eigen::Index>(s.exp.n_market() + s.exp.industry[i])];
      }
      for (usize l = 0; l < ks; ++l) {
        r += style(ii, static_cast<Eigen::Index>(l)) *
             f[static_cast<Eigen::Index>(s.exp.n_market() + g + l)];
      }
      r += s.latent_loading[ii] * h + 0.01 * rng.normal();
      s.ret.r(static_cast<Eigen::Index>(d), ii) = r;
    }
  }
  return s;
}

// Dense design at date t (rows = all assets; this sim has no missing data).
MatX dense_design(const ExposureSeries &e, usize n, bool with_market) {
  const usize g = e.n_ind();
  const usize ks = e.n_style();
  const usize m = with_market ? 1U : 0U;
  MatX x = MatX::Zero(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(m + g + ks));
  for (usize i = 0; i < n; ++i) {
    const Eigen::Index ii = static_cast<Eigen::Index>(i);
    if (with_market) {
      x(ii, 0) = 1.0;
    }
    if (g > 0U) {
      x(ii, static_cast<Eigen::Index>(m + e.industry[i])) = 1.0;
    }
    for (usize l = 0; l < ks; ++l) {
      x(ii, static_cast<Eigen::Index>(m + g + l)) = e.style[0](ii, static_cast<Eigen::Index>(l));
    }
  }
  return x;
}

f64 abs_corr(const VecX &a, const VecX &b) {
  const VecX da = a.array() - a.mean();
  const VecX db = b.array() - b.mean();
  return std::abs(da.dot(db) / std::sqrt(da.squaredNorm() * db.squaredNorm()));
}

// Tests live inside the named namespace (unity-build safe: no file-scope using).

TEST(RiskHybridModel, StructuredRegressionMatchesDenseWls) {
  const usize n = 80;
  const Sim s = simulate(n, 5, 3, 2, 0.0, 101U, /*market=*/false);
  const auto fr = estimate_factor_returns(s.ret, s.exp, 0U, 5U);
  ASSERT_TRUE(fr) << fr.error().message();
  ASSERT_EQ(fr->n_used, 5U);
  const MatX x = dense_design(s.exp, n, false);
  VecX w(static_cast<Eigen::Index>(n));
  for (usize i = 0; i < n; ++i) {
    w[static_cast<Eigen::Index>(i)] = std::sqrt(s.exp.cap[i]);
  }
  for (Eigen::Index t = 0; t < 5; ++t) {
    const VecX y = s.ret.r.row(t).transpose();
    const auto ref = atx::core::linalg::wls(x, y, w);
    ASSERT_TRUE(ref);
    for (Eigen::Index c = 0; c < x.cols(); ++c) {
      EXPECT_NEAR(fr->f(t, c), ref->beta[c], 1e-10);
    }
    for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(n); ++i) {
      EXPECT_NEAR(fr->resid(t, i), ref->residuals[i], 1e-10);
    }
    EXPECT_GT(fr->r2[static_cast<usize>(t)], 0.0);
  }
}

TEST(RiskHybridModel, IndustryConstraintIsExactAndFitUnchanged) {
  const usize n = 90;
  const Sim s = simulate(n, 4, 5, 3, 0.0, 202U, /*market=*/true);
  const auto fr = estimate_factor_returns(s.ret, s.exp, 0U, 4U);
  ASSERT_TRUE(fr);
  ASSERT_EQ(fr->n_used, 4U);
  const MatX x_nomkt = dense_design(s.exp, n, false); // same column space as with market
  VecX w(static_cast<Eigen::Index>(n));
  for (usize i = 0; i < n; ++i) {
    w[static_cast<Eigen::Index>(i)] = std::sqrt(s.exp.cap[i]);
  }
  for (Eigen::Index t = 0; t < 4; ++t) {
    f64 csum = 0.0;
    f64 ctot = 0.0;
    for (usize g = 0; g < 5U; ++g) {
      f64 c = 0.0;
      for (usize i = 0; i < n; ++i) {
        c += (s.exp.industry[i] == g) ? s.exp.cap[i] : 0.0;
      }
      csum += c * fr->f(t, static_cast<Eigen::Index>(1U + g));
      ctot += c;
    }
    EXPECT_NEAR(csum / ctot, 0.0, 1e-14);
    const auto ref = atx::core::linalg::wls(x_nomkt, VecX(s.ret.r.row(t).transpose()), w);
    ASSERT_TRUE(ref);
    for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(n); ++i) {
      EXPECT_NEAR(fr->resid(t, i), ref->residuals[i], 1e-10);
    }
  }
}

TEST(RiskHybridModel, RecoversPlantedLatentFactor) {
  const usize n = 400;
  const usize t = 160;
  const Sim s = simulate(n, t, 4, 2, 0.02, 303U);
  for (const StatFactorSelect sel : {StatFactorSelect::BaiNgIc2, StatFactorSelect::MarchenkoPastur}) {
    HybridCfg cfg;
    cfg.window = t - 1U; // style is static, returns cover rows [0, t)
    cfg.select = sel;
    const auto hm = HybridFactorModelBuilder::build(s.ret, s.exp, cfg, 0U);
    ASSERT_TRUE(hm) << hm.error().message();
    EXPECT_EQ(hm->k_stat, 1U) << "select " << static_cast<int>(sel);
    ASSERT_EQ(hm->assets.size(), n);
    EXPECT_EQ(hm->k_fundamental, 1U + 4U - 1U + 2U); // market + industries − ref + styles
    const MatX &x = hm->model.exposures();
    const VecX b = x.col(x.cols() - 1);
    EXPECT_GT(abs_corr(b, s.latent_loading), 0.95);
  }
  // A pure-noise residual (no latent factor) selects nothing.
  const Sim quiet = simulate(n, t, 4, 2, 0.0, 304U);
  HybridCfg cfg;
  cfg.window = t - 1U;
  const auto hq = HybridFactorModelBuilder::build(quiet.ret, quiet.exp, cfg, 0U);
  ASSERT_TRUE(hq);
  EXPECT_EQ(hq->k_stat, 0U);
}

TEST(RiskHybridModel, LatentFactorRiskIsCaptured) {
  const usize n = 300;
  const usize t = 200;
  const Sim s = simulate(n, t, 3, 2, 0.02, 404U);
  HybridCfg with;
  with.window = t - 1U;
  HybridCfg without = with;
  without.select = StatFactorSelect::Fixed;
  without.n_stat_fixed = 0U;
  const auto a = HybridFactorModelBuilder::build(s.ret, s.exp, with, 0U);
  const auto b = HybridFactorModelBuilder::build(s.ret, s.exp, without, 0U);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  // A portfolio long the latent loading: true variance ≈ (0.02·‖v‖²)² + noise; the
  // hybrid model sees it, the fundamental-only model books it as diversifiable noise.
  std::vector<f64> w(n);
  for (usize i = 0; i < n; ++i) {
    w[i] = s.latent_loading[static_cast<Eigen::Index>(i)] / static_cast<f64>(n);
  }
  f64 vv = 0.0;
  for (const f64 x : w) {
    vv += x * x;
  }
  const f64 truth = 0.02 * 0.02 * static_cast<f64>(n) * static_cast<f64>(n) * vv * vv + 1e-4 * vv;
  const f64 ra = a->model.risk(w);
  const f64 rb = b->model.risk(w);
  EXPECT_NEAR(ra / truth, 1.0, 0.35);
  EXPECT_LT(rb / truth, 0.5);
}

TEST(RiskHybridModel, DeterministicAndPitSafe) {
  const usize n = 120;
  const usize t = 90;
  Sim s = simulate(n, t, 3, 2, 0.01, 505U);
  HybridCfg cfg;
  cfg.window = 60U;
  const auto a = HybridFactorModelBuilder::build(s.ret, s.exp, cfg, 5U);
  const auto b = HybridFactorModelBuilder::build(s.ret, s.exp, cfg, 5U);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  std::vector<f64> w(n, 1.0 / static_cast<f64>(n));
  EXPECT_EQ(std::bit_cast<std::uint64_t>(a->model.risk(w)),
            std::bit_cast<std::uint64_t>(b->model.risk(w)));
  // Scramble the rows NEWER than as_of (rows 0..4): the model must not move.
  for (Eigen::Index r = 0; r < 5; ++r) {
    s.ret.r.row(r).setConstant(0.5);
  }
  const auto c = HybridFactorModelBuilder::build(s.ret, s.exp, cfg, 5U);
  ASSERT_TRUE(c);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(a->model.risk(w)),
            std::bit_cast<std::uint64_t>(c->model.risk(w)));
  EXPECT_EQ(c->model.fit_begin(), 5U);
  EXPECT_EQ(c->model.fit_end(), 65U);
}

TEST(RiskHybridModel, SkipsDatesAndAssetsWithMissingData) {
  const usize n = 60;
  Sim s = simulate(n, 40, 2, 1, 0.0, 606U);
  s.ret.r.row(3).setConstant(kNaN);      // a holiday: whole date missing
  s.ret.r(7, 11) = kNaN;                 // one missing return
  s.exp.style[0](20, 0) = kNaN;          // one asset without a style exposure
  const auto fr = estimate_factor_returns(s.ret, s.exp, 0U, 30U);
  ASSERT_TRUE(fr);
  EXPECT_EQ(fr->used[3], 0U);
  EXPECT_EQ(fr->n_used, 29U);
  EXPECT_TRUE(std::isnan(fr->resid(7, 11)));
  EXPECT_TRUE(std::isnan(fr->resid(0, 20)));
  HybridCfg cfg;
  cfg.window = 30U;
  const auto hm = HybridFactorModelBuilder::build(s.ret, s.exp, cfg, 0U);
  ASSERT_TRUE(hm);
  EXPECT_EQ(hm->assets.size(), n - 1U); // asset 20 has no exposure at as_of
}

TEST(RiskHybridModel, RejectsBadShapes) {
  Sim s = simulate(30, 20, 2, 1, 0.0, 707U);
  EXPECT_FALSE(estimate_factor_returns(s.ret, s.exp, 0U, 0U));
  EXPECT_FALSE(estimate_factor_returns(s.ret, s.exp, 5U, 16U));
  ExposureSeries bad = s.exp;
  bad.cap.pop_back();
  EXPECT_FALSE(estimate_factor_returns(s.ret, bad, 0U, 10U));
  ExposureSeries dyn = s.exp;
  dyn.style.assign(5, s.exp.style[0]); // non-static but too short
  EXPECT_FALSE(estimate_factor_returns(s.ret, dyn, 0U, 10U));
  HybridCfg cfg;
  cfg.window = 1U;
  EXPECT_FALSE(HybridFactorModelBuilder::build(s.ret, s.exp, cfg, 0U));
}

TEST(RiskHybridModel, SelectionKernels) {
  // Spectrum with 2 dominant eigenvalues over a flat noise floor.
  VecX ev(20);
  ev.setConstant(1.0);
  ev[0] = 400.0;
  ev[1] = 150.0;
  EXPECT_EQ(atx::engine::risk::detail::bai_ng_ic2(ev, 200U, 20U, 8U), 2U);
  EXPECT_EQ(atx::engine::risk::detail::bai_ng_ic2(ev, 200U, 20U, 1U), 1U); // k_max cap
  VecX flat(20);
  flat.setConstant(1.0);
  EXPECT_EQ(atx::engine::risk::detail::bai_ng_ic2(flat, 200U, 20U, 8U), 0U);
  // MP: pure noise below the edge ⇒ 0; one spike far above ⇒ 1.
  VecX noise(50);
  noise.setConstant(0.9);
  EXPECT_EQ(atx::engine::risk::detail::mp_edge_count(noise, noise.sum(), 50U, 200U, 8U).k, 0U);
  noise[0] = 20.0;
  const atx::engine::risk::detail::MpEdgeResult mp = atx::engine::risk::detail::mp_edge_count(noise, noise.sum(), 50U, 200U, 8U);
  EXPECT_EQ(mp.k, 1U);
  EXPECT_GT(mp.edge, 1.0);
  // Refinement lowers σ² to the noise share, lowering the edge (same count here).
  const atx::engine::risk::detail::MpEdgeResult mpr =
      atx::engine::risk::detail::mp_edge_count(noise, noise.sum(), 50U, 200U, 8U, true);
  EXPECT_EQ(mpr.k, 1U);
  EXPECT_LT(mpr.edge, mp.edge);
}

using atx::engine::InstrumentId;
using atx::engine::kPanelFieldCount;
using atx::engine::PanelField;
using atx::engine::PanelView;

// Minimal no-wrap PanelView backing store (grid row 0 = newest).
class HybridPanel {
public:
  HybridPanel(usize rows, usize inst, std::uint64_t seed)
      : rows_{rows}, inst_{inst}, cap_{1U}, words_{(inst + 63U) / 64U} {
    while (cap_ < rows) {
      cap_ <<= 1U;
    }
    for (usize i = 0; i < inst; ++i) {
      universe_.push_back(atx::core::domain::Symbol{static_cast<u32>(i + 1U)});
    }
    fields_.assign(kPanelFieldCount * cap_ * inst_, kNaN);
    mask_.assign(cap_ * words_, 0ULL);
    atx::core::Xoshiro256pp rng{seed};
    std::vector<f64> px(inst, 50.0);
    for (usize k = 0; k < rows; ++k) {
      const usize phys = k; // physical 0 = oldest
      const f64 mkt = 0.01 * rng.normal();
      for (usize i = 0; i < inst; ++i) {
        px[i] *= std::exp(mkt + 0.015 * rng.normal());
        for (const PanelField f : {PanelField::Open, PanelField::High, PanelField::Low,
                                   PanelField::Close}) {
          set(f, phys, i, px[i]);
        }
        set(PanelField::Volume, phys, i, 1e5);
        mask_[phys * words_ + (i >> 6U)] |= (1ULL << (i & 63U));
      }
    }
  }
  [[nodiscard]] PanelView view() const noexcept {
    return PanelView{fields_.data(), mask_.data(), std::span<const InstrumentId>{universe_},
                     cap_,           rows_ - 1U,   rows_,
                     words_};
  }

private:
  void set(PanelField f, usize phys, usize inst, f64 v) noexcept {
    fields_[static_cast<usize>(f) * cap_ * inst_ + phys * inst_ + inst] = v;
  }
  usize rows_;
  usize inst_;
  usize cap_;
  usize words_;
  std::vector<InstrumentId> universe_;
  std::vector<f64> fields_;
  std::vector<std::uint64_t> mask_;
};

TEST(RiskHybridModel, PanelAdapterEndToEnd) {
  const usize rows = 160;
  const usize inst = 80;
  const HybridPanel p{rows, inst, 808U};
  std::vector<i64> dates(rows);
  for (usize r = 0; r < rows; ++r) {
    dates[r] = 1000 - static_cast<i64>(r);
  }
  std::vector<f64> cap(inst);
  std::vector<u32> grp(inst);
  for (usize i = 0; i < inst; ++i) {
    cap[i] = 1e9 * (1.0 + static_cast<f64>(i % 7U));
    grp[i] = 10U + static_cast<u32>(i % 3U) * 5U; // sparse ids 10, 15, 20
  }
  FundamentalCfg fc;
  fc.mask = style_bit(StyleFactor::Market) | style_bit(StyleFactor::Size) |
            style_bit(StyleFactor::ResidVol) | style_bit(StyleFactor::STReversal);
  const auto ps = build_panel_series(p.view(), nullptr, fc, dates, cap, grp, 80U);
  ASSERT_TRUE(ps) << ps.error().message();
  EXPECT_EQ(ps->returns.n_dates(), 80U);
  EXPECT_EQ(ps->exposures.style.size(), 81U);
  EXPECT_EQ(ps->exposures.n_style(), 3U);
  EXPECT_EQ(ps->exposures.n_industries, 3U);
  EXPECT_TRUE(ps->exposures.market);
  EXPECT_EQ(ps->exposures.industry[1], 1U);
  HybridCfg cfg;
  cfg.window = 79U;
  const auto hm = HybridFactorModelBuilder::build(ps->returns, ps->exposures, cfg, 0U);
  ASSERT_TRUE(hm) << hm.error().message();
  EXPECT_EQ(hm->assets.size(), inst);
  EXPECT_EQ(hm->k_fundamental, 1U + 3U - 1U + 3U);
  std::vector<f64> w(inst, 1.0 / static_cast<f64>(inst));
  EXPECT_GT(hm->model.risk(w), 0.0);
}

} // namespace atx_test_l7_riskmodel_hybrid
