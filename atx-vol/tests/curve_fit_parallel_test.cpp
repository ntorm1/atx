#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"        // american_price, al_fast_opts, AlOpts
#include "atx/vol/curve_fit.hpp"       // fit_curve_surface, CurveSurfaceReport
#include "atx/vol/dividend.hpp"        // hybrid_forward, HybridDivParams
#include "atx/vol/opra_panel.hpp"      // load_opra_cbbo_parquet, OpraLoadSpec
#include "atx/vol/parallel_for.hpp"    // atx_auto_worker_count
#include "atx/vol/surface_parity.hpp"  // SurfaceParityInputs, SliceContext, CalendarRepair
#include "atx/vol/types.hpp"           // Side
#include "atx/vol/universe.hpp"        // Underlying, Chain, chain_index, Universe, data_install
#include "atx/vol/vol_curve.hpp"       // CurveConfig, IVolCurve

// S0-1 gate: `fit_curve_surface`'s per-chain de-Am pre-pass fans out over
// `in.fit_workers` (parallel_for block-partition), but the fit itself stays
// sequential (calendar-floor dependency). The result — the fitted surface AND
// every SliceContext — must be bit-identical for any worker count.
//
// Case 1 (SyntheticBoardBitIdenticalAcrossWorkers) is the hard gate: no data
// dependency, runs everywhere. Case 2 (SpyBoardBitIdenticalAcrossWorkers)
// proves the same identity on the real 13.9k-contract SPY board and reports
// the measured wall-clock speedup; it SKIPs cleanly if the (gitignored) cached
// parquet fixture is absent — it never triggers a Databento pull.
//
// S0-3 gate (both cases above, via `expect_per_expiry_bit_identical`):
// `build_parity_data` (the second, re-Americanization-diagnostic de-Am) moved
// from the sequential phase-2 walk into this same parallel prepass. `in` in
// both cases leaves `score_parity` at its default (true), so both cases now
// also assert `per_expiry` is bit-identical across worker counts -- a PURE
// PERF REFACTOR characterization, not a behavior change (see task S0-3).

namespace {

using atx::vol::al_fast_opts;
using atx::vol::american_price;
using atx::vol::AmericanMethod;
using atx::vol::CalendarRepair;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CurveConfig;
using atx::vol::data_install;
using atx::vol::ErrorCode;
using atx::vol::VolCurveKind;
using atx::vol::DividendEvent;
using atx::vol::fit_curve_surface;
using atx::vol::HybridDivParams;
using atx::vol::hybrid_forward;
using atx::vol::load_opra_cbbo_parquet;
using atx::vol::OpraLoadSpec;
using atx::vol::Side;
using atx::vol::SliceContext;
using atx::vol::SurfaceParityInputs;
using atx::vol::Underlying;
using atx::vol::Universe;

constexpr double kSpot = 100.0;
constexpr double kRate = 0.03;

// Convex-ish smile in log-moneyness k = ln(K/F): a parabola with a modest
// downside skew, always positive over the +/-20% moneyness range used below.
[[nodiscard]] double smile_sigma(double k) noexcept {
  return 0.22 - 0.10 * k + 0.35 * k * k;
}

// Build one synthetic chain at year-fraction T with `n_strikes` strikes spanning
// +/-20% log-moneyness around the EXACT forward `fit_curve_surface` will itself
// compute (borrow fixed at 0, no cash divs => `resolve_chain_forward` calls the
// same `hybrid_forward` with the same arguments — calling it here directly keeps
// the test's forward bit-identical to the fit's, with no risk of drift from a
// hand-rolled S*e^{rT} approximation).
//
// Only the OTM leg per strike (chain_index(idx, otm_side(k))) is populated, with
// an American premium generated via the SAME cold Andersen-Lake pricer + ACCURATE
// preset (nullopt) the fit's de-Am inverts, at q = q_eff exactly. Per deamer.hpp,
// that round-trip is self-consistent: build_observations_european recovers (to
// solver tolerance) the same `smile_sigma(k)` for every surviving strike. The
// non-preferred leg is left zeroed, which fails the quote-valid gate and is
// harmlessly ignored — both the observation builder and build_parity_data select
// the OTM leg via the identical prefer-call / otm_side rule.
[[nodiscard]] Chain make_chain(double T, int n_strikes) {
  Chain c;
  c.T = T;
  c.expiry_ns = static_cast<std::int64_t>(T * 3.1536e16);  // ACT/365-ish, ns/yr

  const std::vector<DividendEvent> no_divs;
  const double F = hybrid_forward(kSpot, kRate, /*borrow=*/0.0, T, no_divs,
                                  c.expiry_ns, /*now_ts_ns=*/0, HybridDivParams{});
  const double q_eff = kRate - std::log(F / kSpot) / T;

  constexpr double kLo = -0.20;
  constexpr double kHi = 0.20;
  c.strikes.reserve(static_cast<std::size_t>(n_strikes));
  for (int i = 0; i < n_strikes; ++i) {
    const double frac = static_cast<double>(i) / static_cast<double>(n_strikes - 1);
    const double k = kLo + frac * (kHi - kLo);
    c.strikes.push_back(F * std::exp(k));
  }

  const std::size_t n = c.strikes.size();
  c.bids.assign(2 * n, 0.0);
  c.asks.assign(2 * n, 0.0);
  c.bid_sizes.assign(2 * n, 0);
  c.ask_sizes.assign(2 * n, 0);
  c.mids.assign(2 * n, 0.0);
  c.ivs.assign(2 * n, std::numeric_limits<double>::quiet_NaN());
  c.ts_ns.assign(2 * n, 0);
  c.flags.assign(2 * n, 0);

  for (std::size_t i = 0; i < n; ++i) {
    const double K = c.strikes[i];
    const double k = std::log(K / F);
    const Side side = (k >= 0.0) ? Side::Call : Side::Put;
    const double sigma = smile_sigma(k);
    const auto px_res = american_price(kSpot, K, T, sigma, kRate, q_eff, side,
                                       AmericanMethod::AndersenLake, std::nullopt);
    if (!px_res.has_value()) {
      ADD_FAILURE() << "american_price failed for K=" << K << " T=" << T;
      continue;
    }
    const double px = *px_res;
    const double half = std::max(0.0025 * px, 1.0e-4);
    const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
    c.mids[idx] = px;
    c.bids[idx] = px - half;
    c.asks[idx] = px + half;
    c.bid_sizes[idx] = 1;
    c.ask_sizes[idx] = 1;
  }
  return c;
}

[[nodiscard]] Underlying make_synthetic_underlying() {
  Underlying u;
  u.spot = kSpot;
  for (const double T : {0.10, 0.30, 0.60, 1.00}) {
    u.chains.push_back(make_chain(T, 17));
  }
  return u;
}

[[nodiscard]] SurfaceParityInputs base_inputs(unsigned workers) {
  SurfaceParityInputs in{};
  in.S = kSpot;
  in.r = kRate;
  in.deam.imply_borrow = false;  // borrow fixed at 0 -- see make_chain's comment
  in.deam.borrow_fixed = 0.0;
  in.fit_workers = workers;
  return in;
}

// Assert every SliceContext field-wise equal, and every slice's iv(k) on a
// fixed k-grid bit-exact equal, between two CurveSurfaceReports fit from
// IDENTICAL inputs at (possibly) different worker counts.
void expect_bit_identical(const atx::vol::CurveSurfaceReport& a,
                          const atx::vol::CurveSurfaceReport& b,
                          bool strict_finite) {
  ASSERT_EQ(a.n_slices, b.n_slices);
  ASSERT_EQ(a.context.size(), b.context.size());
  for (std::size_t i = 0; i < a.context.size(); ++i) {
    const SliceContext& ca = a.context[i];
    const SliceContext& cb = b.context[i];
    EXPECT_EQ(ca.T, cb.T) << "slice " << i;
    EXPECT_EQ(ca.forward, cb.forward) << "slice " << i;
    EXPECT_EQ(ca.borrow, cb.borrow) << "slice " << i;
    EXPECT_EQ(ca.q_eff, cb.q_eff) << "slice " << i;
    EXPECT_EQ(ca.n_used, cb.n_used) << "slice " << i;
    EXPECT_EQ(ca.n_dropped, cb.n_dropped) << "slice " << i;
  }

  ASSERT_EQ(a.surface.n_slices(), b.surface.n_slices());
  constexpr double kGrid[] = {-0.10, -0.05, -0.02, 0.0, 0.02, 0.05, 0.10};
  for (std::size_t si = 0; si < a.surface.n_slices(); ++si) {
    const auto* ca = a.surface.slices()[si].get();
    const auto* cb = b.surface.slices()[si].get();
    for (const double k : kGrid) {
      const double iva = ca->iv(k);
      const double ivb = cb->iv(k);
      if (strict_finite) {
        ASSERT_TRUE(std::isfinite(iva)) << "slice " << si << " k=" << k;
        ASSERT_TRUE(std::isfinite(ivb)) << "slice " << si << " k=" << k;
      } else {
        EXPECT_EQ(std::isfinite(iva), std::isfinite(ivb))
            << "slice " << si << " k=" << k;
        if (!std::isfinite(iva) || !std::isfinite(ivb)) {
          continue;  // out-of-domain sample on the real board; skip the value check
        }
      }
      EXPECT_EQ(iva, ivb) << "slice " << si << " k=" << k;
    }
  }
}

// S0-3 gate: the SECOND cold de-Am (`build_parity_data`, the re-Americanized
// parity diagnostic's market-side board re-inversion) moved from the
// sequential phase-2 walk into the parallel per-chain prepass. It is a PURE
// PERF REFACTOR -- every `ParityReport` field in `per_expiry` must be
// bit-identical across worker counts with `score_parity=true` (each chain's
// ParityData is an independent, disjoint prepass slot -- the value_chain
// determinism pattern already relied on for the fitted surface itself).
// Compares every field with EXPECT_EQ (bit-exact, not a tolerance check).
void expect_per_expiry_bit_identical(const atx::vol::CurveSurfaceReport& a,
                                     const atx::vol::CurveSurfaceReport& b) {
  ASSERT_EQ(a.per_expiry.size(), b.per_expiry.size());
  for (std::size_t i = 0; i < a.per_expiry.size(); ++i) {
    const auto& pa = a.per_expiry[i];
    const auto& pb = b.per_expiry[i];
    EXPECT_EQ(pa.n, pb.n) << "slice " << i;
    EXPECT_EQ(pa.n_within, pb.n_within) << "slice " << i;
    EXPECT_EQ(pa.frac_fv_within_bidask, pb.frac_fv_within_bidask) << "slice " << i;
    EXPECT_EQ(pa.rmse_mid_price, pb.rmse_mid_price) << "slice " << i;
    EXPECT_EQ(pa.rmse_mid_vol, pb.rmse_mid_vol) << "slice " << i;
    EXPECT_EQ(pa.chi2_reduced, pb.chi2_reduced) << "slice " << i;
    EXPECT_EQ(pa.frac_within_edge_band, pb.frac_within_edge_band) << "slice " << i;
    EXPECT_EQ(pa.mean_edge_vol, pb.mean_edge_vol) << "slice " << i;
  }
}

// Locate the cached SPY parquet across the paths a test binary might run from.
// (Copied from curve_noarb_test.cpp / spy_real_test.cpp — same gitignored fixture.)
[[nodiscard]] std::string find_spy_parquet() {
  const char* candidates[] = {
      "data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
      "../data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
      "../../data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
      "C:/atx/data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
  };
  for (const char* c : candidates) {
    if (std::filesystem::exists(c)) {
      return c;
    }
  }
  return {};
}

// S0-4': portable ATX_VOL_FIT_WORKERS env set/unset for the cap test below.
#if defined(_MSC_VER)
void set_fit_workers_env(const char* value) { ::_putenv_s("ATX_VOL_FIT_WORKERS", value); }
void unset_fit_workers_env() { ::_putenv_s("ATX_VOL_FIT_WORKERS", ""); }
#else
void set_fit_workers_env(const char* value) { ::setenv("ATX_VOL_FIT_WORKERS", value, 1); }
void unset_fit_workers_env() { ::unsetenv("ATX_VOL_FIT_WORKERS"); }
#endif

// RAII guard: captures ATX_VOL_FIT_WORKERS on construction and restores it on
// destruction. Using a destructor (rather than plain end-of-test cleanup)
// means the prior value is restored even if an ASSERT_* below exits the test
// early -- this test can never leak env state into another test sharing the
// process.
class FitWorkersEnvGuard {
 public:
  FitWorkersEnvGuard() {
#if defined(_MSC_VER)
    char* prev = nullptr;
    std::size_t prev_n = 0;
    had_prev_ = (::_dupenv_s(&prev, &prev_n, "ATX_VOL_FIT_WORKERS") == 0) && (prev != nullptr);
    if (prev != nullptr) {
      prev_val_ = prev;
      std::free(prev);
    }
#else
    const char* prev = std::getenv("ATX_VOL_FIT_WORKERS");
    had_prev_ = prev != nullptr;
    if (prev != nullptr) {
      prev_val_ = prev;
    }
#endif
  }
  FitWorkersEnvGuard(const FitWorkersEnvGuard&) = delete;
  FitWorkersEnvGuard& operator=(const FitWorkersEnvGuard&) = delete;
  ~FitWorkersEnvGuard() {
    if (had_prev_) {
      set_fit_workers_env(prev_val_.c_str());
    } else {
      unset_fit_workers_env();
    }
  }

 private:
  bool had_prev_ = false;
  std::string prev_val_;
};

}  // namespace

TEST(CurveFitParallel, SyntheticBoardBitIdenticalAcrossWorkers) {
  const Underlying under = make_synthetic_underlying();
  CurveConfig cfg;  // default = ConvexDense

  const SurfaceParityInputs in1 = base_inputs(1);
  auto rep1 = fit_curve_surface(under, in1, cfg);
  ASSERT_TRUE(rep1.has_value()) << rep1.error().to_string();

  const SurfaceParityInputs in8 = base_inputs(8);
  auto rep8 = fit_curve_surface(under, in8, cfg);
  ASSERT_TRUE(rep8.has_value()) << rep8.error().to_string();

  ASSERT_EQ(rep1->n_slices, 4u);
  expect_bit_identical(*rep1, *rep8, /*strict_finite*/ true);
  // S0-3: base_inputs() leaves score_parity at its default (true), so this
  // also exercises the fanned-out second de-Am -- per_expiry must be
  // bit-identical across worker counts too.
  expect_per_expiry_bit_identical(*rep1, *rep8);
}

// S0-4': the ATX_VOL_FIT_WORKERS env cap changes only what
// `atx_auto_worker_count()` resolves the AUTO (fit_workers=0) case to -- never
// the fitted result. Fit twice with fit_workers=0 (auto): once with the env
// forcing serial (=1), once with the env unset (hardware_concurrency), and
// assert bit-identical output -- proving the cap is perf-only.
TEST(CurveFitParallel, FitBitIdenticalUnderEnvCap) {
  FitWorkersEnvGuard env_guard;  // restores ATX_VOL_FIT_WORKERS on scope exit

  const Underlying under = make_synthetic_underlying();
  CurveConfig cfg;  // default = ConvexDense

  set_fit_workers_env("1");
  ASSERT_EQ(atx::vol::atx_auto_worker_count(), 1u);
  const SurfaceParityInputs in_capped = base_inputs(0);  // auto -- capped to 1 by env
  auto rep_capped = fit_curve_surface(under, in_capped, cfg);
  ASSERT_TRUE(rep_capped.has_value()) << rep_capped.error().to_string();

  unset_fit_workers_env();
  const SurfaceParityInputs in_auto = base_inputs(0);  // auto -- hardware_concurrency
  auto rep_auto = fit_curve_surface(under, in_auto, cfg);
  ASSERT_TRUE(rep_auto.has_value()) << rep_auto.error().to_string();

  ASSERT_EQ(rep_capped->n_slices, 4u);
  expect_bit_identical(*rep_capped, *rep_auto, /*strict_finite*/ true);
  expect_per_expiry_bit_identical(*rep_capped, *rep_auto);
}

TEST(CurveFitParallel, SpyBoardBitIdenticalAcrossWorkers) {
  const std::string path = find_spy_parquet();
  if (path.empty()) {
    GTEST_SKIP() << "cached SPY OPRA parquet not found; run the databento pull + "
                    "opra_dbn_to_parquet to materialise the fixture.";
  }

  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "SPY";
  spec.snapshot_iso = "2026-06-05T19:55:00Z";
  spec.r = 0.043;
  const auto panel = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  Universe u;
  const auto uid = data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value());
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  const Underlying* U = *under;

  SurfaceParityInputs in{};
  in.S = panel->implied_spot;
  in.r = spec.r;
  in.now_ts_ns = panel->frame.snapshot_ts_ns;
  in.band_k = 1.0;
  in.repair = CalendarRepair::None;
  // Same fast-cold Andersen-Lake preset the served path uses (matches
  // curve_noarb_test.cpp) -- this is what production actually serves.
  in.deam.al_opts = al_fast_opts();
  in.deam.iv_tol = 1.0e-5;
  in.deam.n_atm = 1;

  CurveConfig cfg;  // default = ConvexDense (the served dense surface)

  SurfaceParityInputs in1 = in;
  in1.fit_workers = 1;  // serial -- the pre-S0-1 path
  const auto t0 = std::chrono::steady_clock::now();
  auto rep1 = fit_curve_surface(*U, in1, cfg);
  const auto t1 = std::chrono::steady_clock::now();
  ASSERT_TRUE(rep1.has_value()) << rep1.error().to_string();

  SurfaceParityInputs in0 = in;
  in0.fit_workers = 0;  // auto -- hardware_concurrency
  const auto t2 = std::chrono::steady_clock::now();
  auto rep0 = fit_curve_surface(*U, in0, cfg);
  const auto t3 = std::chrono::steady_clock::now();
  ASSERT_TRUE(rep0.has_value()) << rep0.error().to_string();

  const double ms1 = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double ms0 = std::chrono::duration<double, std::milli>(t3 - t2).count();
  // Informational only -- NOT a hard timing assert (flaky; stripped elsewhere
  // in this suite). The bit-identity assertions below are the actual gate.
  std::printf(
      "[CurveFitParallel SPY] fit_workers=1 %.1fms, fit_workers=0(auto) %.1fms, "
      "speedup=%.2fx\n",
      ms1, ms0, (ms0 > 0.0) ? (ms1 / ms0) : 0.0);

  expect_bit_identical(*rep1, *rep0, /*strict_finite*/ false);
  // S0-3: `in` above never sets score_parity, so it defaults true -- both fits
  // above already pay for (fit_workers=1: sequential, fit_workers=0: fanned)
  // the second de-Am, so the ms1 vs ms0 print above IS the parity-ON
  // speedup vs the pre-S0-3 sequential-parity baseline. Assert the fanned-out
  // second de-Am did not perturb the diagnostic itself.
  expect_per_expiry_bit_identical(*rep1, *rep0);
}

// S0-2 gate: `SurfaceParityInputs::score_parity` (default true) lets a caller
// that does not need the re-Americanized parity diagnostic skip the SECOND
// cold de-Am pass (`build_parity_data`) entirely. The fitted surface and every
// `SliceContext` must stay bit-identical regardless of the flag -- parity is a
// pure diagnostic and must not perturb the fit.
TEST(CurveFitParity, ParityOffMatchesParityOnSurface) {
  const Underlying under = make_synthetic_underlying();
  CurveConfig cfg;  // default = ConvexDense

  SurfaceParityInputs in_on = base_inputs(1);
  in_on.score_parity = true;
  auto rep_on = fit_curve_surface(under, in_on, cfg);
  ASSERT_TRUE(rep_on.has_value()) << rep_on.error().to_string();

  SurfaceParityInputs in_off = base_inputs(1);
  in_off.score_parity = false;
  auto rep_off = fit_curve_surface(under, in_off, cfg);
  ASSERT_TRUE(rep_off.has_value()) << rep_off.error().to_string();

  ASSERT_EQ(rep_on->n_slices, 4u);
  expect_bit_identical(*rep_on, *rep_off, /*strict_finite*/ true);

  ASSERT_EQ(rep_on->per_expiry.size(), rep_off->per_expiry.size());
  std::size_t n_scored_on = 0;
  for (const auto& pr : rep_on->per_expiry) {
    if (pr.n > 0) {
      ++n_scored_on;
    }
  }
  EXPECT_GE(n_scored_on, 1u) << "score_parity=true should score at least one slice";

  for (const auto& pr : rep_off->per_expiry) {
    EXPECT_EQ(pr.n, 0u) << "score_parity=false must leave per_expiry zeroed";
  }
  EXPECT_EQ(rep_off->worst_frac_within_bidask, 0.0);
}

// Informational: with the diagnostic off, the fit skips the second cold de-Am
// pass over the real SPY board. Report both wall-clocks + the speedup factor;
// do NOT hard-assert timing (flaky under shared CI load).
TEST(CurveFitParity, ParityOffSkipsSecondDeAmOnSpy) {
  const std::string path = find_spy_parquet();
  if (path.empty()) {
    GTEST_SKIP() << "cached SPY OPRA parquet not found; run the databento pull + "
                    "opra_dbn_to_parquet to materialise the fixture.";
  }

  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "SPY";
  spec.snapshot_iso = "2026-06-05T19:55:00Z";
  spec.r = 0.043;
  const auto panel = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  Universe u;
  const auto uid = data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value());
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  const Underlying* U = *under;

  SurfaceParityInputs in{};
  in.S = panel->implied_spot;
  in.r = spec.r;
  in.now_ts_ns = panel->frame.snapshot_ts_ns;
  in.band_k = 1.0;
  in.repair = CalendarRepair::None;
  in.deam.al_opts = al_fast_opts();
  in.deam.iv_tol = 1.0e-5;
  in.deam.n_atm = 1;
  in.fit_workers = 0;  // auto -- the served worker policy

  CurveConfig cfg;  // default = ConvexDense (the served dense surface)

  SurfaceParityInputs in_on = in;
  in_on.score_parity = true;
  const auto t0 = std::chrono::steady_clock::now();
  auto rep_on = fit_curve_surface(*U, in_on, cfg);
  const auto t1 = std::chrono::steady_clock::now();
  ASSERT_TRUE(rep_on.has_value()) << rep_on.error().to_string();

  SurfaceParityInputs in_off = in;
  in_off.score_parity = false;
  const auto t2 = std::chrono::steady_clock::now();
  auto rep_off = fit_curve_surface(*U, in_off, cfg);
  const auto t3 = std::chrono::steady_clock::now();
  ASSERT_TRUE(rep_off.has_value()) << rep_off.error().to_string();

  const double ms_on = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double ms_off = std::chrono::duration<double, std::milli>(t3 - t2).count();
  std::printf(
      "[CurveFitParity SPY] score_parity=true %.1fms, score_parity=false %.1fms, "
      "speedup=%.2fx\n",
      ms_on, ms_off, (ms_off > 0.0) ? (ms_on / ms_off) : 0.0);

  expect_bit_identical(*rep_on, *rep_off, /*strict_finite*/ false);

  for (const auto& pr : rep_off->per_expiry) {
    EXPECT_EQ(pr.n, 0u);
  }
  EXPECT_EQ(rep_off->worst_frac_within_bidask, 0.0);
}

// FIX A gate: the opt-in per-slice LinearVariance fallback. Force EVERY SplineVol
// slice fit to fail (min_obs set above the per-slice usable-row count). With the
// flag OFF this is the historical behavior — every slice is dropped, the surface
// is empty, and fit_curve_surface returns NotFound. With the flag ON, each failed
// SplineVol slice is retried as LinearVariance (>= 2 nodes) and served, so the
// board that would otherwise error now produces a full surface of linear slices.
TEST(CurveFitSliceFallback, LinearFallbackRecoversForcedSplineFailures) {
  const Underlying under = make_synthetic_underlying();

  CurveConfig cfg;
  cfg.kind = VolCurveKind::SplineVol;
  cfg.spline.min_obs = 999;  // force every SplineVol slice fit to fail

  // Flag OFF (default): every slice fails => surface empty => Err (byte-identical
  // to the historical drop-the-slice path).
  const SurfaceParityInputs in_off = base_inputs(1);
  auto rep_off = fit_curve_surface(under, in_off, cfg);
  ASSERT_FALSE(rep_off.has_value());
  EXPECT_EQ(rep_off.error().code(), ErrorCode::NotFound);

  // Flag ON: each failed SplineVol slice is recovered via LinearVariance.
  SurfaceParityInputs in_on = base_inputs(1);
  in_on.calib.per_slice_linear_fallback = true;
  auto rep_on = fit_curve_surface(under, in_on, cfg);
  ASSERT_TRUE(rep_on.has_value()) << rep_on.error().to_string();
  EXPECT_EQ(rep_on->n_slices, 4u);
  EXPECT_EQ(rep_on->n_slice_linear_fallback, 4u);
  ASSERT_EQ(rep_on->surface.n_slices(), 4u);
  for (std::size_t i = 0; i < rep_on->surface.n_slices(); ++i) {
    EXPECT_EQ(rep_on->surface.slices()[i]->kind(), VolCurveKind::LinearVariance)
        << "slice " << i << " should be a linear fallback";
  }
}

// The fallback flag must not perturb a board whose primary curve already fits:
// with the default ConvexDense config (which fits all four synthetic slices), the
// flag-on surface is bit-identical to flag-off and no fallback slice is used.
TEST(CurveFitSliceFallback, FlagDoesNotPerturbSucceedingFit) {
  const Underlying under = make_synthetic_underlying();
  CurveConfig cfg;  // default = ConvexDense, succeeds on every slice

  const SurfaceParityInputs in_off = base_inputs(1);
  auto rep_off = fit_curve_surface(under, in_off, cfg);
  ASSERT_TRUE(rep_off.has_value()) << rep_off.error().to_string();

  SurfaceParityInputs in_on = base_inputs(1);
  in_on.calib.per_slice_linear_fallback = true;
  auto rep_on = fit_curve_surface(under, in_on, cfg);
  ASSERT_TRUE(rep_on.has_value()) << rep_on.error().to_string();

  EXPECT_EQ(rep_off->n_slice_linear_fallback, 0u);
  EXPECT_EQ(rep_on->n_slice_linear_fallback, 0u);
  ASSERT_EQ(rep_off->n_slices, rep_on->n_slices);
  expect_bit_identical(*rep_off, *rep_on, /*strict_finite*/ true);
}

// F3 (W3.3) gate: the opt-in per-slice Legacy-prep rescue (thin-slice recovery).
// A very tight max_spread_to_mid_pct starves the Configured observation funnel on
// EVERY slice (synthetic spread/mid ~0.5% > the 0.1% cap => build_observations_
// european drops all rows => NotFound), while the permissive
// LegacyEssviCompatibility predicate ignores that spread cap and keeps them. With
// the flag OFF the board starves to NotFound (byte-identical historical drop);
// with the flag ON each starved slice is re-prepared under Legacy and served,
// recovering the whole board — the "80% failure" cohort mechanism in miniature.
TEST(CurveFitLegacyPrepRescue, RecoversConfiguredStarvedSlices) {
  const Underlying under = make_synthetic_underlying();
  CurveConfig cfg;  // default ConvexDense

  // Flag OFF: Configured starves every slice => surface empty => NotFound.
  SurfaceParityInputs in_off = base_inputs(1);
  in_off.calib.max_spread_to_mid_pct = 0.001;  // 0.1% cap starves Configured prep
  auto rep_off = fit_curve_surface(under, in_off, cfg);
  ASSERT_FALSE(rep_off.has_value());
  EXPECT_EQ(rep_off.error().code(), ErrorCode::NotFound);

  // Flag ON: each starved slice recovered under LegacyEssviCompatibility prep.
  SurfaceParityInputs in_on = in_off;
  in_on.per_slice_legacy_prep_fallback = true;
  auto rep_on = fit_curve_surface(under, in_on, cfg);
  ASSERT_TRUE(rep_on.has_value()) << rep_on.error().to_string();
  EXPECT_EQ(rep_on->n_slices, 4u);
  EXPECT_EQ(rep_on->n_slices_legacy_rescued, 4u);
  EXPECT_EQ(rep_on->n_slices_starved, 0u);
  ASSERT_EQ(rep_on->surface.n_slices(), 4u);
}

// The rescue flag must not perturb a board whose Configured preparation already
// succeeds: flag-on is bit-identical to flag-off and no slice is rescued.
TEST(CurveFitLegacyPrepRescue, FlagDoesNotPerturbHealthyBoard) {
  const Underlying under = make_synthetic_underlying();
  CurveConfig cfg;  // default ConvexDense, fits every slice under Configured

  const SurfaceParityInputs in_off = base_inputs(1);
  auto rep_off = fit_curve_surface(under, in_off, cfg);
  ASSERT_TRUE(rep_off.has_value()) << rep_off.error().to_string();

  SurfaceParityInputs in_on = base_inputs(1);
  in_on.per_slice_legacy_prep_fallback = true;
  auto rep_on = fit_curve_surface(under, in_on, cfg);
  ASSERT_TRUE(rep_on.has_value()) << rep_on.error().to_string();

  EXPECT_EQ(rep_off->n_slices_legacy_rescued, 0u);
  EXPECT_EQ(rep_on->n_slices_legacy_rescued, 0u);
  EXPECT_EQ(rep_on->n_slices_starved, 0u);
  ASSERT_EQ(rep_off->n_slices, rep_on->n_slices);
  expect_bit_identical(*rep_off, *rep_on, /*strict_finite*/ true);
}
