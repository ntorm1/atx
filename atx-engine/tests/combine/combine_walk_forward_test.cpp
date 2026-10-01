// combine_walk_forward_test.cpp — Lane 5: PIT walk-forward weight path + IC decay fit.
//
// Suites: CombineWalkForward, CombineDecayFit

#include <cmath>
#include <cstdint>
#include <memory>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/combine/decay_fit.hpp"
#include "atx/engine/combine/signal_combiner.hpp"
#include "atx/engine/combine/walk_forward_combiner.hpp"

namespace atx_test_l5_combine_walk_forward {

using atx::f64;
using atx::usize;
namespace cb = atx::engine::combine;

struct Rng {
  std::uint64_t s;
  f64 uni() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<f64>(s >> 11U) + 0.5) / 9007199254740992.0;
  }
  f64 gauss() {
    const f64 u1 = uni();
    const f64 u2 = uni();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};

constexpr usize kT = 60;
constexpr usize kN = 30;
constexpr usize kK = 3;

struct Raw {
  std::vector<std::vector<f64>> sig;
  std::vector<f64> fwd;
};

Raw make_raw(std::uint64_t seed) {
  Rng g{seed};
  Raw r{std::vector<std::vector<f64>>(kK, std::vector<f64>(kT * kN)), std::vector<f64>(kT * kN)};
  for (usize c = 0U; c < kT * kN; ++c) {
    f64 y = 0.0;
    for (usize a = 0U; a < kK; ++a) {
      r.sig[a][c] = g.gauss();
      y += 0.1 * static_cast<f64>(a + 1U) * r.sig[a][c];
    }
    r.fwd[c] = y + g.gauss();
  }
  return r;
}

cb::SignalStore to_store(const Raw &r) {
  auto st = cb::SignalStore::create(kT, kN);
  EXPECT_TRUE(st.has_value());
  for (const auto &s : r.sig) {
    EXPECT_TRUE(st->add_signal(s).has_value());
  }
  EXPECT_TRUE(st->set_forward_returns(r.fwd).has_value());
  return std::move(*st);
}

// Spy combiner: records every window it is asked to fit, returns uniform weights.
struct SpyCombiner {
  std::shared_ptr<std::vector<cb::FitWindow>> log = std::make_shared<std::vector<cb::FitWindow>>();
  [[nodiscard]] atx::core::Result<cb::CombineWeights> fit(const cb::SignalStore &s,
                                                          cb::FitWindow w) const {
    log->push_back(w);
    cb::CombineWeights out;
    out.w.assign(s.n_alphas(), 1.0 / static_cast<f64>(s.n_alphas()));
    out.fit_begin = w.begin;
    out.fit_end = w.end;
    return atx::core::Ok(std::move(out));
  }
};
static_assert(cb::Combiner<SpyCombiner>);

struct IntermittentInvalidCombiner {
  [[nodiscard]] atx::core::Result<cb::CombineWeights> fit(const cb::SignalStore &s,
                                                          cb::FitWindow w) const {
    cb::CombineWeights out;
    out.w.assign(s.n_alphas(), 1.0 / static_cast<f64>(s.n_alphas()));
    if (w.end == 1U || w.end == 3U) {
      out.w.back() = std::numeric_limits<f64>::quiet_NaN();
    } else if (w.end == 2U || w.end == 5U) {
      out.w.back() = std::numeric_limits<f64>::infinity();
    } else if (w.end >= 6U) {
      out.w.assign(s.n_alphas(), 0.0);
      out.w.front() = 1.0;
    }
    return atx::core::Ok(std::move(out));
  }
};

TEST(CombineWalkForward, InvalidFitsPreserveLastValidWeightsAndAllowRecovery) {
  const cb::SignalStore st = to_store(make_raw(4U));
  cb::WalkForwardCfg cfg;
  cfg.min_train = 1U;
  const auto path = cb::walk_forward(st, cfg, IntermittentInvalidCombiner{});
  ASSERT_TRUE(path.has_value());
  EXPECT_EQ(path->failed_fits, 4U);
  for (usize d = 0U; d < 4U; ++d) {
    EXPECT_FALSE(path->has_weights(d));
  }
  ASSERT_EQ(path->adopted_dates, (std::vector<usize>{4U, 6U}));
  EXPECT_TRUE(path->has_weights(4U));
  EXPECT_TRUE(path->has_weights(5U));
  for (usize a = 0U; a < kK; ++a) {
    EXPECT_DOUBLE_EQ(path->at(4U)[a], 1.0 / static_cast<f64>(kK));
    EXPECT_DOUBLE_EQ(path->at(5U)[a], path->at(4U)[a]);
    EXPECT_DOUBLE_EQ(path->at(6U)[a], a == 0U ? 1.0 : 0.0);
  }
}

TEST(CombineWalkForward, EmbargoCadenceAndRollingWindow) {
  const cb::SignalStore st = to_store(make_raw(1U));
  const SpyCombiner spy;
  cb::WalkForwardCfg cfg;
  cfg.horizon = 3U;
  cfg.min_train = 10U;
  cfg.lookback = 15U;
  cfg.refit_every = 5U;
  const auto p = cb::walk_forward(st, cfg, spy);
  ASSERT_TRUE(p.has_value());
  // First eligible date: fit_end = d − 3 + 1 >= 10 → d = 12.
  ASSERT_FALSE(p->refit_dates.empty());
  EXPECT_EQ(p->refit_dates.front(), 12U);
  ASSERT_EQ(spy.log->size(), p->refit_dates.size());
  for (usize i = 0U; i < p->refit_dates.size(); ++i) {
    const usize d = p->refit_dates[i];
    EXPECT_EQ((d - 12U) % 5U, 0U);
    const cb::FitWindow w = (*spy.log)[i];
    EXPECT_EQ(w.end, d + 1U - 3U) << "embargo: last usable row realizes at d";
    EXPECT_EQ(w.begin, (w.end > 15U) ? w.end - 15U : 0U);
  }
  EXPECT_FALSE(p->has_weights(11U));
  EXPECT_TRUE(p->has_weights(12U));
}

TEST(CombineWalkForward, PointInTimeNoFutureRows) {
  constexpr usize kD = 35;
  constexpr usize kH = 2;
  const Raw a = make_raw(2U);
  Raw b = a;
  Rng g{999U};
  for (usize c = 0U; c < kT * kN; ++c) {
    const usize t = c / kN;
    for (usize k = 0U; k < kK; ++k) {
      if (t > kD) {
        b.sig[k][c] = g.gauss();
      }
    }
    if (t + kH > kD) { // fwd rows t > D − h are not yet realized at D
      b.fwd[c] = g.gauss() * 5.0;
    }
  }
  const cb::SignalStore sa = to_store(a);
  const cb::SignalStore sb = to_store(b);
  cb::WalkForwardCfg cfg;
  cfg.horizon = kH;
  cfg.min_train = 8U;
  cfg.refit_every = 3U;
  for (int method = 0; method < 2; ++method) {
    const auto pa = (method == 0) ? cb::walk_forward(sa, cfg, cb::GrinoldKahnCombiner{})
                                  : cb::walk_forward(sa, cfg, cb::FamaMacBethRidge{0.01});
    const auto pb = (method == 0) ? cb::walk_forward(sb, cfg, cb::GrinoldKahnCombiner{})
                                  : cb::walk_forward(sb, cfg, cb::FamaMacBethRidge{0.01});
    ASSERT_TRUE(pa.has_value());
    ASSERT_TRUE(pb.has_value());
    for (usize d = 0U; d <= kD; ++d) {
      for (usize k = 0U; k < kK; ++k) {
        const f64 x = pa->at(d)[k];
        const f64 y = pb->at(d)[k];
        if (std::isnan(x)) {
          EXPECT_TRUE(std::isnan(y));
        } else {
          EXPECT_EQ(x, y) << "d=" << d << " k=" << k;
        }
      }
    }
    // After D the paths must diverge (the mutation is visible to later fits).
    bool diverged = false;
    for (usize d = kD + 1U; d < kT && !diverged; ++d) {
      diverged = pa->at(d)[0] != pb->at(d)[0];
    }
    EXPECT_TRUE(diverged);
  }
}

TEST(CombineWalkForward, HysteresisSuppressesReadoption) {
  const cb::SignalStore st = to_store(make_raw(3U));
  cb::WalkForwardCfg cfg;
  cfg.min_train = 10U;
  cfg.refit_every = 1U;
  cfg.hysteresis = 0.0;
  const auto loose = cb::walk_forward(st, cfg, cb::IcirEwmaCombiner{20.0, 0.0});
  cfg.hysteresis = 10.0; // > max L1 distance between two gross-1 vectors (2)
  const auto sticky = cb::walk_forward(st, cfg, cb::IcirEwmaCombiner{20.0, 0.0});
  ASSERT_TRUE(loose.has_value());
  ASSERT_TRUE(sticky.has_value());
  EXPECT_EQ(loose->adopted_dates.size(), loose->refit_dates.size());
  EXPECT_EQ(sticky->adopted_dates.size(), 1U);
  const auto first = sticky->at(sticky->adopted_dates.front());
  for (usize d = sticky->adopted_dates.front(); d < kT; ++d) {
    for (usize k = 0U; k < kK; ++k) {
      EXPECT_EQ(sticky->at(d)[k], first[k]);
    }
  }
  const std::vector<f64> pnl = cb::walk_forward_pnl(st, *loose);
  EXPECT_TRUE(std::isnan(pnl[0]));
  EXPECT_TRUE(std::isfinite(pnl[kT - 1U]));
}

// FamaMacBethRidge returns raw betas (not gross-1); hysteresis is measured on
// gross-normalized copies, so rescaling a combiner's output must not change which
// refits are adopted, and a threshold in [0, 2) must bite for FMB as for the others.
struct ScaledFmb {
  f64 scale = 1.0;
  [[nodiscard]] atx::core::Result<cb::CombineWeights> fit(const cb::SignalStore &s,
                                                          cb::FitWindow w) const {
    auto r = cb::FamaMacBethRidge{0.01}.fit(s, w);
    if (r.has_value()) {
      for (f64 &x : r->w) {
        x *= scale;
      }
    }
    return r;
  }
};
static_assert(cb::Combiner<ScaledFmb>);

TEST(CombineWalkForward, HysteresisIsScaleInvariantAcrossMethods) {
  const cb::SignalStore st = to_store(make_raw(5U));
  cb::WalkForwardCfg cfg;
  cfg.min_train = 10U;
  cfg.refit_every = 1U;
  cfg.hysteresis = 0.05;
  const auto small = cb::walk_forward(st, cfg, ScaledFmb{1e-3});
  const auto big = cb::walk_forward(st, cfg, ScaledFmb{1e3});
  const auto raw = cb::walk_forward(st, cfg, ScaledFmb{1.0});
  ASSERT_TRUE(small.has_value());
  ASSERT_TRUE(big.has_value());
  ASSERT_TRUE(raw.has_value());
  EXPECT_EQ(small->adopted_dates, raw->adopted_dates);
  EXPECT_EQ(big->adopted_dates, raw->adopted_dates);
  // The threshold actually suppresses some refits (it is not a no-op at FMB's scale).
  EXPECT_LT(raw->adopted_dates.size(), raw->refit_dates.size());
  EXPECT_GE(raw->adopted_dates.size(), 1U);
}

TEST(CombineWalkForward, RejectsBadConfig) {
  const cb::SignalStore st = to_store(make_raw(4U));
  cb::WalkForwardCfg cfg;
  cfg.horizon = 0U;
  EXPECT_FALSE(cb::walk_forward(st, cfg, cb::GrinoldKahnCombiner{}).has_value());
  cfg.horizon = 1U;
  cfg.refit_every = 0U;
  EXPECT_FALSE(cb::walk_forward(st, cfg, cb::GrinoldKahnCombiner{}).has_value());
}

TEST(CombineDecayFit, ExactExponentialIsRecovered) {
  std::vector<f64> ic(8);
  for (usize k = 0U; k < ic.size(); ++k) {
    ic[k] = -0.05 * std::exp(-0.2 * static_cast<f64>(k));
  }
  const auto f = cb::fit_ic_decay(ic);
  ASSERT_TRUE(f.has_value());
  EXPECT_NEAR(f->phi, 0.2, 1e-12);
  EXPECT_NEAR(f->ic0, -0.05, 1e-12);
  EXPECT_NEAR(f->half_life, std::log(2.0) / 0.2, 1e-10);
  EXPECT_EQ(f->n_lags, 8U);
}

TEST(CombineDecayFit, SignFlipTruncatesAndFlatIsInfinite) {
  const auto f = cb::fit_ic_decay(std::vector<f64>{0.04, 0.02, -0.01, 0.03});
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->n_lags, 2U);
  EXPECT_NEAR(f->half_life, 1.0, 1e-12);
  const auto flat = cb::fit_ic_decay(std::vector<f64>{0.03, 0.03, 0.03});
  ASSERT_TRUE(flat.has_value());
  EXPECT_TRUE(std::isinf(flat->half_life));
  EXPECT_FALSE(cb::fit_ic_decay(std::vector<f64>{0.03, -0.01}).has_value());
  EXPECT_FALSE(cb::fit_ic_decay(std::vector<f64>{0.03}).has_value());
}

TEST(CombineDecayFit, LaggedIcOfAr1SignalRecoversHalfLife) {
  constexpr usize kTT = 240;
  constexpr usize kNN = 400;
  constexpr f64 kRho = 0.8;
  Rng g{21U};
  std::vector<f64> x(kTT * kNN);
  std::vector<f64> r1(kTT * kNN);
  for (usize i = 0U; i < kNN; ++i) {
    x[i] = g.gauss();
  }
  for (usize t = 1U; t < kTT; ++t) {
    for (usize i = 0U; i < kNN; ++i) {
      x[t * kNN + i] = kRho * x[(t - 1U) * kNN + i] + std::sqrt(1.0 - kRho * kRho) * g.gauss();
    }
  }
  for (usize c = 0U; c < r1.size(); ++c) {
    r1[c] = 0.3 * x[c] + g.gauss();
  }
  auto st = cb::SignalStore::create(kTT, kNN);
  ASSERT_TRUE(st.has_value());
  ASSERT_TRUE(st->add_signal(x).has_value());
  const auto ic = cb::lagged_ic(*st, 0U, r1, 8U, {0, kTT});
  ASSERT_TRUE(ic.has_value());
  const auto f = cb::fit_ic_decay(*ic);
  ASSERT_TRUE(f.has_value());
  const f64 truth = std::log(2.0) / -std::log(kRho); // ≈ 3.106
  EXPECT_NEAR(f->half_life, truth, 0.15 * truth);
  EXPECT_GT(f->ic0, 0.2);
  // Firewall: a window ending at 100 never reads r1 rows >= 100.
  std::vector<f64> r1b(r1);
  for (usize c = 100U * kNN; c < r1b.size(); ++c) {
    r1b[c] = 0.0;
  }
  const auto a = cb::lagged_ic(*st, 0U, r1, 5U, {10, 100});
  const auto b = cb::lagged_ic(*st, 0U, r1b, 5U, {10, 100});
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(*a, *b);
}

} // namespace atx_test_l5_combine_walk_forward
