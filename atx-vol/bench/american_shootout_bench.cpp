// American price / IV shootout: µs/op-vs-error frontier for the Andersen-Lake
// engine across a standardized option grid, measured against the published
// Andersen-Lake-Offengenden (ALO) SOTA envelope.
//
//   Reference: L. Andersen, M. Lake, D. Offengenden, "High-Performance American
//   Option Pricing", SSRN 2547027 (2015) — the QuantLib QdFpAmericanEngine basis.
//   Envelope (their Table figures, single American option, modern x86 core):
//     ~10–22 µs/op for a price at the accurate preset, ~60 µs for a full American
//     implied-vol inversion. This harness reports where atx-vol's Andersen-Lake
//     engine stands on that frontier at three cost/accuracy tiers plus the IV
//     inversion, with an accuracy column measured against the highest-accuracy
//     in-repo scheme.
//
// Sub-Sprint A, Task A3 (infrastructure). Self-gating: a check_benchmark_names.py
// CTest pins the registered row names so the shootout cannot silently drop a tier
// (mirrors the W0.1 name-coverage pattern; deliberately does NOT depend on
// compare_baseline.py, which another sprint owns).
//
// Emit the frontier JSON with:
//   atx-vol-american-shootout-bench --benchmark_out=<...>/american-shootout.json \
//       --benchmark_out_format=json
// The per-row real_time is the µs/op; the accuracy is carried in the cnt-style
// custom counters (us_per_op, max_abs_err, med_abs_err, grid) on each row.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/american.hpp"
#include "atx/vol/american_iv.hpp"
#include "atx/vol/simd/american_boundary_batch.hpp" // american_put_boundary_batch, SimdIsa/SimdRoute
#include "atx/vol/simd/cpu.hpp"                      // have_avx2
#include "atx/vol/types.hpp"

#include "bench_util.hpp"

namespace {

using atx::vol::AlOpts;
using atx::vol::AmericanMethod;
using atx::vol::andersen_lake;
using atx::vol::american_implied_vol;
using atx::vol::Side;

struct Contract {
  double S, K, T, sigma, r, q;
  Side side;
};

// Standardized shootout grid: side × moneyness × maturity × vol × (r,q) regime.
// The throughput grid stays American-admissible (put: r>0; call: q>0 via the
// McDonald-Schroder internal put), so every reference price is finite. The r<0 /
// q_eff<0 double-continuation corners are characterized separately (Task A5's FD
// domain map) — they route to NotImplemented, not a throughput number.
[[nodiscard]] std::vector<Contract> shootout_grid() {
  const double moneyness[] = {0.80, 0.90, 0.95, 1.00, 1.05, 1.10, 1.20};
  const double maturities[] = {0.08, 0.25, 1.00, 2.00};
  const double vols[] = {0.15, 0.25, 0.40};
  const struct {
    double r, q;
  } regimes[] = {{0.04, 0.02}, {0.03, 0.06}, {0.06, 0.01}}; // r>0 && q>0 => both sides American
  const Side sides[] = {Side::Put, Side::Call};

  std::vector<Contract> grid;
  grid.reserve(sizeof(moneyness) / sizeof(double) * 4 * 3 * 3 * 2);
  const double S = 100.0;
  for (const Side side : sides) {
    for (const auto reg : regimes) {
      for (const double T : maturities) {
        for (const double sig : vols) {
          for (const double m : moneyness) {
            grid.push_back(Contract{S, S / m, T, sig, reg.r, reg.q, side});
          }
        }
      }
    }
  }
  return grid;
}

// Highest-accuracy in-repo scheme — the accuracy oracle every tier is scored
// against. Strictly finer than the nullopt ACCURATE preset (12/24/48): more
// boundary nodes, a finer fixed-point quadrature, a deeper sweep budget, a
// tighter tol.
[[nodiscard]] AlOpts reference_opts() noexcept {
  return AlOpts{.n_collocation = 16, .n_quadrature = 48, .max_newton_iter = 32, .tol = 1.0e-12};
}

[[nodiscard]] double price_or_nan(const Contract& c, const std::optional<AlOpts>& opts) noexcept {
  const atx::vol::Result<double> r =
      andersen_lake(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, opts);
  return r.has_value() ? *r : std::numeric_limits<double>::quiet_NaN();
}

// A6 A/B seam: price with a FORCED cold seed and (optionally) a trimmed premium
// Gauss-Legendre order on top of `opts`, so the shootout can compare BAW vs the
// Li-2010 QD+ seed and 16 vs 8 premium nodes against the same reference in one
// build. n_quad_price == 0 keeps the preset's premium order.
[[nodiscard]] double price_or_nan_seeded(const Contract& c, const std::optional<AlOpts>& opts,
                                         atx::vol::detail::AlSeedMode seed,
                                         std::uint16_t n_quad_price) noexcept {
  const atx::vol::Result<double> r = atx::vol::detail::andersen_lake_seeded(
      c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, opts, seed, n_quad_price);
  return r.has_value() ? *r : std::numeric_limits<double>::quiet_NaN();
}

// Reference prices for the grid (computed once; the accuracy denominator).
[[nodiscard]] const std::vector<double>& reference_prices(const std::vector<Contract>& grid) {
  static const std::vector<double> refs = [&] {
    std::vector<double> v(grid.size());
    for (std::size_t i = 0; i < grid.size(); ++i) {
      v[i] = price_or_nan(grid[i], reference_opts());
    }
    return v;
  }();
  return refs;
}

// Accuracy of a scheme vs the reference, as max and median |Δprice| over the grid.
struct Accuracy {
  double max_abs = 0.0;
  double med_abs = 0.0;
};
[[nodiscard]] Accuracy accuracy_vs_reference(const std::vector<Contract>& grid,
                                             const std::vector<double>& refs,
                                             const std::optional<AlOpts>& opts) {
  std::vector<double> errs;
  errs.reserve(grid.size());
  double max_abs = 0.0;
  for (std::size_t i = 0; i < grid.size(); ++i) {
    const double p = price_or_nan(grid[i], opts);
    if (!std::isfinite(p) || !std::isfinite(refs[i])) {
      continue;
    }
    const double e = std::abs(p - refs[i]);
    errs.push_back(e);
    max_abs = std::max(max_abs, e);
  }
  Accuracy a;
  a.max_abs = max_abs;
  if (!errs.empty()) {
    std::sort(errs.begin(), errs.end());
    a.med_abs = errs[errs.size() / 2];
  }
  return a;
}

// One price tier: time the whole grid per iteration; report µs/op (real_time is
// per-grid-op via SetItemsProcessed) and the accuracy vs the reference.
void run_price_tier(benchmark::State& state, const std::optional<AlOpts>& opts) {
  const std::vector<Contract> grid = shootout_grid();
  const std::vector<double>& refs = reference_prices(grid);
  double sink = 0.0;
  for (auto _ : state) {
    for (const Contract& c : grid) {
      sink += price_or_nan(c, opts);
    }
    benchmark::DoNotOptimize(sink);
  }
  benchmark::ClobberMemory();
  const double ops = static_cast<double>(state.iterations()) * static_cast<double>(grid.size());
  state.SetItemsProcessed(static_cast<std::int64_t>(ops));
  const Accuracy a = accuracy_vs_reference(grid, refs, opts);
  state.counters["grid"] = static_cast<double>(grid.size());
  // µs/op = wall / total_ops: kIsRate divides by time, kInvert flips to time/op,
  // the 1e-6 scale reports it in microseconds (mirrors ns_per_option elsewhere).
  state.counters["us_per_op"] =
      benchmark::Counter(ops * 1e-6, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["max_abs_err"] = a.max_abs;
  state.counters["med_abs_err"] = a.med_abs;
}

// ── A6 fast-tier seed / premium-quad A/B ──────────────────────────────────
//
// The fast tier (al_fast_opts: 7-node boundary, order-16 fixed-point + premium
// quadrature, 2 JN + 2 FP sweeps) sits at ~1.44e-3 max abs error vs the reference —
// right at its documented bound. Task A6 asks whether the Li-2010 QD+ cold seed
// (steeper near-expiry exponent) buys enough accuracy headroom to then TRIM the
// premium quadrature 16→8 and still hold that bound while dropping µs/op. These
// rows measure all four corners of that A/B against the SAME reference in one build:
//   fast/baw16   — BAW seed,  16 premium nodes = the pre-A6 fast-tier baseline
//   fast/qdplus16 — QD+ seed, 16 premium nodes = seed-only accuracy gain
//   fast/qdplus8  — QD+ seed,  8 premium nodes = seed + trim (the A6 candidate)
//   fast/baw8    — BAW seed,   8 premium nodes = trim WITHOUT the seed (control:
//                  shows the trim alone breaks the bound, so the seed is load-bearing)
[[nodiscard]] Accuracy accuracy_vs_reference_seeded(const std::vector<Contract>& grid,
                                                    const std::vector<double>& refs,
                                                    const std::optional<AlOpts>& opts,
                                                    atx::vol::detail::AlSeedMode seed,
                                                    std::uint16_t n_quad_price) {
  std::vector<double> errs;
  errs.reserve(grid.size());
  double max_abs = 0.0;
  for (std::size_t i = 0; i < grid.size(); ++i) {
    const double p = price_or_nan_seeded(grid[i], opts, seed, n_quad_price);
    if (!std::isfinite(p) || !std::isfinite(refs[i])) {
      continue;
    }
    const double e = std::abs(p - refs[i]);
    errs.push_back(e);
    max_abs = std::max(max_abs, e);
  }
  Accuracy a;
  a.max_abs = max_abs;
  if (!errs.empty()) {
    std::sort(errs.begin(), errs.end());
    a.med_abs = errs[errs.size() / 2];
  }
  return a;
}

void run_price_tier_seeded(benchmark::State& state, const std::optional<AlOpts>& opts,
                           atx::vol::detail::AlSeedMode seed, std::uint16_t n_quad_price) {
  const std::vector<Contract> grid = shootout_grid();
  const std::vector<double>& refs = reference_prices(grid);
  double sink = 0.0;
  for (auto _ : state) {
    for (const Contract& c : grid) {
      sink += price_or_nan_seeded(c, opts, seed, n_quad_price);
    }
    benchmark::DoNotOptimize(sink);
  }
  benchmark::ClobberMemory();
  const double ops = static_cast<double>(state.iterations()) * static_cast<double>(grid.size());
  state.SetItemsProcessed(static_cast<std::int64_t>(ops));
  const Accuracy a = accuracy_vs_reference_seeded(grid, refs, opts, seed, n_quad_price);
  state.counters["grid"] = static_cast<double>(grid.size());
  state.counters["us_per_op"] =
      benchmark::Counter(ops * 1e-6, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["max_abs_err"] = a.max_abs;
  state.counters["med_abs_err"] = a.med_abs;
}

void american_price_fast(benchmark::State& state) {
  run_price_tier(state, std::optional<AlOpts>{atx::vol::al_fast_opts()});
}
void american_price_accurate(benchmark::State& state) {
  run_price_tier(state, std::nullopt); // internal ACCURATE preset (12/24/48)
}
void american_price_reference(benchmark::State& state) {
  run_price_tier(state, std::optional<AlOpts>{reference_opts()});
}

void american_price_fast_baw16(benchmark::State& state) {
  run_price_tier_seeded(state, std::optional<AlOpts>{atx::vol::al_fast_opts()},
                        atx::vol::detail::AlSeedMode::Baw, 16);
}
void american_price_fast_qdplus16(benchmark::State& state) {
  run_price_tier_seeded(state, std::optional<AlOpts>{atx::vol::al_fast_opts()},
                        atx::vol::detail::AlSeedMode::QdPlus, 16);
}
void american_price_fast_qdplus8(benchmark::State& state) {
  run_price_tier_seeded(state, std::optional<AlOpts>{atx::vol::al_fast_opts()},
                        atx::vol::detail::AlSeedMode::QdPlus, 8);
}
void american_price_fast_baw8(benchmark::State& state) {
  run_price_tier_seeded(state, std::optional<AlOpts>{atx::vol::al_fast_opts()},
                        atx::vol::detail::AlSeedMode::Baw, 8);
}

// Full American IV inversion at the ACCURATE preset: a self-consistent round trip —
// each contract's price is produced with the accurate scheme and inverted back with
// the SAME scheme (so recovered sigma == sigma_true to the inverter tol; no forward-
// map mismatch inflating the residual). Times the cold inversion µs/op vs the ALO IV
// envelope (~60 µs; note this is the COLD single-op path — no warm start / AloPricer
// reuse / correction cache, all of which the production inverter uses).
void american_iv_accurate(benchmark::State& state) {
  const std::vector<Contract> grid = shootout_grid();
  // "Market" prices at the accurate preset (the scheme we invert with).
  static const std::vector<double> market = [&] {
    std::vector<double> v(grid.size());
    for (std::size_t i = 0; i < grid.size(); ++i) {
      v[i] = price_or_nan(grid[i], std::nullopt);
    }
    return v;
  }();
  double sink = 0.0;
  for (auto _ : state) {
    for (std::size_t i = 0; i < grid.size(); ++i) {
      if (!std::isfinite(market[i])) {
        continue;
      }
      const Contract& c = grid[i];
      const atx::vol::Result<double> iv =
          american_implied_vol(market[i], c.S, c.K, c.T, c.r, c.q, c.side,
                               AmericanMethod::AndersenLake, 1.0e-7, 64, std::nullopt);
      sink += iv.has_value() ? *iv : 0.0;
    }
    benchmark::DoNotOptimize(sink);
  }
  benchmark::ClobberMemory();
  // Round-trip accuracy. Score only IDENTIFIABLE contracts: near-intrinsic prices
  // (price within ~a cent of intrinsic) do not identify sigma, so their inversion
  // clamps to kIvMin and would swamp the max with a meaningless residual. Report the
  // median (robust) and max over identifiable contracts + the count.
  std::vector<double> iv_errs;
  iv_errs.reserve(grid.size());
  std::size_t inverted = 0;
  for (std::size_t i = 0; i < grid.size(); ++i) {
    if (!std::isfinite(market[i])) {
      continue;
    }
    const Contract& c = grid[i];
    const double intrinsic =
        (c.side == Side::Put) ? std::max(c.K - c.S, 0.0) : std::max(c.S - c.K, 0.0);
    const bool identifiable = (market[i] - intrinsic) > 1.0e-2; // sigma recoverable
    const atx::vol::Result<double> iv =
        american_implied_vol(market[i], c.S, c.K, c.T, c.r, c.q, c.side,
                             AmericanMethod::AndersenLake, 1.0e-7, 64, std::nullopt);
    if (iv.has_value()) {
      ++inverted;
      if (identifiable) {
        iv_errs.push_back(std::abs(*iv - c.sigma));
      }
    }
  }
  double max_iv_err = 0.0;
  double med_iv_err = 0.0;
  if (!iv_errs.empty()) {
    max_iv_err = *std::max_element(iv_errs.begin(), iv_errs.end());
    std::sort(iv_errs.begin(), iv_errs.end());
    med_iv_err = iv_errs[iv_errs.size() / 2];
  }
  const double iv_ops =
      static_cast<double>(state.iterations()) * static_cast<double>(inverted);
  state.SetItemsProcessed(static_cast<std::int64_t>(iv_ops));
  state.counters["grid"] = static_cast<double>(inverted);
  state.counters["identifiable"] = static_cast<double>(iv_errs.size());
  state.counters["us_per_op"] =
      benchmark::Counter(iv_ops * 1e-6, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["med_iv_roundtrip_err"] = med_iv_err;
  state.counters["max_iv_roundtrip_err"] = max_iv_err;
}

// Corner regimes (r<0 and/or q<0): report how the single-boundary ALO scheme
// resolves them (finite price vs NotImplemented). Not a throughput number — a
// resolution census so the shootout row set documents the corner behavior. Full
// FD characterization is Task A5's domain map.
void american_corners_resolve(benchmark::State& state) {
  const Contract corners[] = {
      {100.0, 100.0, 1.0, 0.30, -0.01, 0.00, Side::Put},   // r<0<=q put: European (no early ex)
      {100.0, 100.0, 1.0, 0.30, 0.00, -0.01, Side::Call},  // q<0<=r call: European
      {100.0, 100.0, 1.0, 0.30, -0.005, -0.03, Side::Put}, // q<r<=0 put: double-continuation
      {100.0, 100.0, 1.0, 0.30, -0.03, -0.005, Side::Call},// r<q<=0 call: double-continuation
  };
  std::size_t finite = 0, not_impl = 0;
  for (auto _ : state) {
    finite = 0;
    not_impl = 0;
    for (const Contract& c : corners) {
      const atx::vol::Result<double> r =
          andersen_lake(c.S, c.K, c.T, c.sigma, c.r, c.q, c.side, std::nullopt);
      if (r.has_value() && std::isfinite(*r)) {
        ++finite;
      } else {
        ++not_impl;
      }
    }
    benchmark::DoNotOptimize(finite);
    benchmark::DoNotOptimize(not_impl);
  }
  state.counters["corners"] = static_cast<double>(sizeof(corners) / sizeof(corners[0]));
  state.counters["finite"] = static_cast<double>(finite);
  state.counters["not_implemented"] = static_cast<double>(not_impl);
}

// ── A1 ship-gate measurement: AVX2 boundary batch vs scalar ────────────────
//
// A homogeneous 4096 American-put batch (the same shape the ship-gate uses):
// every lane a genuine single-boundary American put with tiny per-lane variation,
// so every lane takes the full sweep budget. Timed under ForceScalar and
// ForceAvx2; the gate ratio is scalar_us_per_op / avx2_us_per_op. A1 lifts this by
// dropping the wasted per-lane geometry bind on the AVX2 seed. Flip
// kShipAvx2Boundary only when the ratio clears 2.0x best-of-3 on a quiet host.
[[nodiscard]] const std::vector<Contract>& boundary_grid() {
  static const std::vector<Contract> g = [] {
    std::vector<Contract> v;
    constexpr std::size_t kN = 4096;
    v.reserve(kN);
    for (std::size_t i = 0; i < kN; ++i) {
      const double m = 0.85 + 0.30 * static_cast<double>(i % 31) / 31.0;
      const double vol = 0.15 + 0.25 * static_cast<double>(i % 17) / 17.0;
      v.push_back(Contract{100.0, 100.0 * m, 1.0, vol, 0.05, 0.01, Side::Put});
    }
    return v;
  }();
  return g;
}

void run_boundary_batch(benchmark::State& state, atx::vol::simd::SimdIsa isa,
                        const std::optional<AlOpts>& opts) {
  const std::vector<Contract>& g = boundary_grid();
  const std::size_t n = g.size();
  std::vector<double> S(n), K(n), T(n), sig(n), r(n), q(n), out(n);
  for (std::size_t i = 0; i < n; ++i) {
    S[i] = g[i].S; K[i] = g[i].K; T[i] = g[i].T;
    sig[i] = g[i].sigma; r[i] = g[i].r; q[i] = g[i].q;
  }
  for (auto _ : state) {
    atx::vol::simd::american_put_boundary_batch(S.data(), K.data(), T.data(), sig.data(),
                                                r.data(), q.data(), out.data(), n, opts, isa);
    benchmark::DoNotOptimize(out.data());
  }
  benchmark::ClobberMemory();
  const double ops = static_cast<double>(state.iterations()) * static_cast<double>(n);
  state.SetItemsProcessed(static_cast<std::int64_t>(ops));
  state.counters["grid"] = static_cast<double>(n);
  state.counters["us_per_op"] =
      benchmark::Counter(ops * 1e-6, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}
void american_boundary_batch_scalar(benchmark::State& state) {
  run_boundary_batch(state, atx::vol::simd::SimdIsa::ForceScalar, std::nullopt);
}
void american_boundary_batch_avx2(benchmark::State& state) {
  if (!atx::vol::simd::have_avx2()) {
    state.SkipWithError("no AVX2 on host");
    return;
  }
  run_boundary_batch(state, atx::vol::simd::SimdIsa::ForceAvx2, std::nullopt);
}

// K2 marks-tier gate: the SAME 4096-put grid priced on the ql_fast rung
// (nb=7, fp=8, price=32, 2 sweeps — docs/al-preset-ladder.md §4), the scheme the
// backtest/live marks path is intended to adopt (marks-tier, K1 §5). The scalar
// baseline is the (7,8) specialized kernel (american.cpp al_fp_specialized), so the
// ratio is the HONEST best-scalar-vs-avx2 at the tier that actually ships, not the
// accurate (12,24,48) gate above. n_quad_price=32 exercises the decoupled premium.
[[nodiscard]] const std::optional<AlOpts>& qlfast_opts() {
  static const std::optional<AlOpts> o = AlOpts{.n_collocation = 7,
                                                .n_quadrature = 8,
                                                .n_quad_price = 32,
                                                .max_newton_iter = 2,
                                                .tol = 1.0e-8};
  return o;
}
void american_boundary_batch_scalar_qlfast(benchmark::State& state) {
  run_boundary_batch(state, atx::vol::simd::SimdIsa::ForceScalar, qlfast_opts());
}
void american_boundary_batch_avx2_qlfast(benchmark::State& state) {
  if (!atx::vol::simd::have_avx2()) {
    state.SkipWithError("no AVX2 on host");
    return;
  }
  run_boundary_batch(state, atx::vol::simd::SimdIsa::ForceAvx2, qlfast_opts());
}

// Register the shootout rows (data-driven, at static-init). Stable names are pinned
// by the check_benchmark_names.py CTest. apply_common carries the mandated knobs
// (0.5 s warm-up, 5 repetitions, p95 + CV statistics).
void register_all() {
  using atx::vol::bench::apply_common;
  apply_common(benchmark::RegisterBenchmark("american/price/fast", american_price_fast))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("american/price/accurate", american_price_accurate))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("american/price/reference", american_price_reference))
      ->Unit(benchmark::kMicrosecond);
  // A6 fast-tier seed/premium A/B (see the block above american_price_fast_baw16).
  apply_common(
      benchmark::RegisterBenchmark("american/price/fast_baw16", american_price_fast_baw16))
      ->Unit(benchmark::kMicrosecond);
  apply_common(
      benchmark::RegisterBenchmark("american/price/fast_qdplus16", american_price_fast_qdplus16))
      ->Unit(benchmark::kMicrosecond);
  apply_common(
      benchmark::RegisterBenchmark("american/price/fast_qdplus8", american_price_fast_qdplus8))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("american/price/fast_baw8", american_price_fast_baw8))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("american/iv/accurate", american_iv_accurate))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("american/corners/resolve", american_corners_resolve))
      ->Unit(benchmark::kNanosecond);
  apply_common(
      benchmark::RegisterBenchmark("american/boundary_batch/scalar", american_boundary_batch_scalar))
      ->Unit(benchmark::kMicrosecond);
  apply_common(
      benchmark::RegisterBenchmark("american/boundary_batch/avx2", american_boundary_batch_avx2))
      ->Unit(benchmark::kMicrosecond);
  // K2 marks-tier (ql_fast rung) rows — see qlfast_opts() above.
  apply_common(benchmark::RegisterBenchmark("american/boundary_batch/scalar_qlfast",
                                            american_boundary_batch_scalar_qlfast))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("american/boundary_batch/avx2_qlfast",
                                            american_boundary_batch_avx2_qlfast))
      ->Unit(benchmark::kMicrosecond);
}

const bool kRegistered = [] {
  register_all();
  return true;
}();

} // namespace
