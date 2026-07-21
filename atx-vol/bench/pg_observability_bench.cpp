// Pricing/greeks SOTA sprint — bench/counter observability rows (P7, perf-review
// bench-gaps 1-4). Three rows that isolate the fitter's boundary-solve-bound hot
// loops so the sprint's fewer-solves wins become REPORTABLE and future-gateable:
//
//   fit/american_iv/cached_newton — the cached-inversion Newton kernel (perf gap 2).
//       200 cached American-IV inversions through a populated CorrectionCache
//       (Black-76 + one 3D Clenshaw correction traversal per residual, fused
//       price+∂σ per Newton refinement). Makes the F1/F8 fused-Clenshaw win
//       reportable: the landed P1 result was 2353 -> 1343 ClenshawSweeps on this
//       200-inversion fixture. Emits cnt_clenshaw_sweeps (gated) + sl_iv_newton_iters
//       (always-on).
//
//   deam/carry/resolve — the de-Americanization carry (borrow) solve (perf gap 3).
//       resolve_chain_forward -> resolve_chain_carry over a near-ATM co-terminal
//       ladder: up to max_borrow_pairs pairs, each a borrow fixed point of American
//       IV inversions. Makes the P2 warm-start-carry win reportable: 474 -> 139
//       AL boundary solves/slice. Emits sl_al_boundary_solves (always-on) +
//       cnt_boundary_solves (gated).
//
//   correction/cache/build — the per-side correction-cache build (perf gap 4).
//       CorrectionCache::build at the production 16x8x12 grid: one cold Andersen-Lake
//       boundary solve per (T, sigma) node row (n_T*n_s = 96 solves for the whole
//       side) + the DCT-II / derivative-coefficient transforms. Emits
//       sl_al_boundary_solves (always-on, ~96) + cnt_boundary_solves / cnt_cheb_diff_coefs
//       (gated).
//
// COUNTERS ARE THE CITABLE EVIDENCE, NOT WALL TIME. Per the M3 quiet-window
// protocol + the sprint's bench-lease discipline: this host has no CPU-frequency
// pinning and several agents build concurrently, so the ns/op figures these rows
// print are advisory only. The deterministic counter columns are the gate:
//   * sl_*  (solve ledger) — ALWAYS compiled in, so the boundary-solve / Newton-iter
//           wins are readable straight off the shipping rel-avx2 binary.
//   * cnt_* (exact diagnostic counters) — light up only under -DATX_VOL_COUNTERS=ON;
//           the OFF build simply omits those columns (dump_counters is a no-op).
// Both are measured over ONE representative op OUTSIDE the timed loop.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/american.hpp"     // american_price, american_price_cached, AlOpts, al_fast_opts
#include "atx/vol/american_iv.hpp"  // american_implied_vol
#include "atx/vol/correction.hpp"   // CorrectionCache
#include "atx/vol/counters.hpp"     // counters::ledger (always-on solve ledger)
#include "atx/vol/curve.hpp"        // DividendEvent
#include "atx/vol/deamer.hpp"       // resolve_chain_forward, DeAmOptions
#include "atx/vol/dividend.hpp"     // HybridDivParams, hybrid_forward
#include "atx/vol/types.hpp"        // Side
#include "atx/vol/universe.hpp"     // Chain, chain_index

#include "bench_util.hpp" // apply_common, dump_counters

namespace atx::vol::bench {
namespace {

using atx::vol::AmericanMethod;
using atx::vol::american_implied_vol;
using atx::vol::american_price;
using atx::vol::american_price_cached;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CorrectionCache;
using atx::vol::DeAmOptions;
using atx::vol::DividendEvent;
using atx::vol::HybridDivParams;
using atx::vol::hybrid_forward;
using atx::vol::resolve_chain_forward;
using atx::vol::Side;

// Dump the ALWAYS-ON solve-ledger counters as sl_* columns, measured over ONE
// representative op OUTSIDE the timed loop (same discipline dump_counters uses for
// the gated cnt_* plane). The ledger is compiled into EVERY build — rel/rel-avx2
// included — so the boundary-solve / Newton-iter wins the perf sprint gates on are
// readable off the shipping binary, with no ATX_VOL_COUNTERS build. Precondition
// (ledger::reset): quiescent host — one sequential representative op, not the
// timed fan-out.
template <class Fn>
void dump_ledger(benchmark::State &state, Fn &&one_op) {
  namespace led = atx::vol::counters::ledger;
  led::reset();
  one_op();
  const led::Counts counts = led::snapshot();
  for (unsigned i = 0; i < led::kCount; ++i) {
    state.counters[led::kNames[i]] = static_cast<double>(counts.v[i]);
  }
}

// ── fit/american_iv/cached_newton ─────────────────────────────────────────
//
// The cached-inversion Newton kernel isolate. Mirrors the P1 counter fixture
// (american_iv_test.cpp :: FusedCachedInversionTraversalCount): 25 strikes x
// 4 maturities x 2 vols = 200 quotes priced BY the cached map so each inverts to
// a known root THROUGH the same cache (self-consistent round-trip), then times
// `american_implied_vol` routed via the correction cache. The Newton residual is
// american_price_cached (Black-76 + one Clenshaw value traversal); the Newton
// vega is the fused price+∂σ pass (F1/F8). This is the most-executed loop in the
// fitter, and the row exists to make its traversal-count win reportable.

struct CachedIvQuote {
  double K{0.0};
  double T{0.0};
  double sigma{0.0};
  double price{0.0};
};

// The Put correction cache the fixture inverts through. Same box/grid as the P1
// fixture (16 x 12 x 8, r=0.05, q=0, k in [-0.4, 0.4], T in [0.05, 1.0],
// sigma in [0.10, 0.60], Put) so the ClenshawSweeps counts are directly
// comparable to the landed P1 numbers.
constexpr double kCachedIvSpot = 100.0;
constexpr double kCachedIvRate = 0.05;
constexpr double kCachedIvCarry = 0.0;

[[nodiscard]] std::optional<CorrectionCache> build_cached_iv_cache() {
  auto built = CorrectionCache::build(/*n_k=*/16, /*n_T=*/12, /*n_s=*/8, kCachedIvRate,
                                      kCachedIvCarry, /*k_log_min=*/-0.4, /*k_log_max=*/0.4,
                                      /*T_min=*/0.05, /*T_max=*/1.0, /*sigma_min=*/0.10,
                                      /*sigma_max=*/0.60, Side::Put);
  if (!built) {
    return std::nullopt;
  }
  return std::move(*built);
}

[[nodiscard]] std::vector<CachedIvQuote> build_cached_iv_quotes(const CorrectionCache &tbl) {
  std::vector<CachedIvQuote> quotes;
  quotes.reserve(25u * 4u * 2u);
  for (double K = 80.0; K <= 128.5; K += 2.0) {
    for (const double T : {0.15, 0.40, 0.75, 0.90}) {
      for (const double sig : {0.18, 0.35}) {
        const double price =
            american_price_cached(kCachedIvSpot, K, T, sig, kCachedIvRate, kCachedIvCarry,
                                  Side::Put, &tbl);
        if (!(price > 0.0)) {
          continue; // skip a degenerate corner rather than corrupt the batch
        }
        quotes.push_back({K, T, sig, price});
      }
    }
  }
  return quotes;
}

void BM_CachedIvNewton(benchmark::State &state) {
  const std::optional<CorrectionCache> cache = build_cached_iv_cache();
  if (!cache) {
    state.SkipWithError("cached-IV correction cache build failed");
    return;
  }
  const std::vector<CachedIvQuote> quotes = build_cached_iv_quotes(*cache);
  if (quotes.empty()) {
    state.SkipWithError("cached-IV quote batch build failed");
    return;
  }

  const auto invert_all = [&]() {
    double sink = 0.0;
    for (const CachedIvQuote &qt : quotes) {
      const auto iv = american_implied_vol(qt.price, kCachedIvSpot, qt.K, qt.T, kCachedIvRate,
                                           kCachedIvCarry, Side::Put, AmericanMethod::AndersenLake,
                                           1.0e-7, 64, std::nullopt, &cache.value());
      sink += iv.has_value() ? *iv : 0.0;
    }
    benchmark::DoNotOptimize(sink);
  };

  for (auto _ : state) {
    invert_all();
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(quotes.size()));
  state.counters["inversions"] = static_cast<double>(quotes.size());
  // Deterministic counter evidence (one representative pass, outside timing):
  //   sl_iv_newton_iters — always-on residual/Newton steps (F1 denominator)
  //   cnt_clenshaw_sweeps — gated: 3D Clenshaw traversals (F1/F8 win metric)
  dump_ledger(state, invert_all);
  dump_counters(state, invert_all);
}

// ── deam/carry/resolve ────────────────────────────────────────────────────
//
// The de-Americanization carry (borrow) solve. Mirrors the P2 counter fixture
// (deamer_test.cpp :: WarmStartCarryCutsBoundarySolvesConvergedRootUnchanged):
// a full near-ATM co-terminal ladder so the ascending-|K-S| chain has neighbours
// to warm-start from. resolve_chain_forward runs the borrow-implication front
// half of de_americanize_chain (resolve_chain_carry): up to max_borrow_pairs
// pairs, each a bounded borrow fixed point of two American IV inversions with
// deliberately cold caches. The P2 win (warm_start_carry default ON) is the
// AL-boundary-solves-per-slice drop (474 -> 139); this row makes it reportable.

struct CarryFixture {
  Chain chain;
  double S{100.0};
  double r{0.03};
  std::vector<DividendEvent> divs;
  std::int64_t now_ns{0};
  HybridDivParams hyb{};
};

// Year-fraction -> epoch-ns (365.25-day year, matching hybrid_forward /
// deamer_test.cpp's kYearNs).
constexpr double kYearNs = 365.25 * 86400.0 * 1.0e9;
[[nodiscard]] std::int64_t years_to_ns(double y) {
  return static_cast<std::int64_t>(y * kYearNs);
}

// Gentle smile matching the deamer test fixture: base 20% vol with a mild convex
// wing in log-moneyness.
[[nodiscard]] double carry_true_sigma(double k_log) noexcept {
  return 0.20 + 0.15 * k_log * k_log;
}

[[nodiscard]] std::optional<CarryFixture> build_carry_fixture() {
  CarryFixture fx;
  fx.S = 100.0;
  fx.r = 0.03;
  fx.now_ns = 0;
  fx.divs = {{years_to_ns(0.5), 1.20}}; // one 1.20 cash dividend ex-6-months
  fx.hyb = HybridDivParams{/*prop_div_yield=*/0.02, /*blend=*/0.4};

  const double T = 1.0;
  const std::int64_t expiry_ns = years_to_ns(1.0);
  const double b_true = 0.021;
  const std::vector<double> strikes{92.0, 94.0, 96.0, 98.0, 100.0, 102.0, 104.0, 106.0, 108.0};

  const double F =
      hybrid_forward(fx.S, fx.r, b_true, T, fx.divs, expiry_ns, fx.now_ns, fx.hyb);
  const double q_eff = fx.r - std::log(F / fx.S) / T;

  fx.chain.T = T;
  fx.chain.expiry_ns = expiry_ns;
  fx.chain.strikes = strikes;
  const std::size_t two_n = 2u * strikes.size();
  fx.chain.bids.assign(two_n, 0.0);
  fx.chain.asks.assign(two_n, 0.0);
  fx.chain.mids.assign(two_n, 0.0);
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const double K = strikes[i];
    const double sig = carry_true_sigma(std::log(K / F));
    for (const Side side : {Side::Call, Side::Put}) {
      const auto price =
          american_price(fx.S, K, T, sig, fx.r, q_eff, side, AmericanMethod::AndersenLake);
      if (!price || !(*price > 0.0)) {
        return std::nullopt;
      }
      const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
      fx.chain.mids[idx] = *price;
      fx.chain.bids[idx] = *price * 0.99;
      fx.chain.asks[idx] = *price * 1.01;
    }
  }
  return fx;
}

// Registered as two rows (legacy vs warm) so the boundary-solve delta is a
// self-contained A/B on one exe.
void BM_DeAmCarryResolve(benchmark::State &state, bool warm) {
  const std::optional<CarryFixture> fx = build_carry_fixture();
  if (!fx) {
    state.SkipWithError("de-Am carry fixture build failed");
    return;
  }

  const auto resolve_once = [&]() {
    DeAmOptions opts;
    opts.hyb = fx->hyb;
    opts.n_atm = fx->chain.strikes.size();
    opts.max_borrow_pairs = fx->chain.strikes.size();
    opts.warm_start_carry = warm;
    const auto resolved = resolve_chain_forward(fx->chain, fx->S, fx->r, fx->divs, fx->now_ns, opts);
    benchmark::DoNotOptimize(resolved.has_value() ? resolved->borrow : 0.0);
  };

  for (auto _ : state) {
    resolve_once();
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["warm_start"] = warm ? 1.0 : 0.0;
  state.counters["ladder_pairs"] = static_cast<double>(fx->chain.strikes.size());
  // sl_al_boundary_solves — always-on: the P2 gate metric (per-slice AL boundary
  // solves). cnt_boundary_solves — gated mirror.
  dump_ledger(state, resolve_once);
  dump_counters(state, resolve_once);
}

// ── correction/cache/build ────────────────────────────────────────────────
//
// The per-side correction-cache build. CorrectionCache::build at the production
// 16 x 8 x 12 grid (session.cpp build_session_caches kNK/kNT/kNS), so it pays the
// same n_T*n_s = 96 cold Andersen-Lake boundary solves (one per (T, sigma) node
// row; all k_log share the row's boundary via the slice route) + the DCT-II /
// derivative-coefficient transforms the review calls out. Box mirrors a realistic
// SPY-scale session: r=0.043, q=0.02, k in [-0.5, 0.3], T in [0.02, 1.0],
// sigma in [0.05, 1.5]. Both r>0 AND q>0 so BOTH sides actually early-exercise
// (a put needs r>0, a call needs q>0) and therefore pay the full n_T*n_s=96 cold
// boundary solves — at q=0 the American call collapses to European and the build
// short-circuits every solve, measuring the degenerate path rather than the real
// 96-solve build the review wants gated. Makes the 96-solve cost a first-class number.

constexpr std::uint16_t kBuildNK = 16;
constexpr std::uint16_t kBuildNT = 8;
constexpr std::uint16_t kBuildNS = 12;
constexpr double kBuildRate = 0.043;
constexpr double kBuildCarry = 0.02;
constexpr double kBuildKMin = -0.5;
constexpr double kBuildKMax = 0.3;
constexpr double kBuildTMin = 0.02;
constexpr double kBuildTMax = 1.0;
constexpr double kBuildSigMin = 0.05;
constexpr double kBuildSigMax = 1.5;

void BM_CorrectionCacheBuild(benchmark::State &state, Side side) {
  const auto build_once = [&]() {
    auto built = CorrectionCache::build(kBuildNK, kBuildNT, kBuildNS, kBuildRate, kBuildCarry,
                                        kBuildKMin, kBuildKMax, kBuildTMin, kBuildTMax, kBuildSigMin,
                                        kBuildSigMax, side);
    benchmark::DoNotOptimize(built.has_value() ? built->n_k() : 0);
  };

  // Registration-time viability guard: a non-buildable box measures an error return.
  {
    auto probe = CorrectionCache::build(kBuildNK, kBuildNT, kBuildNS, kBuildRate, kBuildCarry,
                                        kBuildKMin, kBuildKMax, kBuildTMin, kBuildTMax, kBuildSigMin,
                                        kBuildSigMax, side);
    if (!probe) {
      state.SkipWithError("correction-cache build box rejected");
      return;
    }
  }

  for (auto _ : state) {
    build_once();
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["n_nodes"] =
      static_cast<double>(kBuildNK) * static_cast<double>(kBuildNT) * static_cast<double>(kBuildNS);
  state.counters["expected_boundary_solves"] =
      static_cast<double>(kBuildNT) * static_cast<double>(kBuildNS); // n_T * n_s = 96
  // sl_al_boundary_solves — always-on: ~96 cold AL solves per side build.
  // cnt_boundary_solves / cnt_cheb_diff_coefs — gated build-cost detail.
  dump_ledger(state, build_once);
  dump_counters(state, build_once);
}

const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("fit/american_iv/cached_newton", BM_CachedIvNewton))
      ->Unit(benchmark::kMicrosecond);
  // Distinct human-readable names per arm (no /0 /1 arg suffix) so the row set is
  // stable for the name-coverage CTest and self-describing in the JSON.
  apply_common(benchmark::RegisterBenchmark(
                   "deam/carry/resolve/legacy",
                   [](benchmark::State &s) { BM_DeAmCarryResolve(s, /*warm=*/false); }))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark(
                   "deam/carry/resolve/warm",
                   [](benchmark::State &s) { BM_DeAmCarryResolve(s, /*warm=*/true); }))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark(
                   "correction/cache/build/put",
                   [](benchmark::State &s) { BM_CorrectionCacheBuild(s, Side::Put); }))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark(
                   "correction/cache/build/call",
                   [](benchmark::State &s) { BM_CorrectionCacheBuild(s, Side::Call); }))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
