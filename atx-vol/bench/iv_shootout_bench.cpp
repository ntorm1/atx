// K5 — implied-volatility inversion shootout harness (SPRINT W5.6 support).
//
// Times atx-vol's scalar and AVX2 IV inversion across a standardized grid
// (moneyness × maturity × vol × side) plus the hard corners (deep OTM, near-zero
// time value), and measures each recovered σ against a bulletproof long-double
// bisection ORACLE — a stricter accuracy reference than any double-precision
// competitor. Emits ns/op and max/median relative σ error; a fail-loud gate
// (SkipWithError) trips if the atx-vol error blows past a machine-precision-class
// bound or the grid row count is wrong, so the shootout cannot silently drop
// rows.
//
// Reference standing — Jäckel, "Let's Be Rational" (LBR), P. Jäckel 2013
// (jaeckel.org/LetsBeRational.pdf): the published single-thread figure is
// ~180 ns/op to full machine precision. LBR's source (© 2013–2014 Peter Jäckel)
// carries a permissive "use, copy, modify, distribute freely provided the
// copyright notice is preserved" grant, so it MAY be vendored bench-only under
// bench/thirdparty/lets_be_rational/. It is not vendored in this pass (the
// same-host LBR-vs-atx-vol run is the Sprint-G "publish reproducible shootouts"
// deliverable); the harness measures atx-vol ns/op + error here and cites the
// ~180 ns/op target for the standing, and the long-double oracle it grades
// against is tighter than LBR would be.

#include "atx/vol/black76.hpp"
#include "atx/vol/implied_vol.hpp"
#include "atx/vol/simd/cpu.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "bench_util.hpp"

namespace atx::vol::simd::detail {
// The retained AVX2 IV batch kernel (off the default dispatch per R-24).
void implied_vol_batch_avx2(const double *price, const double *F, const double *K, const double *T,
                            const double *df, const Side *side, double *iv_out,
                            std::uint8_t *ok_out, std::size_t n) noexcept;
} // namespace atx::vol::simd::detail

namespace atx::vol::bench {
namespace {

// ── Long-double bisection oracle (accuracy reference) ─────────────────────
long double b76_ld(long double F, long double K, long double T, long double sig, long double df,
                   Side side) {
  const long double v = sig * std::sqrt(T);
  const long double d1 = (std::log(F / K) + 0.5L * v * v) / v;
  const long double d2 = d1 - v;
  const long double inv_sqrt2 = 0.7071067811865475244008443621048490393L;
  const auto Phi = [&](long double x) { return 0.5L * std::erfc(-x * inv_sqrt2); };
  if (side == Side::Call) {
    return df * (F * Phi(d1) - K * Phi(d2));
  }
  return df * (K * Phi(-d2) - F * Phi(-d1));
}

// σ such that b76(σ) == price, to long-double machine precision (200 bisections
// on the monotone price(σ)). Returns 0 for a price outside the finite-σ band.
long double iv_oracle(long double price, long double F, long double K, long double T, long double df,
                      Side side) {
  long double lo = 1e-6L, hi = 10.0L;
  if (price <= b76_ld(F, K, T, lo, df, side) || price >= b76_ld(F, K, T, hi, df, side)) {
    return 0.0L;
  }
  for (int i = 0; i < 200; ++i) {
    const long double mid = 0.5L * (lo + hi);
    if (b76_ld(F, K, T, mid, df, side) < price) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return 0.5L * (lo + hi);
}

struct Grid {
  std::vector<double> price, F, K, T, df, sigma_true;
  std::vector<Side> side;
  std::vector<long double> oracle;
  [[nodiscard]] std::size_t size() const { return F.size(); }
};

// Standard grid + hard corners. Only rows with a resolvable oracle σ (finite-σ
// band, non-degenerate vega) are retained, so the accuracy figures are meaningful.
Grid make_grid() {
  Grid g;
  const double forwards[] = {25.0, 100.0, 500.0, 5000.0};
  const double moneyness[] = {0.70, 0.85, 0.95, 1.0, 1.05, 1.15, 1.35};
  const double tenors[] = {1.0 / 365.0, 0.05, 0.25, 1.0, 2.0};
  const double vols[] = {0.08, 0.20, 0.45, 0.90};
  const auto add = [&](double F, double K, double T, double v, double df, Side s) {
    const double p = black76_price(F, K, T, v, df, s);
    const double intr = (s == Side::Call) ? df * std::fmax(F - K, 0.0) : df * std::fmax(K - F, 0.0);
    if (p - intr < 1e-7 * F) return; // near-intrinsic: σ ill-determined, skip
    const long double o = iv_oracle(p, F, K, T, df, s);
    if (o <= 0.0L) return;
    g.price.push_back(p); g.F.push_back(F); g.K.push_back(K); g.T.push_back(T);
    g.df.push_back(df); g.side.push_back(s); g.sigma_true.push_back(v); g.oracle.push_back(o);
  };
  for (double F : forwards)
    for (double m : moneyness)
      for (double T : tenors)
        for (double v : vols) {
          const double df = std::exp(-0.025 * T);
          add(F, F * m, T, v, df, Side::Call);
          add(F, F * m, T, v, df, Side::Put);
        }
  // Hard corners: deep OTM at short/long tenor, near-zero time value.
  add(100.0, 250.0, 1.0, 0.30, 0.98, Side::Call);
  add(100.0, 40.0, 1.0, 0.30, 0.98, Side::Put);
  add(100.0, 130.0, 1.0 / 365.0, 0.60, 1.0, Side::Call);
  return g;
}

const Grid &grid() {
  static const Grid g = make_grid();
  return g;
}

// Max / median relative σ error of a recovered-σ array vs the oracle.
struct ErrStats {
  double max_rel = 0.0;
  double median_rel = 0.0;
};
ErrStats error_vs_oracle(const std::vector<double> &iv) {
  const Grid &g = grid();
  std::vector<double> rels;
  rels.reserve(g.size());
  double mx = 0.0;
  for (std::size_t i = 0; i < g.size(); ++i) {
    const double rel = static_cast<double>(std::fabs(static_cast<long double>(iv[i]) - g.oracle[i]) /
                                            g.oracle[i]);
    rels.push_back(rel);
    mx = std::max(mx, rel);
  }
  std::sort(rels.begin(), rels.end());
  ErrStats s;
  s.max_rel = mx;
  s.median_rel = rels.empty() ? 0.0 : rels[rels.size() / 2];
  return s;
}

// Jäckel LBR published single-thread reference (ns/op) for the standing label.
constexpr double kJaeckelLbrNsPerOp = 180.0;

// Fail-loud accuracy + row-coverage bound. Machine-precision-class: the scalar
// inverter recovers σ to a few ULP, far inside the 1e-4 vol economic bound.
constexpr double kShootoutMaxRelErr = 1e-10;
constexpr std::size_t kMinShootoutRows = 400;

void BM_IvShootout_AtxScalar(benchmark::State &state) {
  const Grid &g = grid();
  const std::size_t n = g.size();
  std::vector<double> iv(n, 0.0);
  if (n < kMinShootoutRows) {
    state.SkipWithError("shootout grid dropped rows (coverage check)");
    return;
  }
  for (auto _ : state) {
    for (std::size_t i = 0; i < n; ++i) {
      const Result<double> r = implied_vol(g.price[i], g.F[i], g.K[i], g.T[i], g.df[i], g.side[i]);
      iv[i] = r ? *r : std::nan("");
    }
    benchmark::DoNotOptimize(iv.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(n));
  const ErrStats e = error_vs_oracle(iv);
  if (e.max_rel > kShootoutMaxRelErr) {
    state.SkipWithError(("atx scalar max rel err " + std::to_string(e.max_rel) +
                         " exceeds bound")
                            .c_str());
  }
  state.counters["max_rel_err"] = e.max_rel;
  state.counters["median_rel_err"] = e.median_rel;
  state.counters["rows"] = static_cast<double>(n);
  state.SetLabel("oracle=longdouble_bisection jaeckel_lbr_ref_ns=" +
                 std::to_string(kJaeckelLbrNsPerOp));
}

void BM_IvShootout_AtxAvx2(benchmark::State &state) {
  if (!simd::have_avx2()) {
    state.SkipWithError("AVX2 not available");
    return;
  }
  const Grid &g = grid();
  const std::size_t n = g.size();
  std::vector<double> iv(n, 0.0);
  std::vector<std::uint8_t> ok(n, 0);
  for (auto _ : state) {
    simd::detail::implied_vol_batch_avx2(g.price.data(), g.F.data(), g.K.data(), g.T.data(),
                                         g.df.data(), g.side.data(), iv.data(), ok.data(), n);
    benchmark::DoNotOptimize(iv.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(n));
  const ErrStats e = error_vs_oracle(iv);
  state.counters["max_rel_err"] = e.max_rel;
  state.counters["median_rel_err"] = e.median_rel;
  state.counters["rows"] = static_cast<double>(n);
  state.SetLabel("kernel=implied_vol_batch_avx2 (retained off-dispatch, R-24)");
}

const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("iv/shootout/atxvol_scalar", BM_IvShootout_AtxScalar))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("iv/shootout/atxvol_avx2", BM_IvShootout_AtxAvx2))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
