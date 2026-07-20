// WS-ZC1 diagnostic: owned-reconstruct vs borrowed-view parity over a REAL archive.
//
// The existing SurfaceArchiveV2 goldens compare a view against the PRE-serialization
// PricedSurface built by synthetic makers. This one compares the two things the replay
// path actually chooses between — `reconstruct_entry` (owned) vs `map_entry` (borrowed)
// — over a real `.atxvsa` produced by the SPY dispersion corpus, and reports the first
// divergence with its field and magnitude.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <utility>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "atx/vol/surface_archive.hpp"

using atx::vol::AmericanGreeks;
using atx::vol::PricedSurface;
using atx::vol::PricedSurfaceView;
using atx::vol::Side;
using atx::vol::SurfaceArchiveV2;

namespace {

constexpr const char *kArchive =
    "C:/atx-data/spy-dispersion/runs/bt-sota-baseline/archives/2026-01-02.atxvsa";

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] double rel(double a, double b) noexcept {
  const double scale = std::max(std::abs(a), std::abs(b));
  return scale > 0.0 ? std::abs(a - b) / scale : 0.0;
}

} // namespace

TEST(ZcViewParity, RealArchiveReconstructVsMapAgreeOnResolution) {
  if (!std::filesystem::exists(kArchive)) {
    GTEST_SKIP() << "reference archive not present";
  }
  auto arch = SurfaceArchiveV2::open_mapped(kArchive);
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();

  std::size_t checked = 0;
  std::size_t sigma_mismatch = 0;
  std::size_t rate_mismatch = 0;
  std::size_t fwd_mismatch = 0;
  std::size_t qeff_mismatch = 0;
  double worst_sigma = 0.0;
  double worst_rate = 0.0;
  std::string first_report;

  for (const auto &e : arch->directory()) {
    auto owned = arch->reconstruct_entry(e);
    ASSERT_TRUE(owned.has_value()) << owned.error().to_string();
    auto borrowed = arch->map_entry(e);
    ASSERT_TRUE(borrowed.has_value()) << borrowed.error().to_string();

    const PricedSurface &a = owned->surface;
    const PricedSurfaceView &v = borrowed->view;

    // Probe a T ladder spanning the surface and a moneyness ladder around each forward.
    for (const double T : {0.02, 0.05, 0.0833, 0.15, 0.25, 0.4, 0.75}) {
      const double fa = a.forward_at(T);
      const double fv = v.forward_at(T);
      const double qa = a.q_eff_at(T);
      const double qv = v.q_eff_at(T);
      const double ra = a.rate_at(T);
      const double rv = v.rate_at(T);
      if (!bits_equal(fa, fv)) {
        ++fwd_mismatch;
      }
      if (!bits_equal(qa, qv)) {
        ++qeff_mismatch;
      }
      if (!bits_equal(ra, rv)) {
        ++rate_mismatch;
        worst_rate = std::max(worst_rate, rel(ra, rv));
        if (first_report.empty()) {
          first_report = "rate_at(T=" + std::to_string(T) + ") owned=" + std::to_string(ra) +
                         " view=" + std::to_string(rv);
        }
      }
      if (!(fa > 0.0)) {
        continue;
      }
      for (const double m : {0.85, 0.95, 1.0, 1.05, 1.15}) {
        const double K = fa * m;
        ++checked;
        const auto pa = a.resolve(K, T);
        const auto pv = v.resolve(K, T);
        if (!bits_equal(pa.sigma, pv.sigma)) {
          ++sigma_mismatch;
          worst_sigma = std::max(worst_sigma, rel(pa.sigma, pv.sigma));
          if (first_report.empty()) {
            first_report = "sigma(K=" + std::to_string(K) + ",T=" + std::to_string(T) +
                           ") owned=" + std::to_string(pa.sigma) +
                           " view=" + std::to_string(pv.sigma);
          }
        }
      }
    }
  }

  std::printf("[zc-diag] surfaces=%zu probes=%zu\n",
              static_cast<std::size_t>(arch->directory().size()), checked);
  std::printf("[zc-diag] fwd_mismatch=%zu qeff_mismatch=%zu rate_mismatch=%zu (worst rel %.3e)\n",
              fwd_mismatch, qeff_mismatch, rate_mismatch, worst_rate);
  std::printf("[zc-diag] sigma_mismatch=%zu (worst rel %.3e)\n", sigma_mismatch, worst_sigma);
  std::printf("[zc-diag] first: %s\n", first_report.c_str());

  EXPECT_EQ(fwd_mismatch, 0u);
  EXPECT_EQ(qeff_mismatch, 0u);
  EXPECT_EQ(rate_mismatch, 0u);
  EXPECT_EQ(sigma_mismatch, 0u);
}

// REGRESSION GUARD for the WS-ZC1 seed fix.
//
// `PricedSurface::full_greek_seed` produces its seed through a ONE-ELEMENT
// `evaluate_batch` so the seed rides the same LANED analytic-Greek kernel the batch
// uses. `PricedSurfaceView`'s was left on the scalar `evaluate()`, which was invisible
// until the replay path started borrowing views — then seeds were solved SCALAR inside
// a LANED book and `seeded == fresh` silently degraded from bit-identity to the
// ~1e-13 AVX2-vs-scalar delta, which the P&L amplified to ~1e-8 in run output.
//
// This asserts BIT-identity between each type's `full_greek_seed` and its OWN
// one-element `evaluate_batch`. Putting either type's seed back on the scalar route
// fails this immediately, on real archive surfaces and on both Greek routes.
TEST(ZcViewParity, FullGreekSeedRidesTheBatchSeamOnBothSurfaceTypes) {
  if (!std::filesystem::exists(kArchive)) {
    GTEST_SKIP() << "reference archive not present";
  }
  auto arch = SurfaceArchiveV2::open_mapped(kArchive);
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();

  using EF = PricedSurface::EvalField;
  constexpr EF kSeedFields = EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder;

  // One element through evaluate_batch — exactly what a correct seed must reproduce.
  const auto batch_of_one = [&](const auto &surface, double K, double T, Side side,
                                bool analytic) {
    std::array<double, 1> Ks{K};
    std::array<double, 1> Ts{T};
    std::array<Side, 1> sides{side};
    std::array<double, 1> iv{};
    std::array<double, 1> px{};
    std::array<AmericanGreeks, 1> gk{};
    std::array<atx::vol::Status, 1> st{};
    const auto rc = surface.evaluate_batch(Ks, Ts, sides, kSeedFields, analytic,
                                           PricedSurface::EvaluationSoA{iv, px, gk, st, {}, {}});
    EXPECT_TRUE(rc.has_value());
    return std::pair{iv[0], gk[0]};
  };

  const auto gk_eq = [](const AmericanGreeks &a, const AmericanGreeks &b) {
    return bits_equal(a.delta, b.delta) && bits_equal(a.gamma, b.gamma) &&
           bits_equal(a.vega, b.vega) && bits_equal(a.theta, b.theta) &&
           bits_equal(a.rho, b.rho) && bits_equal(a.vanna, b.vanna) &&
           bits_equal(a.volga, b.volga) && bits_equal(a.charm, b.charm);
  };

  std::size_t checked = 0;
  for (const auto &e : arch->directory()) {
    auto owned = arch->reconstruct_entry(e);
    ASSERT_TRUE(owned.has_value());
    auto borrowed = arch->map_entry(e);
    ASSERT_TRUE(borrowed.has_value());
    const PricedSurface &a = owned->surface;
    const PricedSurfaceView &v = borrowed->view;

    for (const double T : {0.0833, 0.25}) {
      const double f = a.forward_at(T);
      if (!(f > 0.0)) {
        continue;
      }
      for (const double m : {0.9, 1.0, 1.1}) {
        for (const Side side : {Side::Call, Side::Put}) {
          for (const bool analytic : {true, false}) {
            const double K = f * m;
            ++checked;
            const auto seed_a = a.full_greek_seed(K, T, side, analytic);
            const auto seed_v = v.full_greek_seed(K, T, side, analytic);
            ASSERT_EQ(seed_a.has_value(), seed_v.has_value());
            if (!seed_a.has_value()) {
              continue;
            }
            const auto [biv_a, bgk_a] = batch_of_one(a, K, T, side, analytic);
            const auto [biv_v, bgk_v] = batch_of_one(v, K, T, side, analytic);

            // Each type's seed == its OWN batch (neither may take a second route).
            EXPECT_TRUE(bits_equal(seed_a->iv(), biv_a)) << "owned seed iv off batch seam";
            EXPECT_TRUE(gk_eq(seed_a->greeks(), bgk_a)) << "owned seed greeks off batch seam";
            EXPECT_TRUE(bits_equal(seed_v->iv(), biv_v)) << "view seed iv off batch seam";
            EXPECT_TRUE(gk_eq(seed_v->greeks(), bgk_v)) << "view seed greeks off batch seam";
            // ...and therefore owned seed == borrowed seed.
            EXPECT_TRUE(gk_eq(seed_a->greeks(), seed_v->greeks()))
                << "owned and borrowed seeds disagree";
          }
        }
      }
    }
  }
  EXPECT_GT(checked, 0u);
  std::printf("[zc-seed] seeds checked=%zu\n", checked);
}

// Same two objects, but through the HOT batch seam the pricer actually uses.
TEST(ZcViewParity, RealArchiveReconstructVsMapAgreeOnEvaluateBatch) {
  if (!std::filesystem::exists(kArchive)) {
    GTEST_SKIP() << "reference archive not present";
  }
  auto arch = SurfaceArchiveV2::open_mapped(kArchive);
  ASSERT_TRUE(arch.has_value()) << arch.error().to_string();

  using EF = PricedSurface::EvalField;
  const EF fields = EF::Iv | EF::Price | EF::FirstOrder | EF::SecondOrder;

  std::size_t px_mismatch = 0;
  std::size_t gr_mismatch = 0;
  std::size_t status_mismatch = 0;
  double worst_px = 0.0;
  double worst_gr = 0.0;
  std::string first_report;

  for (const auto &e : arch->directory()) {
    auto owned = arch->reconstruct_entry(e);
    ASSERT_TRUE(owned.has_value());
    auto borrowed = arch->map_entry(e);
    ASSERT_TRUE(borrowed.has_value());
    const PricedSurface &a = owned->surface;
    const PricedSurfaceView &v = borrowed->view;

    // A ladder shaped like the backtest's: shared-T runs, mixed sides. The T set is
    // derived from the surface's OWN pillars plus points just inside/outside them and
    // in the short-end extrapolation region, because those are exactly the corners
    // where the bracket/extrapolation branches can diverge.
    std::vector<double> tenors;
    for (const auto &sc : a.context()) {
      tenors.push_back(sc.T);
      tenors.push_back(sc.T * 0.999);
      tenors.push_back(sc.T * 1.001);
    }
    if (!tenors.empty()) {
      const double t0 = a.context().front().T;
      tenors.push_back(t0 * 0.25); // short-end extrapolation (T < T_front)
      tenors.push_back(t0 * 0.5);
      tenors.push_back(a.context().back().T * 1.5); // long-end flat
    }
    std::sort(tenors.begin(), tenors.end());

    std::vector<double> K, T;
    std::vector<Side> side;
    for (const double t : tenors) {
      const double f = a.forward_at(t);
      if (!(f > 0.0)) {
        continue;
      }
      // Four strikes per tenor => genuine shared-T runs, mixed sides within a run.
      int slot = 0;
      for (const double m : {0.9, 1.0, 1.05, 1.1}) {
        K.push_back(f * m);
        T.push_back(t);
        side.push_back((slot++ % 2) ? Side::Call : Side::Put);
      }
    }
    if (K.empty()) {
      continue;
    }
    const std::size_t n = K.size();

    for (const bool analytic : {true, false}) {
      std::vector<double> iv_a(n), iv_v(n), px_a(n), px_v(n);
      std::vector<AmericanGreeks> gr_a(n), gr_v(n);
      std::vector<atx::vol::Status> st_a(n), st_v(n);
      PricedSurface::EvaluationSoA out_a{iv_a, px_a, gr_a, st_a, {}, {}};
      PricedSurface::EvaluationSoA out_v{iv_v, px_v, gr_v, st_v, {}, {}};
      const auto sa = a.evaluate_batch(K, T, side, fields, analytic, out_a);
      const auto sv = v.evaluate_batch(K, T, side, fields, analytic, out_v);
      ASSERT_EQ(sa.has_value(), sv.has_value());
      for (std::size_t i = 0; i < n; ++i) {
        if (st_a[i].has_value() != st_v[i].has_value()) {
          ++status_mismatch;
        }
        if (!bits_equal(px_a[i], px_v[i])) {
          ++px_mismatch;
          worst_px = std::max(worst_px, rel(px_a[i], px_v[i]));
          if (first_report.empty()) {
            first_report = "price analytic=" + std::to_string(analytic) +
                           " i=" + std::to_string(i) + " owned=" + std::to_string(px_a[i]) +
                           " view=" + std::to_string(px_v[i]);
          }
        }
        const AmericanGreeks &ga = gr_a[i];
        const AmericanGreeks &gv = gr_v[i];
        if (!bits_equal(ga.delta, gv.delta) || !bits_equal(ga.gamma, gv.gamma) ||
            !bits_equal(ga.vega, gv.vega) || !bits_equal(ga.theta, gv.theta) ||
            !bits_equal(ga.rho, gv.rho) || !bits_equal(ga.vanna, gv.vanna) ||
            !bits_equal(ga.volga, gv.volga) || !bits_equal(ga.charm, gv.charm)) {
          ++gr_mismatch;
          worst_gr = std::max(worst_gr, rel(ga.delta, gv.delta));
          if (first_report.empty()) {
            first_report = "greeks analytic=" + std::to_string(analytic) +
                           " i=" + std::to_string(i) +
                           " d_owned=" + std::to_string(ga.delta) +
                           " d_view=" + std::to_string(gv.delta) +
                           " vega_owned=" + std::to_string(ga.vega) +
                           " vega_view=" + std::to_string(gv.vega);
          }
        }
      }
    }
  }

  std::printf("[zc-batch] px_mismatch=%zu (worst rel %.3e) gr_mismatch=%zu (worst rel %.3e) "
              "status_mismatch=%zu\n",
              px_mismatch, worst_px, gr_mismatch, worst_gr, status_mismatch);
  std::printf("[zc-batch] first: %s\n", first_report.c_str());

  EXPECT_EQ(status_mismatch, 0u);
  EXPECT_EQ(px_mismatch, 0u);
  EXPECT_EQ(gr_mismatch, 0u);
}
