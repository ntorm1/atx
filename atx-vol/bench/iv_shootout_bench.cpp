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
// copyright notice is preserved" grant (verified verbatim, WS-0/M2), so it is
// now VENDORED bench-only under bench/thirdparty/lets_be_rational/ (see that
// dir's LICENSE + README for provenance). BM_IvShootout_Jaeckel runs LBR on THIS
// host against the same long-double oracle, next to atx-vol's scalar path
// (BM_IvShootout_AtxScalarCody) — turning "cite 180 ns" into a same-host
// head-to-head. All rows report ns/op and max/median ULP vs the oracle; the atx
// scalar row additionally emits a Halley-step-count histogram per moneyness ×
// maturity regime (via the library implied_vol_traced measurement seam).

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

namespace atx::vol {
// Library measurement seam (defined in src/implied_vol.cpp, deliberately not in
// the public header): same inversion as implied_vol, additionally reporting the
// Halley-step count and which termination test fired. Wired here (WS-0/M2) to
// build the per-regime step histogram from the REAL solver — no bench-local
// mirror, so the histogram cannot drift from production.
[[nodiscard]] Result<double> implied_vol_traced(double price, double F, double K, double T,
                                                double df, Side side, int &iters, int &exit_reason);
} // namespace atx::vol

// Vendored Peter Jäckel "Let's Be Rational" entry point (bench/thirdparty/
// lets_be_rational, license verified permissive). `price` is the UNDISCOUNTED
// (forward) option value; q = +1 call / -1 put; returns σ.
extern "C" double implied_volatility_from_a_transformed_rational_guess(double price, double F,
                                                                       double K, double T, double q);

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

// ULP gap at |x| — the spacing between adjacent representable doubles there.
[[nodiscard]] double ulp_at(double x) noexcept {
  const double a = std::fabs(x);
  const double next = std::nextafter(a, std::numeric_limits<double>::infinity());
  const double gap = next - a;
  return (gap > 0.0) ? gap : std::numeric_limits<double>::denorm_min();
}

// Max / median relative σ error AND ULP distance of a recovered-σ array vs the
// long-double oracle. ULP = |recovered − oracle| / ulp(oracle) — the count of
// representable doubles between the two, the sharpest accuracy metric for a
// machine-precision inverter (relative error saturates at ~1e-16).
struct ErrStats {
  double max_rel = 0.0;
  double median_rel = 0.0;
  double max_ulp = 0.0;
  double median_ulp = 0.0;
};
ErrStats error_vs_oracle(const std::vector<double> &iv) {
  const Grid &g = grid();
  std::vector<double> rels;
  std::vector<double> ulps;
  rels.reserve(g.size());
  ulps.reserve(g.size());
  double mx = 0.0;
  double mx_ulp = 0.0;
  for (std::size_t i = 0; i < g.size(); ++i) {
    const long double abs_err = std::fabs(static_cast<long double>(iv[i]) - g.oracle[i]);
    const double rel = static_cast<double>(abs_err / g.oracle[i]);
    const double ulp = static_cast<double>(abs_err) / ulp_at(static_cast<double>(g.oracle[i]));
    rels.push_back(rel);
    ulps.push_back(ulp);
    mx = std::max(mx, rel);
    mx_ulp = std::max(mx_ulp, ulp);
  }
  std::sort(rels.begin(), rels.end());
  std::sort(ulps.begin(), ulps.end());
  ErrStats s;
  s.max_rel = mx;
  s.median_rel = rels.empty() ? 0.0 : rels[rels.size() / 2];
  s.max_ulp = mx_ulp;
  s.median_ulp = ulps.empty() ? 0.0 : ulps[ulps.size() / 2];
  return s;
}

// Emit the shared max/median rel + ULP counters onto a benchmark row.
void publish_err_counters(benchmark::State &state, const ErrStats &e, std::size_t rows) {
  state.counters["max_rel_err"] = e.max_rel;
  state.counters["median_rel_err"] = e.median_rel;
  state.counters["max_ulp"] = e.max_ulp;
  state.counters["median_ulp"] = e.median_ulp;
  state.counters["rows"] = static_cast<double>(rows);
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
  publish_err_counters(state, e, n);
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
  publish_err_counters(state, e, n);
  state.SetLabel("kernel=implied_vol_batch_avx2 (retained off-dispatch, R-24)");
}

// ── M2: Jäckel LBR same-host head-to-head ─────────────────────────────────
// Vendored LBR on the identical grid + oracle. LBR takes the UNDISCOUNTED price
// (price/df) and q = +1 call / −1 put, returns σ. A units mistake would show as
// a large error, so we fail loud if LBR is not machine-precise here — that would
// mean the harness is calling it wrong, not that LBR regressed.
void BM_IvShootout_Jaeckel(benchmark::State &state) {
  const Grid &g = grid();
  const std::size_t n = g.size();
  std::vector<double> iv(n, 0.0);
  if (n < kMinShootoutRows) {
    state.SkipWithError("shootout grid dropped rows (coverage check)");
    return;
  }
  for (auto _ : state) {
    for (std::size_t i = 0; i < n; ++i) {
      const double q = (g.side[i] == Side::Call) ? 1.0 : -1.0;
      const double undiscounted = g.price[i] / g.df[i];
      iv[i] = implied_volatility_from_a_transformed_rational_guess(undiscounted, g.F[i], g.K[i],
                                                                   g.T[i], q);
    }
    benchmark::DoNotOptimize(iv.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(n));
  const ErrStats e = error_vs_oracle(iv);
  // LBR is a full-machine-precision inverter; anything above this bound means we
  // fed it wrong (units/convention), so trip loudly rather than publish a bogus
  // "LBR is inaccurate" number.
  constexpr double kJaeckelSanityRelErr = 1e-6;
  if (e.max_rel > kJaeckelSanityRelErr) {
    state.SkipWithError(("jaeckel LBR max rel err " + std::to_string(e.max_rel) +
                         " exceeds sanity bound — harness is calling LBR wrong")
                            .c_str());
  }
  publish_err_counters(state, e, n);
  state.SetLabel("kernel=jaeckel_lets_be_rational vendored=bench/thirdparty/lets_be_rational");
}

// ── M2: atx scalar row with the per-regime Halley-step histogram ───────────
// Times the atx scalar implied_vol head-to-head vs Jäckel. K1 (iv-kernel) swaps
// this same entry point's Φ to the Cody rational-erfc in place, so this row is
// the post-K1 "Cody" head-to-head lane (pre-K1 it equals atxvol_scalar).
//
// The Halley-step histogram is gathered OUTSIDE the timed loop via the library
// implied_vol_traced seam (the real solver, no bench mirror), bucketed by
// moneyness (|ln F/K|) × maturity (T). Step counts justify the ns/op: the SR2017
// seed + notional-scaled residual test converge in 1–2 Halley steps grid-wide.
void BM_IvShootout_AtxScalarCody(benchmark::State &state) {
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
    state.SkipWithError(("atx scalar (cody-lane) max rel err " + std::to_string(e.max_rel) +
                         " exceeds bound")
                            .c_str());
  }
  publish_err_counters(state, e, n);

  // Halley-step histogram per moneyness × maturity regime (untimed, deterministic).
  // 3 moneyness bands (ATM / wing / deep) × 3 maturity bands (short / mid / long).
  constexpr int kM = 3, kT = 3;
  long step_sum[kM][kT] = {{0}};
  long bucket_n[kM][kT] = {{0}};
  long hist1 = 0, hist2 = 0, hist3plus = 0;
  long exit_price = 0, exit_volstep = 0;
  long total_steps = 0, worst_steps = 0;
  long traced_mismatch = 0;
  for (std::size_t i = 0; i < n; ++i) {
    int iters = 0, exit_reason = -1;
    const Result<double> tr = implied_vol_traced(g.price[i], g.F[i], g.K[i], g.T[i], g.df[i],
                                                 g.side[i], iters, exit_reason);
    // Guard against the seam drifting from implied_vol: the traced σ must match
    // the production σ. (Both are the same core; this pins that.)
    if (!tr || std::fabs(*tr - iv[i]) > 1e-12) {
      ++traced_mismatch;
    }
    const double y = std::fabs(std::log(g.F[i] / g.K[i]));
    const int mb = (y < 0.05) ? 0 : (y < 0.25 ? 1 : 2);
    const double T = g.T[i];
    const int tb = (T < 0.02) ? 0 : (T < 0.5 ? 1 : 2);
    step_sum[mb][tb] += iters;
    ++bucket_n[mb][tb];
    total_steps += iters;
    worst_steps = std::max(worst_steps, static_cast<long>(iters));
    if (iters <= 1) ++hist1; else if (iters == 2) ++hist2; else ++hist3plus;
    if (exit_reason == 0) ++exit_price; else if (exit_reason == 1) ++exit_volstep;
  }
  if (traced_mismatch > 0) {
    state.SkipWithError(("implied_vol_traced disagreed with implied_vol on " +
                         std::to_string(traced_mismatch) + " rows")
                            .c_str());
  }
  static const char *mlabel[kM] = {"atm", "wing", "deep"};
  static const char *tlabel[kT] = {"short", "mid", "long"};
  for (int m = 0; m < kM; ++m) {
    for (int t = 0; t < kT; ++t) {
      const std::string key = std::string("steps_mean_") + mlabel[m] + "_" + tlabel[t];
      state.counters[key] =
          bucket_n[m][t] > 0 ? static_cast<double>(step_sum[m][t]) / static_cast<double>(bucket_n[m][t])
                             : 0.0;
    }
  }
  state.counters["steps_mean"] = static_cast<double>(total_steps) / static_cast<double>(n);
  state.counters["steps_max"] = static_cast<double>(worst_steps);
  state.counters["frac_1step"] = static_cast<double>(hist1) / static_cast<double>(n);
  state.counters["frac_2step"] = static_cast<double>(hist2) / static_cast<double>(n);
  state.counters["frac_3plus_step"] = static_cast<double>(hist3plus) / static_cast<double>(n);
  state.counters["exit_price_resid"] = static_cast<double>(exit_price);
  state.counters["exit_volstep"] = static_cast<double>(exit_volstep);
  state.SetLabel("kernel=atxvol_scalar_cody_lane oracle=longdouble_bisection "
                 "jaeckel_lbr_ref_ns=" +
                 std::to_string(kJaeckelLbrNsPerOp));
}

const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("iv/shootout/atxvol_scalar", BM_IvShootout_AtxScalar))
      ->Unit(benchmark::kMicrosecond);
  apply_common(
      benchmark::RegisterBenchmark("iv/shootout/atxvol_scalar_cody", BM_IvShootout_AtxScalarCody))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("iv/shootout/jaeckel_lbr", BM_IvShootout_Jaeckel))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("iv/shootout/atxvol_avx2", BM_IvShootout_AtxAvx2))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
