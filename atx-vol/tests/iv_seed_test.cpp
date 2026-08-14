// K2 — IV seed quality gate: the Choi-2023 tighter-bound seed must (a) leave the
// converged σ machine-precise (pure-refactor: the Halley loop converges to the
// same root regardless of seed) and (b) start closer to root so the loop takes
// FEWER Halley steps than the old SR-2017-with-crude-wing-fallback seed.
//
// The seed change is classed PURE-REFACTOR: it cannot move the converged answer
// beyond the loop's residual-noise floor, so these tests assert the recovered σ
// stays within the machine-precision-class bound the shootout bench grades at,
// while the Halley-step MEAN drops below the old seed's 4.71 (measured, WS-B/M2
// baseline) — the K2 lever. Because correctness is guaranteed by the loop and
// only SPEED depends on the seed, the step-count assertion is the substantive
// K2 gate; the accuracy assertions are the no-regression backstop.
//
// Reference for the seed under test: J. Choi, K. Kim, M. Kwak (2023/2024),
// "Tighter uniform bounds for Black-Scholes implied volatility", arXiv:2302.08758
// (Cor. 5.2 lower bound L3, used as the log-price Newton seed there).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/pricing/implied_vol.hpp"

namespace atx::vol {

// Test/measurement seam defined in src/implied_vol.cpp (not in the public
// header): reports the Halley-step count and which termination test fired.
Result<double> implied_vol_traced(double price, double F, double K, double T, double df, Side side,
                                  int &iters, int &exit_reason);

namespace {

using atx::vol::black76_price;
using atx::vol::implied_vol;
using atx::vol::Side;

// Long-double bisection oracle — identical construction to the iv_shootout bench
// so the accuracy gate here matches the bench's grading.
long double b76_ld(long double F, long double K, long double T, long double sig, long double df,
                   Side side) {
  const long double v = sig * std::sqrt(T);
  const long double d1 = (std::log(F / K) + 0.5L * v * v) / v;
  const long double d2 = d1 - v;
  const long double inv_sqrt2 = 0.7071067811865475244008443621048490393L;
  const auto Phi = [&](long double x) { return 0.5L * std::erfc(-x * inv_sqrt2); };
  if (side == Side::Call) return df * (F * Phi(d1) - K * Phi(d2));
  return df * (K * Phi(-d2) - F * Phi(-d1));
}
long double iv_oracle(long double price, long double F, long double K, long double T, long double df,
                      Side side) {
  long double lo = 1e-6L, hi = 10.0L;
  if (price <= b76_ld(F, K, T, lo, df, side) || price >= b76_ld(F, K, T, hi, df, side)) return 0.0L;
  for (int i = 0; i < 200; ++i) {
    const long double mid = 0.5L * (lo + hi);
    if (b76_ld(F, K, T, mid, df, side) < price) lo = mid; else hi = mid;
  }
  return 0.5L * (lo + hi);
}

struct GridStats {
  double median_rel = 0.0;
  double max_rel = 0.0;
  double max_abs = 0.0; // vol points
  double mean_steps = 0.0;
  int max_steps = 0;
  std::size_t n = 0;
};

// Sweep the standardized shootout grid (moneyness × maturity × vol × side ×
// notional), grade recovered σ vs the long-double oracle, and accumulate the
// Halley-step count from the real solver via the traced seam.
GridStats sweep() {
  const double forwards[] = {25.0, 100.0, 500.0, 5000.0};
  const double moneyness[] = {0.70, 0.85, 0.95, 1.0, 1.05, 1.15, 1.35};
  const double tenors[] = {1.0 / 365.0, 0.05, 0.25, 1.0, 2.0};
  const double vols[] = {0.08, 0.20, 0.45, 0.90};

  std::vector<double> rels;
  GridStats s;
  long total_steps = 0;
  for (double F : forwards)
    for (double m : moneyness)
      for (double T : tenors)
        for (double v : vols) {
          const double K = F * m;
          const double df = std::exp(-0.025 * T);
          for (Side side : {Side::Call, Side::Put}) {
            const double p = black76_price(F, K, T, v, df, side);
            const double intr = (side == Side::Call) ? df * std::fmax(F - K, 0.0)
                                                     : df * std::fmax(K - F, 0.0);
            if (p - intr < 1e-7 * F) continue; // near-intrinsic: σ ill-determined
            const long double o = iv_oracle(p, F, K, T, df, side);
            if (o <= 0.0L) continue;
            int iters = 0, exit_reason = -1;
            const Result<double> iv =
                implied_vol_traced(p, F, K, T, df, side, iters, exit_reason);
            EXPECT_TRUE(iv.has_value())
                << "F=" << F << " K=" << K << " T=" << T << " v=" << v;
            if (!iv) continue;
            const long double abs_err = std::fabs(static_cast<long double>(*iv) - o);
            const double rel = static_cast<double>(abs_err / o);
            rels.push_back(rel);
            s.max_rel = std::max(s.max_rel, rel);
            s.max_abs = std::max(s.max_abs, static_cast<double>(abs_err));
            total_steps += iters;
            s.max_steps = std::max(s.max_steps, iters);
            ++s.n;
          }
        }
  std::sort(rels.begin(), rels.end());
  s.median_rel = rels.empty() ? 0.0 : rels[rels.size() / 2];
  s.mean_steps = s.n ? static_cast<double>(total_steps) / static_cast<double>(s.n) : 0.0;
  return s;
}

// (1) K2 accuracy backstop — the seed change is pure-refactor: recovered σ stays
// machine-precision-class vs the oracle, far inside the 1e-4 vol economic bound.
TEST(IvSeed, ConvergedIvMachinePrecisionAcrossCorpus) {
  const GridStats s = sweep();
  ASSERT_GT(s.n, 400u) << "grid coverage dropped";
  std::printf("[IvSeed] n=%zu median_rel=%.3e max_rel=%.3e max_abs=%.3e mean_steps=%.3f max_steps=%d\n",
              s.n, s.median_rel, s.max_rel, s.max_abs, s.mean_steps, s.max_steps);
  // Median relative σ error is a few ULP (bench baseline ~1e-15); the shootout
  // grades max_rel < 1e-10. Give noise headroom but stay machine-precision-class.
  EXPECT_LE(s.median_rel, 2.0e-15) << "median_rel=" << s.median_rel;
  EXPECT_LT(s.max_rel, 1.0e-9) << "max_rel=" << s.max_rel;
  // Economic bound (absolute vol points) — orders inside 1e-4.
  EXPECT_LT(s.max_abs, 1.0e-6) << "max_abs=" << s.max_abs;
}

// (2) K2 LEVER — a tighter seed starts inside the Halley cubic basin, so the mean
// Halley-step count drops well below the old SR-2017 seed's 4.71 (measured
// baseline). This is the assertion the OLD seed fails and the K2 seed passes.
TEST(IvSeed, HalleyStepCountDropsVsOldSeed) {
  const GridStats s = sweep();
  ASSERT_GT(s.n, 400u);
  // Old SR-2017-with-crude-wing-fallback seed: mean 4.71, max 12 (WS-B/M2). The
  // Choi-2023 tighter bound must cut the mean under 3.0 and cap the worst case.
  EXPECT_LT(s.mean_steps, 3.0) << "mean_steps=" << s.mean_steps;
  EXPECT_LE(s.max_steps, 8) << "max_steps=" << s.max_steps;
}

// (3) Hard corners (deep ITM/OTM, tiny/huge maturity): σ is ill-conditioned, so
// grade against the vs-oracle relative bound the shootout uses, and confirm the
// seed keeps the loop convergent (no vega-collapse / exhaustion) there.
TEST(IvSeed, HardCornersConvergeWithinEconomicBound) {
  struct Q { double F, K, T, sig, df; Side side; };
  const Q qs[] = {
      {100.0, 250.0, 1.0, 0.30, 0.98, Side::Call},   // deep OTM call
      {100.0, 40.0, 1.0, 0.30, 0.98, Side::Put},     // deep OTM put
      {100.0, 130.0, 1.0 / 365.0, 0.60, 1.0, Side::Call}, // 1-day wing
      {5000.0, 3500.0, 2.0, 0.85, 0.95, Side::Put},  // high-notional deep ITM put
      {25.0, 40.0, 1.0 / 365.0, 0.90, 1.0, Side::Call},   // tiny-notional 1-day OTM
  };
  int tested = 0;
  for (const Q &q : qs) {
    const double price = black76_price(q.F, q.K, q.T, q.sig, q.df, q.side);
    const double intr = (q.side == Side::Call) ? q.df * std::fmax(q.F - q.K, 0.0)
                                               : q.df * std::fmax(q.K - q.F, 0.0);
    if (price - intr < 1e-7 * q.F) continue; // unidentifiable; skip
    const long double o = iv_oracle(price, q.F, q.K, q.T, q.df, q.side);
    ASSERT_GT(o, 0.0L) << "oracle diverged K=" << q.K;
    int iters = 0, exit_reason = -1;
    const Result<double> iv =
        implied_vol_traced(price, q.F, q.K, q.T, q.df, q.side, iters, exit_reason);
    ASSERT_TRUE(iv.has_value()) << "K=" << q.K;
    const double rel = static_cast<double>(std::fabs(static_cast<long double>(*iv) - o) / o);
    EXPECT_LT(rel, 1.0e-9) << "K=" << q.K << " iv=" << *iv;
    ++tested;
  }
  EXPECT_GT(tested, 0);
}

} // namespace
} // namespace atx::vol
